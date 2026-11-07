#pragma once
#include <string>
#include <vector>

namespace ewr {

    // Highest database schema this build understands; the loader, the OTA
    // validator and the CLI all read this one definition.
    //   3 - pad_groups with a machine-readable 'kind'
    //   4 - per-counter read specs (masks, weights, service limits) and the
    //       optional post-write commit step ('close')
    // Newer files still load: unknown keys are ignored and a warning is shown.
    constexpr int kMaxSupportedDatabaseSchema = 4;

    struct Version
    {
        std::vector<int> components;
        bool isPreRelease = false;
        std::string preReleaseTag;

        static Version Parse(const std::string& versionStr);

        // Pre-releases are ignored unless allowPreRelease is set or this build
        // is itself a pre-release, meaning the user is already on that channel.
        bool IsNewerThan(const Version& target, bool allowPreRelease = false) const;
    };

    struct UpdateMetadata
    {
        // Releases ship as archives, so EWR announces rather than installs.
        bool updateAvailable = false;
        std::string latestVersion;
        std::string releaseNotes;
    };

    enum class DatabaseUpdate
    {
        Updated,    // a newer database replaced the file
        UpToDate,   // the file already matches upstream
        Failed,     // unreachable, invalid or not replaceable; the file is untouched
    };

} // namespace ewr

// The OTA database update and the release check.
namespace ewr::update {

    // Staging file suffix owned by EWR. Only files matching IsTempArtifact()
    // are ever deleted by CleanupStaleTempFiles().
    inline const char* kStagedDatabaseSuffix = ".staged";

    // ---- Pure helpers (no I/O beyond reading the given path; unit-tested) ----

    // Requires at least one plausible model, and refuses a schema_version
    // newer than this build supports.
    bool ValidateDatabase(const std::string& path, int maxSupportedSchema);

    // Interprets a GitHub releases API response (object or array).
    UpdateMetadata ParseReleaseResponse(const std::string& responseBody,
                                        const std::string& currentVersion,
                                        bool includePreRelease = false);

    // True only for EWR's own database staging files.
    bool IsTempArtifact(const std::string& filename);

    // Removes the staging files left beside `dbPath`. Never touches unrelated
    // *.tmp / *.old files in that folder.
    void CleanupStaleTempFiles(const std::string& dbPath);

    // Blocking. Call it before loading the database: the run then uses
    // what was fetched, and the in-memory copy cannot change under a
    // reset. Swapping on exit instead meant a database fix reached a
    // printer only on the run after the one that downloaded it.
    DatabaseUpdate UpdateDatabaseNow(const std::string& dbPath = "database.json",
                                     int maxSupportedSchema = kMaxSupportedDatabaseSchema);

    // Asks GitHub for the latest release and compares it to currentVersion.
    UpdateMetadata CheckLatestRelease(const std::string& currentVersion);

} // namespace ewr::update
