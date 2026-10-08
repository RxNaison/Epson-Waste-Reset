#include "ewr/ewr_c.h"

#include "ewr/deviceid.h"
#include "ewr/generator.h"
#include "ewr/json_out.h"
#include "ewr/log.h"
#include "ewr/session.h"
#include "ewr/snmp_gateway.h"
#include "ewr/version.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The C ABI over the C++ core. Nothing here decides anything: it translates
// handles, callbacks and JSON, and every hardware rule stays where it was.
//
// The rule that shapes the file: no C++ exception and no C++ type may cross
// the boundary. Every entry point returns an ewr_status, and the detail lands
// in the session's last error for the caller to read.

namespace {

    // Callers get their own copy of everything; nothing they hold points into
    // EWR's memory except where the header says so.
    char* DuplicateString(const std::string& text)
    {
        char* copy = static_cast<char*>(std::malloc(text.size() + 1));
        if (!copy)
            return nullptr;

        std::memcpy(copy, text.c_str(), text.size() + 1);
        return copy;
    }

    std::string LowerCase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

} // namespace

struct ewr_session
{
    ewr::UniversalGenerator database;
    // One way to the printer for the session's whole life: the network when
    // it was opened with ewr_session_open_network, USB otherwise.
    ewr::UsbDeviceGateway usb;
    std::unique_ptr<ewr::SnmpDeviceGateway> net;
    ewr::ExecutorOptions options = ewr::DefaultQueryOptions();

    ewr_event_cb eventCallback = nullptr;
    void* eventUser = nullptr;
    ewr_decision_cb blockerCallback = nullptr;
    void* blockerUser = nullptr;
    ewr_decision_cb confirmCallback = nullptr;
    void* confirmUser = nullptr;
    bool allowModelMismatch = false;

    // Owned by the session, handed out by ewr_session_last_error.
    std::string lastError;
    // Removed on close: log::Default() outlives the session.
    int sinkId = 0;

    void Fail(const std::string& detail) { lastError = detail; }

    ewr::IDeviceGateway& Gateway()
    {
        return net ? static_cast<ewr::IDeviceGateway&>(*net) : static_cast<ewr::IDeviceGateway&>(usb);
    }

    bool ClaimPrinter() { return net ? net->ClaimPrinter() : usb.ClaimPrinter(); }

    ewr::DeviceIdQueryResult QueryDeviceId() { return net ? net->QueryDeviceId() : usb.QueryDeviceId(); }

    // Why the printer said nothing. The network can tell a host that does not
    // resolve, or a send this machine refused, from a printer that is silent.
    std::string Unanswered(const std::string& text) const
    {
        return (net && !net->Answered()) ? net->SilenceError() : text;
    }

    // The CLI's --ip scope, held to here too: what was never tried over the
    // network is refused rather than attempted.
    bool RefuseOverNetwork(const char* what)
    {
        if (!net)
            return false;

        Fail(std::string(what) + " needs USB; this session reaches its printer over the network.");
        return true;
    }

    void Emit(const ewr::log::Event& event) const
    {
        if (!eventCallback)
            return;

        const std::string fields = nlohmann::json(event.fields).dump();

        ewr_event out;
        out.size = sizeof(ewr_event);
        out.code = event.code.c_str();
        out.message = event.message.c_str();
        out.level = static_cast<int>(event.level);
        out.stage = static_cast<int>(event.stage);
        out.index = event.HasProgress() ? event.index : -1;
        out.total = event.HasProgress() ? event.total : -1;
        out.fields_json = fields.c_str();

        eventCallback(&out, eventUser);
    }

    // Exact name first, then case-insensitive, then an alias: the same order
    // a person would try.
    bool FindModel(const char* wanted, ewr::DbPrinterModel& out) const
    {
        if (!wanted || !*wanted)
            return false;

        const std::vector<ewr::DbPrinterModel> models = database.GetAvailableModels();
        const std::string needle = LowerCase(wanted);

        for (const auto& model : models)
        {
            if (model.name == wanted)
            {
                out = model;
                return true;
            }
        }

        for (const auto& model : models)
        {
            if (LowerCase(model.name) == needle)
            {
                out = model;
                return true;
            }

            for (const std::string& alias : model.aliases)
            {
                if (LowerCase(alias) == needle)
                {
                    out = model;
                    return true;
                }
            }
        }

        return false;
    }
};

namespace {

    // One place where a C++ exception stops being one: a throw that escaped
    // the core would otherwise cross the ABI and take the process with it.
    template <typename Work>
    int Guard(ewr_session* session, Work work)
    {
        if (!session)
            return EWR_ERR_INVALID_ARGUMENT;

        session->lastError.clear();

        try
        {
            return work();
        }
        catch (const std::exception& error)
        {
            session->Fail(error.what());
            return EWR_ERR_FAILED;
        }
        catch (...)
        {
            session->Fail("Unknown failure.");
            return EWR_ERR_FAILED;
        }
    }

    int Deliver(ewr_session* session, const nlohmann::json& payload, char** out)
    {
        if (!out)
            return EWR_ERR_INVALID_ARGUMENT;

        *out = DuplicateString(payload.dump());
        if (!*out)
        {
            session->Fail("Out of memory.");
            return EWR_ERR_FAILED;
        }

        return EWR_OK;
    }

    int ResolveModel(ewr_session* session, const char* name, ewr::DbPrinterModel& model)
    {
        if (!name || !*name)
        {
            session->Fail("No model name was given.");
            return EWR_ERR_INVALID_ARGUMENT;
        }

        if (session->database.IsEmpty())
        {
            session->Fail("No printer database is loaded.");
            return EWR_ERR_DATABASE;
        }

        if (!session->FindModel(name, model))
        {
            session->Fail("No model in the database matches \"" + std::string(name ? name : "") + "\".");
            return EWR_ERR_MODEL_UNKNOWN;
        }

        return EWR_OK;
    }

    // `host` null is a USB session.
    int OpenSession(const char* databasePath, const char* host, ewr_session** out)
    {
        std::unique_ptr<ewr_session> session;
        try
        {
            session = std::make_unique<ewr_session>();
            if (host)
                session->net = std::make_unique<ewr::SnmpDeviceGateway>(host);
        }
        catch (...)
        {
            return EWR_ERR_FAILED;
        }

        ewr_session* raw = session.get();
        session->sinkId = ewr::log::Default().AddSink([raw](const ewr::log::Event& event)
        {
            raw->Emit(event);
        });

        // Before the database, so a second process learns why it cannot start
        // rather than spending a second parsing 1450 models first.
        if (!session->ClaimPrinter())
        {
            ewr::log::Default().RemoveSink(session->sinkId);
            return EWR_ERR_ANOTHER_RUN;
        }

        const std::string path = (databasePath && *databasePath) ? databasePath : "database.json";
        if (!session->database.LoadDatabase(path))
        {
            session->Fail("Could not load the printer database at \"" + path + "\".");
            // Kept open: the caller can still read the error, and the handle is
            // theirs to close.
            *out = session.release();
            return EWR_ERR_DATABASE;
        }

        // The claim opened the channel, so a host that does not resolve is
        // known now rather than at the first call. Kept open like the above,
        // and the database calls still work.
        if (session->net && !session->net->OpenError().empty())
        {
            session->Fail(session->net->OpenError());
            *out = session.release();
            return EWR_ERR_DEVICE_NOT_FOUND;
        }

        *out = session.release();
        return EWR_OK;
    }

} // namespace

extern "C" {

const char* ewr_version(void)
{
    return EWR_VERSION;
}

int ewr_abi_version(void)
{
    return 2;
}

int ewr_json_contract_version(void)
{
    return ewr::JsonEmitter::kContractVersion;
}

const char* ewr_status_name(int status)
{
    switch (status)
    {
        case EWR_OK:                   return "ok";
        case EWR_ERR_FAILED:           return "failed";
        case EWR_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case EWR_ERR_ANOTHER_RUN:      return "another_run";
        case EWR_ERR_DEVICE_NOT_FOUND: return "device_not_found";
        case EWR_ERR_DATABASE:         return "database";
        case EWR_ERR_MODEL_UNKNOWN:    return "model_unknown";
        case EWR_ERR_NOT_SUPPORTED:    return "not_supported";
        case EWR_ERR_READ_FAILED:      return "read_failed";
        case EWR_ERR_INCOMPLETE_DUMP:  return "incomplete_dump";
        case EWR_ERR_BLOCKED:          return "blocked";
        case EWR_ERR_WRITE_FAILED:     return "write_failed";
        case EWR_ERR_WRITE_UNVERIFIED: return "write_unverified";
        case EWR_ERR_IO:               return "io_error";
        case EWR_ERR_MODEL_MISMATCH:   return "model_mismatch";
        default:                       return "unknown";
    }
}

void ewr_string_free(char* text)
{
    std::free(text);
}

int ewr_session_open(const char* database_path, ewr_session** out_session)
{
    if (!out_session)
        return EWR_ERR_INVALID_ARGUMENT;

    *out_session = nullptr;
    return OpenSession(database_path, nullptr, out_session);
}

int ewr_session_open_network(const char* database_path, const char* host, ewr_session** out_session)
{
    if (!out_session)
        return EWR_ERR_INVALID_ARGUMENT;

    *out_session = nullptr;

    // Not "no host, so USB": a USB session in its place could reset
    // whichever printer is plugged in instead of the one that was meant.
    if (!host || !*host)
        return EWR_ERR_INVALID_ARGUMENT;

    return OpenSession(database_path, host, out_session);
}

void ewr_session_close(ewr_session* session)
{
    if (!session)
        return;

    ewr::log::Default().RemoveSink(session->sinkId);
    delete session;
}

void ewr_session_set_event_callback(ewr_session* session, ewr_event_cb callback, void* user)
{
    if (!session)
        return;

    session->eventCallback = callback;
    session->eventUser = user;
}

void ewr_session_set_blocker_callback(ewr_session* session, ewr_decision_cb callback, void* user)
{
    if (!session)
        return;

    session->blockerCallback = callback;
    session->blockerUser = user;
}

void ewr_session_set_confirm_callback(ewr_session* session, ewr_decision_cb callback, void* user)
{
    if (!session)
        return;

    session->confirmCallback = callback;
    session->confirmUser = user;
}

void ewr_session_set_interface(ewr_session* session, int candidate)
{
    if (session)
        session->options.interfaceCandidate = candidate;
}

void ewr_session_set_soft_reset(ewr_session* session, int enabled)
{
    if (session)
        session->options.usbSoftReset = (enabled != 0);
}

void ewr_session_set_allow_model_mismatch(ewr_session* session, int allowed)
{
    if (session)
        session->allowModelMismatch = (allowed != 0);
}

const char* ewr_session_last_error(ewr_session* session)
{
    return session ? session->lastError.c_str() : "";
}

int ewr_list_interfaces(ewr_session* session, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        if (session->RefuseOverNetwork("Listing USB interfaces"))
            return EWR_ERR_NOT_SUPPORTED;

        nlohmann::json interfaces = nlohmann::json::array();

        std::vector<ewr::ModelNameEntry> entries;
        for (const auto& model : session->database.GetAvailableModels())
            entries.push_back({ model.name, model.aliases });

        for (const ewr::InterfaceInfo& info : session->usb.ListInterfaces())
        {
            const ewr::DeviceIdInfo reported = ewr::ParseIeee1284DeviceId(info.deviceId);
            const std::vector<std::string> matches = reported.model.empty()
                ? std::vector<std::string>{} : ewr::MatchModelEntries(reported.model, entries);
            interfaces.push_back(ewr::JsonInterface(info, matches.empty() ? std::string() : matches[0]));
        }

        if (interfaces.empty())
        {
            session->Fail("No Epson USB interfaces found.");
            return EWR_ERR_DEVICE_NOT_FOUND;
        }

        return Deliver(session, { { "interfaces", std::move(interfaces) } }, out_json);
    });
}

int ewr_detect_model(ewr_session* session, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        const ewr::DeviceIdQueryResult query = session->QueryDeviceId();
        if (!query.found)
        {
            session->Fail(session->Unanswered("No Epson interface answered the device ID query."));
            return EWR_ERR_DEVICE_NOT_FOUND;
        }

        const ewr::DeviceIdInfo info = ewr::ParseIeee1284DeviceId(query.deviceId);

        std::vector<ewr::ModelNameEntry> entries;
        for (const auto& model : session->database.GetAvailableModels())
            entries.push_back({ model.name, model.aliases });

        const std::vector<std::string> matches = info.model.empty()
            ? std::vector<std::string>{} : ewr::MatchModelEntries(info.model, entries);

        nlohmann::json payload;
        payload["device_id"] = query.deviceId;
        payload["reported_model"] = info.model.empty() ? nlohmann::json(nullptr) : nlohmann::json(info.model);
        payload["model"] = matches.empty() ? nlohmann::json(nullptr) : nlohmann::json(matches[0]);
        return Deliver(session, payload, out_json);
    });
}

int ewr_discover_network(ewr_session* session, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        // Before two seconds of listening, not after.
        if (!out_json)
            return EWR_ERR_INVALID_ARGUMENT;

        std::string error;
        const std::vector<ewr::NetworkPrinter> found = ewr::DiscoverNetworkPrinters(ewr::kDiscoveryWaitMs, error);
        if (!error.empty())
        {
            session->Fail(error);
            return EWR_ERR_IO;
        }

        std::vector<ewr::ModelNameEntry> entries;
        for (const auto& model : session->database.GetAvailableModels())
            entries.push_back({ model.name, model.aliases });

        nlohmann::json printers = nlohmann::json::array();
        for (const ewr::NetworkPrinter& printer : found)
        {
            const std::vector<std::string> matches = printer.model.empty()
                ? std::vector<std::string>{} : ewr::MatchModelEntries(printer.model, entries);
            printers.push_back(ewr::JsonNetworkPrinter(printer, matches.empty() ? std::string() : matches[0]));
        }

        return Deliver(session, { { "printers", std::move(printers) } }, out_json);
    });
}

int ewr_list_models(ewr_session* session, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        if (session->database.IsEmpty())
        {
            session->Fail("No printer database is loaded.");
            return EWR_ERR_DATABASE;
        }

        nlohmann::json models = nlohmann::json::array();
        for (const auto& model : session->database.GetAvailableModels())
        {
            nlohmann::json entry;
            entry["name"] = model.name;
            entry["aliases"] = model.aliases;
            entry["resettable"] = !model.GetAllAddresses().empty();
            entry["has_ink_reset"] = model.HasInkReset();
            models.push_back(std::move(entry));
        }

        return Deliver(session, { { "models", std::move(models) } }, out_json);
    });
}

int ewr_plan(ewr_session* session, const char* model, int ink, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::DbPrinterModel target;
        const int found = ResolveModel(session, model, target);
        if (found != EWR_OK)
            return found;

        if (ink && !target.HasInkReset())
        {
            session->Fail(target.name + " has no cartridge ink map in the database.");
            return EWR_ERR_NOT_SUPPORTED;
        }

        const std::vector<uint16_t> addresses = ink ? target.GetInkAddresses() : target.GetAllAddresses();
        const std::vector<uint8_t> values = ink ? target.GetInkResetValues() : target.GetAllResetValues();

        if (addresses.empty())
        {
            session->Fail(target.name + " has no reset addresses in the database.");
            return EWR_ERR_NOT_SUPPORTED;
        }

        nlohmann::json planned = nlohmann::json::array();
        for (std::size_t i = 0; i < addresses.size(); ++i)
        {
            nlohmann::json write;
            write["address"] = addresses[i];
            write["value"] = (i < values.size()) ? values[i] : 0;
            planned.push_back(std::move(write));
        }

        nlohmann::json payload;
        payload["model"] = target.name;
        payload["target"] = ink ? "ink" : "waste";
        payload["planned_writes"] = std::move(planned);
        // Which pads those writes clear; an ink reset clears none.
        payload["reset_covers"] = ink ? nlohmann::json::array() : ewr::JsonResetCoverage(target);
        return Deliver(session, payload, out_json);
    });
}

int ewr_read_status(ewr_session* session, const char* model, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::DbPrinterModel target;
        const int found = ResolveModel(session, model, target);
        if (found != EWR_OK)
            return found;

        ewr::Session reader(target, session->Gateway(), ewr::log::Default(), session->options);
        const ewr::StateSnapshot state = reader.ReadState();

        if (!state.available)
        {
            session->Fail(session->Unanswered("The printer did not answer the status query."));
            return EWR_ERR_READ_FAILED;
        }

        return Deliver(session, ewr::JsonStateData(target, state), out_json);
    });
}

int ewr_dump(ewr_session* session, const char* model, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::DbPrinterModel target;
        const int found = ResolveModel(session, model, target);
        if (found != EWR_OK)
            return found;

        if (session->RefuseOverNetwork("An EEPROM dump"))
            return EWR_ERR_NOT_SUPPORTED;

        std::vector<uint16_t> addresses;
        const uint32_t end = std::min<uint32_t>(target.mem_high, 0xFF);
        for (uint32_t address = 0; address <= end; ++address)
            addresses.push_back(static_cast<uint16_t>(address));

        ewr::Session reader(target, session->Gateway(), ewr::log::Default(), session->options);
        const ewr::StateSnapshot state = reader.ReadAddresses(addresses);

        if (!state.available)
        {
            session->Fail("The printer did not answer the EEPROM read.");
            return EWR_ERR_READ_FAILED;
        }

        std::size_t answered = 0;
        for (const auto& value : state.values)
            answered += (value.second >= 0) ? 1 : 0;

        nlohmann::json payload;
        payload["model"] = target.name;
        payload["answered"] = answered;
        payload["total"] = state.values.size();
        payload["values"] = ewr::JsonCounterValues(state.values);

        const int delivered = Deliver(session, payload, out_json);
        if (delivered != EWR_OK)
            return delivered;

        // The bytes are handed over either way; the code is what says they do
        // not add up to a backup.
        if (answered < state.values.size())
        {
            session->Fail("The printer answered " + std::to_string(answered) + " of "
                          + std::to_string(state.values.size()) + " EEPROM bytes.");
            return EWR_ERR_INCOMPLETE_DUMP;
        }

        return EWR_OK;
    });
}

int ewr_reset(ewr_session* session, const char* model, int ink, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::DbPrinterModel target;
        const int found = ResolveModel(session, model, target);
        if (found != EWR_OK)
            return found;

        if (ink && !target.HasInkReset())
        {
            session->Fail(target.name + " has no cartridge ink map in the database.");
            return EWR_ERR_NOT_SUPPORTED;
        }

        if (ink && session->RefuseOverNetwork("The cartridge ink reset"))
            return EWR_ERR_NOT_SUPPORTED;

        // The CLI refuses a wrong-model write at its own gate; an embedder
        // has no such gate, so the check lives here. Only a printer that
        // names a database entry can contradict the caller: an unlisted one
        // says nothing either way. Allowed, the mismatch still goes on record.
        std::string mismatchPassed;
        {
            const ewr::DeviceIdQueryResult query = session->QueryDeviceId();

            // As the CLI stops at --ip silence: there is no other interface to
            // try, and the confirm callback would be asked about a printer
            // that has never answered.
            if (!query.found && session->net && !session->net->Answered())
            {
                session->Fail(session->net->SilenceError());
                return EWR_ERR_DEVICE_NOT_FOUND;
            }

            if (query.found)
            {
                const ewr::DeviceIdInfo reported = ewr::ParseIeee1284DeviceId(query.deviceId);
                if (!reported.model.empty())
                {
                    std::vector<ewr::ModelNameEntry> entries;
                    for (const auto& known : session->database.GetAvailableModels())
                        entries.push_back({ known.name, known.aliases });

                    const std::vector<std::string> matches = ewr::MatchModelEntries(reported.model, entries);
                    if (!matches.empty() && matches[0] != target.name)
                    {
                        if (!session->allowModelMismatch)
                        {
                            session->Fail("The printer reports \"" + reported.model + "\" (" + matches[0]
                                          + "), not " + target.name + ".");
                            return EWR_ERR_MODEL_MISMATCH;
                        }

                        mismatchPassed = matches[0];
                    }
                }
            }
        }

        ewr::ResetHandlers handlers;

        handlers.onBlocker = [session](const ewr::Blocker& blocker) -> bool
        {
            if (!session->blockerCallback)
                return false; // no host to ask: never assume consent

            return session->blockerCallback(ewr::JsonBlocker(blocker).dump().c_str(), session->blockerUser) != 0;
        };

        handlers.confirmWrite = [session, &target, ink](const ewr::StateSnapshot& state) -> bool
        {
            if (!session->confirmCallback)
                return false;

            nlohmann::json ask = ewr::JsonStateData(target, state);
            ask["target"] = ink ? "ink" : "waste";
            return session->confirmCallback(ask.dump().c_str(), session->confirmUser) != 0;
        };

        ewr::Session lifecycle(target, session->Gateway(), ewr::log::Default(), session->options);
        ewr::ResetOutcome outcome = ink ? lifecycle.ResetInk(handlers) : lifecycle.Reset(handlers);

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        const int delivered = Deliver(session, ewr::JsonResetData(target, ink != 0, outcome), out_json);
        if (delivered != EWR_OK)
            return delivered;

        if (outcome.success)
            return EWR_OK;

        session->Fail(outcome.error);

        switch (outcome.phase)
        {
            case ewr::ResetPhase::Aborted:        return EWR_ERR_BLOCKED;
            case ewr::ResetPhase::DeviceNotFound: return EWR_ERR_DEVICE_NOT_FOUND;
            case ewr::ResetPhase::NotSupported:   return EWR_ERR_NOT_SUPPORTED;
            default: break;
        }

        return (outcome.verificationRan && outcome.verifyMismatches > 0)
            ? EWR_ERR_WRITE_UNVERIFIED : EWR_ERR_WRITE_FAILED;
    });
}

} // extern "C"
