#include "cli.h"
#include "ewr/discovery.h"
#include "ewr/gates.h"
#include "ewr/keysearch.h"
#include "ewr/log.h"
#include "ewr/maintenance.h"
#include "ewr/paths.h"
#include "ewr/restore.h"
#include "ewr/session.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace ewr::cli {

    // --find-key: for a printer no database entry matches. It runs before the
    // model menu because there is no model to pick - the search is for one.
    int RunReadKeySearch(ewr::IDeviceGateway& gateway,
                                const std::vector<ewr::DbPrinterModel>& models,
                                const CliOptions& cli,
                                const std::string& detectedMdl,
                                const std::string& detectedMatch)
    {
        const std::vector<ewr::keysearch::Candidate> candidates = ewr::keysearch::CollectCandidates(models);

        ewr::ExecutorOptions options = ewr::ExecutorOptions::ForQueries();
        options.interfaceCandidate = cli.interfaceCandidate;
        options.usbSoftReset = cli.usbSoftReset;
        options.trace.printerReports = detectedMdl;
        options.trace.detectedEntry = detectedMatch;
        options.trace.selectedBy = "--find-key";

        std::cout << "\n[*] READ KEY SEARCH";
        if (!detectedMdl.empty())
            std::cout << " for \"" << detectedMdl << "\"";
        std::cout << ": trying the " << candidates.size() << " read keys in the database." << std::endl;
        std::cout << "    Read-only: only read commands are sent. This takes a few minutes." << std::endl;

        const bool animate = !g_json && StdoutIsTerminal();
        const ewr::keysearch::Result search = ewr::keysearch::Search(gateway, candidates, options,
            [animate](size_t tried, size_t total)
            {
                if (animate)
                    std::cout << "\r[*] Trying read keys... " << tried << "/" << total << std::flush;
            });

        if (animate && !search.probes.empty())
            std::cout << std::endl;

        g_jsonData = ewr::json::KeySearchData(detectedMdl, search);

        if (!search.deviceFound)
        {
            std::cerr << "\n[ERROR] No printer answered. Is it turned on and plugged in?" << std::endl;
            JsonFail("device_not_found", search.error);
            return FinishRun(1);
        }

        if (!search.completed)
        {
            std::cerr << "\n[ERROR] The search stopped after " << search.probes.size() << " of "
                      << candidates.size() << " keys: " << search.error << std::endl;
            std::cerr << "        Check ewr_trace.log for the hardware trace." << std::endl;
            JsonFail("read_failed", search.error);
            return FinishRun(1);
        }

        const size_t found = search.Count(ewr::ReadAnswer::Value);
        const size_t refused = search.Count(ewr::ReadAnswer::Refused);
        const size_t empty = search.Count(ewr::ReadAnswer::Empty);
        const size_t silent = search.Count(ewr::ReadAnswer::Silent);
        const size_t other = search.Count(ewr::ReadAnswer::Other);

        std::cout << "\n---------- READ KEY SEARCH -----------" << std::endl;
        std::cout << "  Keys tried:              " << search.probes.size() << std::endl;
        std::cout << "  Answered with a value:   " << found << std::endl;
        std::cout << "  Refused (':41:NA;'):     " << refused << std::endl;
        std::cout << "  Empty answer ('||:;'):   " << empty << std::endl;
        std::cout << "  No answer in 3 tries:    " << silent << std::endl;
        if (other > 0)
            std::cout << "  Something else:          " << other << std::endl;
        std::cout << "--------------------------------------" << std::endl;

        if (found > 0)
        {
            for (const ewr::keysearch::Probe& probe : search.probes)
            {
                if (probe.answer != ewr::ReadAnswer::Value)
                    continue;

                char line[160];
                snprintf(line, sizeof(line),
                         "\n[SUCCESS] Read key 0x%04X (%u) with %u-byte addresses works: address 0x00 holds 0x%02X.",
                         probe.candidate.rkey, probe.candidate.rkey,
                         static_cast<unsigned>(probe.candidate.addressLength), probe.value);
                std::cout << line << std::endl;

                std::cout << "          Database entries with this key: ";
                const std::vector<std::string>& names = probe.candidate.models;
                for (size_t i = 0; i < names.size() && i < 6; ++i)
                    std::cout << (i ? ", " : "") << names[i];
                if (names.size() > 6)
                    std::cout << " (+" << (names.size() - 6) << " more)";
                std::cout << std::endl;

                bool ownEntry = false;
                for (const std::string& name : names)
                    ownEntry = ownEntry || (!detectedMatch.empty() && name == detectedMatch);

                if (ownEntry)
                {
                    std::cout << "          " << detectedMatch
                              << " is the entry this printer matches: the database already has its key." << std::endl;
                }
                else
                {
                    std::cout << "\n    The printer speaks EWR's protocol with that key. To look at its memory,\n"
                                 "    read it with one of those entries. Neither writes the EEPROM; the second\n"
                                 "    asks before it runs its two head cleanings:\n"
                                 "        ewr --model \"" << names.front() << "\" --dump\n"
                                 "        ewr --model \"" << names.front() << "\" --find-addresses\n"
                                 "\n    Do NOT run a reset with that entry: the key fits, but its addresses are\n"
                                 "    that model's, not this printer's. Open an issue with ewr_trace.log and\n"
                                 "    the dump instead." << std::endl;
                }
            }

            return FinishRun(0);
        }

        std::cout << "\n[RESULT] No read key in the database works on this printer." << std::endl;

        if (empty == search.probes.size())
        {
            std::cout << "    Every key drew the same empty answer, which does not even echo the read's\n"
                         "    action code. The printer most likely does not take the keyed read command\n"
                         "    EWR uses at all, in which case no key would work." << std::endl;
        }
        else if (refused == search.probes.size())
        {
            std::cout << "    Every key was refused: the printer knows the read command and turns these\n"
                         "    keys down. Its own key is simply not in the database." << std::endl;
        }
        else if (silent == search.probes.size())
        {
            std::cout << "    The printer answered none of the reads. That is what a wrong key looks\n"
                         "    like on some firmware, so its own key may just not be in the database." << std::endl;
        }

        std::cout << "    Please attach ewr_trace.log to an issue or a discussion: it records how\n"
                     "    every key was answered." << std::endl;

        // The search ran to the end; finding nothing is an answer, not a failure.
        return FinishRun(0);
    }

    // Address discovery: three read-only passes around head cleanings.
    int RunFindAddresses(RunContext& ctx)
    {
        CliOptions& cli = ctx.cli;
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;

        if (selected.isReplay)
        {
            std::cerr << "[!] --find-addresses needs a Smart Protocol model: it reads the EEPROM\n"
                         "    with the database read key, and Replay dumps carry none." << std::endl;
            JsonFail("not_supported", "--find-addresses needs a Smart Protocol model; a Replay dump carries no read key.");
            return FinishRun(1);
        }

        if (selected.smartModel.jig)
        {
            std::cerr << "[!] --find-addresses does not support JIG-mode printers yet." << std::endl;
            JsonFail("not_supported", "--find-addresses does not support JIG-mode printers yet.");
            return FinishRun(1);
        }

        const std::vector<uint16_t> addresses = ewr::restore::DumpAddresses(selected.smartModel, cli.dumpFull);
        const uint32_t end = addresses.empty() ? 0 : addresses.back();

        std::cout << "\n[*] ADDRESS DISCOVERY for " << selected.smartModel.name << std::endl;
        std::cout << "    EWR will read the EEPROM three times with a head cleaning between reads -" << std::endl;
        std::cout << "    the one action guaranteed to move the waste counter. Bytes that rise after" << std::endl;
        std::cout << "    BOTH cleanings are the counters; a byte that moves only once is a timer or" << std::endl;
        std::cout << "    a page count. No EEPROM byte is written at any point." << std::endl;
        std::cout << "\n    A cleaning uses real ink and pushes the waste counter up, so this costs" << std::endl;
        std::cout << "    a little of both. Two cleanings is the minimum that proves a trend." << std::endl;

        // A borrowed entry's cleaning byte is that model's command: on another
        // printer the same byte can be another one - 0x04 is a cleaning on some
        // models and the ink charge on others. Only the printer's own entry
        // gets the factory route; anything else gets the printer's own
        // cleaning, sent as print data.
        const bool ownEntry = !ctx.detectedMatch.empty() && ctx.detectedMatch == selected.smartModel.name;

        // EWR can run the cleanings itself; a person at the panel is
        // the fallback, and the only way under --json without --yes.
        bool autoClean = false;
        if (cli.assumeYes)
        {
            autoClean = true;
            std::cout << "\n[i] --yes: EWR runs the two cleanings itself." << std::endl;
        }
        else if (cli.json)
        {
            std::cerr << "[!] --find-addresses under --json runs the cleanings itself, which uses ink:\n"
                         "    pass --yes to agree to that." << std::endl;
            JsonFail("blocked", "--find-addresses under --json needs --yes to run the two cleanings.");
            return FinishRun(1);
        }
        else
        {
            std::cout << "\nShould EWR run the two cleanings itself? [Y/n] (n: you start each one from the\n"
                         "printer's panel): ";
            std::string answer;
            if (!std::getline(std::cin, answer))
            {
                std::cerr << "\n[!] No input available; stopping. Nothing was changed." << std::endl;
                JsonFail("blocked", "The cleaning question needs someone at the keyboard.");
                return FinishRun(1);
            }
            const std::string a = toLower(answer);
            autoClean = a.empty() || a == "y" || a == "yes";
        }

        if (autoClean && !ownEntry)
            std::cout << "\n[i] The printer is not known to be " << selected.smartModel.name << ", so EWR asks it for\n"
                         "    its own normal cleaning, sent as print data, not with that entry's command." << std::endl;
        std::cout << std::endl;

        std::vector<ewr::discovery::EepromSnapshot> snapshots;
        const int passes = 3;
        int shortPasses = 0;

        for (int pass = 1; pass <= passes; ++pass)
        {
            std::cout << "[*] Reading the EEPROM, pass " << pass << " of " << passes << "..." << std::endl;

            ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
            const ewr::StateSnapshot state = session.ReadAddresses(addresses);

            if (!state.available)
            {
                std::cerr << "[ERROR] Could not read the printer on pass " << pass
                          << ". Is it powered on and connected?" << std::endl;
                std::cerr << "        See ewr_trace.log for the hardware trace." << std::endl;
                JsonFail("read_failed", "The printer did not answer on pass " + std::to_string(pass) + ".");
                return FinishRun(1);
            }

            size_t answered = 0;
            for (const auto& v : state.values)
                answered += (v.second >= 0) ? 1 : 0;

            snapshots.push_back(state.values);
            std::cout << "    Pass " << pass << ": " << answered << " of " << state.values.size()
                      << " byte(s) answered." << std::endl;

            // Discovery drops an address that went unread in any pass, so a
            // short pass quietly narrows the search rather than skewing it.
            if (answered < state.values.size())
            {
                ++shortPasses;
                std::cout << "    [!] Pass " << pass << " is incomplete. A byte that went unread in any pass"
                             " is dropped\n        from the comparison, so a counter can go missing from the"
                             " result." << std::endl;
            }

            if (pass == passes)
                break;

            if (autoClean)
            {
                std::cout << "\n[*] Head cleaning " << pass << " of " << (passes - 1) << "..." << std::endl;

                ewr::MaintenanceRequest clean;
                clean.remoteOnly = !ownEntry;
                ewr::Maintenance maintenance(gateway, &selected.smartModel, ewr::log::Default(), sessionOptions);

                ewr::MaintenanceHandlers handlers;
                handlers.onBlocker = [&](const ewr::Blocker& blocker)
                {
                    std::cout << "\n[!] " << blocker.errorName << ". " << blocker.explanation << std::endl;
                    if (cli.forceYes)
                        return true;
                    std::cout << "    Stopping: a cleaning the printer objects to proves nothing." << std::endl;
                    return false;
                };
                const bool animate = !g_json && StdoutIsTerminal();
                handlers.onProgress = [animate](const ewr::PrinterStatus& status, int seconds)
                {
                    if (animate)
                        std::cout << "\r[*] " << seconds << " s: " << status.stateName << "          " << std::flush;
                };

                const ewr::MaintenanceOutcome cleaned = maintenance.Run(clean, handlers);
                if (animate)
                    std::cout << std::endl;

                if (!cleaned.success)
                {
                    std::cerr << "[ERROR] Cleaning " << pass << " did not complete ("
                              << ewr::ToString(cleaned.phase) << "): " << cleaned.error << std::endl;
                    std::cerr << "        Run --find-addresses again and answer 'n' to start the cleanings from"
                                 " the printer's panel." << std::endl;
                    JsonFail(MaintenanceErrorCode(cleaned.phase),
                             "Cleaning " + std::to_string(pass) + " did not complete: " + cleaned.error);
                    return FinishRun(1);
                }

                std::cout << std::endl;
                continue;
            }

            std::cout << "\n    Dump " << pass << " is taken. Run a head cleaning cycle on the printer"
                      << " now,\n    wait until it has completely finished, then press Enter for the"
                      << " next dump." << std::endl;
            std::cout << "    (or type 'exit' to stop here): ";

            std::string line;
            if (!std::getline(std::cin, line))
            {
                std::cerr << "\n[!] No input available; stopping." << std::endl;
                JsonFail("blocked", "The pass-to-pass prompt needs someone at the keyboard.");
                return FinishRun(1);
            }
            if (toLower(line) == "exit" || toLower(line) == "quit")
            {
                std::cout << "[i] Stopped before pass " << (pass + 1) << ". Nothing was written." << std::endl;
                return FinishRun(0);
            }
            std::cout << std::endl;
        }

        const std::vector<ewr::discovery::ByteTrend> trends = ewr::discovery::FindTrendingBytes(snapshots);

        // The shape docs/json-output.md has promised since the command had no --json.
        nlohmann::json readings = nlohmann::json::array();
        for (const ewr::discovery::ByteTrend& t : trends)
            readings.push_back({ { "addresses", t.addresses }, { "pair", t.IsPair() }, { "values", t.values },
                                 { "deltas", t.deltas }, { "direction", t.rising ? "rising" : "falling" } });
        g_jsonData["model"] = selected.smartModel.name;
        g_jsonData["file"] = nullptr;
        g_jsonData["passes"] = snapshots.size();
        g_jsonData["short_passes"] = shortPasses;
        g_jsonData["cleanings"] = autoClean ? "ewr" : "panel";
        g_jsonData["trends"] = readings;

        if (shortPasses > 0)
        {
            std::cout << "\n[!] " << shortPasses << " of " << passes << " pass(es) came back incomplete, so the"
                         " result below covers\n    less of the EEPROM than it looks. Three complete passes"
                         " are worth rerunning for." << std::endl;
        }

        std::cout << "\n------------------------- BYTES THAT MOVED -------------------------" << std::endl;
        if (trends.empty())
        {
            std::cout << "  None. No byte moved the same way after both cleanings." << std::endl;
            std::cout << "\n  That usually means one of three things: the cleanings did not actually"
                      << "\n  run, this model keeps its counters above 0x" << std::hex << end << std::dec
                      << " (outside the range read here),"
                      << "\n  or the counter lives on the maintenance box chip rather than in the"
                      << "\n  printer's EEPROM, in which case no reset over USB can reach it." << std::endl;
            return FinishRun(0);
        }

        std::cout << "  ADDRESS          WIDTH       DUMP 1   DUMP 2   DUMP 3   TREND" << std::endl;
        for (const ewr::discovery::ByteTrend& t : trends)
        {
            std::string addr;
            for (size_t i = 0; i < t.addresses.size(); ++i)
            {
                char one[12];
                snprintf(one, sizeof(one), "0x%04X", t.addresses[i]);
                addr += (i ? " " : "") + std::string(one);
            }

            char line[160];
            snprintf(line, sizeof(line), "  %-16.16s %-9.9s", addr.c_str(),
                     t.IsPair() ? "16-bit" : "8-bit");
            std::cout << line;

            for (uint32_t v : t.values)
            {
                snprintf(line, sizeof(line), "%9u", v);
                std::cout << line;
            }

            snprintf(line, sizeof(line), "   %s (", t.rising ? "rising " : "falling");
            std::cout << line;
            for (size_t i = 0; i < t.deltas.size(); ++i)
                std::cout << (i ? ", " : "") << (t.deltas[i] > 0 ? "+" : "") << t.deltas[i];
            std::cout << ")" << std::endl;
        }
        std::cout << "--------------------------------------------------------------------" << std::endl;

        std::cout << "\n  A waste counter only ever rises, so the rising rows are the candidates"
                  << "\n  and the falling ones are something else - an ink level, a countdown."
                  << "\n  Nothing here says what any of them should be reset TO." << std::endl;

        // The model's own write path, so the saved file is a complete entry.
        std::ostringstream body;
        body << "            \"mem_high\": " << selected.smartModel.mem_high << ",\n";
        body << "            \"rkey\": " << selected.smartModel.rkey << ",\n";
        body << "            \"rlen\": " << selected.smartModel.rlen << ",\n";
        body << "            \"wkey\": " << ewr::db::WriteKeyToJson(selected.smartModel.wkey) << ",\n";
        if (!selected.smartModel.wkey1.empty())
            body << "            \"wkey1\": " << ewr::db::WriteKeyToJson(selected.smartModel.wkey1) << ",\n";
        body << "            \"wlen\": " << selected.smartModel.wlen << ",\n";

        std::string safeName;
        for (char ch : selected.smartModel.name)
            safeName += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';

        const std::string outPath = ewr::paths::InDataDir(safeName + "-found-addresses.json");
        std::ofstream out(outPath);
        if (!out)
        {
            std::cerr << "[!] Found the candidates but could not write " << outPath << "." << std::endl;
            JsonFail("io_error", "Could not write " + outPath + ".");
            return FinishRun(1);
        }
        out << ewr::discovery::ToJson(selected.smartModel.name, body.str(), trends);
        out.close();
        g_jsonData["file"] = outPath;

        size_t rising = 0;
        for (const ewr::discovery::ByteTrend& t : trends)
            rising += t.rising ? 1 : 0;

        std::cout << "\n[SUCCESS] Wrote " << trends.size() << " reading(s) (" << rising
                  << " rising) to " << outPath << "." << std::endl;
        std::cout << "\n[!] That file records what was read, not a reset plan. Its 'pad_groups' is\n"
                     "    empty on purpose: which of these is a waste counter, and what it resets\n"
                     "    to, is a judgement no dump can make - some pad groups reset certain\n"
                     "    bytes to 0x5E (94) rather than 0.\n"
                     "\n    Please open an issue or a pull request with this file and your printer\n"
                     "    model. Checking it against a known-good capture is what turns a reading\n"
                     "    into a database entry that is safe to write." << std::endl;
        return FinishRun(0);
    }

} // namespace ewr::cli
