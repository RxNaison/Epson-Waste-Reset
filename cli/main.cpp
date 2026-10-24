#include <iostream>
#include <algorithm>
#include <string>
#include <cctype>
#include <filesystem>
#include "ewr/payload.h"
#include "ewr/paths.h"
#include "ewr/restore.h"
#include "ewr/info.h"
#include "ewr/maintenance.h"
#include "ewr/parser.h"
#include "ewr/session.h"
#include "ewr/snmp_gateway.h"
#include "ewr/usb.h"
#include "ewr/deviceid.h"
#include "ewr/generator.h"
#include "ewr/status.h"
#include "ewr/updater.h"
#include "ewr/discover.h"
#include "ewr/version.h"
#include "ewr/log.h"
#include "ewr/json_out.h"
#include "backup_store.h"
#include "color.h"
#include "console.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <streambuf>
#include <utility>
#include <vector>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <chrono>
#include <csignal>
#include <thread>
#include <ctime>

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
    bool dumpFull = false;       // --full (with --dump): all of 0..mem_high
    bool findAddresses = false;  // --find-addresses: repeated dumps around head cleanings
    bool findKey = false;        // --find-key: try every read key in the database, read-only
    bool noUpdate = false;       // --no-update: offline run, nothing checked or swapped
    bool assumeYes = false;      // --yes: answer the write confirmation, nothing else
    bool forceYes = false;       // --force-yes: also overrule the gates that would stop it
    bool cartridge = false;      // --cartridge: reset ink levels instead of the waste pads
    int interfaceCandidate = 0;  // --interface <n>: 1-based pin, 0 = auto
    bool usbSoftReset = false;   // --usb-soft-reset: reset the USB channel once, then wait
    bool json = false;           // --json: the machine-readable contract on stdout
    std::string modelOverride;   // --model <name>: skip the menu
    std::string ip;              // --ip <address>: read a network printer over SNMP
    bool ipAuto = false;         // --ip auto: search the network even with a printer on USB
    std::string restoreFile;     // --restore <file>: write back a backup's or a dump's counter bytes
    bool info = false;           // --info: firmware, serial, the printer's own counters, fatal codes
    bool nozzleCheck = false;    // --nozzle-check [alt]: print the nozzle check pattern
    int nozzleVariant = 0;
    bool headClean = false;      // --head-clean [level]: a head cleaning
    std::string cleanLevel = "normal";
    std::string cleanGroup;      // --clean-group all|black|color
    bool powerClean = false;     // --power-clean: the ink charge
    bool remoteOnly = false;     // --print-data: the print-data route even where a factory level exists
};

// ---- --json (docs/json-output.md) --------------------------------------
//
// The contract owns stdout in this mode, so the human rendering is sent to a
// sink that drops it rather than guarded at two hundred call sites. Nothing
// below does anything unless --json was passed.

namespace {

    class NullBuffer final : public std::streambuf
    {
    protected:
        int overflow(int ch) override { return ch; }
    };

    NullBuffer g_nullBuffer;
    std::unique_ptr<std::ostream> g_realStdout;
    std::unique_ptr<ewr::JsonEmitter> g_json;

    // Filled in as the run goes; spent by the single result line.
    std::string g_jsonCommand = "unknown";
    std::string g_jsonErrorCode;
    std::string g_jsonErrorText;
    nlohmann::json g_jsonData = nlohmann::json::object();
    bool g_jsonHelloSent = false;

    const char* PlatformName()
    {
#ifdef _WIN32
        return "windows";
#elif defined(__APPLE__)
        return "macos";
#else
        return "linux";
#endif
    }

    // `hello` waits for the command name, which parsing decides - but it must
    // still come first when parsing is what failed.
    void JsonHello(const CliOptions& cli)
    {
        if (!g_json || g_jsonHelloSent)
            return;

        g_jsonHelloSent = true;

        nlohmann::json flags;
        flags["no_update"] = cli.noUpdate;
        flags["yes"] = cli.assumeYes;
        flags["force_yes"] = cli.forceYes;
        flags["cartridge"] = cli.cartridge;
        flags["usb_soft_reset"] = cli.usbSoftReset;
        flags["interface"] = cli.interfaceCandidate;
        flags["ip"] = cli.ipAuto       ? nlohmann::json("auto")
                    : cli.ip.empty()   ? nlohmann::json(nullptr)
                                       : nlohmann::json(cli.ip);

        g_json->Hello(EWR_VERSION, PlatformName(), g_jsonCommand,
                      cli.modelOverride.empty() ? nlohmann::json(nullptr) : nlohmann::json(cli.modelOverride),
                      flags);
    }

    // The reason this run did not do what it was asked. The first one wins:
    // later failures are consequences of it.
    void JsonFail(const char* errorCode, const std::string& detail = "")
    {
        if (!g_json || !g_jsonErrorCode.empty())
            return;

        g_jsonErrorCode = errorCode;
        g_jsonErrorText = detail;
    }

} // namespace

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
              << "                   the exact name, an alias, or the name the printer\n"
              << "                   reports (e.g. --model \"EPSON ET-2803 Series\").\n"
              << "  --interface <n>  Pin the whole run to interface <n> from --list and\n"
              << "                   disable the automatic fallback. For composite\n"
              << "                   devices where detection picks the wrong interface.\n"
              << "  --ip <address>   Reach a printer on the network instead of USB, over SNMP\n"
              << "                   (UDP 161). Works with --status, --dry-run and the waste\n"
              << "                   ink pad reset, with the same gates and read-back as USB.\n"
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
              << "                   addresses for. Reads the EEPROM three times and asks you\n"
              << "                   to run a head cleaning between reads; the bytes that rise\n"
              << "                   every time are the counters. Read-only.\n"
              << "  --find-key       For a printer the database has no entry for: try every\n"
              << "                   read key in the database and report which one, if any,\n"
              << "                   the printer answers. Takes no --model. Read-only.\n"
              << "  --json           Machine-readable output: one JSON object per line on\n"
              << "                   stdout, for callers driving EWR from another language.\n"
              << "                   Never prompts; see docs/json-output.md for the contract.\n"
              << "                   --json-version <n> pins the contract version (1).\n"
              << "  --help, -h       Show this help.\n";
}

// Only the filled cells take the color, so a bar keeps its width either way.
static std::string GaugeBar(int percent, const char* color = nullptr)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    const int filled = (percent + 9) / 10; // any non-zero percent shows a cell

    return "[" + ewr::cli::Paint(std::string(filled, '#'), color) + std::string(10 - filled, '-') + "]";
}

// A gauge in its own ink. Black keeps the terminal's text color: a black bar
// vanishes on a dark background.
static const char* InkColor(const std::string& name)
{
    using namespace ewr::cli::ansi;
    if (name == "Cyan")          return kDarkCyan;
    if (name == "Light Cyan")    return kCyan;
    if (name == "Magenta")       return kDarkMagenta;
    if (name == "Light Magenta") return kMagenta;
    if (name == "Yellow")        return kYellow;
    if (name == "Dark Yellow" || name == "Orange") return kDarkYellow;
    if (name == "Red")           return kRed;
    if (name == "Blue")          return kBlue;
    if (name == "Gray" || name == "Light Gray" || name == "Light Black") return kGray;
    return nullptr;
}

static const char* LevelWordColor(const std::string& word)
{
    using namespace ewr::cli::ansi;
    if (word == "OK")      return kGreen;
    if (word == "UNKNOWN") return kGray;
    if (word == "EMPTY" || word == "MISSING" || word == "FAIL") return kRed;
    return nullptr;
}

// How full a pad or maintenance box is, in the same shade as its bar.
static std::string FillPercent(int percent)
{
    char text[16];
    snprintf(text, sizeof(text), "%3d%%", percent);
    return ewr::cli::Paint(text, ewr::cli::RampColor(percent));
}

static const char* ConditionColor(const ewr::PrinterStatus& st)
{
    using namespace ewr::cli::ansi;
    if (st.hasError)            return kRed;
    if (st.stateName == "IDLE") return kGreen;
    return kYellow;
}

static void PrintPrinterStatus(const ewr::PrinterStatus& st)
{
    if (!st.valid)
    {
        std::cout << "[i] The printer answered, but the status report could not be parsed." << std::endl;
        return;
    }

    std::cout << "\n----------- PRINTER STATUS -----------" << std::endl;
    std::cout << "  State:  " << ewr::cli::Paint(ewr::DescribePrinterCondition(st), ConditionColor(st)) << std::endl;

    if (!st.serial.empty())
        std::cout << "  Serial: " << st.serial << std::endl;

    if (!st.inks.empty())
    {
        std::cout << "  Ink levels:" << std::endl;
        for (const auto& ink : st.inks)
        {
            const std::string word = ewr::cli::Paint(ink.statusText, LevelWordColor(ink.statusText));
            char line[192];
            if (ink.level >= 0)
                snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s",
                         ink.colorName.c_str(), GaugeBar(ink.level, InkColor(ink.colorName)).c_str(),
                         ink.level, word.c_str());
            else
                snprintf(line, sizeof(line), "    %-14.14s %s",
                         ink.colorName.c_str(), word.c_str());
            std::cout << line << std::endl;
        }
    }

    if (st.maintenanceBoxLevel >= 0 || !st.maintenanceBoxText.empty())
    {
        const std::string word = ewr::cli::Paint(st.maintenanceBoxText, LevelWordColor(st.maintenanceBoxText));
        char line[192];
        if (st.maintenanceBoxLevel >= 0)
            snprintf(line, sizeof(line), "    %-14.14s %s %s  %s", "Maint. box",
                     GaugeBar(st.maintenanceBoxLevel, ewr::cli::RampColor(st.maintenanceBoxLevel)).c_str(),
                     FillPercent(st.maintenanceBoxLevel).c_str(), word.c_str());
        else
            snprintf(line, sizeof(line), "    %-14.14s %s", "Maint. box", word.c_str());
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

        char line[256];
        if (percent >= 0)
        {
            const std::string limit = (percent >= 100)
                ? ewr::cli::Paint("  <-- service limit reached", ewr::cli::ansi::kRed)
                : std::string();
            snprintf(line, sizeof(line), "    %-28.28s %s %s  (%u / %u)%s",
                     name.c_str(), GaugeBar(percent, ewr::cli::RampColor(percent)).c_str(),
                     FillPercent(percent).c_str(),
                     static_cast<unsigned>(reading.value),
                     static_cast<unsigned>(reading.max_value),
                     limit.c_str());
        }
        else
        {
            snprintf(line, sizeof(line), "    %-28.28s value %u (no service limit on record)",
                     name.c_str(), static_cast<unsigned>(reading.value));
        }
        std::cout << line << std::endl;
    }
}

// The gauges above show only pads with a counter, and 809 of the 1337
// resettable models also reset a pad without one - an R220's platen pad, an
// L6490's only pad - so without this a person sees one pad, or none, on a
// printer where a reset clears more (#42). Same list as `reset_covers`.
static void PrintResetCoverage(const ewr::DbPrinterModel& model)
{
    const std::vector<ewr::PadCoverage> covers = model.GetResetCoverage();
    if (covers.empty())
        return;

    std::cout << "  A waste pad reset clears:" << std::endl;
    for (const auto& pad : covers)
    {
        const std::string name = pad.name.empty() ? std::string("Waste pad") : pad.name;
        char line[200];
        snprintf(line, sizeof(line), "    %-28.28s %s", name.c_str(),
                 pad.readable ? "has a counter"
                              : "no counter - only the printer's own error says when it is full");
        std::cout << line << std::endl;
    }
}

// Diagnostic runs are often piped, so they skip the interactive pause. Decided
// once the command line parses: one rejected before that was typed or spawned,
// never double-clicked, so it has no window about to vanish.
static bool g_exitPause = false;

// The pause exists for the Windows user who ran ewr.exe by double-clicking and
// would otherwise watch the window vanish. A caller that pipes stdin has no
// keyboard behind it: a pipe that stays open never delivers the Enter, and the
// run hangs forever holding the printer (issue #39 found one sitting for seven
// hours). Piped answers still work - only the final keypress is skipped.
// _isatty is true for any character device, NUL included, so a caller that
// spawns EWR with stdin on devnull got the pause. Only a console has a mode.
static bool StdinIsInteractive()
{
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

static bool StdoutIsTerminal()
{
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

// A Windows console reads ANSI escapes only in VT mode, which is off until a
// program asks and refused before Windows 10 - that console gets no color.
static bool TerminalTakesColor(bool forStdout)
{
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(forStdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return false;
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
        || SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    const char* term = std::getenv("TERM");
    return isatty(fileno(forStdout ? stdout : stderr)) != 0 && !(term && std::string(term) == "dumb");
#endif
}

// The buffers are never freed: std::cout and std::cerr flush through them
// during static destruction, after main has returned.
static void InstallColor()
{
    const char* noColor = std::getenv("NO_COLOR");
    if (noColor && *noColor)
        return;

    ewr::cli::ColorState& colors = ewr::cli::Colors();
    colors.out = TerminalTakesColor(true);
    colors.err = TerminalTakesColor(false);

    if (colors.out)
        std::cout.rdbuf(new ewr::cli::TagColorBuf(std::cout.rdbuf()));
    if (colors.err)
        std::cerr.rdbuf(new ewr::cli::TagColorBuf(std::cerr.rdbuf()));
}

#ifdef _WIN32
static BOOL WINAPI ShowCursorOnInterrupt(DWORD)
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info{};
    if (GetConsoleCursorInfo(out, &info))
    {
        info.bVisible = TRUE;
        SetConsoleCursorInfo(out, &info);
    }
    return FALSE; // the default handler still ends the process
}
#else
static void ShowCursorOnInterrupt(int sig)
{
    static const char kShow[] = "\033[?25h";
    (void)!write(STDOUT_FILENO, kShow, sizeof(kShow) - 1);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

// Ctrl+C mid-spin skips the destructor and would leave the person's terminal
// without a cursor, so an interrupt handler shows it again on the way out.
class HiddenCursor
{
public:
    HiddenCursor()
    {
#ifdef _WIN32
        SetConsoleCtrlHandler(ShowCursorOnInterrupt, TRUE);
        m_out = GetStdHandle(STD_OUTPUT_HANDLE);
        m_saved = GetConsoleCursorInfo(m_out, &m_info) != 0;
        if (m_saved)
        {
            CONSOLE_CURSOR_INFO hidden = m_info;
            hidden.bVisible = FALSE;
            SetConsoleCursorInfo(m_out, &hidden);
        }
#else
        m_previous = std::signal(SIGINT, ShowCursorOnInterrupt);
        std::cout << "\033[?25l" << std::flush;
#endif
    }

    ~HiddenCursor()
    {
#ifdef _WIN32
        if (m_saved)
            SetConsoleCursorInfo(m_out, &m_info);
        SetConsoleCtrlHandler(ShowCursorOnInterrupt, FALSE);
#else
        std::cout << "\033[?25h" << std::flush;
        std::signal(SIGINT, m_previous);
#endif
    }

    HiddenCursor(const HiddenCursor&) = delete;
    HiddenCursor& operator=(const HiddenCursor&) = delete;

private:
#ifdef _WIN32
    HANDLE m_out{ nullptr };
    CONSOLE_CURSOR_INFO m_info{};
    bool m_saved{ false };
#else
    void (*m_previous)(int){ SIG_DFL };
#endif
};

// Turns | / - \ in place after text already on the line until `done`. Only on a
// terminal: on a pipe the redraws would pile up in the log, and under --json
// stdout is not the person's.
static void SpinUntil(const std::function<bool()>& done)
{
    static const char kFrames[] = { '|', '/', '-', '\\' };

    std::cout << std::flush;
    if (g_json || !StdoutIsTerminal())
    {
        while (!done())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return;
    }

    HiddenCursor hidden;
    for (size_t frame = 0; !done(); ++frame)
    {
        std::cout << kFrames[frame % 4] << '\b' << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Releases ship as archives, so EWR announces a new one instead of installing it.
static void PrintNewVersionBanner(const std::string& latestVersion)
{
    using namespace ewr::cli;
    const std::string rule = Paint("************************************************************", ansi::kYellow);
    std::cout << "\n  " << rule << "\n"
              << "    " << Paint("A NEW VERSION OF EWR IS AVAILABLE: " + latestVersion, ansi::kYellow)
              << " (you have " << kEwrCurrentVersion << ")\n"
              << "    Download it here:\n"
              << "    " << Paint(kReleasesPageUrl, ansi::kCyan) << "\n"
              << "  " << rule << std::endl;
}

// Every exit path funnels through here so, in --json mode, the stream always
// ends with exactly one result.
static int FinishRun(int exitCode)
{
    if (g_json)
    {
        const bool ok = (exitCode == 0);

        // A run can fail without anything having named a reason. "failed" is
        // that case, and it is part of the contract for exactly this purpose.
        nlohmann::json errorCode = nullptr;
        if (!ok)
            errorCode = g_jsonErrorCode.empty() ? std::string("failed") : g_jsonErrorCode;

        g_json->Result(g_jsonCommand, ok, exitCode, errorCode,
                       g_jsonErrorText.empty() ? nlohmann::json(nullptr) : nlohmann::json(g_jsonErrorText),
                       g_jsonData);
    }

    if (g_exitPause)
    {
        std::cout << "\nPress Enter to exit..." << std::endl;
        std::cin.get();
    }

    return exitCode;
}

// A rejected command line still owes a --json caller its one result line.
static int UsageError(const CliOptions& cli, const std::string& message);

static int UsageError(const CliOptions& cli, const std::string& message)
{
    std::cerr << "[!] " << message << std::endl;
    JsonHello(cli);
    JsonFail("bad_usage", message);
    return FinishRun(2);
}

static void PrintNetworkSearchHints()
{
    std::cout << "    A printer on the network has to be on, on this computer's network, and have\n"
                 "    SNMP enabled in its web settings. A VPN, a firewall, or a system setting that\n"
                 "    keeps programs off the local network (on macOS, possibly Local Network under\n"
                 "    Privacy & Security) can stop the search reaching it. Its address is on its\n"
                 "    network status sheet: ewr --ip <address> goes straight to it." << std::endl;
}

// ChooseNetworkPrinter's answers besides an index: "none of these", and no
// stdin to ask, which needs telling apart - its fix is --ip.
constexpr int kNoPrinterChosen = -1;
constexpr int kNoInputForPrinter = -2;

// The network search's menu. Even a lone printer is picked, not assumed: the
// network may hold someone else's Epson.
static int ChooseNetworkPrinter(const std::vector<ewr::NetworkPrinter>& found,
                                const std::vector<ewr::ModelNameEntry>& entries)
{
    std::cout << std::endl;
    for (std::size_t i = 0; i < found.size(); ++i)
    {
        const ewr::NetworkPrinter& printer = found[i];
        std::cout << "  [" << i + 1 << "] " << (printer.model.empty() ? "(no model name)" : printer.model)
                  << "   " << printer.address;

        const std::vector<std::string> matches = printer.model.empty()
            ? std::vector<std::string>{} : ewr::MatchModelEntries(printer.model, entries);
        if (!matches.empty())
            std::cout << "   (database: " << matches[0] << ")";
        std::cout << std::endl;
    }
    std::cout << "  [0] None of these" << std::endl;

    while (true)
    {
        std::cout << "\nSelect a printer [0-" << found.size() << "]: ";
        std::string line;
        if (!std::getline(std::cin, line))
            return kNoInputForPrinter;

        try
        {
            const int choice = std::stoi(line);
            if (choice == 0)
                return kNoPrinterChosen;
            if (choice >= 1 && choice <= static_cast<int>(found.size()))
                return choice - 1;
        }
        catch (...)
        {
        }

        std::cout << "[-] Please enter a number from 0 to " << found.size() << "." << std::endl;
    }
}

static int FinishReset(bool resetOk)
{
    using namespace ewr::cli;
    if (resetOk)
    {
        const std::string rule = Paint("========================================", ansi::kGreen);
        std::cout << "\n" << rule << std::endl;
        std::cout << Paint(" SUCCESS! Turn the printer OFF, then ON.", ansi::kGreen) << std::endl;
        std::cout << rule << std::endl;
    }
    else
    {
        const std::string rule = PaintErr("========================================", ansi::kRed);
        std::cerr << "\n" << rule << std::endl;
        std::cerr << PaintErr(" RESET FAILED. See messages above and", ansi::kRed) << std::endl;
        std::cerr << PaintErr(" ewr_trace.log for details.", ansi::kRed) << std::endl;
        std::cerr << rule << std::endl;
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
                                   const std::vector<std::pair<uint16_t, int>>& values,
                                   const std::string& deviceId, const std::string& serial)
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
    std::string filename = ewr::paths::InDataDir(base + ".txt");
    for (int n = 2; fs::exists(filename) && n < 100; ++n)
        filename = ewr::paths::InDataDir(base + "_" + std::to_string(n) + ".txt");

    std::ofstream out(filename);
    if (!out)
        return "";

    size_t answered = 0;
    for (const auto& v : values)
        answered += (v.second >= 0) ? 1 : 0;

    // Format 2 adds the header lines after "model"; a reader of format 1
    // skips '#' lines it does not know, and the body is unchanged, so two
    // dumps of either format still diff line for line.
    out << "# EWR EEPROM dump\n";
    out << "# format: 2\n";
    out << "# model: " << model.name << "\n";
    out << "# ewr: " << kEwrCurrentVersion << "\n";
    out << "# created: " << ewr::UtcTimestamp() << "\n";
    if (!deviceId.empty())
        out << "# device_id: " << deviceId << "\n";
    if (!serial.empty())
        out << "# serial: " << serial << "\n";
    out << "# address_length: " << static_cast<int>(model.ReadAddressLength()) << "\n";
    out << "# answered: " << answered << " of " << values.size() << "\n";
    out << "# address,value,note ('--' = no reply: unknown)\n";
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

// --find-key: for a printer no database entry matches. It runs before the
// model menu because there is no model to pick - the search is for one.
static int RunReadKeySearch(ewr::IDeviceGateway& gateway,
                            const std::vector<ewr::DbPrinterModel>& models,
                            const CliOptions& cli,
                            const std::string& detectedMdl,
                            const std::string& detectedMatch)
{
    const std::vector<ewr::ReadKeyCandidate> candidates = ewr::CollectReadKeyCandidates(models);

    ewr::ExecutorOptions options = ewr::DefaultQueryOptions();
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
    const ewr::ReadKeySearch search = ewr::SearchReadKey(gateway, candidates, options,
        [animate](size_t tried, size_t total)
        {
            if (animate)
                std::cout << "\r[*] Trying read keys... " << tried << "/" << total << std::flush;
        });

    if (animate && !search.probes.empty())
        std::cout << std::endl;

    g_jsonData = ewr::JsonKeySearchData(detectedMdl, search);

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
        for (const ewr::ReadKeyProbe& probe : search.probes)
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
                             "    read it with one of those entries - both of these are read-only:\n"
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
static int RunInfo(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options)
{
    std::cout << "\n[*] Reading what the printer says about itself (read-only)..." << std::endl;

    const ewr::PrinterInfo info = ewr::ReadPrinterInfo(gateway, model, ewr::log::Default(), options);
    g_jsonData = ewr::JsonPrinterInfo(model, info);

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
static int RunMaintenance(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway,
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
        g_jsonData = ewr::JsonMaintenanceData(model, request, {});
        return FinishRun(1);
    }

    std::cout << (route == "factory"
                      ? "    With the printer's own maintenance command, from its database entry."
                      : "    Sent as print data (ESC/P2 remote mode), which the printer takes like a job.")
              << std::endl;

    if (cli.dryRun)
    {
        const ewr::StateSnapshot state = ewr::ReadPrinterStatus(gateway, options);
        if (!state.available)
        {
            std::cerr << "[!] The printer did not answer the status query." << std::endl;
            JsonFail("read_failed", "The printer did not answer the status query.");
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);
        if (const auto blocker = ewr::EvaluateMaintenanceBlocker(state.status, request.action))
            std::cout << "\n[!] A real run would stop here: " << blocker->errorName << ".\n    "
                      << blocker->explanation << std::endl;

        std::cout << "\n[DRY RUN] Nothing was sent to the printer." << std::endl;
        ewr::MaintenanceOutcome preview;
        preview.route = route;
        preview.before = state;
        g_jsonData = ewr::JsonMaintenanceData(model, request, preview);
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

    g_jsonData = ewr::JsonMaintenanceData(model, request, outcome);

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

// One line per planned byte: what it holds now, what the file says.
static size_t PrintRestoreDiff(const ewr::RestorePlan& plan, const std::vector<std::pair<uint16_t, int>>& now)
{
    size_t changing = 0;
    std::cout << "  Bytes the restore writes back:" << std::endl;
    for (const auto& w : plan.writes)
    {
        int current = -1;
        for (const auto& entry : now)
        {
            if (entry.first == w.first)
                current = entry.second;
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
    return changing;
}

// A restore can put a pad back at its service limit: say so before asking.
static void WarnOnFullPads(const ewr::DbPrinterModel& model, const std::vector<std::pair<uint16_t, int>>& after)
{
    for (const auto& spec : model.GetAllCounters())
    {
        const ewr::CounterReading reading = ewr::EvaluateCounter(spec, after);
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
static int RunRestore(const ewr::DbPrinterModel& model, const ewr::Backup& source, const std::string& file,
                      ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options, const CliOptions& cli,
                      const std::string& deviceId, const std::string& mismatchPassed)
{
    const ewr::RestorePlan plan = ewr::PlanRestore(model, source);
    const bool overNetwork = gateway.OverNetwork();

    // In place before any early exit, so a refused restore still names
    // what it was asked to write.
    g_jsonData = ewr::JsonRestorePlan(source, plan, file);

    std::cout << "\n[*] RESTORE for " << model.name << " from " << file << std::endl;
    std::cout << "    " << (source.source == "dump" ? "An EEPROM dump" : "A backup EWR kept before a write");
    if (!source.created.empty())
        std::cout << ", taken " << source.created;
    if (!source.serial.empty())
        std::cout << ", of the printer with serial " << source.serial;
    std::cout << "." << std::endl;

    size_t unmapped = 0;
    size_t unread = 0;
    size_t other = 0;
    for (const auto& skip : plan.skipped)
    {
        if (skip.reason == "unmapped")
            unmapped++;
        else if (skip.reason == "unread")
            unread++;
        else
            other++;
    }

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

        std::cout << "\n[DRY RUN] Nothing was written. " << changing << " byte(s) would change; run without"
                  << " --dry-run to restore them." << std::endl;

        g_jsonData["printer"] = ewr::JsonPrinterStatus(state.status);
        g_jsonData["before"] = ewr::JsonCounterValues(state.values);
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

    g_jsonData = ewr::JsonRestoreData(model, source, plan, file, outcome);

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

int main(int argc, char* argv[])
{
    // A file named on the command line is relative to where the user is,
    // not to the folder the run moves into next.
    std::error_code launchError;
    const fs::path launchDir = fs::current_path(launchError);

    SetWorkingDirectoryToExecutable();

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
        g_realStdout = std::make_unique<std::ostream>(std::cout.rdbuf());
        std::cout.rdbuf(&g_nullBuffer);
        g_json = std::make_unique<ewr::JsonEmitter>(*g_realStdout);

        // Events become contract lines instead of console text; the CLI's own
        // stderr writes are untouched and stay human.
        ewr::log::Default().ClearSinks();
        ewr::log::Default().AddSink([](const ewr::log::Event& event)
        {
            if (g_json)
                g_json->Event(event);
        });
    }
    else
    {
        InstallColor();
    }

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
        else if (arg == "--model" || arg == "--interface" || arg == "--ip" || arg == "--restore")
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
    }

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

    // Over the network: --status, --dry-run and the waste ink pad reset.
    if (!cli.ip.empty() || cli.ipAuto)
    {
        if (cli.listOnly || cli.findKey || cli.dump || cli.findAddresses || cli.cartridge)
            return UsageError(cli, "--ip works with --status, --dry-run and the waste ink pad reset; --list,"
                                   " --find-key, --dump, --find-addresses and --cartridge need USB.");
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

    const bool statusOnly = cli.statusOnly;

    g_jsonCommand = cli.listOnly       ? "list"
                  : cli.findKey        ? "find-key"
                  : cli.statusOnly     ? "status"
                  : cli.dump           ? "dump"
                  : cli.findAddresses  ? "find-addresses"
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

    ewr::CleanupStaleTempFiles();

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
            ewr::ParseBackupText(text, restoreSource, error);
        }

        if (!error.empty())
        {
            std::cerr << "[ERROR] " << error << std::endl;
            JsonFail("bad_backup", error);
            return FinishRun(1);
        }

        if (cli.modelOverride.empty())
            cli.modelOverride = restoreSource.model;

        std::cout << "[i] RESTORE: writing back what " << file.filename().string() << " holds for "
                  << restoreSource.model << (cli.dryRun ? " - dry run, nothing is written." : ".") << "\n" << std::endl;
    }

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

        if (!listGateway.ClaimPrinter())
        {
            JsonFail("another_run", "Another EWR run is already driving a printer on this machine.");
            return FinishRun(1);
        }

        const std::vector<ewr::InterfaceInfo> interfaces = listGateway.ListInterfaces();
        nlohmann::json jsonInterfaces = nlohmann::json::array();

        if (interfaces.empty())
            std::cout << "\nNo Epson USB interfaces found." << std::endl;
        else
            std::cout << "\nDetected Epson USB interfaces (in automatic fallback order):" << std::endl;

        for (const auto& iface : interfaces)
        {
            std::cout << "\n  [" << iface.index << "] " << iface.className;
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;
            std::cout << "\n      " << iface.path << std::endl;

            if (iface.deviceId.empty())
            {
                std::cout << "      IEEE 1284 device ID: (no reply)" << std::endl;
                jsonInterfaces.push_back(ewr::JsonInterface(iface, ""));
                continue;
            }

            const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(iface.deviceId);
            std::cout << "      IEEE 1284 device ID: " << (devId.model.empty() ? iface.deviceId : "MDL \"" + devId.model + "\"") << std::endl;

            std::string match;
            if (!devId.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::MatchModelEntries(devId.model, listEntries);
                if (!matches.empty())
                {
                    std::cout << "      Database entry:      " << matches[0] << std::endl;
                    match = matches[0];
                }
            }

            jsonInterfaces.push_back(ewr::JsonInterface(iface, match));
        }

        if (!interfaces.empty())
            std::cout << "\nUse --interface <n> to pin a run to one specific interface." << std::endl;
        g_jsonData["interfaces"] = std::move(jsonInterfaces);

        std::cout << "\n[*] Searching the local network for Epson printers (SNMP, read-only)..." << std::endl;
        std::string searchError;
        std::ostringstream searchTrace;
        const std::vector<ewr::NetworkPrinter> network =
            ewr::DiscoverNetworkPrinters(ewr::kDiscoveryWaitMs, searchError, &searchTrace);
        listGateway.AppendTrace(searchTrace.str());
        nlohmann::json jsonNetwork = nlohmann::json::array();

        if (!searchError.empty())
            std::cerr << "\n[!] " << searchError << std::endl;
        else if (network.empty())
            std::cout << "\nNo Epson printer answered on the network." << std::endl;
        else
            std::cout << "\nEpson printers on the network:" << std::endl;

        for (const auto& printer : network)
        {
            std::cout << "\n  " << printer.address << std::endl;
            std::cout << "      IEEE 1284 device ID: "
                      << (printer.model.empty() ? printer.deviceId : "MDL \"" + printer.model + "\"") << std::endl;

            std::string match;
            if (!printer.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::MatchModelEntries(printer.model, listEntries);
                if (!matches.empty())
                {
                    std::cout << "      Database entry:      " << matches[0] << std::endl;
                    match = matches[0];
                }
            }

            jsonNetwork.push_back(ewr::JsonNetworkPrinter(printer, match));
        }

        if (!network.empty())
            std::cout << "\nUse --ip <address> to reach one of them." << std::endl;
        g_jsonData["network_printers"] = std::move(jsonNetwork);

        if (interfaces.empty() && network.empty())
        {
            std::cout << "\nIs the printer on, and plugged in or on this network?" << std::endl;
            PrintNetworkSearchHints();
            JsonFail("device_not_found", "No Epson printer found on USB or on the network.");
            return FinishRun(1);
        }

        return FinishRun(0);
    }

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
        auto database = std::async(std::launch::async, [] { return ewr::Updater::UpdateDatabaseNow("database.json"); });
        auto release = std::async(std::launch::async, ewr::Updater::CheckLatestRelease, kEwrCurrentVersion);

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

    ewr::UniversalGenerator generator;
    if (!generator.LoadDatabase("database.json"))
        std::cerr << "[!] Could not load database.json (missing or corrupted). Smart Protocol models are unavailable this run." << std::endl;

    auto replayModels = ewr::ScanModelsFolder("models");
    auto smartModels = generator.GetAvailableModels();

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
    const std::vector<ewr::InterfaceInfo> interfaces = (netGateway || cli.ipAuto)
        ? std::vector<ewr::InterfaceInfo>{} : usbGateway.ListInterfaces();

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
    std::vector<ewr::ModelNameEntry> smartEntries;
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
            ewr::DiscoverNetworkPrinters(ewr::kDiscoveryWaitMs, searchError, &searchTrace);

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
        const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(devIdQuery.deviceId);
        detectedMdl = devId.model;

        if (!detectedMdl.empty())
        {
            std::cout << "found \"" << detectedMdl << "\"." << std::endl;

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

    // Bound only now: the network search above can change the transport.
    ewr::IDeviceGateway& gateway = netGateway ? static_cast<ewr::IDeviceGateway&>(*netGateway)
                                              : static_cast<ewr::IDeviceGateway&>(usbGateway);

    if (cli.findKey)
        return RunReadKeySearch(gateway, smartModels, cli, detectedMdl, detectedMatch);

    MenuOption selected;
    bool hasSelected = false;

    // How `selected` was chosen, for the trace banner.
    std::string selectedBy;

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
                JsonFail("model_unknown", "--model \"" + cli.modelOverride + "\" matches no usable model.");
                return FinishRun(1);
            }

            if (uniqueNames.size() > 1)
            {
                std::cerr << "[!] --model \"" << cli.modelOverride << "\" is ambiguous. Closest matches:" << std::endl;
                for (size_t i = 0; i < uniqueNames.size() && i < 6; ++i)
                    std::cerr << "      " << uniqueNames[i] << std::endl;
                std::cerr << "    Be more specific - the full name always works." << std::endl;
                JsonFail("model_ambiguous", "--model \"" + cli.modelOverride + "\" matches "
                                            + std::to_string(uniqueNames.size()) + " database entries.");
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
        {
            selectedBy = "--model \"" + cli.modelOverride + "\"";
            std::cout << "\n[i] --model: using " << selected.displayName << "." << std::endl;
        }
    }

    // --yes or --json with no --model: the printer named itself, so take that
    // entry instead of opening a menu nobody is there to answer. It is also
    // why neither can write to the wrong model on its own - the selection and
    // the detection are then the same answer.
    if (!hasSelected && (cli.assumeYes || cli.json))
    {
        const char* flag = cli.assumeYes ? "--yes" : "--json";

        if (detectedMatch.empty())
        {
            std::cerr << "\n[!] " << flag << " needs a model and the printer did not supply one." << std::endl;
            if (!detectedMdl.empty())
                std::cerr << "    It reports \"" << detectedMdl
                          << "\", which matches no database entry." << std::endl;
            else
                std::cerr << "    No Epson interface answered the device ID query." << std::endl;
            std::cerr << "    Pass --model <name>, or run 'ewr --list' to see what is connected."
                      << std::endl;
            JsonFail("model_required", "The printer supplied no model that matches the database.");
            return FinishRun(1);
        }

        for (const auto& opt : options)
        {
            if (!opt.isReplay && opt.smartModel.name == detectedMatch)
            {
                selected = opt;
                hasSelected = true;
                selectedBy = std::string(flag) + ": the detected model";
                std::cout << "\n[i] " << flag << ": using the detected model " << opt.smartModel.name << "."
                          << std::endl;
                break;
            }
        }
    }

    // --json never prompts: there is nobody to ask, and a menu on a silenced
    // stdout would look like a hang. Reached only when the detected entry is
    // not one this run can use.
    if (!hasSelected && cli.json)
    {
        std::cerr << "[!] --json needs the model on the command line: pass --model <name>." << std::endl;
        JsonFail("model_required", "--json cannot open the model menu; pass --model <name>.");
        return FinishRun(1);
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
            JsonFail("model_required", "No model was given and stdin is closed.");
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
                    selectedBy = "the detected model, confirmed at the prompt";
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
            // A Replay dump is bytes for a USB channel; over the network there
            // is nothing it could be sent through.
            if (netGateway && opt.isReplay)
                continue;

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
                selectedBy = "picked from the menu, search \"" + searchQuery + "\"";
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
    ewr::ExecutorOptions sessionOptions = ewr::DefaultQueryOptions();
    sessionOptions.interfaceCandidate = cli.interfaceCandidate;
    sessionOptions.usbSoftReset = cli.usbSoftReset;
    sessionOptions.trace.printerReports = detectedMdl;
    sessionOptions.trace.detectedEntry = detectedMatch;
    sessionOptions.trace.selectedEntry = selected.isReplay ? selected.replayModel.name : selected.smartModel.name;
    sessionOptions.trace.selectedBy = selected.isReplay ? selectedBy + ", replay dump" : selectedBy;

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

        // Built by the one function that builds it everywhere else, so the
        // status payload cannot quietly lack a key the dry run has - it lacked
        // pads_total exactly that way (#42). Built before the read is judged,
        // so a caller that got nothing still learns which model it asked about
        // and how many pads that model has.
        if (selected.isReplay)
        {
            // No read key in a dump, so there is no model to count pads on.
            g_jsonData["model"] = selected.replayModel.name;
            g_jsonData["printer"] = ewr::JsonPrinterStatus(state.status);
            g_jsonData["counters"] = nlohmann::json::array();
            g_jsonData["pads"] = nlohmann::json::array();
            g_jsonData["pads_total"] = 0;
            // Opaque bytes: which pads a replay covers is unknown, not none.
            g_jsonData["reset_covers"] = nullptr;
        }
        else
        {
            g_jsonData = ewr::JsonStateData(selected.smartModel, state);
        }

        g_jsonData["detected_model"] = detectedMatch.empty() ? nlohmann::json(nullptr)
                                                            : nlohmann::json(detectedMatch);
        g_jsonData["planned_writes"] = nullptr;

        if (!state.available)
        {
            std::cerr << (netGateway ? "[ERROR] Could not read the printer status over the network."
                                     : "[ERROR] Could not read the printer status. Is it turned on and plugged in?") << std::endl;
            std::cerr << "        Check ewr_trace.log for the hardware trace." << std::endl;
            JsonFail("read_failed", "The printer did not answer the status query.");
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);
        PrintCounterValues(state.values, "Waste counter EEPROM values:");

        if (!selected.isReplay)
        {
            PrintCounterSummary(selected.smartModel, state.values);
            PrintResetCoverage(selected.smartModel);
        }

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
            JsonFail("not_supported", "--dump needs a Smart Protocol model; a Replay dump carries no read key.");
            return FinishRun(1);
        }

        // A dump walks 0x00-0xFF, and JIG memory is only known from 0x100.
        if (selected.smartModel.jig)
        {
            std::cerr << "[!] --dump does not support JIG-mode printers yet. Use --status to read\n"
                         "    the waste counters." << std::endl;
            JsonFail("not_supported", "--dump does not support JIG-mode printers yet.");
            return FinishRun(1);
        }

        std::cout << "\n[*] DUMP for " << selected.smartModel.name
                  << ": reading the EEPROM (read-only, no writes)..." << std::endl;

        // The low page, where the classic models keep everything, plus every
        // byte the database names above it - a restore needs those.
        const std::vector<uint16_t> addresses = ewr::DumpAddresses(selected.smartModel, cli.dumpFull);
        if (cli.dumpFull)
            std::cout << "    --full: all " << addresses.size() << " bytes up to mem_high. This takes a while." << std::endl;

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadAddresses(addresses);

        if (!state.available)
        {
            std::cerr << "[ERROR] Could not read the printer. Is it powered on and connected?" << std::endl;
            std::cerr << "        See ewr_trace.log for the hardware trace." << std::endl;
            JsonFail("read_failed", "The printer did not answer the EEPROM read.");
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);

        size_t answered = 0;
        for (const auto& v : state.values)
            answered += (v.second >= 0) ? 1 : 0;

        g_jsonData["model"] = selected.smartModel.name;
        g_jsonData["answered"] = answered;
        g_jsonData["total"] = state.values.size();
        g_jsonData["values"] = ewr::JsonCounterValues(state.values);
        g_jsonData["file"] = nullptr;

        const std::string path = WriteEepromDump(selected.smartModel, state.values, devIdQuery.deviceId,
                                                 state.status.serial);
        if (path.empty())
        {
            std::cerr << "[ERROR] Read the EEPROM, but could not write the dump file." << std::endl;
            JsonFail("io_error", "Read the EEPROM, but could not write the dump file.");
            return FinishRun(1);
        }

        g_jsonData["file"] = path;

        // A diff reads '--' on both sides as "unchanged" when it means
        // "unknown", so a short dump must not look like a backup (#39).
        if (answered < state.values.size())
        {
            std::cerr << "\n[ERROR] Incomplete dump: the printer answered " << answered << " of "
                      << state.values.size() << " EEPROM byte(s). Saved to " << path << ",\n"
                      << "        but it is NOT a backup and must not be diffed: a '--' line is a\n"
                      << "        byte that was never read." << std::endl;
            if (answered == 0)
                std::cerr << "        No byte answered at all: the read key is likely wrong for this\n"
                             "        printer - try a sibling model from the same line." << std::endl;
            else
                std::cerr << "        Run --dump again; if it comes back short twice, power-cycle the\n"
                             "        printer first. See ewr_trace.log for the hardware trace." << std::endl;

            JsonFail("incomplete_dump", "The printer answered " + std::to_string(answered) + " of "
                                        + std::to_string(state.values.size()) + " EEPROM bytes.");
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

    // ---- Address discovery: three read-only passes around head cleanings. --
    if (cli.findAddresses)
    {
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

        const std::vector<uint16_t> addresses = ewr::DumpAddresses(selected.smartModel, cli.dumpFull);
        const uint32_t end = addresses.empty() ? 0 : addresses.back();

        std::cout << "\n[*] ADDRESS DISCOVERY for " << selected.smartModel.name << std::endl;
        std::cout << "    EWR will read the EEPROM three times with a head cleaning between reads -" << std::endl;
        std::cout << "    the one action guaranteed to move the waste counter. Bytes that rise after" << std::endl;
        std::cout << "    BOTH cleanings are the counters; a byte that moves only once is a timer or" << std::endl;
        std::cout << "    a page count. No EEPROM byte is written at any point." << std::endl;
        std::cout << "\n    A cleaning uses real ink and pushes the waste counter up, so this costs" << std::endl;
        std::cout << "    a little of both. Two cleanings is the minimum that proves a trend." << std::endl;

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
        std::cout << std::endl;

        std::vector<ewr::EepromSnapshot> snapshots;
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
                              << ewr::MaintenancePhaseName(cleaned.phase) << "): " << cleaned.error << std::endl;
                    std::cerr << "        Run --find-addresses again and answer 'n' to start the cleanings from"
                                 " the printer's panel." << std::endl;
                    JsonFail("not_started", "Cleaning " + std::to_string(pass) + " did not complete: " + cleaned.error);
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

        const std::vector<ewr::ByteTrend> trends = ewr::FindTrendingBytes(snapshots);

        nlohmann::json readings = nlohmann::json::array();
        for (const ewr::ByteTrend& t : trends)
            readings.push_back({ { "addresses", t.addresses }, { "values", t.values },
                                 { "deltas", t.deltas }, { "rising", t.rising } });
        g_jsonData["model"] = selected.smartModel.name;
        g_jsonData["passes"] = snapshots.size();
        g_jsonData["cleanings"] = autoClean ? "ewr" : "panel";
        g_jsonData["readings"] = readings;
        g_jsonData["file"] = nullptr;

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
        for (const ewr::ByteTrend& t : trends)
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
        body << "            \"wkey\": " << ewr::WriteKeyToJson(selected.smartModel.wkey) << ",\n";
        if (!selected.smartModel.wkey1.empty())
            body << "            \"wkey1\": " << ewr::WriteKeyToJson(selected.smartModel.wkey1) << ",\n";
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
        out << ewr::FormatDiscoveryJson(selected.smartModel.name, body.str(), trends);
        out.close();
        g_jsonData["file"] = outPath;

        size_t rising = 0;
        for (const ewr::ByteTrend& t : trends)
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

        g_jsonData = ewr::JsonStateData(selected.smartModel, state);
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
    else if (netGateway && selected.smartModel.HasInkReset())
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
        const std::string where = netGateway ? " at " + cli.ip : std::string();

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
        backup.deviceId = devIdQuery.deviceId;
        backup.host = netGateway ? cli.ip : std::string();

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
    const nlohmann::json resetData = ewr::JsonResetData(selected.smartModel, resetInk, outcome);
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
