#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

namespace ewr {

    // Highest database schema this build understands; the loader, the OTA
    // validator and the CLI all read this one definition.
    //   3 - pad_groups with a machine-readable 'kind'
    //   4 - per-counter read specs (masks, weights, service limits) and the
    //       optional post-write commit step ('close')
    // Newer files still load: unknown keys are ignored and a warning is shown.
    constexpr int kMaxSupportedDatabaseSchema = 4;

    // Staging file suffix owned by EWR. Only files matching IsEwrTempArtifact()
    // are ever deleted by CleanupStaleTempFiles().
    inline const char* kStagedDatabaseSuffix = ".staged";

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

    // ---- Pure helpers (no I/O beyond reading the given path; unit-tested) ----

    // Requires at least one plausible model, and refuses a schema_version
    // newer than this build supports.
    bool ValidateDatabasePayload(const std::string& path, int maxSupportedSchema);

    // Interprets a GitHub releases API response (object or array).
    UpdateMetadata ParseReleaseResponse(const std::string& responseBody,
                                        const std::string& currentVersion,
                                        bool includePreRelease = false);

    // True only for EWR's own database staging files.
    bool IsEwrTempArtifact(const std::string& filename);

    // Never touches unrelated *.tmp / *.old files in the working directory.
    void CleanupStaleTempFiles();

    // ---- Platform layer (the only per-OS code) ----
    namespace platform {

        bool HttpDownloadToFile(const std::string& url, const std::string& destPath);
        bool HttpGetString(const std::string& url, std::string& outBody);

        // As close to atomic as the OS allows. Not named ReplaceFile because
        // windows.h defines that as a macro (ReplaceFileA).
        bool ReplaceFileAtomic(const std::string& from, const std::string& to);

        // Called once before any worker thread performs network I/O.
        void InitNetworking();

    } // namespace platform

    class Updater
    {
    public:
        // Blocking. Only for the first run, when no database exists yet and
        // there is nothing a swap could race against.
        static bool SyncDatabaseNow(const std::string& dbPath = "database.json",
                                    int maxSupportedSchema = kMaxSupportedDatabaseSchema);

        // Validates into `<dbPath>.staged` without touching the live file, so
        // the in-memory database can never change underneath a running reset.
        static bool StageDatabaseUpdate(const std::string& dbPath, std::string& outStagedPath,
                                        int maxSupportedSchema = kMaxSupportedDatabaseSchema);

        // Asks GitHub for the latest release and compares it to currentVersion.
        static UpdateMetadata CheckLatestRelease(const std::string& currentVersion);
    };

    // Off the main thread so startup stays responsive. Nothing is applied
    // while the program runs; the staged file is swapped in on exit.
    class BackgroundUpdater
    {
    public:
        static BackgroundUpdater& Instance();

        void StartAsync(int maxSupportedSchema = kMaxSupportedDatabaseSchema);

        void Stop();

        bool IsRunning() const;

        // Joins the worker, then applies any staged update. Call last.
        void ApplyStagedUpdatesOnExit();

    private:
        BackgroundUpdater() = default;
        ~BackgroundUpdater();

        BackgroundUpdater(const BackgroundUpdater&) = delete;
        BackgroundUpdater& operator=(const BackgroundUpdater&) = delete;

        void WorkerRoutine(int maxSupportedSchema);

        std::thread m_worker;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stopRequested{false};

        mutable std::mutex m_mutex;
        std::string m_stagedDatabasePath;
        std::string m_databasePath;
    };

} // namespace ewr
