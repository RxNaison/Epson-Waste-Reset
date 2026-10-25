#include "cli.h"
#include "ewr/gates.h"
#include "ewr/log.h"
#include "ewr/restore.h"
#include "ewr/session.h"
#include "backup_store.h"

#include <cstdio>
#include <iostream>

namespace ewr::cli {

    // `values` with the plan's bytes written in, for showing what a restore
    // leaves each pad at before it happens.
    static std::vector<std::pair<uint16_t, int>> WithRestoredBytes(std::vector<std::pair<uint16_t, int>> values,
                                                                   const ewr::RestorePlan& plan)
    {
        for (const auto& w : plan.writes)
        {
            bool found = false;
            for (auto& entry : values)
            {
                if (entry.first == w.first)
                {
                    entry.second = w.second;
                    found = true;
                }
            }
            if (!found)
                values.push_back({ w.first, w.second });
        }
        return values;
    }

    // One line per planned byte: what it holds now, what the file says. A long
    // plan - a full restore - lists only the bytes that change.
    static size_t PrintRestoreDiff(const ewr::RestorePlan& plan, const std::vector<std::pair<uint16_t, int>>& now)
    {
        const bool brief = plan.writes.size() > 32;
        size_t changing = 0;
        size_t unchanged = 0;
        std::cout << "  Bytes the restore writes back:" << std::endl;
        for (const auto& w : plan.writes)
        {
            int current = -1;
            for (const auto& entry : now)
            {
                if (entry.first == w.first)
                    current = entry.second;
            }

            if (current == w.second)
            {
                unchanged++;
                if (brief)
                    continue;
            }

            char line[96];
            if (current == w.second)
                snprintf(line, sizeof(line), "    EEPROM 0x%04X = 0x%02X already - left alone", w.first, w.second);
            else if (current >= 0)
                snprintf(line, sizeof(line), "    EEPROM 0x%04X: 0x%02X -> 0x%02X", w.first, current, w.second);
            else
                snprintf(line, sizeof(line), "    EEPROM 0x%04X: (no reply) -> 0x%02X", w.first, w.second);
            std::cout << line << std::endl;

            changing += (current == w.second) ? 0 : 1;
        }

        if (brief && unchanged > 0)
            std::cout << "    (" << unchanged << " more byte(s) already hold the file's value and are left alone)" << std::endl;

        return changing;
    }

    // A restore can put a pad back at its service limit: say so before asking.
    static void WarnOnFullPads(const ewr::DbPrinterModel& model, const std::vector<std::pair<uint16_t, int>>& after)
    {
        for (const auto& spec : model.GetAllCounters())
        {
            const ewr::CounterReading reading = spec.Evaluate(after);
            if (reading.complete && reading.Percent() >= 100)
            {
                std::cout << "\n[!] After the restore, " << (reading.description.empty() ? "a counter" : reading.description)
                          << " reads " << reading.Percent() << "%: the printer will report its" << std::endl;
                std::cout << "    waste ink pads full again and may refuse to print until they are reset." << std::endl;
            }
        }
    }

    // --restore: write back what a backup or a dump holds of this model's own
    // counter bytes, through the reset's lifecycle and its gates.
    int RunRestore(const ewr::DbPrinterModel& model, const ewr::Backup& source, const std::string& file,
                          ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options, const CliOptions& cli,
                          const std::string& deviceId, const std::string& mismatchPassed)
    {
        const bool all = cli.restoreAll;
        const ewr::RestorePlan plan = all ? ewr::restore::PlanAll(model, source) : ewr::restore::Plan(model, source);
        const bool overNetwork = gateway.OverNetwork();

        // In place before any early exit, so a refused restore still names
        // what it was asked to write.
        g_jsonData = ewr::json::RestorePlanData(source, plan, file);

        std::cout << "\n[*] RESTORE for " << model.name << " from " << file << std::endl;
        std::cout << "    " << (source.source == "dump" ? "An EEPROM dump" : "A backup EWR kept before a write");
        if (!source.created.empty())
            std::cout << ", taken " << source.created;
        if (!source.serial.empty())
            std::cout << ", of the printer with serial " << source.serial;
        std::cout << "." << std::endl;

        size_t unmapped = 0;
        size_t unread = 0;
        size_t identity = 0;
        size_t other = 0;
        for (const auto& skip : plan.skipped)
        {
            if (skip.reason == "unmapped")
                unmapped++;
            else if (skip.reason == "unread")
                unread++;
            else if (skip.reason == "identity")
                identity++;
            else
                other++;
        }

        if (identity > 0)
            std::cout << "    " << identity << " byte(s) in the file name this unit (serial, head ID, USB ID,"
                      << " network address) and are never written." << std::endl;

        if (unmapped > 0)
            std::cout << "    " << unmapped << " byte(s) in the file are not bytes EWR writes on " << model.name
                      << ", and are left alone." << std::endl;
        if (unread > 0)
            std::cout << "    " << unread << " counter byte(s) in the file were never read ('--'), and are left alone."
                      << std::endl;
        if (other > 0)
            std::cout << "    " << other << " byte(s) lie beyond what this model's EEPROM commands can reach, and are"
                      << " left alone." << std::endl;

        if (!plan.missing.empty())
        {
            std::cout << "\n[!] The file does not hold " << plan.missing.size() << " of " << model.name
                      << "'s counter byte(s)";
            if (source.source == "dump")
                std::cout << " - a dump made before EWR 1.6\n    read only up to 0xFF";
            std::cout << ". Those stay as they are." << std::endl;
        }

        if (all)
        {
            std::cout << "\n[!] --restore-all writes every byte of the dump that differs from the printer,\n"
                         "    not only its counters: settings, timers, calibration - bytes no reset writes.\n"
                         "    It never writes the serial number, head ID, USB ID or network address, and\n"
                         "    it runs only on the printer the dump was taken from." << std::endl;
            if (!plan.serial.empty())
                std::cout << "    Unit check: the serial number " << plan.serial << "." << std::endl;
            else
                std::cout << "    Unit check: " << plan.unitProof.size() << " identity byte(s) the printer must"
                          << " still hold." << std::endl;
        }

        if (plan.writes.empty())
        {
            std::cerr << "\n[ERROR] The file holds none of the bytes EWR writes on " << model.name
                      << ", so there is nothing to restore." << std::endl;
            JsonFail("bad_backup", "The file holds none of the bytes EWR writes on " + model.name + ".");
            return FinishRun(1);
        }

        if (plan.touchesInk && overNetwork)
        {
            std::cerr << "\n[!] The file holds cartridge ink bytes, and writing those needs USB." << std::endl;
            JsonFail("not_supported", "Restoring cartridge ink bytes needs USB.");
            return FinishRun(1);
        }

        ewr::Session session(model, gateway, ewr::log::Default(), options);

        std::vector<uint16_t> reads = model.GetReadAddresses();
        for (const auto& w : plan.writes)
        {
            if (std::find(reads.begin(), reads.end(), w.first) == reads.end())
                reads.push_back(w.first);
        }
        for (const auto& proof : plan.unitProof)
        {
            if (std::find(reads.begin(), reads.end(), proof.first) == reads.end())
                reads.push_back(proof.first);
        }

        if (cli.dryRun)
        {
            std::cout << "\n[*] DRY RUN: reading what those bytes hold now (no writes)..." << std::endl;
            const ewr::StateSnapshot state = session.ReadAddresses(reads);
            if (!state.available)
            {
                std::cerr << "[!] The printer did not answer the read-only query." << std::endl;
                JsonFail("read_failed", "The printer did not answer the read-only query.");
                return FinishRun(1);
            }

            PrintPrinterStatus(state.status);
            const size_t changing = PrintRestoreDiff(plan, state.values);

            const auto after = WithRestoredBytes(state.values, plan);
            std::cout << "\n  Now:" << std::endl;
            PrintCounterSummary(model, state.values);
            std::cout << "  After the restore:" << std::endl;
            PrintCounterSummary(model, after);
            WarnOnFullPads(model, after);

            if (!plan.serial.empty() && !state.status.serial.empty() && plan.serial != state.status.serial)
                std::cout << "\n[!] The file comes from the printer with serial " << plan.serial
                          << "; this one reports " << state.status.serial << "." << std::endl;

            if (plan.requireSameUnit && (plan.serial.empty() || state.status.serial.empty()))
            {
                size_t matching = 0;
                for (const auto& proof : plan.unitProof)
                {
                    for (const auto& entry : state.values)
                        matching += (entry.first == proof.first && entry.second == proof.second) ? 1 : 0;
                }
                const bool proven = plan.unitProof.size() >= ewr::restore::kMinimumUnitProof && matching == plan.unitProof.size();
                std::cout << "\n" << (proven ? "[i] Same printer: " : "[!] A real run would stop: only ")
                          << matching << " of " << plan.unitProof.size() << " identity byte(s) match the dump's." << std::endl;
            }

            std::cout << "\n[DRY RUN] Nothing was written. " << changing << " byte(s) would change; run without"
                      << " --dry-run to restore them." << std::endl;

            g_jsonData["printer"] = ewr::json::Status(state.status);
            g_jsonData["before"] = ewr::json::CounterValues(state.values);
            return FinishRun(0);
        }

        ewr::ResetHandlers handlers;

        handlers.onPreflight = [&](const ewr::StateSnapshot& before)
        {
            PrintPrinterStatus(before.status);
            PrintCounterSummary(model, before.values);
        };

        handlers.onBlocker = [&](const ewr::Blocker& blocker)
        {
            if (blocker.errorCode >= 0)
                std::cout << "\n[!] WARNING: the printer reports an active error: " << blocker.errorName << "." << std::endl;
            else
                std::cout << "\n[!] WARNING: " << blocker.errorName << "." << std::endl;

            if (!blocker.explanation.empty())
                std::cout << "    " << blocker.explanation << std::endl;

            if (cli.forceYes)
            {
                std::cout << "\n[!] --force-yes: continuing past that objection." << std::endl;
                return true;
            }

            if (cli.json)
            {
                JsonFail("blocked", "EWR will not write past: " + blocker.errorName + ". Pass --force-yes to overrule it.");
                return false;
            }

            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes answers the restore confirmation, not this. Pass --force-yes to overrule it."
                          << std::endl;
                return false;
            }

            std::cout << "\nRestore anyway? [y/N]: ";
            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        handlers.confirmWrite = [&](const ewr::StateSnapshot& before)
        {
            std::cout << std::endl;
            PrintRestoreDiff(plan, before.values);

            const auto after = WithRestoredBytes(before.values, plan);
            std::cout << "  After the restore:" << std::endl;
            PrintCounterSummary(model, after);
            WarnOnFullPads(model, after);

            const std::string where = overNetwork ? " at " + cli.ip : std::string();

            // Bytes no reset writes: only a person who typed it means it.
            if (all)
            {
                std::cout << "\nType 'restore-all' to write these bytes on " << model.name << where
                          << ", anything else to stop: ";
                std::string typed;
                std::getline(std::cin, typed);
                return toLower(typed) == "restore-all";
            }

            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes: restoring " << model.name << where << " without asking." << std::endl;
                return true;
            }

            if (cli.json)
            {
                JsonFail("blocked", "The restore confirmation was not given: pass --yes.");
                return false;
            }

            std::cout << "\nRestore these bytes on " << model.name << where << " now? [y/N]: ";
            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        handlers.saveBackup = [&](const ewr::Backup& taken, std::string& location)
        {
            ewr::Backup backup = taken;
            backup.deviceId = deviceId;
            backup.host = overNetwork ? cli.ip : std::string();

            std::string error;
            if (ewr::cli::StoreBackup(backup, location, error))
                return true;

            std::cerr << "[!] " << error << std::endl;
            return false;
        };

        handlers.onVerify = [&](const ewr::StateSnapshot& after)
        {
            PrintCounterSummary(model, after.values);
        };

        ewr::ResetOutcome outcome = session.Restore(plan, handlers);

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        if (outcome.verificationRan && (outcome.verifyMismatches > 0 || outcome.verifyUnread > 0))
        {
            PrintCounterValues(outcome.before.values, "EEPROM values BEFORE the restore:");
            PrintCounterValues(outcome.after.values, "EEPROM values AFTER the restore:");
        }

        g_jsonData = ewr::json::RestoreData(model, source, plan, file, outcome);

        if (outcome.success && outcome.writesTotal == 0)
            return FinishRun(0); // every byte already held the file's value

        if (!outcome.success)
        {
            if (outcome.phase == ewr::ResetPhase::Aborted)
                JsonFail("blocked", outcome.error.empty() ? "The run stopped at a gate; nothing was written." : outcome.error);
            else if (outcome.phase == ewr::ResetPhase::DeviceNotFound)
                JsonFail("device_not_found", outcome.error);
            else if (outcome.phase == ewr::ResetPhase::NotSupported)
                JsonFail("not_supported", outcome.error);
            else if (outcome.phase == ewr::ResetPhase::NotStarted)
                JsonFail("read_failed", outcome.error);
            else if (outcome.verificationRan && outcome.verifyMismatches > 0)
                JsonFail("write_unverified", outcome.error);
            else
                JsonFail("write_failed", outcome.error);

            if (outcome.phase != ewr::ResetPhase::WriteFailed)
                return FinishRun(1);
        }

        return FinishReset(outcome.success);
    }

} // namespace ewr::cli
