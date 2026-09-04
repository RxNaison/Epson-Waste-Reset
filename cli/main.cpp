#include <iostream>
#include <algorithm>
#include <string>
#include <cctype>
#include <filesystem>
#include "ewr/payload.h"
#include "ewr/parser.h"
#include "ewr/session.h"
#include "ewr/usb.h"
#include "ewr/deviceid.h"
#include "ewr/generator.h"
#include "ewr/status.h"
#include "ewr/updater.h"
#include "ewr/version.h"
#include "ewr/log.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

struct MenuOption
{
    std::string displayName;
    bool isReplay;
    ewr::PrinterModel replayModel;
    ewr::DbPrinterModel smartModel;
};

std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

struct CliOptions
{
    bool statusOnly = false;     // --status: read-only status + counters
    bool listOnly = false;       // --list: interface survey, then exit
    bool dryRun = false;         // --dry-run: everything except the writes
    bool dump = false;           // --dump: read-only EEPROM dump to a file
    bool noUpdate = false;       // --no-update: offline run, nothing checked or swapped
    int interfaceCandidate = 0;  // --interface <n>: 1-based pin, 0 = auto
    bool usbSoftReset = false;   // --usb-soft-reset: clear the channel on every open
    std::string modelOverride;   // --model <name>: skip the menu
};

static void SetWorkingDirectoryToExecutable()
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        std::error_code ec;
        fs::current_path(fs::path(path).parent_path(), ec);
    }
#elif defined(__APPLE__)
    // macOS has no procfs, so /proc/self/exe silently fails there and the run
    // keeps the shell's CWD - which puts database.json and models/ wherever
    // the user happened to be standing.
    char path[PATH_MAX];
    uint32_t size = static_cast<uint32_t>(sizeof(path));
    if (_NSGetExecutablePath(path, &size) == 0)
    {
        std::error_code ec;
        // The path may be a symlink or contain '..'; canonical() resolves both,
        // and the raw path is still a usable fallback if it cannot.
        const fs::path exe = fs::canonical(fs::path(path), ec);
        fs::current_path((ec ? fs::path(path) : exe).parent_path(), ec);
    }
#else
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0)
    {
        path[len] = '\0';
        std::error_code ec;
        fs::current_path(fs::path(path).parent_path(), ec);
    }
#endif
}

// Never hardcode the version here: the updater compares this string against
// release tags, and it comes from project(EWR VERSION ...) via version.h.
const std::string kEwrCurrentVersion = EWR_VERSION;

const std::string kReleasesPageUrl = "https://github.com/RxNaison/Epson-Waste-Reset/releases";

static void PrintUsage()
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
              << "                   the exact name, an alias, or a unique part of one\n"
              << "                   (e.g. --model ET-2803).\n"
              << "  --interface <n>  Pin the whole run to interface <n> from --list and\n"
              << "                   disable the automatic fallback. For composite\n"
              << "                   devices where detection picks the wrong interface.\n"
              << "  --dry-run        Detect, read status and counters, and show exactly\n"
              << "                   what a reset would write - then stop. No writes.\n"
              << "  --dump           Detect, select the model, then read the EEPROM into a\n"
              << "                   timestamped file next to ewr. Read-only. Dump twice\n"
              << "                   around a change and diff the files to map a printer\n"
              << "                   the ink database does not know yet.\n"
              << "  --no-update      Fully offline run: no update check, no download, no\n"
              << "                   staged swap on exit. For testing local database edits\n"
              << "                   (custom addresses, new models) before a pull request.\n"
              << "  --usb-soft-reset Clear the USB channel on every session open (Windows\n"
              << "                   only). Off by default: on ET-2xxx units it stalls the\n"
              << "                   next write. Diagnostic switch for hardware testing.\n"
              << "  --help, -h       Show this help.\n";
}

static std::string GaugeBar(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    const int filled = (percent + 9) / 10; // any non-zero percent shows a cell

    std::string bar = "[";
    for (int i = 0; i < 10; ++i)
        bar += (i < filled) ? '#' : '-';
    bar += "]";
    return bar;
}

static void PrintPrinterStatus(const ewr::PrinterStatus& st)
{
    if (!st.valid)
    {
        std::cout << "[i] The printer answered, but the status report could not be parsed." << std::endl;
        return;
    }

    std::cout << "\n----------- PRINTER STATUS -----------" << std::endl;
    std::cout << "  State:  " << ewr::DescribePrinterCondition(st) << std::endl;

    if (!st.serial.empty())
        std::cout << "  Serial: " << st.serial << std::endl;

    if (!st.inks.empty())
    {
        std::cout << "  Ink levels:" << std::endl;
        for (const auto& ink : st.inks)
        {
            char line[96];
            if (ink.level >= 0)
                snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s",
                         ink.colorName.c_str(), GaugeBar(ink.level).c_str(),
                         ink.level, ink.statusText.c_str());
            else
                snprintf(line, sizeof(line), "    %-14.14s %s",
                         ink.colorName.c_str(), ink.statusText.c_str());
            std::cout << line << std::endl;
        }
    }

    if (st.maintenanceBoxLevel >= 0 || !st.maintenanceBoxText.empty())
    {
        char line[96];
        if (st.maintenanceBoxLevel >= 0)
            snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s", "Maint. box",
                     GaugeBar(st.maintenanceBoxLevel).c_str(),
                     st.maintenanceBoxLevel, st.maintenanceBoxText.c_str());
        else
            snprintf(line, sizeof(line), "    %-14.14s %s", "Maint. box",
                     st.maintenanceBoxText.c_str());
        std::cout << line << std::endl;
    }

    std::cout << "--------------------------------------" << std::endl;
}

static void PrintCounterValues(const std::vector<std::pair<uint16_t, int>>& values, const char* label)
{
    if (values.empty())
        return;

    std::cout << "  " << label << std::endl;
    for (const auto& entry : values)
    {
        char line[64];
        if (entry.second >= 0)
            snprintf(line, sizeof(line), "    EEPROM 0x%04X = 0x%02X (%d)", entry.first, entry.second, entry.second);
        else
            snprintf(line, sizeof(line), "    EEPROM 0x%04X = (no reply)", entry.first);
        std::cout << line << std::endl;
    }
}

// Raw EEPROM bytes to "how full is this pad", via the per-counter masks,
// weights and service limits carried by the database.
static void PrintCounterSummary(const ewr::DbPrinterModel& model,
                                const std::vector<std::pair<uint16_t, int>>& values)
{
    const auto specs = model.GetAllCounters();
    if (specs.empty() || values.empty())
        return;

    bool printedHeader = false;
    for (const auto& spec : specs)
    {
        const ewr::CounterReading reading = ewr::EvaluateCounter(spec, values);
        if (!reading.complete)
            continue;

        if (!printedHeader)
        {
            std::cout << "  Waste pad usage:" << std::endl;
            printedHeader = true;
        }

        const std::string name = reading.description.empty() ? "Counter" : reading.description;
        const int percent = reading.Percent();

        char line[160];
        if (percent >= 0)
        {
            snprintf(line, sizeof(line), "    %-28.28s %s %3d%%  (%u / %u)%s",
                     name.c_str(), GaugeBar(percent).c_str(), percent,
                     static_cast<unsigned>(reading.value),
                     static_cast<unsigned>(reading.max_value),
                     percent >= 100 ? "  <-- service limit reached" : "");
        }
        else
        {
            snprintf(line, sizeof(line), "    %-28.28s value %u (no service limit on record)",
                     name.c_str(), static_cast<unsigned>(reading.value));
        }
        std::cout << line << std::endl;
    }
}

// Diagnostic runs are often piped, so they skip the interactive pause.
static bool g_exitPause = true;

// Every exit path funnels through here so a staged update always applies.
static int FinishRun(int exitCode)
{
    if (g_exitPause)
    {
        std::cout << "\nPress Enter to exit..." << std::endl;
        std::cin.get();
    }

    ewr::BackgroundUpdater::Instance().ApplyStagedUpdatesOnExit();

    return exitCode;
}

static int FinishReset(bool resetOk)
{
    if (resetOk)
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << " SUCCESS! Turn the printer OFF, then ON." << std::endl;
        std::cout << "========================================" << std::endl;
    }
    else
    {
        std::cerr << "\n========================================" << std::endl;
        std::cerr << " RESET FAILED. See messages above and" << std::endl;
        std::cerr << " ewr_trace.log for details." << std::endl;
        std::cerr << "========================================" << std::endl;
    }

    return FinishRun(resetOk ? 0 : 1);
}

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

// Diffing two dumps taken around a single change locates counters on
// printers the database has not mapped yet.
static std::string WriteEepromDump(const ewr::DbPrinterModel& model,
                                   const std::vector<std::pair<uint16_t, int>>& values)
{
    std::vector<std::pair<uint16_t, std::string>> notes;
    for (const auto& g : model.pad_groups)
    {
        const std::string label = g.description.empty() ? std::string("waste counter") : g.description;
        for (uint16_t a : g.addresses)
            notes.push_back({ a, label });
    }
    for (const auto& g : model.ink_groups)
    {
        for (uint16_t a : g.addresses)
            notes.push_back({ a, "cartridge ink: " + (g.color.empty() ? std::string("?") : g.color) });
    }

    auto noteFor = [&notes](uint16_t addr) -> std::string {
        for (const auto& n : notes)
        {
            if (n.first == addr)
                return n.second;
        }
        return "";
    };

    std::string safeName;
    for (char c : model.name)
        safeName += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';

    const std::string base = "ewr_dump_" + safeName + "_" +
        std::to_string(static_cast<long long>(std::time(nullptr)));

    // Epoch granularity: two dumps in the same second must not collide, since
    // diffing #1 against #2 is the entire point.
    std::string filename = base + ".txt";
    for (int n = 2; fs::exists(filename) && n < 100; ++n)
        filename = base + "_" + std::to_string(n) + ".txt";

    std::ofstream out(filename);
    if (!out)
        return "";

    out << "# EWR EEPROM dump\n";
    out << "# model: " << model.name << "\n";
    out << "# address,value,note ('--' = no reply)\n";
    for (const auto& v : values)
    {
        char line[24];
        if (v.second < 0)
            snprintf(line, sizeof(line), "0x%02X,--", v.first);
        else
            snprintf(line, sizeof(line), "0x%02X,0x%02X", v.first, v.second & 0xFF);

        out << line << "," << noteFor(v.first) << "\n";
    }

    return filename;
}

int main(int argc, char* argv[])
{
    SetWorkingDirectoryToExecutable();

    // The core never prints on its own. Warnings and errors go to stderr so
    // they survive a redirected stdout.
    ewr::log::Default().AddSink(ewr::log::ConsoleSink(std::cout, std::cerr));

    CliOptions cli;
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
        else if (arg == "--no-update")
        {
            cli.noUpdate = true;
        }
        else if (arg == "--usb-soft-reset")
        {
            cli.usbSoftReset = true;
        }
        else if (arg == "--model" || arg == "--interface")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "[!] " << arg << " needs a value (see --help)." << std::endl;
                return 2;
            }

            const std::string value = argv[++i];
            if (arg == "--model")
            {
                cli.modelOverride = value;
            }
            else
            {
                try { cli.interfaceCandidate = std::stoi(value); }
                catch (...) { cli.interfaceCandidate = 0; }

                if (cli.interfaceCandidate < 1)
                {
                    std::cerr << "[!] --interface needs a number from --list (1, 2, ...)." << std::endl;
                    return 2;
                }
            }
        }
        else if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::cerr << "[!] Unknown option: " << arg << " (see --help)." << std::endl;
            return 2;
        }
    }

    const bool statusOnly = cli.statusOnly;
    g_exitPause = !(cli.statusOnly || cli.listOnly || cli.dryRun || cli.dump);

    std::cout << "========================================" << std::endl;
    std::cout << "       EWR - Epson Waste Reset          " << std::endl;
    std::cout << "       Version " << kEwrCurrentVersion << std::endl;
    std::cout << "========================================\n" << std::endl;

    if (statusOnly)
        std::cout << "[i] READ-ONLY STATUS MODE: no EEPROM writes will be sent.\n" << std::endl;
    else if (cli.dryRun)
        std::cout << "[i] DRY RUN: EWR will detect, read and plan, but write nothing.\n" << std::endl;
    else if (cli.dump)
        std::cout << "[i] DUMP MODE: reading the EEPROM to a file, no writes will be sent.\n" << std::endl;

    ewr::CleanupStaleTempFiles();

    // No update checks, no D4 traffic, no menu - diagnostics only.
    if (cli.listOnly)
    {
        ewr::UniversalGenerator listGenerator;
        std::vector<ewr::ModelNameEntry> listEntries;
        if (listGenerator.LoadDatabase("database.json"))
        {
            for (const auto& m : listGenerator.GetAvailableModels())
                listEntries.push_back({ m.name, m.aliases });
        }

        std::cout << "[*] Scanning Epson USB interfaces (read-only)..." << std::endl;
        ewr::UsbDeviceGateway listGateway;
        const std::vector<ewr::InterfaceInfo> interfaces = listGateway.ListInterfaces();

        if (interfaces.empty())
        {
            std::cout << "\nNo Epson USB interfaces found. Is the printer on and plugged in?" << std::endl;
            return 1;
        }

        std::cout << "\nDetected Epson USB interfaces (in automatic fallback order):" << std::endl;
        for (const auto& iface : interfaces)
        {
            std::cout << "\n  [" << iface.index << "] " << iface.className;
            if (!iface.role.empty())
                std::cout << " [" << iface.role << "]";
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;
            std::cout << "\n      " << iface.path << std::endl;

            if (iface.deviceId.empty())
            {
                std::cout << "      IEEE 1284 device ID: (no reply)" << std::endl;
                continue;
            }

            const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(iface.deviceId);
            std::cout << "      IEEE 1284 device ID: " << (devId.model.empty() ? iface.deviceId : "MDL \"" + devId.model + "\"") << std::endl;

            if (!devId.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::MatchModelEntries(devId.model, listEntries);
                if (!matches.empty())
                    std::cout << "      Database entry:      " << matches[0] << std::endl;
            }
        }

        std::cout << "\nUse --interface <n> to pin a run to one specific interface." << std::endl;
        return 0;
    }

    // Only the first run blocks: without a database there is nothing to show,
    // and nothing loaded that a swap could race against.
    if (!cli.noUpdate && !fs::exists("database.json"))
    {
        std::cout << "[i] Downloading printer payload database... ";
        std::cout.flush();
        if (ewr::Updater::SyncDatabaseNow("database.json"))
            std::cout << "SUCCESS.\n" << std::endl;
        else
            std::cout << "FAILED.\n" << std::endl;
    }

    if (cli.noUpdate)
    {
        // Fully offline run: no release check, no download, no staged swap.
        std::cout << "[i] --no-update: offline run, database.json will not be modified." << std::endl;
    }
    else
    {
        // Releases ship as archives, so EWR announces them instead of self-installing.
        std::cout << "[i] Checking for updates... " << std::flush;
        const ewr::UpdateMetadata update = ewr::Updater::CheckLatestRelease(kEwrCurrentVersion);
        if (update.updateAvailable)
            std::cout << "version " << update.latestVersion << " is available!\n"
                      << "    Download it at " << kReleasesPageUrl << std::endl;
        else if (update.latestVersion.empty())
            std::cout << "could not reach GitHub." << std::endl;
        else
            std::cout << "you are up to date." << std::endl;

        // Staged, not applied: the swap happens on exit, so the database
        // loaded below stays stable for the whole run.
        ewr::BackgroundUpdater::Instance().StartAsync(ewr::kMaxSupportedDatabaseSchema);
    }

    ewr::UniversalGenerator generator;
    if (!generator.LoadDatabase("database.json"))
        std::cerr << "[!] Could not load database.json (missing or corrupted). Smart Protocol models are unavailable this run." << std::endl;

    auto replayModels = ewr::ScanModelsFolder("models");
    auto smartModels = generator.GetAvailableModels();

    if (replayModels.empty() && smartModels.empty())
    {
        std::cerr << "\n[!] No payloads found: database.json is missing or unreadable." << std::endl;
        std::cerr << "    It ships next to ewr - re-extract the archive, or run once with internet access to fetch it." << std::endl;
        return FinishRun(1);
    }

    std::vector<MenuOption> options;
    size_t hiddenModels = 0;

    for (const auto& sm : smartModels)
    {
        // A model earns a menu slot for either reset: waste pads or ink map.
        if (!sm.HasResettableCounters() && !sm.HasInkReset())
        {
            hiddenModels++;
            continue;
        }
        options.push_back({ sm.name + " (Smart Protocol - Recommended)", false, {}, sm });
    }

    std::cout << "[i] Loaded " << (smartModels.size() - hiddenModels) << " Smart Protocol payloads." << std::endl;

    if (hiddenModels > 0)
        std::cout << "[i] " << hiddenModels << " database entries have no USB-resettable counters and are hidden." << std::endl;

    std::cout << "[i] Loaded " << replayModels.size() << " Custom payloads." << std::endl;

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
    ewr::UsbDeviceGateway gateway;

    // Once per run: powers detection, the banner and --interface validation.
    std::string detectedMdl;
    std::string detectedMatch;

    std::cout << "\n[i] Detecting the connected printer... " << std::flush;
    const std::vector<ewr::InterfaceInfo> interfaces = gateway.ListInterfaces();

    if (cli.interfaceCandidate >= 1 && cli.interfaceCandidate > static_cast<int>(interfaces.size()))
    {
        std::cout << "found " << interfaces.size() << " interface(s)." << std::endl;
        std::cerr << "[!] --interface " << cli.interfaceCandidate << " does not exist: only "
                  << interfaces.size() << " Epson interface(s) are present. Run 'ewr --list' to see them." << std::endl;
        return FinishRun(1);
    }

    // The pinned interface's when --interface is set, else the first answer.
    ewr::DeviceIdQueryResult devIdQuery;
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

    if (devIdQuery.found)
    {
        const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(devIdQuery.deviceId);
        detectedMdl = devId.model;

        if (!detectedMdl.empty())
        {
            std::cout << "found \"" << detectedMdl << "\"." << std::endl;

            // The database owns the name table, so no model strings here.
            std::vector<ewr::ModelNameEntry> smartEntries;
            for (const auto& opt : options)
            {
                if (!opt.isReplay)
                    smartEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
            }

            const std::vector<std::string> matches = ewr::MatchModelEntries(detectedMdl, smartEntries);
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
    else
    {
        std::cout << "no answer (printer off, unplugged, or driver limitation)." << std::endl;
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
            if (!iface.role.empty())
                std::cout << " [" << iface.role << "]";
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;

            if (!iface.deviceId.empty())
            {
                const std::string bannerMdl = ewr::ParseIeee1284DeviceId(iface.deviceId).model;
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

    MenuOption selected;
    bool hasSelected = false;

    // Same matcher detection uses, so names, aliases and families all work.
    if (!cli.modelOverride.empty())
    {
        const std::string wantedLower = toLower(cli.modelOverride);

        // Exact equality is never ambiguous, even when the same text also
        // partial-matches other entries.
        std::string resolvedName;
        for (const auto& opt : options)
        {
            const std::string ownName = opt.isReplay ? opt.replayModel.name : opt.smartModel.name;
            if (toLower(ownName) == wantedLower)
            {
                resolvedName = ownName;
                break;
            }

            if (!opt.isReplay)
            {
                for (const auto& alias : opt.smartModel.aliases)
                {
                    if (toLower(alias) == wantedLower)
                    {
                        resolvedName = opt.smartModel.name;
                        break;
                    }
                }
            }

            if (!resolvedName.empty())
                break;
        }

        // Fall back to the fuzzy matcher: normalization, aliases, families.
        if (resolvedName.empty())
        {
            std::vector<ewr::ModelNameEntry> allEntries;
            for (const auto& opt : options)
            {
                if (opt.isReplay)
                    allEntries.push_back({ opt.replayModel.name, {} });
                else
                    allEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
            }

            const std::vector<std::string> matches = ewr::MatchModelEntries(cli.modelOverride, allEntries);

            std::vector<std::string> uniqueNames;
            for (const auto& match : matches)
            {
                bool seen = false;
                for (const auto& name : uniqueNames)
                    seen = seen || (name == match);
                if (!seen)
                    uniqueNames.push_back(match);
            }

            if (uniqueNames.empty())
            {
                std::cerr << "[!] --model \"" << cli.modelOverride << "\" matches no usable model." << std::endl;
                std::cerr << "    (Database models without resettable waste counters are not offered.)" << std::endl;
                std::cerr << "    Run without --model and use the menu search, or check 'ewr --list'." << std::endl;
                return FinishRun(1);
            }

            if (uniqueNames.size() > 1)
            {
                std::cerr << "[!] --model \"" << cli.modelOverride << "\" is ambiguous. Closest matches:" << std::endl;
                for (size_t i = 0; i < uniqueNames.size() && i < 6; ++i)
                    std::cerr << "      " << uniqueNames[i] << std::endl;
                std::cerr << "    Be more specific - the full name always works." << std::endl;
                return FinishRun(1);
            }

            resolvedName = uniqueNames[0];
        }

        // Same name on a Smart Protocol and a Replay entry: prefer Smart.
        for (const auto& opt : options)
        {
            if (!opt.isReplay && opt.smartModel.name == resolvedName)
            {
                selected = opt;
                hasSelected = true;
                break;
            }
        }

        if (!hasSelected)
        {
            for (const auto& opt : options)
            {
                if (opt.isReplay && opt.replayModel.name == resolvedName)
                {
                    selected = opt;
                    hasSelected = true;
                    break;
                }
            }
        }

        if (hasSelected)
            std::cout << "\n[i] --model: using " << selected.displayName << "." << std::endl;
    }

    while (!hasSelected)
    {
        if (!detectedMatch.empty())
            std::cout << "\nPress Enter to use the detected model [" << detectedMatch
                      << "], enter another model to search, or type 'exit' to quit: ";
        else
            std::cout << "\nEnter printer model to search (e.g., 'L3150' or 'XP') or type 'exit' to quit: ";

        std::string searchQuery;
        if (!std::getline(std::cin, searchQuery))
        {
            // No stdin to read (a pipe, a redirect, a service context). Every
            // other prompt here needs a typed word and so aborts on its own;
            // this one treats empty as "search again" and would spin forever.
            std::cerr << "\n[ERROR] No input available: EWR needs a model to work with and stdin\n"
                         "        is closed. Pass --model <name> to choose one non-interactively." << std::endl;
            return FinishRun(1);
        }

        if (searchQuery.empty())
        {
            if (detectedMatch.empty())
                continue;

            for (const auto& opt : options)
            {
                if (!opt.isReplay && opt.smartModel.name == detectedMatch)
                {
                    selected = opt;
                    hasSelected = true;
                    break;
                }
            }
            continue;
        }

        std::string searchLower = toLower(searchQuery);
        if (searchLower == "exit" || searchLower == "quit")
            return FinishRun(0);

        std::vector<MenuOption> filteredOptions;
        for (const auto& opt : options)
        {
            if (toLower(opt.displayName).find(searchLower) != std::string::npos)
                filteredOptions.push_back(opt);
        }

        if (filteredOptions.empty())
        {
            std::cout << "[-] No printers found matching '" << searchQuery << "'. Please try again.\n";
            continue;
        }

        std::cout << "\nFound " << filteredOptions.size() << " matching printers:\n";

        for (size_t i = 0; i < filteredOptions.size(); ++i)
            std::cout << "[" << i + 1 << "] " << filteredOptions[i].displayName << "\n";

        std::cout << "[0] Search again...\n";

        std::cout << "\nSelect your printer [0-" << filteredOptions.size() << "]: ";
        std::string choiceStr;
        std::getline(std::cin, choiceStr);

        try
        {
            int choice = std::stoi(choiceStr);
            if (choice == 0)
            {
                continue;
            }
            else if (choice >= 1 && choice <= static_cast<int>(filteredOptions.size()))
            {
                selected = filteredOptions[choice - 1];
                hasSelected = true;
            }
            else
            {
                std::cout << "[-] Invalid selection. Please try again.\n";
            }
        }
        catch (...)
        {
            std::cout << "[-] Invalid input. Please enter a number.\n";
        }
    }

    // The top cause of wrong-model writes, so it needs an explicit yes.
    // Read-only status mode is exempt.
    if (!statusOnly && !cli.dryRun && !cli.dump && !selected.isReplay
        && !detectedMatch.empty() && selected.smartModel.name != detectedMatch)
    {
        std::cout << "\n[!] WARNING: the connected printer reports \"" << detectedMdl << "\""
                  << " (database entry: " << detectedMatch << ")," << std::endl;
        std::cout << "    but you selected " << selected.smartModel.name << "." << std::endl;
        std::cout << "    Writing another model's reset values into the EEPROM can misconfigure the printer." << std::endl;
        std::cout << "\nContinue with " << selected.smartModel.name << " anyway? [y/N]: ";

        std::string answer;
        std::getline(std::cin, answer);
        const std::string a = toLower(answer);
        if (a != "y" && a != "yes")
        {
            std::cout << "[i] Aborted before any EEPROM write. Re-run and press Enter to use the" << std::endl;
            std::cout << "    detected model." << std::endl;
            return FinishRun(1);
        }
    }

    // One object for every device session this run, so the --interface pin
    // and the soft-reset switch reach queries, writes and replays alike.
    ewr::ExecutorOptions sessionOptions = ewr::DefaultQueryOptions();
    sessionOptions.interfaceCandidate = cli.interfaceCandidate;
    sessionOptions.usbSoftResetOnOpen = cli.usbSoftReset;

    if (statusOnly)
    {
        std::cout << "\n[*] Querying printer status (read-only, no EEPROM writes)..." << std::endl;

        ewr::StateSnapshot state;
        if (selected.isReplay)
        {
            // No read key in a dump: the '@BDC ST2' status is all we can ask.
            state = ewr::ReadPrinterStatus(gateway, sessionOptions);
        }
        else
        {
            ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
            state = session.ReadState();
        }

        if (!state.available)
        {
            std::cerr << "[ERROR] Could not read the printer status. Is it turned on and plugged in?" << std::endl;
            std::cerr << "        Check ewr_trace.log for the hardware trace." << std::endl;
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);
        PrintCounterValues(state.values, "Waste counter EEPROM values:");

        if (!selected.isReplay)
            PrintCounterSummary(selected.smartModel, state.values);

        if (selected.isReplay)
            std::cout << "[i] Counter values are not available for Replay models (no read key in the dump)." << std::endl;

        return FinishRun(0);
    }

    // ---- Dump: read a range of the EEPROM to a file, then stop. -----------
    // Needs a Smart Protocol model: the read key lives in the database.
    if (cli.dump)
    {
        if (selected.isReplay)
        {
            std::cerr << "[!] --dump needs a Smart Protocol model: it reads the EEPROM with the\n"
                         "    database read key, and Replay dumps carry none." << std::endl;
            return FinishRun(1);
        }

        std::cout << "\n[*] DUMP for " << selected.smartModel.name
                  << ": reading the EEPROM (read-only, no writes)..." << std::endl;

        // The whole addressable space on 1-byte models, and where the counters
        // live on the classic six-color printers.
        const uint32_t dumpEnd = std::min<uint32_t>(selected.smartModel.mem_high, 0xFF);
        std::vector<uint16_t> addresses;
        for (uint32_t a = 0; a <= dumpEnd; ++a)
            addresses.push_back(static_cast<uint16_t>(a));

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadAddresses(addresses);

        if (!state.available)
        {
            std::cerr << "[ERROR] Could not read the printer. Is it powered on and connected?" << std::endl;
            std::cerr << "        See ewr_trace.log for the hardware trace." << std::endl;
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);

        size_t answered = 0;
        for (const auto& v : state.values)
            answered += (v.second >= 0) ? 1 : 0;

        const std::string path = WriteEepromDump(selected.smartModel, state.values);
        if (path.empty())
        {
            std::cerr << "[ERROR] Read the EEPROM, but could not write the dump file." << std::endl;
            return FinishRun(1);
        }

        std::cout << "\n[SUCCESS] Wrote " << answered << " of " << state.values.size()
                  << " EEPROM byte(s) to " << path << "." << std::endl;
        std::cout << "    To map a color: dump once, print that color (or swap its cartridge),\n"
                     "    dump again, then diff the two files. The bytes that changed are that\n"
                     "    color's ink counter.\n"
                     "    Mind the mirror trap: a byte that comes back by itself after a power\n"
                     "    cycle is rewritten by the firmware from the cartridge chip - that level\n"
                     "    lives on the chip and cannot be reset from the PC." << std::endl;
        return FinishRun(0);
    }

    // ---- Dry run: everything except the writes. ---------------------------
    if (cli.dryRun)
    {
        if (selected.isReplay)
        {
            // Opaque bytes: count what it would send, send nothing.
            const std::vector<std::vector<unsigned char>> dumpSequence =
                ewr::ParseWiresharkDump(selected.replayModel.filepath);

            size_t dumpWrites = 0;
            for (const auto& packet : dumpSequence)
            {
                if (ewr::IsWritePacket(packet))
                    dumpWrites++;
            }

            std::cout << "\n[DRY RUN] " << selected.displayName << ": the dump holds "
                      << dumpSequence.size() << " packets, " << dumpWrites << " of them EEPROM writes." << std::endl;
            std::cout << "[DRY RUN] Nothing was sent to the printer." << std::endl;
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
            std::cout << "[i] The printer did not answer the read-only query - showing the plan anyway." << std::endl;
        }

        const std::vector<uint16_t> planAddresses = selected.smartModel.GetAllAddresses();
        const std::vector<uint8_t> planValues = selected.smartModel.GetAllResetValues();

        std::cout << "\nA real run would write " << planAddresses.size() << " EEPROM byte(s):" << std::endl;
        for (size_t i = 0; i < planAddresses.size(); ++i)
        {
            char line[64];
            snprintf(line, sizeof(line), "    EEPROM 0x%04X <- 0x%02X", planAddresses[i],
                     i < planValues.size() ? planValues[i] : 0);
            std::cout << line << std::endl;
        }

        for (const auto& op : selected.smartModel.close_ops)
        {
            char line[96];
            snprintf(line, sizeof(line), "    commit: read 0x%04X, apply AND 0x%02X / OR 0x%02X, write back",
                     op.address, op.and_mask, op.or_mask);
            std::cout << line << std::endl;
        }

        if (!selected.smartModel.wkey1.empty())
            std::cout << "    (an alternate write keyword is available if the primary is rejected)" << std::endl;

        std::cout << "\n[DRY RUN] Nothing was written. Run without --dry-run to perform the reset." << std::endl;
        return FinishRun(0);
    }

    // ---- Replay path: send a Wireshark dump byte-for-byte. ----------------
    // The dump's shape is unknown, so the session safeguards do not apply.
    if (selected.isReplay)
    {
        std::cout << "\n[!] Parsing replay Wireshark dump for " << selected.displayName << "..." << std::endl;
        const std::vector<std::vector<unsigned char>> executionSequence =
            ewr::ParseWiresharkDump(selected.replayModel.filepath);

        if (executionSequence.empty())
        {
            std::cerr << "[-] Failed to construct payload. Exiting.\n";
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
        replayOptions.usbSoftResetOnOpen = cli.usbSoftReset;

        const ewr::ResetRunResult run = gateway.RunReset(executionSequence, replayOptions);

        if (!run.deviceFound)
        {
            std::cerr << "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?" << std::endl;
            return FinishRun(1);
        }

        return FinishReset(run.exec.success);
    }

    // ---- Smart Protocol path: ewr::Session owns the whole lifecycle. ------
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

    // ---- Cartridge ink reset choice: models with a per-color ink map offer
    // it; everything else goes straight to the classic waste-pad reset.
    bool resetInk = false;
    if (selected.smartModel.HasInkReset() && !selected.smartModel.HasResettableCounters())
    {
        // Ink map but no waste-pad addresses: the ink reset is the only path.
        std::cout << "\n" << selected.smartModel.name
                  << " has a cartridge ink map but no USB-resettable waste pad counters," << std::endl;
        std::cout << "    so the cartridge ink level reset is the available path." << std::endl;
        resetInk = true;
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
            std::getline(std::cin, choice);

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

    // Arms only on a typed word; Enter alone must never write. On chipped
    // cartridges the EEPROM bytes mirror the chip, so the reset will not hold.
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
        std::cout << "\nType 'reset' to zero every color's ink counter, anything else to abort: ";

        std::string confirm;
        std::getline(std::cin, confirm);
        if (toLower(confirm) != "reset")
        {
            std::cout << "[i] Aborted before any EEPROM write. Nothing was changed." << std::endl;
            return FinishRun(0);
        }
    }

    ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);

    ewr::ResetHandlers handlers;

    handlers.onPreflight = [&](const ewr::StateSnapshot& before)
    {
        PrintPrinterStatus(before.status);
        if (resetInk)
        {
            PrintCounterValues(before.values, "Cartridge ink counter EEPROM values BEFORE reset:");
            PrintInkSummary(selected.smartModel, before.values);
        }
        else
        {
            PrintCounterValues(before.values, "Waste counter EEPROM values BEFORE reset:");
            PrintCounterSummary(selected.smartModel, before.values);
        }
    };

    // Anything but an explicit yes aborts before any write.
    handlers.onBlocker = [](const ewr::Blocker& blocker)
    {
        if (blocker.errorCode >= 0)
            std::cout << "\n[!] WARNING: the printer reports an active error: "
                      << blocker.errorName << "." << std::endl;
        else
            std::cout << "\n[!] WARNING: " << blocker.errorName << "." << std::endl;

        if (!blocker.explanation.empty())
            std::cout << "    " << blocker.explanation << std::endl;

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

        std::cout << "\nReset the waste ink pad counters of " << selected.smartModel.name
                  << " now? [y/N]: ";

        std::string answer;
        std::getline(std::cin, answer);
        const std::string a = toLower(answer);
        return a == "y" || a == "yes";
    };

    handlers.onVerify = [&](const ewr::StateSnapshot& after)
    {
        if (resetInk)
        {
            PrintCounterValues(after.values, "Cartridge ink counter EEPROM values AFTER reset:");
            PrintInkSummary(selected.smartModel, after.values);
        }
        else
        {
            PrintCounterValues(after.values, "Waste counter EEPROM values AFTER reset:");
            PrintCounterSummary(selected.smartModel, after.values);
        }
    };

    const ewr::ResetOutcome outcome = resetInk ? session.ResetInk(handlers)
                                               : session.Reset(handlers);

    // The session already narrated why it stopped; skip the FAILED banner.
    if (outcome.phase == ewr::ResetPhase::Aborted
        || outcome.phase == ewr::ResetPhase::DeviceNotFound)
        return FinishRun(1);

    return FinishReset(outcome.success);
}
