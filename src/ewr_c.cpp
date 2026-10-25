#include "ewr/ewr_c.h"

#include "ewr/deviceid.h"
#include "ewr/database.h"
#include "ewr/json_out.h"
#include "ewr/info.h"
#include "ewr/log.h"
#include "ewr/maintenance.h"
#include "ewr/paths.h"
#include "ewr/session.h"
#include "ewr/snmp_gateway.h"
#include "ewr/updater.h"
#include "ewr/usb.h"
#include "ewr/version.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
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

    // ewr_level's numbers, not log::Level's: that enum also has Debug, so a
    // cast sent every Info event as EWR_LEVEL_WARNING and every error as 4.
    int CLevel(ewr::log::Level level)
    {
        switch (level)
        {
            case ewr::log::Level::Trace:
            case ewr::log::Level::Debug:   return EWR_LEVEL_TRACE;
            case ewr::log::Level::Info:    return EWR_LEVEL_INFO;
            case ewr::log::Level::Warning: return EWR_LEVEL_WARNING;
            case ewr::log::Level::Error:   return EWR_LEVEL_ERROR;
        }
        return EWR_LEVEL_INFO;
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
    ewr::Database database;
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
    ewr_backup_cb backupCallback = nullptr;
    void* backupUser = nullptr;
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
        out.level = CLevel(event.level);
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

        const std::vector<ewr::DbPrinterModel> models = database.Models();
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

    // The CLI refuses a wrong-model write at its own gate; an embedder has no
    // such gate, so the check lives here. Only a printer that names a
    // database entry can contradict the caller: an unlisted one says nothing
    // either way. Allowed, the mismatch still goes on record.
    int CheckReportedModel(ewr_session* session, const ewr::DbPrinterModel& target,
                           std::string& mismatchPassed, std::string& deviceId)
    {
        const ewr::DeviceIdQueryResult query = session->QueryDeviceId();

        // As the CLI stops at --ip silence: there is no other interface to
        // try, and the confirm callback would be asked about a printer that
        // has never answered.
        if (!query.found && session->net && !session->net->Answered())
        {
            session->Fail(session->net->SilenceError());
            return EWR_ERR_DEVICE_NOT_FOUND;
        }

        if (!query.found)
            return EWR_OK;

        deviceId = query.deviceId;

        const ewr::DeviceIdInfo reported = ewr::ParseIeee1284DeviceId(query.deviceId);
        if (reported.model.empty())
            return EWR_OK;

        std::vector<ewr::ModelNameEntry> entries;
        for (const auto& known : session->database.Models())
            entries.push_back({ known.name, known.aliases });

        const std::vector<std::string> matches = ewr::MatchModelEntries(reported.model, entries);
        if (matches.empty() || matches[0] == target.name)
            return EWR_OK;

        if (!session->allowModelMismatch)
        {
            session->Fail("The printer reports \"" + reported.model + "\" (" + matches[0]
                          + "), not " + target.name + ".");
            return EWR_ERR_MODEL_MISMATCH;
        }

        mismatchPassed = matches[0];
        return EWR_OK;
    }

    // The host's callbacks as the lifecycle's hooks. Nobody to ask means no:
    // a blocker or a missing confirmation stops the write.
    ewr::ResetHandlers WriteHandlers(ewr_session* session, const ewr::DbPrinterModel& target,
                                     const std::string& what, const std::string& deviceId)
    {
        ewr::ResetHandlers handlers;

        handlers.onBlocker = [session](const ewr::Blocker& blocker) -> bool
        {
            if (!session->blockerCallback)
                return false; // no host to ask: never assume consent

            return session->blockerCallback(ewr::JsonBlocker(blocker).dump().c_str(), session->blockerUser) != 0;
        };

        handlers.confirmWrite = [session, target, what](const ewr::StateSnapshot& state) -> bool
        {
            if (!session->confirmCallback)
                return false;

            nlohmann::json ask = ewr::JsonStateData(target, state);
            ask["target"] = what;
            return session->confirmCallback(ask.dump().c_str(), session->confirmUser) != 0;
        };

        // Unset keeps ABI 2's behavior: nothing is gated on a backup.
        if (session->backupCallback)
        {
            handlers.saveBackup = [session, deviceId](const ewr::Backup& taken, std::string&) -> bool
            {
                ewr::Backup backup = taken;
                backup.deviceId = deviceId;
                if (session->net)
                    backup.host = session->net->Host();
                return session->backupCallback(ewr::BackupToJson(backup).c_str(), session->backupUser) != 0;
            };
        }

        return handlers;
    }

    int OutcomeStatus(const ewr::ResetOutcome& outcome)
    {
        switch (outcome.phase)
        {
            case ewr::ResetPhase::Aborted:        return EWR_ERR_BLOCKED;
            case ewr::ResetPhase::DeviceNotFound: return EWR_ERR_DEVICE_NOT_FOUND;
            case ewr::ResetPhase::NotSupported:   return EWR_ERR_NOT_SUPPORTED;
            default: break;
        }

        return (outcome.verificationRan && outcome.verifyMismatches > 0)
            ? EWR_ERR_WRITE_UNVERIFIED : EWR_ERR_WRITE_FAILED;
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
        if (!session->database.Load(path))
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

namespace {

    int RunMaintenance(ewr_session* session, const char* model, const ewr::MaintenanceRequest& request,
                       char** out_json)
    {
        ewr::DbPrinterModel target;
        const bool named = model && *model;
        if (named)
        {
            const int found = ResolveModel(session, model, target);
            if (found != EWR_OK)
                return found;
        }

        std::string mismatchPassed;
        std::string deviceId;
        if (named)
        {
            const int reported = CheckReportedModel(session, target, mismatchPassed, deviceId);
            if (reported != EWR_OK)
                return reported;
        }

        ewr::MaintenanceHandlers handlers;
        handlers.onBlocker = [session](const ewr::Blocker& blocker) -> bool
        {
            if (!session->blockerCallback)
                return false;
            return session->blockerCallback(ewr::JsonBlocker(blocker).dump().c_str(), session->blockerUser) != 0;
        };
        handlers.confirm = [session, &target, named, &request](const ewr::StateSnapshot& state,
                                                              const std::string& route) -> bool
        {
            if (!session->confirmCallback)
                return false;

            nlohmann::json ask = named ? ewr::JsonStateData(target, state) : nlohmann::json::object();
            if (!named)
                ask["printer"] = ewr::JsonPrinterStatus(state.status);
            ask["target"] = ewr::MaintenanceActionName(request.action);
            ask["route"] = route;
            return session->confirmCallback(ask.dump().c_str(), session->confirmUser) != 0;
        };

        ewr::Maintenance maintenance(session->Gateway(), named ? &target : nullptr, ewr::log::Default(),
                                     session->options);
        ewr::MaintenanceOutcome outcome = maintenance.Run(request, handlers);

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        const int delivered = Deliver(session, ewr::JsonMaintenanceData(named ? &target : nullptr, request, outcome),
                                      out_json);
        if (delivered != EWR_OK)
            return delivered;

        if (outcome.success)
            return EWR_OK;

        session->Fail(outcome.error);
        switch (outcome.phase)
        {
            case ewr::MaintenancePhase::Aborted:        return EWR_ERR_BLOCKED;
            case ewr::MaintenancePhase::NotSupported:   return EWR_ERR_NOT_SUPPORTED;
            case ewr::MaintenancePhase::DeviceNotFound: return EWR_ERR_DEVICE_NOT_FOUND;
            case ewr::MaintenancePhase::NeverStarted:   return EWR_ERR_NOT_STARTED;
            case ewr::MaintenancePhase::Error:          return EWR_ERR_PRINTER_ERROR;
            default:                                    return EWR_ERR_FAILED;
        }
    }

} // namespace

namespace {

    int RunRestore(ewr_session* session, const char* model, const char* file_contents, bool all, char** out_json)
    {
        if (!file_contents)
        {
            session->Fail("No backup or dump was given.");
            return EWR_ERR_INVALID_ARGUMENT;
        }

        ewr::Backup source;
        std::string error;
        if (!ewr::ParseBackupText(file_contents, source, error))
        {
            session->Fail(error);
            return EWR_ERR_BAD_BACKUP;
        }

        ewr::DbPrinterModel target;
        const int found = ResolveModel(session, (model && *model) ? model : source.model.c_str(), target);
        if (found != EWR_OK)
            return found;

        if (target.name != source.model)
        {
            session->Fail("The file was taken from " + source.model + ", not " + target.name + ".");
            return EWR_ERR_BAD_BACKUP;
        }

        if (all && source.source != "dump")
        {
            session->Fail("A full restore takes an --dump file; a backup holds only what one reset wrote.");
            return EWR_ERR_BAD_BACKUP;
        }

        const ewr::RestorePlan plan = all ? ewr::PlanRestoreAll(target, source) : ewr::PlanRestore(target, source);
        if (plan.writes.empty())
        {
            session->Fail("The file holds none of the bytes EWR writes on " + target.name + ".");
            return EWR_ERR_BAD_BACKUP;
        }

        if (plan.touchesInk && session->RefuseOverNetwork("Restoring cartridge ink bytes"))
            return EWR_ERR_NOT_SUPPORTED;

        std::string mismatchPassed;
        std::string deviceId;
        const int reported = CheckReportedModel(session, target, mismatchPassed, deviceId);
        if (reported != EWR_OK)
            return reported;

        ewr::ResetHandlers handlers = WriteHandlers(session, target, "restore", deviceId);

        ewr::Session lifecycle(target, session->Gateway(), ewr::log::Default(), session->options);
        ewr::ResetOutcome outcome = lifecycle.Restore(plan, handlers);

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        const int delivered = Deliver(session, ewr::JsonRestoreData(target, source, plan, "", outcome), out_json);
        if (delivered != EWR_OK)
            return delivered;

        if (outcome.success)
            return EWR_OK;

        session->Fail(outcome.error);
        return OutcomeStatus(outcome);
    }

} // namespace

extern "C" {

const char* ewr_version(void)
{
    return EWR_VERSION;
}

int ewr_abi_version(void)
{
    return 3;
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
        case EWR_ERR_BAD_BACKUP:       return "bad_backup";
        case EWR_ERR_NOT_STARTED:      return "not_started";
        case EWR_ERR_PRINTER_ERROR:    return "printer_error";
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

void ewr_session_set_backup_callback(ewr_session* session, ewr_backup_cb callback, void* user)
{
    if (!session)
        return;

    session->backupCallback = callback;
    session->backupUser = user;
}

int ewr_database_check(const char* path, char** out_json)
{
    if (!path || !*path || !out_json)
        return EWR_ERR_INVALID_ARGUMENT;

    *out_json = nullptr;

    try
    {
        if (!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema))
            return EWR_ERR_DATABASE;

        ewr::Database db;
        if (!db.Load(path) || db.IsEmpty())
            return EWR_ERR_DATABASE;

        std::ifstream in(path);
        const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
        const int schema = (root.is_object() && root.contains("schema_version") && root["schema_version"].is_number_integer())
            ? root["schema_version"].get<int>() : 0;

        const nlohmann::json payload = { { "models", db.Models().size() },
                                         { "schema_version", schema } };
        *out_json = DuplicateString(payload.dump());
        return *out_json ? EWR_OK : EWR_ERR_FAILED;
    }
    catch (...)
    {
        return EWR_ERR_DATABASE;
    }
}

void ewr_set_data_dir(const char* dir)
{
    ewr::paths::SetDataDir(dir ? dir : "");
}

#if defined(__ANDROID__)
void ewr_android_set_usb_fds(const int* fds, size_t count)
{
    std::vector<int> list;
    for (size_t i = 0; fds && i < count; ++i)
        list.push_back(fds[i]);
    ewr::SetAndroidUsbFds(list);
}
#endif

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
        for (const auto& model : session->database.Models())
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
        for (const auto& model : session->database.Models())
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
        std::ostringstream searchTrace;
        const std::vector<ewr::NetworkPrinter> found =
            ewr::DiscoverNetworkPrinters(ewr::kDiscoveryWaitMs, error, &searchTrace);

        if (session->net)
            session->net->AppendTrace(searchTrace.str());
        else
            session->usb.AppendTrace(searchTrace.str());

        if (!error.empty())
        {
            session->Fail(error);
            return EWR_ERR_IO;
        }

        std::vector<ewr::ModelNameEntry> entries;
        for (const auto& model : session->database.Models())
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
        for (const auto& model : session->database.Models())
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

        const std::vector<uint16_t> addresses = ewr::DumpAddresses(target);

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

        std::string mismatchPassed;
        std::string deviceId;
        const int reported = CheckReportedModel(session, target, mismatchPassed, deviceId);
        if (reported != EWR_OK)
            return reported;

        ewr::ResetHandlers handlers = WriteHandlers(session, target, ink ? "ink" : "waste", deviceId);

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
        return OutcomeStatus(outcome);
    });
}


int ewr_restore(ewr_session* session, const char* model, const char* file_contents, char** out_json)
{
    return Guard(session, [&]() -> int { return RunRestore(session, model, file_contents, false, out_json); });
}

int ewr_restore_all(ewr_session* session, const char* model, const char* dump_contents,
                    int allow_unmapped, char** out_json)
{
    return Guard(session, [&]() -> int
    {
#ifdef EWR_RESTORE_ALL
        if (allow_unmapped != 1)
        {
            session->Fail("A full restore writes bytes no reset writes; pass allow_unmapped = 1 to mean it.");
            return EWR_ERR_INVALID_ARGUMENT;
        }
        return RunRestore(session, model, dump_contents, true, out_json);
#else
        (void)model;
        (void)dump_contents;
        (void)allow_unmapped;
        (void)out_json;
        session->Fail("This build does not include the full restore: it waits for hardware verification.");
        return EWR_ERR_NOT_SUPPORTED;
#endif
    });
}

int ewr_read_info(ewr_session* session, const char* model, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::DbPrinterModel target;
        const bool named = model && *model;
        if (named)
        {
            const int found = ResolveModel(session, model, target);
            if (found != EWR_OK)
                return found;
        }

        const ewr::PrinterInfo info = ewr::ReadPrinterInfo(session->Gateway(), named ? &target : nullptr,
                                                           ewr::log::Default(), session->options);
        if (!info.available)
        {
            session->Fail(session->Unanswered("The printer did not answer the status query."));
            return EWR_ERR_READ_FAILED;
        }

        return Deliver(session, ewr::JsonPrinterInfo(named ? &target : nullptr, info), out_json);
    });
}


int ewr_head_clean(ewr_session* session, const char* model, const char* level, int group,
                   int power, int print_data, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        if (group < EWR_CLEAN_ALL || group > EWR_CLEAN_COLOR)
        {
            session->Fail("Unknown cleaning group.");
            return EWR_ERR_INVALID_ARGUMENT;
        }

        ewr::MaintenanceRequest request;
        request.action = power ? ewr::MaintenanceAction::PowerClean : ewr::MaintenanceAction::HeadClean;
        request.level = (level && *level) ? level : "normal";
        request.group = group == EWR_CLEAN_BLACK ? ewr::CleanGroup::Black
                      : group == EWR_CLEAN_COLOR ? ewr::CleanGroup::Color
                                                 : ewr::CleanGroup::All;
        request.remoteOnly = (print_data != 0);
        return RunMaintenance(session, model, request, out_json);
    });
}

int ewr_nozzle_check(ewr_session* session, const char* model, int variant, char** out_json)
{
    return Guard(session, [&]() -> int
    {
        ewr::MaintenanceRequest request;
        request.action = ewr::MaintenanceAction::NozzleCheck;
        request.nozzleVariant = variant;
        return RunMaintenance(session, model, request, out_json);
    });
}

} // extern "C"
