#pragma once
#include <string>

// Where EWR writes what a run produces: ewr_trace.log, backups, dumps. The
// CLI runs from its own folder and writes there, which an installed package
// (/usr/bin) or an Android app cannot - so the folder is a setting, and the
// default is the current directory, which keeps every existing host as it is.
namespace ewr {
namespace paths {

    // Empty restores the default.
    void SetDataDir(const std::string& dir);

    // Empty when the default is in force.
    std::string DataDir();

    // `name` inside the data dir, or `name` itself under the default.
    std::string InDataDir(const std::string& name);

    // A per-user folder that is EWR's to write: %LOCALAPPDATA%\EWR,
    // ~/Library/Application Support/EWR, $XDG_DATA_HOME/ewr or
    // ~/.local/share/ewr. Not created here; empty when nothing names one.
    std::string UserDataDir();

    // Creates `dir` if needed and proves it writable by writing a file there.
    bool EnsureWritableDir(const std::string& dir);

} // namespace paths
} // namespace ewr
