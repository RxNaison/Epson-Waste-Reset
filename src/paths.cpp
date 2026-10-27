#include "ewr/paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace ewr {
namespace paths {

    namespace {

        std::mutex g_mutex;
        std::string g_dataDir;

        std::string Env(const char* name)
        {
            const char* value = std::getenv(name);
            return (value && *value) ? std::string(value) : std::string();
        }

    } // namespace

    void SetDataDir(const std::string& dir)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_dataDir = dir;
    }

    std::string DataDir()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_dataDir;
    }

    std::string InDataDir(const std::string& name)
    {
        const std::string dir = DataDir();
        if (dir.empty())
            return name;

        return (fs::path(dir) / name).string();
    }

    std::string UserDataDir()
    {
#ifdef _WIN32
        const std::string base = Env("LOCALAPPDATA");
        return base.empty() ? std::string() : (fs::path(base) / "EWR").string();
#elif defined(__APPLE__)
        const std::string home = Env("HOME");
        return home.empty() ? std::string()
                            : (fs::path(home) / "Library" / "Application Support" / "EWR").string();
#else
        const std::string xdg = Env("XDG_DATA_HOME");
        if (!xdg.empty())
            return (fs::path(xdg) / "ewr").string();

        const std::string home = Env("HOME");
        return home.empty() ? std::string() : (fs::path(home) / ".local" / "share" / "ewr").string();
#endif
    }

    bool EnsureWritableDir(const std::string& dir)
    {
        if (dir.empty())
            return false;

        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        if (!fs::is_directory(fs::path(dir), ec))
            return false;

        // Permission bits do not answer this on every system (ACLs, read-only
        // mounts, a sandbox), so ask by writing.
        const fs::path probe = fs::path(dir) / ".ewr-write-probe";
        {
            std::ofstream out(probe, std::ios::out | std::ios::trunc);
            if (!out)
                return false;
            out << "ewr";
            if (!out)
                return false;
        }

        fs::remove(probe, ec);
        return true;
    }

} // namespace paths
} // namespace ewr
