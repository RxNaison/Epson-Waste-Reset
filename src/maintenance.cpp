#include "ewr/maintenance.h"
#include "ewr/gates.h"
#include "ewr/status.h"
#include "ewr/ctrl.h"
#include "ewr/d4session.h"
#include "ewr/remote_mode.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace ewr {

    namespace {

        constexpr int kStateIdle = 0x04;
        constexpr int kServiceRequest = 0x10;
        constexpr int kCartridgeOverflow = 0x2C;
        constexpr int kInkOut = 0x05;
        constexpr int kPaperOut = 0x06;

        // Three silent polls in a row: the printer is gone, not busy.
        constexpr int kSilentPollsAllowed = 3;

        std::string ReplyText(const std::vector<unsigned char>& raw)
        {
            const std::vector<unsigned char> payload = d4::ExtractPayload(raw);
            std::string text(payload.begin(), payload.end());
            const size_t start = text.find("||");
            if (start != std::string::npos)
                text = text.substr(start);
            const size_t end = text.find(';');
            if (end != std::string::npos)
                text = text.substr(0, end + 1);
            return text;
        }

        const char* GroupName(CleanGroup group)
        {
            switch (group)
            {
                case CleanGroup::Black: return "black";
                case CleanGroup::Color: return "color";
                default:                return "all";
            }
        }

        std::string Percent(const CounterReading& reading)
        {
            char text[32];
            snprintf(text, sizeof(text), "%.1f%%",
                     reading.max_value ? (100.0 * reading.value) / reading.max_value : 0.0);
            return text;
        }

    } // namespace

    const char* ToString(MaintenanceAction action)
    {
        switch (action)
        {
            case MaintenanceAction::NozzleCheck: return "nozzle-check";
            case MaintenanceAction::PowerClean:  return "power-clean";
            default:                             return "head-clean";
        }
    }

    const char* ToString(MaintenancePhase phase)
    {
        switch (phase)
        {
            case MaintenancePhase::Aborted:        return "aborted";
            case MaintenancePhase::NotSupported:   return "not_supported";
            case MaintenancePhase::DeviceNotFound: return "device_not_found";
            case MaintenancePhase::SendFailed:     return "send_failed";
            case MaintenancePhase::NeverStarted:   return "never_started";
            case MaintenancePhase::Error:          return "error";
            case MaintenancePhase::TimedOut:       return "timed_out";
            case MaintenancePhase::Done:           return "done";
            default:                               return "not_started";
        }
    }

    std::optional<Blocker> gates::EvaluateMaintenance(const PrinterStatus& status, MaintenanceAction action)
    {
        const bool printing = action == MaintenanceAction::NozzleCheck;

        Blocker blocker;
        blocker.errorCode = status.hasError ? status.errorCode : -1;

        if (!status.valid)
        {
            blocker.errorName = "STATUS UNREADABLE";
            blocker.explanation = "The printer did not report its state, so EWR cannot tell whether it is\n"
                                  "    ready, or afterwards whether the command ran.";
            return blocker;
        }

        if (status.hasError)
        {
            blocker.errorName = status.errorName;

            if (status.errorCode == kServiceRequest || status.errorCode == kCartridgeOverflow)
                blocker.explanation = "The printer will not clean or print while its waste ink pads count as full.\n"
                                      "    Reset the waste ink pad counters first (run ewr without options).";
            else if (status.errorCode == kInkOut)
                blocker.explanation = printing
                    ? "The printer does not print with an empty cartridge. Replace it first."
                    : "A cleaning with an empty cartridge draws air into the print head instead of\n"
                      "    ink. Replace the empty cartridge first.";
            else if (status.errorCode == kPaperOut && printing)
                blocker.explanation = "Load plain paper, then run the nozzle check again.";
            else
                blocker.explanation = "Clear the error on the printer first, then run EWR again.";

            return blocker;
        }

        if (status.stateCode != kStateIdle)
        {
            blocker.errorName = "PRINTER BUSY";
            blocker.explanation = "The printer is " + status.stateName + ". Wait until it is idle and run EWR again.";
            return blocker;
        }

        return std::nullopt;
    }

    Maintenance::Maintenance(IDeviceGateway& gateway, const DbPrinterModel* model,
                             log::Reporter& reporter, ExecutorOptions options)
        : m_gateway(gateway)
        , m_model(model)
        , m_reporter(reporter)
        , m_options(std::move(options))
    {
    }

    std::vector<uint8_t> Maintenance::FactoryPayload(const MaintenanceRequest& request) const
    {
        if (!m_model || m_model->rkey == 0 || request.action == MaintenanceAction::NozzleCheck)
            return {};

        const CleaningSpec& cleaning = m_model->cleaning;
        const std::map<std::string, std::vector<uint8_t>>* levels = &cleaning.clean;
        std::string level = request.level;

        if (request.action == MaintenanceAction::PowerClean)
        {
            levels = &cleaning.charge;
            level = "normal";
        }
        else if (request.group == CleanGroup::Black)
            levels = &cleaning.black;
        else if (request.group == CleanGroup::Color)
            levels = &cleaning.color;

        const auto it = levels->find(level);
        if (it == levels->end())
            return {};

        // One byte is the only payload ever verified; 0xFF is listed for
        // models where it is not known what it does.
        if (it->second.size() != 1 || it->second[0] == 0xFF)
            return {};

        return it->second;
    }

    std::string Maintenance::Route(const MaintenanceRequest& request, std::string& why) const
    {
        if (request.action != MaintenanceAction::NozzleCheck && !request.remoteOnly
            && !FactoryPayload(request).empty())
            return "factory";

        // The print-data form has no levels: it is the printer's own cleaning.
        if (request.action == MaintenanceAction::HeadClean && request.level != "normal")
        {
            why = "This printer's database entry lists no \"" + request.level + "\" cleaning; the print-data"
                  " cleaning has only the printer's own (\"normal\").";
            return "";
        }

        return "remote";
    }

    MaintenanceOutcome Maintenance::Run(const MaintenanceRequest& request, const MaintenanceHandlers& handlers)
    {
        MaintenanceOutcome out;

        std::string why;
        out.route = Route(request, why);
        if (out.route.empty())
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "maintenance.no_route", "[!] " + why);
            out.phase = MaintenancePhase::NotSupported;
            out.error = why;
            return out;
        }

        // ---- Preflight: the state it starts from, and the counters a
        // cleaning will move, when the model can read them.
        m_reporter.Log(log::Level::Info, log::Stage::Read, "maintenance.preflight",
                       "\n[*] Reading the printer's current state...");

        if (m_model && !m_model->GetReadAddresses().empty())
        {
            Session reader(*m_model, m_gateway, m_reporter, m_options);
            out.before = reader.ReadState();
        }
        else
        {
            out.before = status::Read(m_gateway, m_options);
        }

        if (!out.before.available)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Read, "maintenance.unanswered",
                           "[ERROR] The printer did not answer the status query. Nothing was sent.");
            out.phase = MaintenancePhase::DeviceNotFound;
            out.error = "The printer did not answer the status query.";
            return out;
        }

        if (handlers.onPreflight)
            handlers.onPreflight(out.before);

        // ---- Gate: the same policy as a reset's - nobody to ask is a no.
        if (const std::optional<Blocker> blocker = gates::EvaluateMaintenance(out.before.status, request.action))
        {
            if (!(handlers.onBlocker && handlers.onBlocker(*blocker)))
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "maintenance.aborted",
                               "[i] Stopped before sending anything. Nothing was changed.");
                out.phase = MaintenancePhase::Aborted;
                out.error = "Stopped on " + blocker->errorName + ".";
                return out;
            }

            out.overrides.push_back({ "blocker", blocker->errorName, blocker->errorCode });
        }

        if (handlers.confirm && !handlers.confirm(out.before, out.route))
        {
            m_reporter.Log(log::Level::Info, log::Stage::Read, "maintenance.declined",
                           "[i] Stopped before sending anything. Nothing was changed.");
            out.phase = MaintenancePhase::Aborted;
            out.error = "Declined at the confirmation.";
            return out;
        }

        // ---- Send, once.
        if (out.route == "factory")
        {
            const std::vector<unsigned char> command = ctrl::FactoryPacket(
                m_model->rkey, ctrl::CMD_CLEAN, FactoryPayload(request));

            m_reporter.Log(log::Level::Info, log::Stage::Write, "maintenance.sending",
                           std::string("[*] Sending the ") + ToString(request.action) +
                               " to the printer (its own maintenance command, level " +
                               (request.action == MaintenanceAction::PowerClean ? "charge" : request.level) + ")...",
                           { { "route", "factory" }, { "group", GroupName(request.group) } });

            const CommandRunResult run = m_gateway.RunCommand(command, m_options);
            if (!run.supported || !run.deviceFound || !run.sent)
            {
                out.phase = run.deviceFound ? MaintenancePhase::SendFailed : MaintenancePhase::DeviceNotFound;
                out.error = run.error.empty() ? "The command could not be sent." : run.error;
                m_reporter.Log(log::Level::Error, log::Stage::Write, "maintenance.send_failed", "[ERROR] " + out.error);
                return out;
            }

            out.sent = true;
            out.reply = ReplyText(run.reply);

            // An answer that is not OK is a refusal: a wrong key, or a model
            // that does not take the command. No answer at all may still
            // have run, and the status says.
            if (!out.reply.empty() && out.reply.find(":84:OK") == std::string::npos)
            {
                m_reporter.Log(log::Level::Error, log::Stage::Write, "maintenance.refused",
                               "[!] The printer refused the command (" + out.reply + ").",
                               { { "reply", out.reply } });
                out.phase = MaintenancePhase::SendFailed;
                out.error = "The printer refused the command (" + out.reply + ").";
                return out;
            }
        }
        else
        {
            std::vector<unsigned char> job;
            if (request.action == MaintenanceAction::NozzleCheck)
                job = remote::BuildJob({ remote::NozzleCheck(request.nozzleVariant) });
            else
            {
                const uint8_t group = request.group == CleanGroup::Black ? 1 : request.group == CleanGroup::Color ? 2 : 0;
                job = remote::BuildJob({ remote::HeadClean(group, request.action == MaintenanceAction::PowerClean) });
            }

            m_reporter.Log(log::Level::Info, log::Stage::Write, "maintenance.sending",
                           std::string("[*] Sending the ") + ToString(request.action) +
                               " to the printer as print data...",
                           { { "route", "remote" }, { "group", GroupName(request.group) } });

            const PrintDataResult run = m_gateway.SendPrintData(job, m_options);
            if (!run.supported)
            {
                out.phase = MaintenancePhase::NotSupported;
                out.error = "This connection to the printer cannot carry print data.";
                m_reporter.Log(log::Level::Error, log::Stage::Write, "maintenance.send_failed", "[ERROR] " + out.error);
                return out;
            }
            if (!run.sent)
            {
                out.phase = run.deviceFound ? MaintenancePhase::SendFailed : MaintenancePhase::DeviceNotFound;
                out.error = run.error.empty() ? "The print data was not accepted." : run.error;
                m_reporter.Log(log::Level::Error, log::Stage::Write, "maintenance.send_failed", "[ERROR] " + out.error);
                return out;
            }

            out.sent = true;
        }

        Watch(request, handlers, out);

        // ---- What it did to the pads. Display only.
        if (out.started && m_model && !m_model->GetAllCounters().empty())
        {
            Session reader(*m_model, m_gateway, m_reporter, m_options);
            out.after = reader.ReadState();

            for (const auto& spec : m_model->GetAllCounters())
            {
                const CounterReading was = spec.Evaluate(out.before.values);
                const CounterReading now = spec.Evaluate(out.after.values);
                if (!was.complete || !now.complete || !now.max_value)
                    continue;

                m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.pad_delta",
                               "[i] " + (spec.description.empty() ? std::string("Waste counter") : spec.description) +
                                   ": " + Percent(was) + " -> " + Percent(now),
                               { { "counter", spec.description }, { "before", std::to_string(was.value) },
                                 { "after", std::to_string(now.value) }, { "max", std::to_string(now.max_value) } });
            }
        }

        return out;
    }

    void Maintenance::Watch(const MaintenanceRequest& request, const MaintenanceHandlers& handlers,
                            MaintenanceOutcome& out)
    {
        const int timeout = request.action == MaintenanceAction::PowerClean ? chargeTimeoutSeconds
                          : request.action == MaintenanceAction::NozzleCheck ? nozzleTimeoutSeconds
                          : cleanTimeoutSeconds;

        const auto sentAt = std::chrono::steady_clock::now();
        auto elapsed = [&]() {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - sentAt).count());
        };

        m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.watching",
                       "[*] Watching the printer's status until it is done...");

        int silent = 0;
        for (int poll = 0;; ++poll)
        {
            if (pollIntervalMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));

            const StateSnapshot now = status::Read(m_gateway, m_options);
            out.seconds = elapsed();

            if (!now.available || !now.status.valid)
            {
                if (++silent >= kSilentPollsAllowed)
                {
                    out.phase = MaintenancePhase::Error;
                    out.error = "The printer stopped answering while it was being watched.";
                    m_reporter.Log(log::Level::Error, log::Stage::Verify, "maintenance.lost", "[ERROR] " + out.error);
                    return;
                }
                continue;
            }

            silent = 0;
            out.last = now.status;

            if (handlers.onProgress)
                handlers.onProgress(now.status, out.seconds);

            m_reporter.Log(log::Level::Trace, log::Stage::Verify, "maintenance.poll",
                           "[i] " + std::to_string(out.seconds) + " s: " + status::Describe(now.status),
                           { { "state", now.status.stateName }, { "seconds", std::to_string(out.seconds) } });

            if (now.status.hasError)
            {
                out.phase = MaintenancePhase::Error;
                out.error = "The printer reports " + now.status.errorName + ".";
                m_reporter.Log(log::Level::Error, log::Stage::Verify, "maintenance.error",
                               "[!] " + out.error, { { "error", now.status.errorName },
                                                     { "error_code", std::to_string(now.status.errorCode) } });
                return;
            }

            if (now.status.stateCode != kStateIdle)
            {
                if (!out.started)
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.started",
                                   "[*] The printer is at it (" + now.status.stateName + ").",
                                   { { "state", now.status.stateName } });
                out.started = true;
            }
            else if (out.started)
            {
                out.phase = MaintenancePhase::Done;
                out.success = true;
                m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.done",
                               "[SUCCESS] The printer finished after " + std::to_string(out.seconds) + " s.",
                               { { "seconds", std::to_string(out.seconds) } });
                return;
            }

            if (!out.started && out.seconds >= startWindowSeconds)
            {
                out.phase = MaintenancePhase::NeverStarted;
                out.error = "The printer took the command but never left IDLE.";
                m_reporter.Log(log::Level::Warning, log::Stage::Verify, "maintenance.never_started",
                               "[!] " + out.error + " This model may not take it this way.");
                return;
            }

            if (out.started && out.seconds >= timeout)
            {
                out.phase = MaintenancePhase::TimedOut;
                out.error = "The printer was still at it after " + std::to_string(timeout) + " s.";
                m_reporter.Log(log::Level::Warning, log::Stage::Verify, "maintenance.timed_out", "[!] " + out.error);
                return;
            }
        }
    }

} // namespace ewr
