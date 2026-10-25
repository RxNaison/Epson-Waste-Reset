#include "cli.h"
#include "ewr/gates.h"
#include "ewr/log.h"
#include "ewr/session.h"
#include "backup_store.h"

#include <cstdio>
#include <iostream>

namespace ewr::cli {

    // The first byte of each color's record is its consumption counter
    // (0x00 = full .. 0x64 = empty), which is the byte the ink reset zeroes.
    static void PrintInkSummary(const ewr::DbPrinterModel& model,
                                const std::vector<std::pair<uint16_t, int>>& values)
    {
        if (model.ink_groups.empty())
            return;

        std::cout << "  Cartridge ink consumption (0 = full, 100 = empty):" << std::endl;
        for (const auto& group : model.ink_groups)
        {
            if (group.addresses.empty())
                continue;

            int used = -1;
            for (const auto& v : values)
            {
                if (v.first == group.addresses[0])
                {
                    used = v.second;
                    break;
                }
            }

            std::cout << "    " << (group.color.empty() ? std::string("(unnamed)") : group.color) << ": ";
            if (used >= 0)
                std::cout << used << std::endl;
            else
                std::cout << "(no reply)" << std::endl;
        }
    }

    // Dry run: everything except the writes.
    int RunDryRun(RunContext& ctx)
    {
        CliOptions& cli = ctx.cli;
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        const std::string& detectedMatch = ctx.detectedMatch;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;

        if (selected.isReplay)
        {
            // Opaque bytes: count what it would send, send nothing.
            const std::vector<std::vector<unsigned char>> dumpSequence =
                ewr::replay::ParseWiresharkDump(selected.replayModel.filepath);

            const size_t dumpWrites = ewr::replay::CountWrites(dumpSequence);

            std::cout << "\n[DRY RUN] " << selected.displayName << ": the dump holds "
                      << dumpSequence.size() << " packets, " << dumpWrites << " of them EEPROM writes." << std::endl;
            std::cout << "[DRY RUN] Nothing was sent to the printer." << std::endl;

            g_jsonData["model"] = selected.replayModel.name;
            g_jsonData["printer"] = nullptr;
            g_jsonData["counters"] = nlohmann::json::array();
            g_jsonData["pads"] = nlohmann::json::array();
            // A replay dump is opaque bytes: the packet count is all it knows.
            g_jsonData["pads_total"] = 0;
            g_jsonData["reset_covers"] = nullptr;
            g_jsonData["planned_writes"] = nullptr;
            g_jsonData["replay_packets"] = dumpSequence.size();
            g_jsonData["replay_writes"] = dumpWrites;
            return FinishRun(0);
        }

        std::cout << "\n[*] DRY RUN for " << selected.smartModel.name << ": reading status and counters (no writes)..." << std::endl;

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadState();

        if (state.available)
        {
            PrintPrinterStatus(state.status);
            PrintCounterValues(state.values, "Waste counter EEPROM values:");
            PrintCounterSummary(selected.smartModel, state.values);
        }
        else
        {
            // On stderr like every other failure notice: this run now exits 1,
            // and a caller that captures one stream should not have to capture
            // both to learn why (#42).
            std::cerr << "[!] The printer did not answer the read-only query - showing the plan anyway."
                      << std::endl;
            std::cerr << "    The plan below is the database's for this model, not a reading of this printer."
                      << std::endl;
        }

        if (selected.smartModel.jig)
        {
            PrintResetCoverage(selected.smartModel);
            std::cerr << "\n[!] " << selected.smartModel.name << " is read in JIG mode. EWR cannot reset it yet:\n"
                         "    its write command is not known. Nothing was written." << std::endl;
            JsonFail("not_supported", selected.smartModel.name + " cannot be reset yet: no JIG write command is known.");
            return FinishRun(1);
        }

        // The plan has to be the plan of the run --cartridge would perform,
        // or the dry run describes a reset nobody asked for.
        const std::vector<uint16_t> planAddresses = cli.cartridge
            ? selected.smartModel.GetInkAddresses() : selected.smartModel.GetAllAddresses();
        const std::vector<uint8_t> planValues = cli.cartridge
            ? selected.smartModel.GetInkResetValues() : selected.smartModel.GetAllResetValues();

        std::cout << "\nA real run would reset the "
                  << (cli.cartridge ? "cartridge ink levels" : "waste ink pad counters")
                  << ", writing " << planAddresses.size() << " EEPROM byte(s):" << std::endl;
        for (size_t i = 0; i < planAddresses.size(); ++i)
        {
            char line[64];
            snprintf(line, sizeof(line), "    EEPROM 0x%04X <- 0x%02X", planAddresses[i],
                     i < planValues.size() ? planValues[i] : 0);
            std::cout << line << std::endl;
        }

        // Which pads those bytes belong to; an ink reset clears none.
        if (!cli.cartridge)
            PrintResetCoverage(selected.smartModel);

        // The schema-4 close step belongs to the waste-pad reset; the ink
        // lifecycle has no commit.
        if (!cli.cartridge)
        {
            for (const auto& op : selected.smartModel.close_ops)
            {
                char line[96];
                snprintf(line, sizeof(line), "    commit: read 0x%04X, apply AND 0x%02X / OR 0x%02X, write back",
                         op.address, op.and_mask, op.or_mask);
                std::cout << line << std::endl;
            }
        }

        if (!selected.smartModel.wkey1.empty())
            std::cout << "    (an alternate write keyword is available if the primary is rejected)" << std::endl;

        std::cout << "\n[DRY RUN] Nothing was written. Run without --dry-run to perform the reset." << std::endl;

        nlohmann::json planned = nlohmann::json::array();
        for (size_t i = 0; i < planAddresses.size(); ++i)
        {
            nlohmann::json write;
            write["address"] = planAddresses[i];
            write["value"] = (i < planValues.size()) ? planValues[i] : 0;
            planned.push_back(std::move(write));
        }

        g_jsonData = ewr::json::StateData(selected.smartModel, state);
        g_jsonData["detected_model"] = detectedMatch.empty() ? nlohmann::json(nullptr)
                                                             : nlohmann::json(detectedMatch);
        g_jsonData["target"] = cli.cartridge ? "ink" : "waste";
        g_jsonData["planned_writes"] = std::move(planned);

        // An ink reset writes ink levels and clears no pad at all.
        if (cli.cartridge)
            g_jsonData["reset_covers"] = nlohmann::json::array();

        // The plan is the database's reset pattern for this model, not
        // anything read from a printer. A dry run that never got a reading has
        // not done what it was asked, so it says so instead of returning a
        // plan under a green light.
        if (!state.available)
        {
            JsonFail("read_failed", "The printer did not answer the read-only query; the plan below is the"
                                    " database's, not this printer's.");
            return FinishRun(1);
        }

        return FinishRun(0);
    }

    // Replay path: send a Wireshark dump byte-for-byte.
    // The dump's shape is unknown, so the session safeguards do not apply.
    int RunReplay(RunContext& ctx)
    {
        CliOptions& cli = ctx.cli;
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;

        std::cout << "\n[!] Parsing replay Wireshark dump for " << selected.displayName << "..." << std::endl;
        const std::vector<std::vector<unsigned char>> executionSequence =
            ewr::replay::ParseWiresharkDump(selected.replayModel.filepath);

        if (executionSequence.empty())
        {
            std::cerr << "[-] Failed to construct payload. Exiting.\n";
            JsonFail("not_supported", "The replay dump for " + selected.replayModel.name
                                      + " holds no packets EWR can send.");
            return FinishRun(1);
        }

        std::cout << "Scanning USB ports for Epson device..." << std::endl;

        // No handshake validation, no write verification, no substitution.
        ewr::ExecutorOptions replayOptions;
        replayOptions.validateHandshake = false;
        replayOptions.resendCreditOnRetry = false;
        replayOptions.verifyWrites = false;
        replayOptions.useSessionLayer = false;
        replayOptions.interfaceCandidate = cli.interfaceCandidate;
        replayOptions.usbSoftReset = cli.usbSoftReset;
        replayOptions.trace = sessionOptions.trace;

        const ewr::ResetRunResult run = gateway.RunReset(executionSequence, replayOptions);

        if (!run.deviceFound)
        {
            std::cerr << "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?" << std::endl;
            JsonFail("device_not_found", "No Epson USB interface answered.");
            return FinishRun(1);
        }

        g_jsonData["model"] = selected.replayModel.name;
        g_jsonData["target"] = "waste";
        g_jsonData["phase"] = run.exec.success ? "done" : "write_failed";
        g_jsonData["writes"] = {
            { "verified", run.exec.writesVerified },
            { "total", run.exec.writesTotal },
        };
        // A replay dump has no read key, so there is nothing to read back.
        g_jsonData["verification"] = { { "ran", false }, { "mismatches", 0 }, { "unread", 0 } };
        g_jsonData["before"] = nlohmann::json::array();
        g_jsonData["after"] = nlohmann::json::array();

        if (!run.exec.success)
            JsonFail("write_failed", run.exec.error);

        return FinishReset(run.exec.success);
    }

    // The Smart Protocol path: ewr::Session owns the whole lifecycle.
    int RunReset(RunContext& ctx)
    {
        CliOptions& cli = ctx.cli;
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;
        const std::string& mismatchPassed = ctx.mismatchPassed;

        if (selected.smartModel.IsPlatenOnly())
        {
            std::cout << "\n================================================================================" << std::endl;
            std::cout << "[!] NOTICE FOR " << selected.smartModel.name << ":" << std::endl;
            std::cout << "    This printer model ONLY has EEPROM counters for the PLATEN PAD (borderless ink pad)." << std::endl;
            std::cout << "    The MAIN WASTE INK BOX on this printer uses a physical hardware chip on the" << std::endl;
            std::cout << "    maintenance tank and CANNOT be reset over USB EEPROM." << std::endl;
            std::cout << "    To reset the Main Waste Ink Box, replace the maintenance box or use a physical chip resetter." << std::endl;
            std::cout << "================================================================================\n" << std::endl;
        }

        // The Session refuses too; this says so before any prompt.
        if (selected.smartModel.jig)
        {
            std::cerr << "\n[!] " << selected.smartModel.name << " is read in JIG mode, and EWR cannot reset it yet:\n"
                         "    its write command is not known. Nothing was written. Use --status to read\n"
                         "    its waste counters." << std::endl;
            JsonFail("not_supported", selected.smartModel.name + " cannot be reset yet: no JIG write command is known.");
            return FinishRun(1);
        }

        // No addresses and no ink map: the entry carries a working read key and
        // nothing to write. Say so and point at discovery rather than running a
        // reset that would send zero writes and report success.
        if (!selected.smartModel.HasResettableCounters() && !selected.smartModel.HasInkReset())
        {
            std::cout << "\n================================================================================" << std::endl;
            std::cout << "[!] " << selected.smartModel.name
                      << " has no reset addresses in the database yet." << std::endl;
            std::cout << "    Its read key works, so EWR can read this printer - it just does not know" << std::endl;
            std::cout << "    which bytes hold the waste counter, and it will not guess." << std::endl;
            std::cout << "\n    To find them, run:" << std::endl;
            std::cout << "        ewr --model \"" << selected.smartModel.name << "\" --find-addresses" << std::endl;
            std::cout << "\n    EWR will read the EEPROM three times and ask you to run a head cleaning" << std::endl;
            std::cout << "    between reads. Nothing is written. The bytes that rise every time are the" << std::endl;
            std::cout << "    counters, and the result is saved for a pull request." << std::endl;
            std::cout << "================================================================================" << std::endl;
            return FinishRun(0);
        }

        // ---- Cartridge ink reset choice: --cartridge names it outright, models
        // with a per-color ink map offer it, everything else goes straight to the
        // classic waste-pad reset.
        bool resetInk = cli.cartridge;
        if (cli.cartridge)
        {
            // Named the target explicitly, so there is no menu and no default.
            std::cout << "\n[i] --cartridge: resetting the cartridge ink levels of "
                      << selected.smartModel.name << ", not its waste ink pads." << std::endl;
        }
        else if (ctx.overNetwork && selected.smartModel.HasInkReset())
        {
            // --cartridge is refused with --ip on the command line; the menu and
            // the ink-only default below must not reach that reset another way.
            if (!selected.smartModel.HasResettableCounters())
            {
                const std::string why = selected.smartModel.name
                    + " has only a cartridge ink map, and the cartridge ink reset needs USB.";
                std::cerr << "\n[!] " << why << std::endl;
                JsonFail("not_supported", why);
                return FinishRun(1);
            }

            std::cout << "\n[i] " << selected.smartModel.name
                      << " also offers a cartridge ink reset, which needs USB;" << std::endl;
            std::cout << "    this network run resets the waste ink pad counters." << std::endl;
        }
        else if (selected.smartModel.HasInkReset() && !selected.smartModel.HasResettableCounters())
        {
            // Ink map but no waste-pad addresses: the ink reset is the only path.
            // --yes answers for the waste pads only, so here it has nothing to
            // answer - reaching the ink reset takes --cartridge, deliberately.
            if (cli.assumeYes)
            {
                std::cerr << "\n[!] --yes has nothing to confirm on " << selected.smartModel.name << "."
                          << std::endl;
                std::cerr << "    This model has no USB-resettable waste pad counters, only a cartridge"
                          << std::endl;
                std::cerr << "    ink map, and --yes alone never arms the cartridge ink reset: it can"
                          << std::endl;
                std::cerr << "    make an empty cartridge report full and run the head dry. Add"
                          << std::endl;
                std::cerr << "    --cartridge to mean it, or drop --yes to confirm at the keyboard."
                          << std::endl;
                JsonFail("blocked", selected.smartModel.name + " has only a cartridge ink map, and --yes never"
                                    " arms the ink reset; pass --cartridge to mean it.");
                return FinishRun(1);
            }

            std::cout << "\n" << selected.smartModel.name
                      << " has a cartridge ink map but no USB-resettable waste pad counters," << std::endl;
            std::cout << "    so the cartridge ink level reset is the available path." << std::endl;
            resetInk = true;
        }
        else if (selected.smartModel.HasInkReset() && (cli.assumeYes || cli.json))
        {
            // Both paths exist, so this menu would otherwise block a caller that
            // has no keyboard. --yes is a yes to the reset EWR is named after;
            // --cartridge is how the other one gets chosen.
            std::cout << "\n[i] " << selected.smartModel.name
                      << " also offers a cartridge ink reset; --yes takes the waste ink pad" << std::endl;
            std::cout << "    counters. Pass --cartridge to reset the ink levels instead." << std::endl;
        }
        else if (selected.smartModel.HasInkReset())
        {
            std::cout << "\n" << selected.smartModel.name
                      << " also has a per-color cartridge ink map in the database." << std::endl;
            std::cout << "\nWhat should be reset?" << std::endl;
            std::cout << "[1] Waste ink pad counters (the classic EWR reset)" << std::endl;
            std::cout << "[2] Cartridge ink levels (works only where levels live in the printer's EEPROM)" << std::endl;

            while (true)
            {
                std::cout << "\nSelect [1-2] (Enter = 1): ";
                std::string choice;
                if (!std::getline(std::cin, choice))
                {
                    // Enter picks 1, and so would a closed stdin read as empty.
                    std::cerr << "\n[!] No input available; stopping. Nothing was written." << std::endl;
                    JsonFail("blocked", "The reset-target menu needs someone at the keyboard.");
                    return FinishRun(1);
                }

                if (choice.empty() || choice == "1")
                    break;

                if (choice == "2")
                {
                    resetInk = true;
                    break;
                }

                std::cout << "[-] Invalid selection. Please enter 1 or 2." << std::endl;
            }
        }

        // Arms only on a typed word or an explicit --yes; Enter alone must never
        // write. On chipped cartridges the EEPROM bytes mirror the chip, so the
        // reset will not hold.
        if (resetInk)
        {
            std::cout << "\n[!] Cartridge ink reset rewrites the printer's per-color ink accounting." << std::endl;
            std::cout << "    EWR cannot refill ink: a genuinely empty cartridge will report full and" << std::endl;
            std::cout << "    can run dry mid-print, which can damage the print head." << std::endl;
            std::cout << "    The reset holds only on printers that keep the ink accounting in their" << std::endl;
            std::cout << "    own EEPROM. Cartridges with a smart chip carry the levels on the chip:" << std::endl;
            std::cout << "    the firmware treats the chip as the truth and rewrites the EEPROM from" << std::endl;
            std::cout << "    it, so on those models the reset cannot stick (the R220 generation is" << std::endl;
            std::cout << "    like this - verified on hardware)." << std::endl;

            // --cartridge chose this reset and --yes confirms it, so the typed
            // word has already been said twice on the command line.
            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes: zeroing every color's ink counter without asking." << std::endl;
            }
            else if (cli.json)
            {
                // The prompt below would wait on a stdin nobody types into, and a
                // caller holding it open would wait forever.
                JsonFail("blocked", "The cartridge ink reset needs --yes under --json.");
                return FinishRun(1);
            }
            else
            {
                std::cout << "\nType 'reset' to zero every color's ink counter, anything else to abort: ";

                std::string confirm;
                std::getline(std::cin, confirm);
                if (toLower(confirm) != "reset")
                {
                    std::cout << "[i] Aborted before any EEPROM write. Nothing was changed." << std::endl;
                    JsonFail("blocked", "The cartridge ink reset was declined; nothing was written.");
                    return FinishRun(1);
                }
            }
        }

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);

        ewr::ResetHandlers handlers;

        // Raw bytes stay off this screen unless the read-back disagrees (below):
        // the gauges say the same thing in words, and --status shows the bytes.
        handlers.onPreflight = [&](const ewr::StateSnapshot& before)
        {
            PrintPrinterStatus(before.status);
            if (resetInk)
            {
                PrintInkSummary(selected.smartModel, before.values);
            }
            else
            {
                PrintCounterSummary(selected.smartModel, before.values);
                PrintResetCoverage(selected.smartModel);
            }
        };

        // Anything but an explicit yes aborts before any write.
        handlers.onBlocker = [&](const ewr::Blocker& blocker)
        {
            if (blocker.errorCode >= 0)
                std::cout << "\n[!] WARNING: the printer reports an active error: "
                          << blocker.errorName << "." << std::endl;
            else
                std::cout << "\n[!] WARNING: " << blocker.errorName << "." << std::endl;

            if (!blocker.explanation.empty())
                std::cout << "    " << blocker.explanation << std::endl;

            // The blocker gate is the printer's own objection, not the ask-once
            // confirmation, so --yes declines it rather than answering it.
            if (cli.forceYes)
            {
                std::cout << "\n[!] --force-yes: continuing past that objection." << std::endl;
                return true;
            }

            // Before the --yes branch, which writes only to the silenced stdout.
            if (cli.json)
            {
                // Same answer as --yes, and for the same reason: only --force-yes
                // overrules what the printer reports.
                JsonFail("blocked", std::string(blocker.errorCode >= 0 ? "The printer reports a condition EWR will not"
                                                                       : "EWR will not")
                                    + " write past: " + blocker.errorName + ". Pass --force-yes to overrule it.");
                return false;
            }

            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes does not push past this: it answers the reset confirmation,"
                          << std::endl;
                std::cout << "    not a state the printer reports. Clear the condition, re-run without"
                          << std::endl;
                std::cout << "    --yes to decide at the keyboard, or pass --force-yes to overrule it."
                          << std::endl;
                return false;
            }

            std::cout << "\nTry the reset anyway? [y/N]: ";

            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        // Every conditional gate can stay silent and a BUSY status can hide a
        // real error, so this one always fires. The ink path already armed on the
        // typed 'reset', so it answers itself rather than asking twice.
        handlers.confirmWrite = [&](const ewr::StateSnapshot&)
        {
            if (resetInk)
                return true;

            // Over the network the model name alone could be anyone's printer.
            const std::string where = ctx.overNetwork ? " at " + cli.ip : std::string();

            // The one prompt --yes exists to answer.
            if (cli.assumeYes)
            {
                std::cout << "\n[i] --yes: resetting the waste ink pad counters of "
                          << selected.smartModel.name << where << " without asking." << std::endl;
                return true;
            }

            if (cli.json)
            {
                JsonFail("blocked", "The reset confirmation was not given: pass --yes.");
                return false;
            }

            std::cout << "\nReset the waste ink pad counters of " << selected.smartModel.name
                      << where << " now? [y/N]: ";

            std::string answer;
            std::getline(std::cin, answer);
            const std::string a = toLower(answer);
            return a == "y" || a == "yes";
        };

        // Kept before the first write, so --restore can undo this reset.
        handlers.saveBackup = [&](const ewr::Backup& taken, std::string& location)
        {
            ewr::Backup backup = taken;
            backup.deviceId = ctx.deviceId;
            backup.host = ctx.overNetwork ? cli.ip : std::string();

            std::string error;
            if (ewr::cli::StoreBackup(backup, location, error))
                return true;

            std::cerr << "[!] " << error << std::endl;
            return false;
        };

        handlers.onVerify = [&](const ewr::StateSnapshot& after)
        {
            if (resetInk)
                PrintInkSummary(selected.smartModel, after.values);
            else
                PrintCounterSummary(selected.smartModel, after.values);
        };

        ewr::ResetOutcome outcome = resetInk ? session.ResetInk(handlers)
                                             : session.Reset(handlers);

        if (!mismatchPassed.empty())
            outcome.overrides.insert(outcome.overrides.begin(), { "model_mismatch", mismatchPassed, -1 });

        // A read-back that disagrees is what a bug report needs, byte for byte.
        if (outcome.verificationRan && (outcome.verifyMismatches > 0 || outcome.verifyUnread > 0))
        {
            const char* noun = resetInk ? "Cartridge ink counter" : "Waste counter";
            PrintCounterValues(outcome.before.values, (std::string(noun) + " EEPROM values BEFORE reset:").c_str());
            PrintCounterValues(outcome.after.values, (std::string(noun) + " EEPROM values AFTER reset:").c_str());
        }

        // Built where ewr_reset builds it, so the two answers cannot drift. Named:
        // items() on the temporary would outlive it.
        const nlohmann::json resetData = ewr::json::ResetData(selected.smartModel, resetInk, outcome);
        for (const auto& item : resetData.items())
            g_jsonData[item.key()] = item.value();

        if (!outcome.success && !outcome.error.empty())
        {
            // The phase says what stage it reached; these are the contract's
            // reasons for the same thing.
            if (outcome.phase == ewr::ResetPhase::Aborted)
                JsonFail("blocked", outcome.error);
            else if (outcome.phase == ewr::ResetPhase::DeviceNotFound)
                JsonFail("device_not_found", outcome.error);
            else if (outcome.phase == ewr::ResetPhase::NotSupported)
                JsonFail("not_supported", outcome.error);
            else if (outcome.verificationRan && outcome.verifyMismatches > 0)
                JsonFail("write_unverified", outcome.error);
            else
                JsonFail("write_failed", outcome.error);
        }

        // The session already narrated why it stopped; skip the FAILED banner.
        if (outcome.phase == ewr::ResetPhase::Aborted
            || outcome.phase == ewr::ResetPhase::DeviceNotFound
            || outcome.phase == ewr::ResetPhase::NotSupported)
        {
            if (outcome.phase == ewr::ResetPhase::Aborted)
                JsonFail("blocked", "The run stopped at a gate; nothing was written.");
            else if (outcome.phase == ewr::ResetPhase::NotSupported)
                JsonFail("not_supported", outcome.error);
            else
                JsonFail("device_not_found", "No Epson interface answered at write time.");

            return FinishRun(1);
        }

        return FinishReset(outcome.success);
    }

} // namespace ewr::cli
