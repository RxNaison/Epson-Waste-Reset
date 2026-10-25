// The updater's platform layer where EWR does no networking of its own: the
// Android app downloads database.json itself and checks it with
// ewr_database_check before it swaps it in. Every transfer says "unreachable",
// which the updater already treats as "keep the database on disk".
#include "ewr/updater.h"
#include "ewr/update_platform.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace ewr {
    namespace update::platform {

        void InitNetworking()
        {
        }

        bool HttpDownloadToFile(const std::string&, const std::string&)
        {
            return false;
        }

        bool HttpGetString(const std::string&, std::string&)
        {
            return false;
        }

        bool ReplaceFileAtomic(const std::string& from, const std::string& to)
        {
            std::error_code ec;
            fs::rename(from, to, ec);
            return !ec;
        }

    } // namespace update::platform
} // namespace ewr
