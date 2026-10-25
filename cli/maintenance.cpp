#include "cli.h"
#include "ewr/gates.h"
#include "ewr/info.h"
#include "ewr/log.h"
#include "ewr/maintenance.h"

#include <cstdio>
#include <iostream>

namespace ewr::cli {

    // The database's counter keys in a person's words. A key without one reads
    // as its own words: "total_print_cd_r_counter" -> "Total print cd r".
    static std::string InfoCounterLabel(const std::string& key)
    {
        static const std::pair<const char*, const char*> kLabels[] = {
            { "total_print_page_counter", "Pages printed" },
            { "total_print_pass_counter", "Print passes" },
            { "total_print_page_counter_duplex", "Pages printed, both sides" },
            { "total_scan_counter", "Pages scanned" },
            { "total_scan_counter_adf", "Pages scanned from the feeder" },
            { "manual_cleaning_counter", "Head cleanings (manual)" },
            { "timer_cleaning_counter", "Head cleanings (automatic)" },
            { "power_cleaning_counter", "Power cleanings" },
            { "ink_replacement_cleaning_counter", "Cleanings after an ink change" },
            { "maintenance_box_replacement_counter", "Maintenance box changes" },
            { "total_print_cd_r_counter", "CDs/DVDs printed" },
        };

        for (const auto& label : kLabels)
        {
            if (key == label.first)
                return label.second;
        }

        std::string words = key;
        const std::string suffix = "_counter";
        const size_t at = words.find(suffix);
        if (at != std::string::npos)
            words.erase(at, suffix.size());
        for (char& c : words)
            c = (c == '_') ? ' ' : c;
        if (!words.empty())
            words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
        return words;
    }

    // --info: what the printer says about itself. Read-only.
    int RunInfo(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options)
    {
        std::cout << "\n[*] Reading what the printer says about itself (read-only)..." << std::endl;

        const ewr::PrinterInfo info = ewr::info::Read(gateway, model, ewr::log::Default(), options);
        g_jsonData = ewr::json::InfoData(model, info);

        if (!info.available)
        {
            std::cerr << "[ERROR] The printer did not answer. Is it powered on and connected?" << std::endl;
            JsonFail("read_failed", "The printer did not answer the status query.");
            return FinishRun(1);
        }

        PrintPrinterStatus(info.status);

        std::cout << "  Firmware: " << (info.firmware.empty() ? std::string("(not reported)") : info.firmware) << std::endl;
        if (info.serialSource == "eeprom")
            std::cout << "  Serial:   " << info.serial << " (from the EEPROM)" << std::endl;
        else if (info.serial.empty())
            std::cout << "  Serial:   (not reported)" << std::endl;

        bool header = false;
        for (const auto& counter : info.counters)
        {
            if (!header)
            {
                std::cout << "  The printer's own counters:" << std::endl;
                header = true;
            }

            char line[160];
            if (counter.value >= 0)
                snprintf(line, sizeof(line), "    %-34.34s %lld", InfoCounterLabel(counter.key).c_str(), counter.value);
            else
                snprintf(line, sizeof(line), "    %-34.34s (no reply)", InfoCounterLabel(counter.key).c_str());
            std::cout << line << std::endl;
        }

        if (!info.fatal.empty())
        {
            bool any = false;
            std::cout << "  Last fatal errors (most recent first):" << std::endl;
            for (const auto& slot : info.fatal)
            {
                if (slot.code <= 0)
                    continue;
                any = true;
                char line[96];
                snprintf(line, sizeof(line), "    %-8s #%d  code 0x%02X", slot.kind.c_str(), slot.slot, slot.code);
                std::cout << line << std::endl;
            }
            if (!any)
                std::cout << "    none logged" << std::endl;
        }

        if (!model || (model->report.empty() && model->fatal.empty()))
            std::cout << "\n[i] The database has no counter addresses for this model, so only the status\n"
                         "    and firmware can be shown." << std::endl;

        return FinishRun(0);
    }

    // --nozzle-check, --head-clean, --power-clean: the printer's own maintenance,
    // with the gates and the one question a reset has, watched until it is done.
    int RunMaintenance(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway,
                              const ewr::ExecutorOptions& options, const CliOptions& cli,
                              const std::string& mismatchPassed)
    {
        ewr::MaintenanceRequest request;
        request.action = cli.nozzleCheck ? ewr::MaintenanceAction::NozzleCheck
                       : cli.powerClean  ? ewr::MaintenanceAction::PowerClean
                                         : ewr::MaintenanceAction::HeadClean;
        request.level = cli.cleanLevel;
        request.group = cli.cleanGroup == "black" ? ewr::CleanGroup::Black
                      : cli.cleanGroup == "color" ? ewr::CleanGroup::Color
                                                  : ewr::CleanGroup::All;
        request.nozzleVariant = cli.nozzleVariant;
        request.remoteOnly = cli.remoteOnly;

        ewr::Maintenance maintenance(gateway, model, ewr::log::Default(), options);

        std::string why;
        const std::string route = maintenance.Route(request, why);

        const std::string name = model ? model->name : std::string("the printer");
        const char* what = cli.nozzleCheck ? "NOZZLE CHECK" : cli.powerClean ? "POWER CLEANING" : "HEAD CLEANING";
        std::cout << "\n[*] " << what << " on " << name;
        if (cli.headClean)
            std::cout << " (" << request.level << ")";
        if (!cli.nozzleCheck && request.group != ewr::CleanGroup::All)
            std::cout << ", " << cli.cleanGroup << " only";
        std::cout << std::endl;

        if (route.empty())
        {
            std::cerr << "[!] " << why << std::endl;
            JsonFail("not_supported", why);
            g_jsonData = ewr::json::MaintenanceData(model, request, {});
            return FinishRun(1);
        }

        std::cout << (route == "factory"
                          ? "    With the printer's own maintenance command, from its database entry."
                          : "    Sent as print data (ESC/P2 remote mode), which the printer takes like a job.")
                  << std::endl;

        if (cli.dryRun)
        {
            const ewr::StateSnapshot state = ewr::status::Read(gateway, options);
            if (!state.available)
            {
                std::cerr << "[!] The printer did not answer the status query." << std::endl;
                JsonFail("read_failed", "The printer did not answer the status query.");
                return FinishRun(1);
            }

            PrintPrinterStatus(state.status);
            if (const auto blocker = ewr::gates::EvaluateMaintenance(state.status, request.action))
                std::cout << "\n[!] A real run would stop here: " << blocker->errorName << ".\n    "
                          << blocker->explanation << std::endl;

            std::cout << "\n[DRY RUN] Nothing was sent to the printer." << std::endl;
            ewr::MaintenanceOutcome preview;
            preview.route = route;
            preview.before = state;
            g_jsonData = ewr::json::MaintenanceData(model, request, preview);
            return FinishRun(0);
        }

        ewr::MaintenanceHandlers handlers;

        handlers.onPreflight = [&](const ewr::StateSnapshot& before)
        {
            PrintPrinterStatus(before.status);
            if (model)
                PrintCounterSummary(*model, before.values);
        };

        handlers.onBlocker = [&](const ewr::Blocker& blocker)
        {
            if (blocker.errorCode >= 0)
                std::cout << "\n[!] The printer reports an active error: " << blocker.errorName << "." << std::endl;
            else
                std::cout << "\n[!] " << blocker.errorName << "." << std::endl;
            if (!blocker.explanation.empty())
                std::cout << "    " << blocker.explanation << std::endl;

            if (cli.forceYes)
            {
                std::cout << "\n[!] --force-yes: continuing past that objection." << std::endl;
                return true;
            }

            if (cli.json)
            {
                JsonFail("blocked", "EWR will not go past: " + blocker.errorName + ". Pass --force-yes to overrule it.");
                return false;
            }

            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes answers the confirmation, not this. Pass --force-yes to overrule it." << std::endl;
                return false;
            }

            std::cout << "\nGo ahead anyway? [y/N]: ";
            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        handlers.confirm = [&](const ewr::StateSnapshot&, const std::string&)
        {
            if (cli.nozzleCheck)
                std::cout << "\n[i] Load plain paper: the printer prints one page." << std::endl;
            else if (cli.powerClean)
                std::cout << "\n[!] A power cleaning (ink charge) uses a large amount of ink - on some models a\n"
                             "    good part of a cartridge - and fills the waste ink pads quickly. Run it only\n"
                             "    when normal cleanings did not clear the nozzles." << std::endl;
            else
                std::cout << "\n[i] A cleaning uses ink, and the ink it uses goes into the waste ink pads." << std::endl;

            if (cli.assumeYes)
                return true;

            if (cli.json)
            {
                JsonFail("blocked", "The confirmation was not given: pass --yes.");
                return false;
            }

            if (cli.powerClean)
            {
                std::cout << "\nType 'charge' to run the power cleaning, anything else to stop: ";
                std::string typed;
                std::getline(std::cin, typed);
                return toLower(typed) == "charge";
            }

            std::cout << "\n" << (cli.nozzleCheck ? "Print the nozzle check now? [y/N]: " : "Clean the print head now? [y/N]: ");
            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        const bool animate = !g_json && StdoutIsTerminal();
        bool lineOpen = false;
        handlers.onProgress = [&](const ewr::PrinterStatus& status, int seconds)
        {
            if (!animate)
                return;
            std::cout << "\r[*] " << seconds << " s: " << status.stateName << "          " << std::flush;
            lineOpen = true;
        };

        ewr::MaintenanceOutcome outcome = maintenance.Run(request, handlers);
        if (lineOpen)
            std::cout << std::endl;

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        g_jsonData = ewr::json::MaintenanceData(model, request, outcome);

        switch (outcome.phase)
        {
            case ewr::MaintenancePhase::Done:
                if (!cli.nozzleCheck)
                    std::cout << "\n[i] Print a nozzle check (ewr --nozzle-check) to see whether it helped." << std::endl;
                return FinishRun(0);
            case ewr::MaintenancePhase::Aborted:        JsonFail("blocked", outcome.error); break;
            case ewr::MaintenancePhase::NotSupported:   JsonFail("not_supported", outcome.error); break;
            case ewr::MaintenancePhase::DeviceNotFound: JsonFail("device_not_found", outcome.error); break;
            case ewr::MaintenancePhase::SendFailed:     JsonFail("send_failed", outcome.error); break;
            case ewr::MaintenancePhase::NeverStarted:   JsonFail("not_started", outcome.error); break;
            case ewr::MaintenancePhase::Error:          JsonFail("printer_error", outcome.error); break;
            case ewr::MaintenancePhase::TimedOut:       JsonFail("timed_out", outcome.error); break;
            default:                                    JsonFail("failed", outcome.error); break;
        }

        return FinishRun(1);
    }

} // namespace ewr::cli
