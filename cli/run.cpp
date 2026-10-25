#include "cli.h"
#include "ewr/log.h"
#include "ewr/paths.h"
#include "color.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <streambuf>
#include <thread>

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

    } // namespace

    std::unique_ptr<ewr::json::Emitter> g_json;
    std::string g_jsonCommand = "unknown";
    std::string g_jsonErrorCode;
    std::string g_jsonErrorText;
    nlohmann::json g_jsonData = nlohmann::json::object();
    bool g_jsonHelloSent = false;

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
    void JsonFail(const char* errorCode, const std::string& detail)
    {
        if (!g_json || !g_jsonErrorCode.empty())
            return;

        g_jsonErrorCode = errorCode;
        g_jsonErrorText = detail;
    }

    void SetWorkingDirectoryToExecutable()
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

    // Where this run reads database.json and writes everything it produces.
    // Beside ewr is the classic layout, and stays the answer wherever ewr can
    // write there. An installed copy cannot - /usr/bin, a package's folder - so
    // it writes to the per-user folder instead, and keeps its own database.json
    // there, seeded from the packaged one (/usr/share/ewr beside a /usr/bin/ewr),
    // for the updater to keep fresh.
    std::string ChooseDataLocation()
    {
        std::string seed = "database.json";
        bool installed = false;
        std::error_code ec;
        if (!fs::exists(seed, ec))
        {
            const fs::path shared = fs::path("..") / "share" / "ewr" / "database.json";
            if (fs::exists(shared, ec))
            {
                seed = shared.string();
                installed = true;
            }
        }

        // A packaged ewr writes to the user's folder even under sudo, where it
        // could write beside itself: ewr_trace.log has no business in /usr/bin.
        if (!installed && ewr::paths::EnsureWritableDir("."))
            return seed;

        const std::string user = ewr::paths::UserDataDir();
        if (user.empty() || !ewr::paths::EnsureWritableDir(user))
            return seed;

        ewr::paths::SetDataDir(user);

        // Newest wins: a package upgrade's database replaces the copy the last
        // one seeded, and an updater download, newer than both, stays. The copy
        // keeps the seed's time, so the next upgrade compares what the files
        // hold rather than when they were copied, and goes through a staging
        // file, so one cut short never leaves a truncated database behind.
        const std::string own = ewr::paths::InDataDir("database.json");
        const fs::file_time_type seedTime = fs::last_write_time(seed, ec);
        if (!ec && (!fs::exists(own, ec) || seedTime > fs::last_write_time(own, ec)))
        {
            const std::string staged = own + ".seed.tmp";
            if (fs::copy_file(seed, staged, fs::copy_options::overwrite_existing, ec))
            {
                fs::last_write_time(staged, seedTime, ec);
                fs::rename(staged, own, ec);
            }
            fs::remove(staged, ec);
        }

        return fs::exists(own, ec) ? own : seed;
    }

    const std::string kReleasesPageUrl = "https://github.com/RxNaison/Epson-Waste-Reset/releases";

    // Diagnostic runs are often piped, so they skip the interactive pause. Decided
    // once the command line parses: one rejected before that was typed or spawned,
    // never double-clicked, so it has no window about to vanish.
    bool g_exitPause = false;

    // The pause exists for the Windows user who ran ewr.exe by double-clicking and
    // would otherwise watch the window vanish. A caller that pipes stdin has no
    // keyboard behind it: a pipe that stays open never delivers the Enter, and the
    // run hangs forever holding the printer (issue #39 found one sitting for seven
    // hours). Piped answers still work - only the final keypress is skipped.
    // _isatty is true for any character device, NUL included, so a caller that
    // spawns EWR with stdin on devnull got the pause. Only a console has a mode.
    bool StdinIsInteractive()
    {
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
        return isatty(fileno(stdin)) != 0;
#endif
    }

    bool StdoutIsTerminal()
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
    void InstallColor()
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
    void SpinUntil(const std::function<bool()>& done)
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
    void PrintNewVersionBanner(const std::string& latestVersion)
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
    int FinishRun(int exitCode)
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
    int UsageError(const CliOptions& cli, const std::string& message);

    int UsageError(const CliOptions& cli, const std::string& message)
    {
        std::cerr << "[!] " << message << std::endl;
        JsonHello(cli);
        JsonFail("bad_usage", message);
        return FinishRun(2);
    }

    int FinishReset(bool resetOk)
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

    // --json: stdout belongs to the contract from here on, and events become
    // its lines instead of console text.
    void StartJsonOutput()
    {
        g_realStdout = std::make_unique<std::ostream>(std::cout.rdbuf());
        std::cout.rdbuf(&g_nullBuffer);
        g_json = std::make_unique<ewr::json::Emitter>(*g_realStdout);

        // Events become contract lines instead of console text; the CLI's own
        // stderr writes are untouched and stay human.
        ewr::log::Default().ClearSinks();
        ewr::log::Default().AddSink([](const ewr::log::Event& event)
        {
            if (g_json)
                g_json->Event(event);
        });
    }

} // namespace ewr::cli
