#include "ewr/maintenance.h"
#include "ewr/gates.h"
#include "ewr/status.h"
#include "protocol/ctrl.h"
#include "protocol/d4session.h"
#include "protocol/remote_mode.h"

#include <algorithm>
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

        // "45 s", "5 min 23 s".
        std::string Duration(int seconds)
        {
            if (seconds < 60)
                return std::to_string(seconds) + " s";
            return std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s";
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
        const bool cleaning = request.action != MaintenanceAction::NozzleCheck;
        const bool listed = cleaning && !FactoryPayload(request).empty();

        if (request.factoryAsPrintData)
        {
            if (listed)
                return "factory_print";
            why = "Sending the factory command as print data needs this printer's database entry to list a"
                  " cleaning byte for this request, and it lists none.";
            return "";
        }

        if (listed && !request.remoteOnly)
            return "factory";

        // The print-data form has no levels: it is the printer's own cleaning.
        if (request.action == MaintenanceAction::HeadClean && request.level != "normal")
        {
            const std::string level = "\"" + request.level + "\"";
            const std::string nozzles = std::string(GroupName(request.group)) + " nozzles";
            const bool alone = request.group != CleanGroup::All;
            const bool listedForAll = m_model && m_model->cleaning.clean.count(request.level) > 0;

            if (listed)
                why = "The print-data cleaning has only the printer's own level (\"normal\"); " + level +
                      " is a factory command, which the print-data route does not send.";
            else if (alone && listedForAll)
                why = "This printer's database entry lists a " + level + " cleaning for all nozzles, but none for"
                      " the " + nozzles + " alone. Those go only as print data, at the printer's own level"
                      " (\"normal\").";
            else
                why = "This printer's database entry lists no " + level + " cleaning" +
                      (alone ? " for the " + nozzles : std::string()) +
                      "; the print-data cleaning has only the printer's own level (\"normal\").";
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
                // NA is a plain no: on an L1800 and an L3060 nothing ran
                // (PR #64), so the cleaning goes once more as print data. The
                // printer's own cleaning (CH) is its normal level and ran on
                // all three printers there; any other level goes as the same
                // command inside a print job, which the L3060 ran.
                const bool declined = out.reply.find(":84:NA") != std::string::npos;

                m_reporter.Log(declined ? log::Level::Warning : log::Level::Error, log::Stage::Write,
                               "maintenance.refused",
                               "[!] The printer refused the command (" + out.reply + ").",
                               { { "reply", out.reply } });

                if (!declined)
                {
                    out.phase = MaintenancePhase::SendFailed;
                    out.error = "The printer refused the command (" + out.reply + ").";
                    return out;
                }

                const bool printersOwn = request.action == MaintenanceAction::PowerClean || request.level == "normal";
                out.fallbackFrom = "factory";
                out.route = printersOwn ? "remote" : "factory_print";
                m_reporter.Log(log::Level::Warning, log::Stage::Write, "maintenance.fallback",
                               printersOwn
                                   ? "[!] Nothing ran. Sending the printer's own cleaning as print data instead, once."
                                   : "[!] Nothing ran. Sending the same command inside a print job instead, once.",
                               { { "from", "factory" }, { "to", out.route }, { "reply", out.reply } });
            }
        }

        if (out.route != "factory")
        {
            std::vector<unsigned char> job;
            std::string how = " as print data...";
            if (out.route == "factory_print")
            {
                job = remote::BuildFactoryJob(ctrl::FactoryBody(m_model->rkey, ctrl::CMD_CLEAN, FactoryPayload(request)));
                how = " as print data, in the form of its own maintenance command (level " +
                      (request.action == MaintenanceAction::PowerClean ? std::string("charge") : request.level) + ")...";
            }
            else if (request.action == MaintenanceAction::NozzleCheck)
                job = remote::BuildJob({ remote::NozzleCheck(request.nozzleVariant) }, true);
            else
            {
                const uint8_t group = request.group == CleanGroup::Black ? 1 : request.group == CleanGroup::Color ? 2 : 0;
                job = remote::BuildJob({ remote::HeadClean(group, request.action == MaintenanceAction::PowerClean) });
            }

            m_reporter.Log(log::Level::Info, log::Stage::Write, "maintenance.sending",
                           std::string("[*] Sending the ") + ToString(request.action) + " to the printer" + how,
                           { { "route", out.route }, { "group", GroupName(request.group) } });

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
        int timeout = request.action == MaintenanceAction::PowerClean ? chargeTimeoutSeconds
                    : request.action == MaintenanceAction::NozzleCheck ? nozzleTimeoutSeconds
                    : cleanTimeoutSeconds;

        const auto sentAt = std::chrono::steady_clock::now();
        auto elapsed = [&]() {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - sentAt).count());
        };

        m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.watching",
                       "[*] Watching the printer's status until it is done...");

        // Print data and the status query share one channel, and the query
        // waits behind the job: a printer at work on it does not answer in
        // D4 (L350, L1800, L3060 in PR #64). An L3060 still sends its status
        // outside packet mode about once a second, which the poll takes; an
        // L350 sends nothing, and there silence is the work.
        const bool printData = out.route != "factory";
        ExecutorOptions pollOptions = m_options;
        pollOptions.acceptPlainStatus = printData;

        int silent = 0;
        for (;;)
        {
            if (pollIntervalMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));

            const StateSnapshot now = status::Read(m_gateway, pollOptions);
            out.seconds = elapsed();

            if (!now.available || !now.status.valid)
            {
                if (printData && now.deviceFound)
                {
                    if (!out.silentWhileWorking)
                        m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.silent",
                                       "[*] The printer does not answer while it works through the print data."
                                       " EWR keeps asking until it is idle again.",
                                       { { "seconds", std::to_string(out.seconds) } });
                    out.silentWhileWorking = true;
                    out.started = true;
                    silent = 0;

                    if (out.seconds >= timeout)
                    {
                        out.phase = MaintenancePhase::TimedOut;
                        out.error = "The printer was still not answering after " + std::to_string(timeout) + " s.";
                        m_reporter.Log(log::Level::Warning, log::Stage::Verify, "maintenance.timed_out", "[!] " + out.error);
                        return;
                    }
                    continue;
                }

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

                // Reported from the first CLEANING report on, which may follow
                // a WAITING one, so it gets its own event.
                if (out.expectedSeconds == 0 && now.status.cleaningSeconds > 0)
                {
                    out.expectedSeconds = now.status.cleaningSeconds;
                    timeout = std::max(timeout, out.seconds + out.expectedSeconds + expectedMarginSeconds);
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.expected",
                                   "[*] The printer says this takes about " + Duration(out.expectedSeconds) + ".",
                                   { { "seconds", std::to_string(out.expectedSeconds) } });
                }
            }
            else if (out.started)
            {
                out.phase = MaintenancePhase::Done;
                out.success = true;
                m_reporter.Log(log::Level::Info, log::Stage::Verify, "maintenance.done",
                               out.silentWhileWorking
                                   ? "[SUCCESS] The printer is idle again after " + std::to_string(out.seconds) +
                                         " s. It did not answer while it worked, so EWR could not watch it."
                                   : "[SUCCESS] The printer finished after " + std::to_string(out.seconds) + " s.",
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
