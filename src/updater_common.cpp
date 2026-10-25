#include "ewr/updater.h"
#include "ewr/update_platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ewr/log.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ewr {

    namespace {

        const char* kDatabaseUrl =
            "https://raw.githubusercontent.com/RxNaison/Epson-Waste-Reset/main/database.json";
        const char* kLatestReleaseUrl =
            "https://api.github.com/repos/RxNaison/Epson-Waste-Reset/releases/latest";

        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool EndsWith(const std::string& value, const std::string& suffix)
        {
            return value.size() >= suffix.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool StartsWith(const std::string& value, const std::string& prefix)
        {
            return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
        }

        std::vector<std::string> Split(const std::string& value, char delimiter)
        {
            std::vector<std::string> parts;
            std::stringstream stream(value);
            std::string part;
            while (std::getline(stream, part, delimiter))
                parts.push_back(part);
            return parts;
        }

        bool IsAllDigits(const std::string& value)
        {
            return !value.empty() &&
                   std::all_of(value.begin(), value.end(),
                               [](unsigned char c) { return std::isdigit(c) != 0; });
        }

        // SemVer identifier ordering: numeric parts compare numerically (so rc.9
        // sorts before rc.10), numeric ranks lower than alphanumeric.
        int CompareIdentifier(const std::string& lhs, const std::string& rhs)
        {
            const bool lhsNum = IsAllDigits(lhs);
            const bool rhsNum = IsAllDigits(rhs);

            if (lhsNum && rhsNum)
            {
                const long long a = std::stoll(lhs);
                const long long b = std::stoll(rhs);
                if (a == b) return 0;
                return a < b ? -1 : 1;
            }

            if (lhsNum != rhsNum)
                return lhsNum ? -1 : 1;

            if (lhs == rhs) return 0;
            return lhs < rhs ? -1 : 1;
        }

        int ComparePreReleaseTags(const std::string& lhs, const std::string& rhs)
        {
            const std::vector<std::string> a = Split(lhs, '.');
            const std::vector<std::string> b = Split(rhs, '.');

            for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
            {
                if (i >= a.size()) return -1;
                if (i >= b.size()) return 1;

                const int result = CompareIdentifier(a[i], b[i]);
                if (result != 0) return result;
            }
            return 0;
        }

        bool FilesAreIdentical(const fs::path& lhs, const fs::path& rhs)
        {
            std::error_code ec;
            if (!fs::exists(rhs, ec) || fs::file_size(lhs, ec) != fs::file_size(rhs, ec))
                return false;

            std::ifstream a(lhs, std::ios::binary);
            std::ifstream b(rhs, std::ios::binary);
            if (!a || !b) return false;

            return std::equal(std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>(),
                              std::istreambuf_iterator<char>(b));
        }

        void RemoveQuietly(const fs::path& path)
        {
            std::error_code ec;
            fs::remove(path, ec);
        }

    } // namespace

    // ------------------------------------------------------------------ Version

    Version Version::Parse(const std::string& versionStr)
    {
        Version version;

        std::string text = versionStr;
        text.erase(0, text.find_first_not_of(" \t\r\n"));
        if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
            text.erase(0, 1);

        // Split "1.2.3-rc.1+build" into core / pre-release, dropping build metadata.
        const size_t plus = text.find('+');
        if (plus != std::string::npos)
            text.erase(plus);

        const size_t dash = text.find('-');
        std::string core = text;
        if (dash != std::string::npos)
        {
            core = text.substr(0, dash);
            version.preReleaseTag = text.substr(dash + 1);
            version.isPreRelease = !version.preReleaseTag.empty();
        }

        for (const std::string& part : Split(core, '.'))
        {
            std::string digits;
            for (char c : part)
            {
                if (std::isdigit(static_cast<unsigned char>(c)) == 0) break;
                digits.push_back(c);
            }
            version.components.push_back(digits.empty() ? 0 : std::stoi(digits));
        }

        if (version.components.empty())
            version.components.push_back(0);

        return version;
    }

    bool Version::IsNewerThan(const Version& target, bool allowPreRelease) const
    {
        // Never push a pre-release onto someone tracking stable releases.
        if (isPreRelease && !allowPreRelease && !target.isPreRelease)
            return false;

        const size_t count = std::max(components.size(), target.components.size());
        for (size_t i = 0; i < count; ++i)
        {
            const int mine = i < components.size() ? components[i] : 0;
            const int theirs = i < target.components.size() ? target.components[i] : 0;
            if (mine != theirs)
                return mine > theirs;
        }

        // Same numeric version: 1.2.0 supersedes 1.2.0-rc.1.
        if (!isPreRelease && target.isPreRelease) return true;
        if (isPreRelease && !target.isPreRelease) return false;
        if (!isPreRelease && !target.isPreRelease) return false;

        return ComparePreReleaseTags(preReleaseTag, target.preReleaseTag) > 0;
    }

    // ------------------------------------------------------------- Pure helpers

    bool update::ValidateDatabase(const std::string& path, int maxSupportedSchema)
    {
        try
        {
            std::ifstream file(path);
            if (!file) return false;

            json root = json::parse(file);
            if (!root.is_object()) return false;

            // Refuse to install a database this build cannot read; the user
            // would end up with an app that no longer finds any printer. The
            // check is on the root whatever shape follows it: the shipped
            // database.json is the flat form, so nesting this under "models"
            // meant the gate never ran for the format actually in use.
            if (root.contains("schema_version") && root["schema_version"].is_number_integer() &&
                root["schema_version"].get<int>() > maxSupportedSchema)
                return false;

            const json* models = &root;
            if (root.contains("models"))
                models = &root["models"];

            if (!models->is_object() || models->empty()) return false;

            for (auto it = models->begin(); it != models->end(); ++it)
            {
                if (it.value().is_object() && it.value().contains("wkey"))
                    return true;
            }
            return false;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    UpdateMetadata update::ParseReleaseResponse(const std::string& responseBody,
                                        const std::string& currentVersion,
                                        bool includePreRelease)
    {
        UpdateMetadata metadata;

        try
        {
            json root = json::parse(responseBody);
            const json* release = nullptr;

            if (root.is_array())
            {
                // The list endpoint is newest-first; skip drafts and, unless the
                // user opted in, pre-releases.
                for (const json& candidate : root)
                {
                    if (!candidate.is_object()) continue;
                    if (candidate.value("draft", false)) continue;
                    if (!includePreRelease && candidate.value("prerelease", false)) continue;

                    release = &candidate;
                    break;
                }
            }
            else if (root.is_object() && !root.value("draft", false))
            {
                release = &root;
            }

            if (release == nullptr || !release->contains("tag_name"))
                return metadata;

            metadata.latestVersion = release->value("tag_name", std::string());
            metadata.releaseNotes = release->value("body", std::string());

            const Version latest = Version::Parse(metadata.latestVersion);
            const Version current = Version::Parse(currentVersion);
            metadata.updateAvailable = latest.IsNewerThan(current, includePreRelease);
        }
        catch (const std::exception&)
        {
            return UpdateMetadata{};
        }

        return metadata;
    }

    bool update::IsTempArtifact(const std::string& filename)
    {
        const std::string name = ToLower(filename);

        return StartsWith(name, "database.json.") &&
               (EndsWith(name, ".tmp") || EndsWith(name, ".staged"));
    }

    void update::CleanupStaleTempFiles()
    {
        std::error_code ec;

        // Staged database files always live in the working directory.
        const fs::path directory = fs::current_path(ec);
        if (ec || directory.empty()) return;

        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            if (update::IsTempArtifact(it->path().filename().string()))
                RemoveQuietly(it->path());
        }
    }

    // ------------------------------------------------------------------ Network

    DatabaseUpdate update::UpdateDatabaseNow(const std::string& dbPath, int maxSupportedSchema)
    {
        update::platform::InitNetworking();

        const fs::path staged = fs::path(dbPath + update::kStagedDatabaseSuffix);

        if (!update::platform::HttpDownloadToFile(kDatabaseUrl, staged.string())
            || !update::ValidateDatabase(staged.string(), maxSupportedSchema))
        {
            RemoveQuietly(staged);
            ewr::log::Log(ewr::log::Level::Info, ewr::log::Stage::Update, "update.database_failed",
                          "[i] Could not download the latest printer database - using the one on disk.");
            return DatabaseUpdate::Failed;
        }

        if (FilesAreIdentical(staged, fs::path(dbPath)))
        {
            RemoveQuietly(staged);
            return DatabaseUpdate::UpToDate;
        }

        if (!update::platform::ReplaceFileAtomic(staged.string(), dbPath))
        {
            RemoveQuietly(staged);
            ewr::log::Log(ewr::log::Level::Info, ewr::log::Stage::Update, "update.database_failed",
                          "[i] Could not replace " + dbPath + " - using the one on disk.");
            return DatabaseUpdate::Failed;
        }

        ewr::log::Log(ewr::log::Level::Info, ewr::log::Stage::Update,
                      "update.database_applied", "[+] Printer database updated.");
        return DatabaseUpdate::Updated;
    }

    UpdateMetadata update::CheckLatestRelease(const std::string& currentVersion)
    {
        update::platform::InitNetworking();

        std::string body;
        if (!update::platform::HttpGetString(kLatestReleaseUrl, body))
            return UpdateMetadata{};

        return update::ParseReleaseResponse(body, currentVersion);
    }

} // namespace ewr
