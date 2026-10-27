#pragma once
#include <string>

// The updater's only per-OS code: updater_windows.cpp, updater_linux.cpp and
// updater_none.cpp each implement it.
namespace ewr::update::platform {

    bool HttpDownloadToFile(const std::string& url, const std::string& destPath);
    bool HttpGetString(const std::string& url, std::string& outBody);

    // As close to atomic as the OS allows. Not named ReplaceFile because
    // windows.h defines that as a macro (ReplaceFileA).
    bool ReplaceFileAtomic(const std::string& from, const std::string& to);

    // Called once before any worker thread performs network I/O.
    void InitNetworking();

} // namespace ewr::update::platform
