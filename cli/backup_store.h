#pragma once
#include "ewr/backup.h"
#include "ewr/paths.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ewr {
namespace cli {

    // backups/ in the data dir first - beside ewr, where people look - then
    // the per-user folder, for a copy of ewr in a folder it cannot write to.
    inline std::vector<std::string> BackupFolders()
    {
        std::vector<std::string> folders = { paths::InDataDir("backups") };

        const std::string user = paths::UserDataDir();
        if (!user.empty())
        {
            const std::string fallback = (std::filesystem::path(user) / "backups").string();
            if (fallback != folders.front())
                folders.push_back(fallback);
        }

        return folders;
    }

    // `location` is the file written; false when no folder took it.
    inline bool StoreBackup(const Backup& backup, std::string& location, std::string& error)
    {
        for (const std::string& folder : BackupFolders())
        {
            if (WriteBackupFile(backup, folder, location, error))
                return true;
        }

        return false;
    }

} // namespace cli
} // namespace ewr
