#include "cli.h"

#include <iostream>
#include <sstream>
#include <streambuf>

namespace ewr::cli {

    void PrintUsage()
    {
        std::cout << "EWR - Epson Waste Reset " << kEwrCurrentVersion << "\n\n"
                  << "Usage: ewr [options]\n\n"
                  << "Running without options is the recommended path: EWR finds the printer,\n"
                  << "shows its status and counters, asks once, resets, and verifies.\n\n"
                  << "Options:\n"
                  << "  --status, -s     Read-only: printer status, ink levels and waste\n"
                  << "                   counter values. No EEPROM writes are sent.\n"
                  << "  --list, -l       List every Epson USB interface with its IEEE 1284\n"
                  << "                   device ID and database match, then exit. Read-only.\n"
                  << "  --model <name>   Skip the menu and use this database model. Accepts\n"
                  << "                   the exact name, an alias, or the name the printer\n"
                  << "                   reports (e.g. --model \"EPSON ET-2803 Series\").\n"
                  << "  --interface <n>  Pin the whole run to interface <n> from --list and\n"
                  << "                   disable the automatic fallback. For composite\n"
                  << "                   devices where detection picks the wrong interface.\n"
                  << "  --ip <address>   Reach a printer on the network instead of USB, over SNMP\n"
                  << "                   (UDP 161). Works with --status, --dry-run, the waste ink\n"
                  << "                   pad reset, --restore, --info and maintenance, with the\n"
                  << "                   same gates and read-back as USB.\n"
                  << "  --ip auto        Search the network and pick the printer from a list,\n"
                  << "                   even when another one is on USB. Asks at the keyboard,\n"
                  << "                   so not with --yes or --json: pass the address from\n"
                  << "                   --list instead.\n"
                  << "  --dry-run        Detect, read status and counters, and show exactly\n"
                  << "                   what a reset would write - then stop. No writes.\n"
                  << "  --dump           Detect, select the model, then read the EEPROM into a\n"
                  << "                   timestamped file next to ewr. Read-only. Dump twice\n"
                  << "                   around a change and diff the files to map a printer\n"
                  << "                   the ink database does not know yet. --full reads every\n"
                  << "                   byte up to the model's mem_high instead of the low page.\n"
                  << "  --info           Read-only: firmware version, serial number, the printer's\n"
                  << "                   own counters (pages, cleanings, ink changes) and the codes\n"
                  << "                   of its last fatal errors, where the database knows them.\n"
                  << "  --nozzle-check   Print the nozzle check pattern (load plain paper). 'alt'\n"
                  << "                   asks for the alternate pattern some models print.\n"
                  << "  --head-clean [level]  A head cleaning: gentle, normal (the default),\n"
                  << "                   medium or strong, as far as the model's entry lists them.\n"
                  << "                   Uses ink and adds to the waste ink pad counter.\n"
                  << "  --power-clean    The ink charge: a much stronger cleaning that uses a lot\n"
                  << "                   of ink. Only when normal cleanings did not help.\n"
                  << "  --clean-group <all|black|color>  Which nozzles a cleaning is for.\n"
                  << "  --print-data     Send a cleaning as print data (ESC/P2 remote mode) even\n"
                  << "                   where the model has its own maintenance command.\n"
                  << "  --restore <file> Write back what a backup (from backups/) or a --dump file\n"
                  << "                   holds of this model's counter bytes - only those, and only\n"
                  << "                   the ones that differ. Same gates, confirmation and\n"
                  << "                   read-back as a reset; --dry-run shows the plan.\n"
                  << "  --yes, -y        Answer the reset confirmation with yes, for callers that\n"
                  << "                   drive EWR non-interactively. It confirms the waste ink pad\n"
                  << "                   reset and nothing else: on a model that also offers the\n"
                  << "                   cartridge ink reset it picks the waste pads, and it still\n"
                  << "                   stops on a printer error, a database conflict or a model\n"
                  << "                   mismatch instead of writing. Without --model it uses the\n"
                  << "                   model the printer reports, and stops if there is none.\n"
                  << "  --force-yes      --yes, and overrule the two gates that would stop it: a\n"
                  << "                   model mismatch and a printer error or database conflict.\n"
                  << "                   This writes another model's values, or writes while the\n"
                  << "                   printer objects. Last resort, not an automation default.\n"
                  << "  --cartridge, -c  Reset the cartridge ink levels instead of the waste ink\n"
                  << "                   pad counters, on models that carry a per-color ink map.\n"
                  << "                   Chooses the target only - it confirms nothing, and it\n"
                  << "                   cannot refill ink: an empty cartridge will report full.\n"
                  << "  --no-update      Fully offline run: no update check, no download,\n"
                  << "                   database.json left as it is. For testing local edits\n"
                  << "                   (custom addresses, new models) before a pull request.\n"
                  << "  --usb-soft-reset Reset the USB channel once, before the run's first\n"
                  << "                   session, then wait (up to 90 s) for the printer to\n"
                  << "                   finish the re-initialization it starts (Windows only).\n"
                  << "                   Diagnostic switch for hardware testing.\n"
                  << "  --find-addresses Find the waste counters on a model the database has no\n"
                  << "                   addresses for. Reads the EEPROM three times with a head\n"
                  << "                   cleaning between reads - EWR runs them, or you do from the\n"
                  << "                   panel - and reports the bytes that rise every time as the\n"
                  << "                   counters. No EEPROM byte is written.\n"
                  << "  --find-key       For a printer the database has no entry for: try every\n"
                  << "                   read key in the database and report which one, if any,\n"
                  << "                   the printer answers. Takes no --model. Read-only.\n"
                  << "  --json           Machine-readable output: one JSON object per line on\n"
                  << "                   stdout, for callers driving EWR from another language.\n"
                  << "                   Never prompts; see docs/json-output.md for the contract.\n"
                  << "                   --json-version <n> pins the contract version (1).\n"
                  << "  --help, -h       Show this help.\n";
    }

    // The whole command line, after the --json pre-scan. An exit code when the
    // run ends here (--help, a usage error), nothing when it goes on.
    std::optional<int> ParseCommandLine(int argc, char* argv[], CliOptions& cli)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--status" || arg == "-s")
            {
                cli.statusOnly = true;
            }
            else if (arg == "--list" || arg == "-l")
            {
                cli.listOnly = true;
            }
            else if (arg == "--dry-run")
            {
                cli.dryRun = true;
            }
            else if (arg == "--dump")
            {
                cli.dump = true;
            }
            else if (arg == "--full")
            {
                cli.dumpFull = true;
            }
            else if (arg == "--info")
            {
                cli.info = true;
            }
            else if (arg == "--nozzle-check")
            {
                cli.nozzleCheck = true;
                if (i + 1 < argc && std::string(argv[i + 1]) == "alt")
                {
                    cli.nozzleVariant = 1;
                    ++i;
                }
            }
            else if (arg == "--head-clean")
            {
                cli.headClean = true;
                if (i + 1 < argc)
                {
                    const std::string next = toLower(argv[i + 1]);
                    if (next == "gentle" || next == "normal" || next == "medium" || next == "strong")
                    {
                        cli.cleanLevel = next;
                        ++i;
                    }
                }
            }
            else if (arg == "--power-clean")
            {
                cli.powerClean = true;
            }
            else if (arg == "--print-data")
            {
                cli.remoteOnly = true;
            }
            else if (arg == "--clean-group")
            {
                if (i + 1 >= argc)
                    return UsageError(cli, "--clean-group needs all, black or color.");
                cli.cleanGroup = toLower(argv[++i]);
                if (cli.cleanGroup != "all" && cli.cleanGroup != "black" && cli.cleanGroup != "color")
                    return UsageError(cli, "--clean-group takes all, black or color.");
            }
            else if (arg == "--find-addresses")
            {
                cli.findAddresses = true;
            }
            else if (arg == "--find-key")
            {
                cli.findKey = true;
            }
            else if (arg == "--no-update")
            {
                cli.noUpdate = true;
            }
            else if (arg == "--yes" || arg == "-y")
            {
                cli.assumeYes = true;
            }
            else if (arg == "--force-yes")
            {
                // Forcing without confirming is meaningless: it answers the same
                // prompt --yes does, then overrules the gates that would stop it.
                cli.forceYes = true;
                cli.assumeYes = true;
            }
            else if (arg == "--cartridge" || arg == "-c")
            {
                cli.cartridge = true;
            }
            else if (arg == "--usb-soft-reset")
            {
                cli.usbSoftReset = true;
            }
            else if (arg == "--json")
            {
                cli.json = true;
            }
            else if (arg == "--json-version")
            {
                // Named now so a caller can pin the contract from the first
                // release that has one: a future break raises the version and
                // keeps this flag answering for the old shape.
                if (i + 1 >= argc)
                {
                    return UsageError(cli, "--json-version needs a number (the contract version, 1).");
                }

                const std::string value = argv[++i];
                if (value != "1")
                {
                    return UsageError(cli, "--json-version " + value + " is not supported by this build (it speaks 1).");
                }

                cli.json = true;
            }
            else if (arg == "--model" || arg == "--interface" || arg == "--ip" || arg == "--restore"
                     || arg == "--restore-all")
            {
                if (i + 1 >= argc)
                {
                    return UsageError(cli, arg + " needs a value (see --help).");
                }

                const std::string value = argv[++i];
                if (arg == "--model")
                {
                    cli.modelOverride = value;
                }
                else if (arg == "--restore")
                {
                    if (value.empty())
                        return UsageError(cli, "--restore needs a file: a backup from backups/ or an --dump file.");
                    cli.restoreFile = value;
                }
                else if (arg == "--restore-all")
                {
#ifdef EWR_RESTORE_ALL
                    if (value.empty())
                        return UsageError(cli, "--restore-all needs a --dump file of this printer.");
                    cli.restoreFile = value;
                    cli.restoreAll = true;
#else
                    return UsageError(cli, "--restore-all is not enabled in this build: it writes bytes no reset"
                                           " writes, and waits for hardware verification on more models.");
#endif
                }
                else if (arg == "--ip")
                {
                    // Empty is how the rest of main() reads "no --ip": an unset
                    // variable in `--ip "$PRINTER"` would reset a USB printer.
                    if (value.empty())
                    {
                        return UsageError(cli, "--ip needs an address, e.g. --ip 192.168.1.100.");
                    }

                    if (toLower(value) == "auto")
                        cli.ipAuto = true;
                    else
                        cli.ip = value;
                }
                else
                {
                    try { cli.interfaceCandidate = std::stoi(value); }
                    catch (...) { cli.interfaceCandidate = 0; }

                    if (cli.interfaceCandidate < 1)
                    {
                        return UsageError(cli, "--interface needs a number from --list (1, 2, ...).");
                    }
                }
            }
            else if (arg == "--help" || arg == "-h")
            {
                if (cli.json)
                {
                    // The usage text is written for a terminal, so it travels as
                    // one string rather than as silenced stdout.
                    std::ostringstream usage;
                    std::streambuf* const console = std::cout.rdbuf(usage.rdbuf());
                    PrintUsage();
                    std::cout.rdbuf(console);

                    g_jsonCommand = "help";
                    JsonHello(cli);
                    g_jsonData["usage"] = usage.str();
                    return FinishRun(0);
                }

                PrintUsage();
                return 0;
            }
            else
            {
                return UsageError(cli, "Unknown option: " + arg + " (see --help).");
            }
        }

        {
            const int modes = (cli.info ? 1 : 0) + (cli.nozzleCheck ? 1 : 0) + (cli.headClean ? 1 : 0)
                            + (cli.powerClean ? 1 : 0) + (!cli.restoreFile.empty() ? 1 : 0) + (cli.dump ? 1 : 0)
                            + (cli.statusOnly ? 1 : 0) + (cli.findAddresses ? 1 : 0) + (cli.findKey ? 1 : 0)
                            + (cli.listOnly ? 1 : 0);
            if (modes > 1)
                return UsageError(cli, "Pick one of --status, --list, --dump, --find-addresses, --find-key, --restore,"
                                       " --info, --nozzle-check, --head-clean and --power-clean per run.");

            const bool maintenance = cli.nozzleCheck || cli.headClean || cli.powerClean;
            if (maintenance && cli.cartridge)
                return UsageError(cli, "--cartridge chooses what a reset writes; maintenance writes nothing.");
            if (!cli.cleanGroup.empty() && !(cli.headClean || cli.powerClean))
                return UsageError(cli, "--clean-group goes with --head-clean or --power-clean.");
            if (cli.remoteOnly && !maintenance)
                return UsageError(cli, "--print-data goes with --nozzle-check, --head-clean or --power-clean.");
            if (cli.info && cli.dryRun)
                return UsageError(cli, "--info only reads; it has no dry run.");
            // It would run its head cleanings all the same.
            if (cli.findAddresses && cli.dryRun)
                return UsageError(cli, "--find-addresses has no dry run: it reads the EEPROM and runs two head"
                                       " cleanings, which it asks about first.");
        }

        if (cli.restoreAll && (cli.json || cli.assumeYes))
            return UsageError(cli, "--restore-all is answered by typing restore-all at the keyboard; it does not"
                                   " combine with --json, --yes or --force-yes.");

        if (cli.dumpFull && !cli.dump && !cli.findAddresses)
            return UsageError(cli, "--full belongs to --dump or --find-addresses: it reads every byte up to mem_high.");

        if (!cli.restoreFile.empty()
            && (cli.listOnly || cli.findKey || cli.statusOnly || cli.dump || cli.findAddresses || cli.cartridge))
            return UsageError(cli, "--restore writes back what its file holds; it does not combine with --list,"
                                   " --find-key, --status, --dump, --find-addresses or --cartridge. --dry-run shows"
                                   " what it would write.");

        // The search is for a printer whose model is unknown; naming one would
        // only suggest that entry's key gets special treatment.
        if (cli.findKey && !cli.modelOverride.empty())
            return UsageError(cli, "--find-key takes no --model: it tries every read key in the database.");

        // Over the network: everything but the USB-only modes below.
        if (!cli.ip.empty() || cli.ipAuto)
        {
            if (cli.listOnly || cli.findKey || cli.dump || cli.findAddresses || cli.cartridge)
                return UsageError(cli, "--ip works with --status, --dry-run, the waste ink pad reset, --restore, --info"
                                       " and maintenance; --list, --find-key, --dump, --find-addresses and --cartridge"
                                       " need USB.");
            if (cli.interfaceCandidate >= 1 || cli.usbSoftReset)
                return UsageError(cli, "--interface and --usb-soft-reset are USB switches and do not combine with --ip.");
        }

        // --yes answers "reset this printer?", never "which printer?": picking
        // for an unattended run could reset someone else's Epson, even when only
        // one answers - the one meant may be off.
        if (cli.ipAuto && (cli.assumeYes || cli.forceYes || cli.json))
            return UsageError(cli, "--ip auto asks at the keyboard which printer to use, so it does not combine with"
                                   " --yes, --force-yes or --json. Find the address with ewr --list (--json shows it"
                                   " as network_printers) and pass it: --ip <address>.");

        return std::nullopt;
    }

} // namespace ewr::cli
