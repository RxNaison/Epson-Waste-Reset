#include "cli.h"
#include "ewr/log.h"
#include "ewr/updater.h"
#include "ewr/usb.h"
#include "color.h"
#include "console.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <streambuf>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace ewr::cli {

    int Run(int argc, char* argv[])
    {
        // A file named on the command line is relative to where the user is,
        // not to the folder the run moves into next.
        std::error_code launchError;
        const fs::path launchDir = fs::current_path(launchError);

        SetWorkingDirectoryToExecutable();
        const std::string databasePath = ChooseDataLocation();

        // The core never prints on its own. Warnings and errors go to stderr so
        // they survive a redirected stdout.
        ewr::log::Default().AddSink(ewr::cli::ConsoleFor(std::cout, std::cerr, StdoutIsTerminal()));

        CliOptions cli;

        // Decided before anything prints: in --json mode stdout belongs to the
        // contract, and a command line that gets rejected still owes the caller a
        // result line.
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--json" || arg == "--json-version")
            {
                cli.json = true;
                break;
            }
        }

        if (cli.json)
        {
            StartJsonOutput();
        }
        else
        {
            InstallColor();
        }

        if (const auto exit = ParseCommandLine(argc, argv, cli))
            return *exit;

        const bool statusOnly = cli.statusOnly;

        g_jsonCommand = cli.listOnly       ? "list"
                      : cli.findKey        ? "find-key"
                      : cli.statusOnly     ? "status"
                      : cli.dump           ? "dump"
                      : cli.findAddresses  ? "find-addresses"
                      : cli.restoreAll     ? "restore-all"
                      : !cli.restoreFile.empty() ? "restore"
                      : cli.info           ? "info"
                      : cli.nozzleCheck    ? "nozzle-check"
                      : cli.headClean      ? "head-clean"
                      : cli.powerClean     ? "power-clean"
                      : cli.dryRun         ? "dry-run"
                                           : "reset";
        JsonHello(cli);

        // --yes means nobody is at the keyboard, so the closing "press Enter" read
        // would block the caller after the reset it just asked for.
        g_exitPause = !cli.json && StdinIsInteractive()
            && !(cli.statusOnly || cli.listOnly || cli.dryRun || cli.dump || cli.findKey || cli.assumeYes || cli.info);

        std::cout << "========================================" << std::endl;
        std::cout << "       " << ewr::cli::Paint("EWR - Epson Waste Reset", ewr::cli::ansi::kCyan) << "          " << std::endl;
        std::cout << "       Version " << kEwrCurrentVersion << std::endl;
        std::cout << "========================================\n" << std::endl;

        if (cli.findKey)
            std::cout << "[i] READ KEY SEARCH: read commands only, no EEPROM writes will be sent.\n" << std::endl;
        else if (statusOnly)
            std::cout << "[i] READ-ONLY STATUS MODE: no EEPROM writes will be sent.\n" << std::endl;
        else if (cli.dryRun)
            std::cout << "[i] DRY RUN: EWR will detect, read and plan, but write nothing.\n" << std::endl;
        else if (cli.dump)
            std::cout << "[i] DUMP MODE: reading the EEPROM to a file, no writes will be sent.\n" << std::endl;
        else if (cli.findAddresses)
            std::cout << "[i] ADDRESS DISCOVERY: repeated read-only EEPROM dumps, no writes will be sent.\n" << std::endl;
        else if (cli.info)
            std::cout << "[i] PRINTER INFO: read-only, nothing is written.\n" << std::endl;
        else if (cli.nozzleCheck || cli.headClean || cli.powerClean)
            std::cout << "[i] MAINTENANCE: no EEPROM byte is written; the printer is asked to "
                      << (cli.nozzleCheck ? "print its nozzle check." : "clean its print head.") << "\n" << std::endl;
        else if (cli.forceYes)
            std::cout << "[!] --force-yes: the confirmation is answered AND the gates that would stop\n"
                         "    this run are overruled. A wrong-model write can misconfigure the printer.\n" << std::endl;
        else if (cli.assumeYes)
            std::cout << "[i] --yes: the reset confirmation is answered automatically. Every other gate\n"
                         "    still stops the run before any write.\n" << std::endl;

        ewr::update::CleanupStaleTempFiles();

        // Read before anything is asked of the printer: a file EWR cannot trust
        // stops the run here. The file names the model, so it picks the entry
        // unless --model does.
        ewr::Backup restoreSource;
        if (!cli.restoreFile.empty())
        {
            fs::path file(cli.restoreFile);
            if (file.is_relative() && !launchDir.empty())
                file = launchDir / file;
            cli.restoreFile = file.string();

            std::string error;
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                error = "Could not open " + cli.restoreFile + ".";
            }
            else
            {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                ewr::backup::Parse(text, restoreSource, error);
            }

            if (!error.empty())
            {
                std::cerr << "[ERROR] " << error << std::endl;
                JsonFail("bad_backup", error);
                return FinishRun(1);
            }

            if (cli.restoreAll && restoreSource.source != "dump")
            {
                std::cerr << "[ERROR] --restore-all takes a --dump file. A backup holds only what one reset"
                             " wrote: use --restore for it." << std::endl;
                JsonFail("bad_backup", "--restore-all takes a --dump file, not a backup.");
                return FinishRun(1);
            }

            if (cli.modelOverride.empty())
                cli.modelOverride = restoreSource.model;

            std::cout << "[i] RESTORE: writing back what " << file.filename().string() << " holds for "
                      << restoreSource.model << (cli.dryRun ? " - dry run, nothing is written." : ".") << "\n" << std::endl;
        }

        if (cli.listOnly)
            return RunList(databasePath);

        if (cli.noUpdate)
        {
            // Fully offline run: no release check, no download.
            std::cout << "[i] --no-update: offline run, database.json will not be modified." << std::endl;
        }
        else
        {
            // The database is refreshed before it is loaded, so this run already
            // writes with it. The release check runs alongside: offline, the two
            // wait out one connect timeout instead of one each.
            auto database = std::async(std::launch::async, [&databasePath] { return ewr::update::UpdateDatabaseNow(databasePath); });
            auto release = std::async(std::launch::async, ewr::update::CheckLatestRelease, kEwrCurrentVersion);

            auto ready = [](const auto& task) { return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };

            std::cout << "[i] Checking for database update... ";
            SpinUntil([&] { return ready(database) && ready(release); });

            switch (database.get())
            {
                case ewr::DatabaseUpdate::Updated:  std::cout << "updated." << std::endl; break;
                case ewr::DatabaseUpdate::UpToDate: std::cout << "up to date." << std::endl; break;
                case ewr::DatabaseUpdate::Failed:   std::cout << "failed - using the copy on disk." << std::endl; break;
            }

            const ewr::UpdateMetadata update = release.get();
            if (update.updateAvailable)
                PrintNewVersionBanner(update.latestVersion);
            else if (update.latestVersion.empty())
                std::cout << "[i] Could not reach GitHub to check for a newer EWR release." << std::endl;
            else
                std::cout << "[i] EWR " << kEwrCurrentVersion << " is the latest version." << std::endl;
        }

        ewr::Database db;
        if (!db.Load(databasePath))
            std::cerr << "[!] Could not load database.json (missing or corrupted). Smart Protocol models are unavailable this run." << std::endl;

        auto replayModels = ewr::replay::ScanFolder("models");
        auto smartModels = db.Models();

        if (replayModels.empty() && smartModels.empty())
        {
            std::cerr << "\n[!] No payloads found: database.json is missing or unreadable." << std::endl;
            JsonFail("database", "No payloads found: database.json is missing or unreadable.");
            std::cerr << "    It ships next to ewr - re-extract the archive, or run once with internet access to fetch it." << std::endl;
            return FinishRun(1);
        }

        std::vector<MenuOption> options;

        for (const auto& sm : smartModels)
        {
            // No addresses yet, but the read key is enough to dump the EEPROM and
            // go looking for them, so the entry is offered rather than hidden.
            const bool resettable = sm.HasResettableCounters() || sm.HasInkReset();
            const char* label = sm.jig     ? " (JIG mode - read-only for now)"
                              : resettable ? " (Smart Protocol - Recommended)"
                                           : " (no reset addresses yet - read-only)";

            options.push_back({ sm.name + label, false, {}, sm });
        }

        for (const auto& lm : replayModels)
            options.push_back({ lm.name + " (Replay)", true, lm, {} });

        std::sort(options.begin(), options.end(), [](const MenuOption& a, const MenuOption& b)
            {
                const std::string& an = a.isReplay ? a.replayModel.name : a.smartModel.name;
                const std::string& bn = b.isReplay ? b.replayModel.name : b.smartModel.name;
                if (an != bn)
                    return an < bn;

                // Same model from both sources. Sorting the display name would put
                // "(Replay)" above "(Smart Protocol - Recommended)" and offer the
                // path with no read-back verification as choice [1].
                return !a.isReplay && b.isReplay;
            });

        // One per run: it owns the ewr_trace.log lifecycle, so the first device
        // call starts the file fresh and every later session appends.
        ewr::UsbDeviceGateway usbGateway;

        // --ip: the same seam over SNMP. USB is not touched at all on such a run.
        // Also set when the network search below finds the printer instead.
        std::unique_ptr<ewr::SnmpDeviceGateway> netGateway;
        if (!cli.ip.empty())
            netGateway = std::make_unique<ewr::SnmpDeviceGateway>(cli.ip);

        // Before any device call, so a second run says why it stopped instead of
        // reporting a printer that cannot be read. The gateway logs the reason as
        // an event; the result line has to name it too, because a caller is
        // promised it can act on the verdict alone.
        const bool claimed = netGateway ? netGateway->ClaimPrinter() : usbGateway.ClaimPrinter();
        if (!claimed)
        {
            JsonFail("another_run", "Another EWR run is already driving a printer on this machine.");
            return FinishRun(1);
        }

        // Once per run: powers detection, the banner and --interface validation.
        std::string detectedMdl;
        std::string detectedMatch;

        if (netGateway)
            std::cout << "\n[i] Asking the printer at " << cli.ip << " (SNMP)... " << std::flush;
        else if (!cli.ipAuto)
            std::cout << "\n[i] Detecting the connected printer... " << std::flush;

        // --ip auto leaves USB alone like --ip does: a printer there is not the
        // one that was asked for.
        const std::vector<ewr::usb::InterfaceInfo> interfaces = (netGateway || cli.ipAuto)
            ? std::vector<ewr::usb::InterfaceInfo>{} : usbGateway.ListInterfaces();

        if (cli.interfaceCandidate >= 1 && cli.interfaceCandidate > static_cast<int>(interfaces.size()))
        {
            std::cout << "found " << interfaces.size() << " interface(s)." << std::endl;
            std::cerr << "[!] --interface " << cli.interfaceCandidate << " does not exist: only "
                      << interfaces.size() << " Epson interface(s) are present. Run 'ewr --list' to see them." << std::endl;
            JsonFail("interface_not_found", "--interface " + std::to_string(cli.interfaceCandidate)
                                            + " does not exist: " + std::to_string(interfaces.size())
                                            + " Epson interface(s) are present.");
            return FinishRun(1);
        }

        // The pinned interface's when --interface is set, else the first answer.
        ewr::DeviceIdQueryResult devIdQuery;
        if (netGateway)
        {
            devIdQuery = netGateway->QueryDeviceId();

            // Silence here is the whole answer: there is no second interface to
            // fall back to, and a menu would only postpone the same timeout.
            if (!netGateway->Answered())
            {
                const std::string why = netGateway->SilenceError();

                std::cout << "no answer." << std::endl;
                std::cerr << "[ERROR] " << why << std::endl;
                if (netGateway->SendBlocked())
                {
                    std::cerr << "        The request never left this machine, so this says nothing about the printer.\n"
                                 "        A VPN or firewall can cut off the local network, and an operating system can\n"
                                 "        keep a program off it (on macOS, see System Settings > Privacy & Security >\n"
                                 "        Local Network)." << std::endl;
                }
                else if (netGateway->OpenError().empty())
                {
                    std::cerr << "        Check that the printer is on and awake, that the address is the one on its\n"
                                 "        network status sheet, that this computer is on the same network, and that\n"
                                 "        SNMP has not been switched off in the printer's web settings." << std::endl;
                }
                JsonFail("device_not_found", why);
                return FinishRun(1);
            }
        }

        for (const auto& iface : interfaces)
        {
            if (cli.interfaceCandidate >= 1 && iface.index != cli.interfaceCandidate)
                continue;

            if (!iface.deviceId.empty())
            {
                devIdQuery.found = true;
                devIdQuery.deviceId = iface.deviceId;
                break;
            }
        }

        // The database owns the name table, so no model strings here.
        std::vector<ewr::deviceid::ModelNameEntry> smartEntries;
        for (const auto& opt : options)
        {
            if (!opt.isReplay)
                smartEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
        }

        // Nothing Epson on USB at all: the printer may be on the network. A
        // person picks it, since a network can hold someone else's Epson, and
        // from the pick on this is an --ip run. --json has no one to ask, and
        // the USB-only modes have nothing to do with what it would find.
        // --ip auto asks for the search outright.
        const bool searchNetwork = cli.ipAuto
            || (!netGateway && interfaces.empty() && !cli.json
                && cli.interfaceCandidate < 1 && !cli.usbSoftReset
                && !cli.findKey && !cli.dump && !cli.findAddresses && !cli.cartridge);
        if (searchNetwork)
        {
            if (cli.ipAuto)
                std::cout << "\n";
            else
                std::cout << "no answer on USB." << std::endl;
            std::cout << "[i] Searching the local network for Epson printers (SNMP)... " << std::flush;

            std::string searchError;
            std::ostringstream searchTrace;
            const std::vector<ewr::NetworkPrinter> found =
                ewr::snmp::DiscoverPrinters(ewr::snmp::kDiscoveryWaitMs, searchError, &searchTrace);

            // Before the lock can move on: writing the trace needs the claim.
            usbGateway.AppendTrace(searchTrace.str());

            if (!searchError.empty())
            {
                std::cout << "not possible." << std::endl;
                std::cerr << "[!] " << searchError << std::endl;
            }
            else if (found.empty())
            {
                std::cout << "none found." << std::endl;
                PrintNetworkSearchHints();
            }
            else
            {
                std::cout << "found " << found.size() << "." << std::endl;

                const int choice = ChooseNetworkPrinter(found, smartEntries);
                if (choice == kNoInputForPrinter)
                {
                    // The model prompt after this would ask for --model, but the
                    // printer is what is missing.
                    std::cerr << "\n[ERROR] No input available: EWR needs you to pick the printer and stdin\n"
                                 "        is closed. Pass --ip <address>, one from the list above, to choose\n"
                                 "        it non-interactively." << std::endl;
                    JsonFail("blocked", "No printer was chosen from the network search, and stdin is closed.");
                    return FinishRun(1);
                }

                if (choice >= 0)
                {
                    const ewr::NetworkPrinter& chosen = found[static_cast<std::size_t>(choice)];
                    cli.ip = chosen.address;
                    netGateway = std::make_unique<ewr::SnmpDeviceGateway>(cli.ip);
                    netGateway->ContinueRunTrace();
                    netGateway->AdoptRunLock(usbGateway.ReleaseRunLock());

                    devIdQuery.found = true;
                    devIdQuery.deviceId = chosen.deviceId;
                    std::cout << "\n[i] Using the printer at " << cli.ip << " (SNMP)... " << std::flush;
                }
            }

            // Asked for the network, so no falling back to a USB printer or to
            // the model menu: it is that printer or none.
            if (cli.ipAuto && !netGateway)
            {
                if (!searchError.empty() || found.empty())
                {
                    JsonFail("device_not_found", searchError.empty() ? "No Epson printer answered on the network."
                                                                     : searchError);
                }
                else
                {
                    std::cout << "[i] No printer chosen. Nothing was changed." << std::endl;
                    JsonFail("blocked", "No printer was chosen from the network search.");
                }
                return FinishRun(1);
            }
        }

        if (devIdQuery.found)
        {
            const ewr::deviceid::Info devId = ewr::deviceid::Parse(devIdQuery.deviceId);
            detectedMdl = devId.model;

            if (!detectedMdl.empty())
            {
                std::cout << "found \"" << detectedMdl << "\"." << std::endl;

                const std::vector<std::string> matches = ewr::deviceid::MatchEntries(detectedMdl, smartEntries);
                if (!matches.empty())
                {
                    detectedMatch = matches[0];
                    std::cout << "[i] Database match: " << detectedMatch << std::endl;
                }
                else
                {
                    std::cout << "[!] No database entry matches \"" << detectedMdl << "\" - pick your model manually." << std::endl;
                }
            }
            else
            {
                std::cout << "the device answered, but reported no model name." << std::endl;
            }
        }
        else if (!searchNetwork)
        {
            std::cout << (netGateway ? "it answers SNMP, but returned no device ID."
                                     : "no answer (printer off, unplugged, or driver limitation).") << std::endl;
        }

        // Composite devices expose several interfaces and only one of them is
        // the printer engine - show the order instead of guessing silently.
        if (interfaces.size() > 1)
        {
            std::cout << "[i] " << interfaces.size() << " Epson USB interfaces present"
                      << (cli.interfaceCandidate >= 1 ? " (pinned by --interface):" : " (tried in this order):") << std::endl;
            for (const auto& iface : interfaces)
            {
                std::cout << "      [" << iface.index << "] " << iface.className;
                if (iface.interfaceNumber >= 0)
                    std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;

                if (!iface.deviceId.empty())
                {
                    const std::string bannerMdl = ewr::deviceid::Parse(iface.deviceId).model;
                    if (!bannerMdl.empty())
                        std::cout << " - \"" << bannerMdl << "\"";
                    else
                        std::cout << " - answered the device ID query";
                }
                else
                {
                    std::cout << " - no device ID reply";
                }

                if (cli.interfaceCandidate == iface.index)
                    std::cout << "   <-- pinned";

                std::cout << std::endl;
            }
        }

        // Bound only now: the network search above can change the transport.
        ewr::IDeviceGateway& gateway = netGateway ? static_cast<ewr::IDeviceGateway&>(*netGateway)
                                                  : static_cast<ewr::IDeviceGateway&>(usbGateway);

        if (cli.findKey)
            return RunReadKeySearch(gateway, smartModels, cli, detectedMdl, detectedMatch);

        MenuOption selected;
        std::string selectedBy;
        if (const auto exit = SelectModel(cli, options, detectedMdl, detectedMatch, netGateway != nullptr,
                                          selected, selectedBy))
            return *exit;

        // The top cause of wrong-model writes, so it needs an explicit yes.
        // The read-only modes are exempt: they send no writes to misplace.
        // A replay dump is opaque bytes for a D4 channel; there is nothing in it
        // the network transport could confirm. Its status read still works.
        if (netGateway && selected.isReplay && !statusOnly)
        {
            std::cerr << "\n[!] " << selected.displayName << " is a Replay dump, and those can only be sent over USB."
                      << std::endl;
            JsonFail("not_supported", "A Replay dump cannot be sent over the network; pick a database model or use USB.");
            return FinishRun(1);
        }

        const bool readOnly = statusOnly || cli.dryRun || cli.dump || cli.findAddresses || cli.info;
        const bool modelMismatch = !selected.isReplay && !detectedMatch.empty()
                                   && selected.smartModel.name != detectedMatch;

        // Set once the gate below is passed, for the result's `overrides`.
        std::string mismatchPassed;

        // No gate, but a person would otherwise read another model's counters off
        // this printer with nothing saying so. --json has detected_model.
        if (readOnly && modelMismatch)
        {
            std::cout << "\n[!] The connected printer reports \"" << detectedMdl << "\" (database entry: "
                      << detectedMatch << "), not " << selected.smartModel.name << "." << std::endl;
            std::cout << "    Nothing is written in this mode, but what follows is read with "
                      << selected.smartModel.name << "'s addresses." << std::endl;
        }

        if (!readOnly && modelMismatch)
        {
            std::cout << "\n[!] WARNING: the connected printer reports \"" << detectedMdl << "\""
                      << " (database entry: " << detectedMatch << ")," << std::endl;
            std::cout << "    but you selected " << selected.smartModel.name << "." << std::endl;
            std::cout << "    Writing another model's reset values into the EEPROM can misconfigure the printer." << std::endl;

            // A wrong-model write is the one hazard --yes must never produce, so
            // here it stops the run instead of standing in for the answer.
            // --force-yes is the explicit opt-out, and says so in the trace.
            if (cli.forceYes)
            {
                std::cout << "\n[!] --force-yes: proceeding with " << selected.smartModel.name
                          << " against a printer that reports \"" << detectedMdl << "\"." << std::endl;
            }
            else if (cli.assumeYes)
            {
                std::cerr << "\n[!] --yes confirms a reset, it does not overrule the detected model."
                          << std::endl;
                std::cerr << "    Drop --model to use " << detectedMatch
                          << ", re-run without --yes to confirm the mismatch yourself, or pass"
                          << std::endl;
                std::cerr << "    --force-yes if you really mean to write " << selected.smartModel.name
                          << " values to it." << std::endl;
                JsonFail("model_mismatch", "--model " + selected.smartModel.name
                                           + " does not match the detected " + detectedMatch + ".");
                return FinishRun(1);
            }
            else if (cli.json)
            {
                std::cerr << "\n[!] --model names something other than the printer that answered."
                          << " Pass --force-yes to overrule it." << std::endl;
                JsonFail("model_mismatch", "--model " + selected.smartModel.name
                                           + " does not match the detected " + detectedMatch + ".");
                return FinishRun(1);
            }
            else
            {
                std::cout << "\nContinue with " << selected.smartModel.name << " anyway? [y/N]: ";

                std::string answer;
                std::getline(std::cin, answer);
                const std::string a = toLower(answer);
                if (a != "y" && a != "yes")
                {
                    std::cout << "[i] Aborted before any EEPROM write. Re-run and press Enter to use the" << std::endl;
                    std::cout << "    detected model." << std::endl;
                    JsonFail("blocked", "The model mismatch was declined; nothing was written.");
                    return FinishRun(1);
                }
            }

            mismatchPassed = detectedMatch;
        }

        // Fail before any device I/O: the session would only reach the same
        // conclusion after opening a channel to the printer.
        if (cli.cartridge)
        {
            if (selected.isReplay)
            {
                std::cerr << "\n[!] --cartridge needs a Smart Protocol model: it writes the per-color ink"
                          << std::endl;
                std::cerr << "    addresses from the database, and a Replay dump carries none." << std::endl;
                JsonFail("not_supported", "--cartridge needs a Smart Protocol model; a Replay dump carries no ink map.");
                return FinishRun(1);
            }

            if (!selected.smartModel.HasInkReset())
            {
                std::cerr << "\n[!] --cartridge: " << selected.smartModel.name
                          << " has no cartridge ink map in the database," << std::endl;
                std::cerr << "    so there are no ink addresses to write. Its waste ink pad reset is"
                          << std::endl;
                std::cerr << "    unaffected - run without --cartridge for that." << std::endl;
                JsonFail("not_supported", selected.smartModel.name + " has no cartridge ink map in the database.");
                return FinishRun(1);
            }
        }

        // One object for every device session this run, so the --interface pin
        // and the soft-reset switch reach queries, writes and replays alike. The
        // gateway spends the soft reset on the first of them.
        ewr::ExecutorOptions sessionOptions = ewr::ExecutorOptions::ForQueries();
        sessionOptions.interfaceCandidate = cli.interfaceCandidate;
        sessionOptions.usbSoftReset = cli.usbSoftReset;
        sessionOptions.trace.printerReports = detectedMdl;
        sessionOptions.trace.detectedEntry = detectedMatch;
        sessionOptions.trace.selectedEntry = selected.isReplay ? selected.replayModel.name : selected.smartModel.name;
        sessionOptions.trace.selectedBy = selected.isReplay ? selectedBy + ", replay dump" : selectedBy;

        RunContext ctx{ cli, gateway, netGateway != nullptr, selected, detectedMatch, devIdQuery.deviceId,
                        sessionOptions, mismatchPassed };

        // ---- Printer info and maintenance: no EEPROM write in either. --------
        // A Replay entry has no keys, so the printer is asked as an unlisted one.
        if (cli.info || cli.nozzleCheck || cli.headClean || cli.powerClean)
        {
            const ewr::DbPrinterModel* model = selected.isReplay ? nullptr : &selected.smartModel;
            if (cli.info)
                return RunInfo(model, gateway, sessionOptions);
            return RunMaintenance(model, gateway, sessionOptions, cli, mismatchPassed);
        }

        if (statusOnly)
            return RunStatus(ctx);

        if (cli.dump)
            return RunDump(ctx);

        if (cli.findAddresses)
            return RunFindAddresses(ctx);

        // ---- Restore: a backup or a dump written back; --dry-run previews it. ----
        if (!cli.restoreFile.empty())
        {
            if (selected.isReplay)
            {
                std::cerr << "[!] --restore needs a Smart Protocol model: it reads and writes with the"
                             " database's keys, and a Replay dump carries none." << std::endl;
                JsonFail("not_supported", "--restore needs a Smart Protocol model; a Replay dump carries no keys.");
                return FinishRun(1);
            }

            // A restore writes a model's own bytes back, so the entry the file
            // names is the only one it runs on.
            if (selected.smartModel.name != restoreSource.model)
            {
                std::cerr << "\n[!] The file was taken from " << restoreSource.model << ", and this run uses "
                          << selected.smartModel.name << "." << std::endl;
                std::cerr << "    A restore puts a model's own bytes back, so it runs only on the entry the"
                             " file names." << std::endl;
                std::cerr << "    Drop --model to use " << restoreSource.model << "." << std::endl;
                JsonFail("bad_backup", "The file was taken from " + restoreSource.model + ", not "
                                       + selected.smartModel.name + ".");
                return FinishRun(1);
            }

            return RunRestore(selected.smartModel, restoreSource, cli.restoreFile, gateway, sessionOptions, cli,
                              devIdQuery.deviceId, mismatchPassed);
        }

        if (cli.dryRun)
            return RunDryRun(ctx);

        if (selected.isReplay)
            return RunReplay(ctx);

        return RunReset(ctx);
    }

} // namespace ewr::cli

int main(int argc, char* argv[])
{
    return ewr::cli::Run(argc, argv);
}
