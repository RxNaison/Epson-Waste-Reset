#include "support.h"

void test_updater_version_parsing()
{
    std::cout << "[TEST] test_updater_version_parsing" << std::endl;

    ewr::Version v123 = ewr::Version::Parse("v1.2.3");
    ewr::Version v1231 = ewr::Version::Parse("v1.2.3.1");
    ewr::Version v124 = ewr::Version::Parse("v1.2.4");
    ewr::Version v130a = ewr::Version::Parse("v1.3.0-alpha");
    ewr::Version v130b = ewr::Version::Parse("v1.3.0-beta");
    ewr::Version v130 = ewr::Version::Parse("v1.3.0");
    ewr::Version v131a = ewr::Version::Parse("v1.3.1-alpha");

    // Extended version v1.2.3.1 > v1.2.3
    CHECK(v1231.IsNewerThan(v123, false) == true);
    CHECK(v123.IsNewerThan(v1231, false) == false);

    // Patch version v1.2.4 > v1.2.3.1
    CHECK(v124.IsNewerThan(v1231, false) == true);

    // Stable vs Pre-release of same version (1.3.0 > 1.3.0-beta > 1.3.0-alpha)
    CHECK(v130.IsNewerThan(v130b, false) == true);
    CHECK(v130b.IsNewerThan(v130a, true) == true);
    CHECK(v130a.IsNewerThan(v130, false) == false);

    // Stable local v1.3.0 vs Remote v1.3.1-alpha
    CHECK(v131a.IsNewerThan(v130, false) == false); // Default stable channel -> Ignored
    CHECK(v131a.IsNewerThan(v130, true) == true);   // Beta channel -> Offered
}

void test_stale_temp_file_cleanup()
{
    std::cout << "[TEST] test_stale_temp_file_cleanup" << std::endl;

    // Run in an isolated scratch directory so the cleanup sweep can never
    // touch real files in the working directory (e.g. a staged OTA update).
    const fs::path previousCwd = fs::current_path();
    const fs::path scratch = previousCwd / "test_cleanup_scratch";
    fs::create_directories(scratch);
    fs::current_path(scratch);

    // EWR owns these and must remove them.
    const std::string ours[] = {"database.json.staged", "database.json.tmp", "database.json.seed.tmp"};

    // These belong to the user and must survive: EWR runs in whatever folder the
    // user dropped the executable into, often the Desktop or Downloads.
    const std::string theirs[] = {"my_thesis_backup.tmp", "family_photos.old", "invoice.old_copy"};

    for (const std::string& name : ours)
    {
        std::ofstream out(name);
        out << "staged";
    }
    for (const std::string& name : theirs)
    {
        std::ofstream out(name);
        out << "precious";
    }

    ewr::update::CleanupStaleTempFiles("database.json");

    for (const std::string& name : ours)
        CHECK(!fs::exists(name));

    for (const std::string& name : theirs)
    {
        CHECK(fs::exists(name));
        fs::remove(name);
    }

    // An installed copy keeps its database in the per-user folder, and its
    // download is staged there, not in the working directory.
    fs::create_directories("user");
    {
        std::ofstream out("user/database.json.staged");
        out << "staged";
    }
    ewr::update::CleanupStaleTempFiles("user/database.json");
    CHECK(!fs::exists("user/database.json.staged"));

    CHECK(!ewr::update::IsTempArtifact("notes.tmp"));
    CHECK(!ewr::update::IsTempArtifact("database.json"));

    fs::current_path(previousCwd);
    fs::remove_all(scratch);
}

void test_updater_prerelease_ordering()
{
    std::cout << "[TEST] test_updater_prerelease_ordering" << std::endl;

    const ewr::Version stable = ewr::Version::Parse("v1.2.3");
    const ewr::Version older = ewr::Version::Parse("1.2.2");
    const ewr::Version rc9 = ewr::Version::Parse("1.3.0-rc.9");
    const ewr::Version rc10 = ewr::Version::Parse("1.3.0-rc.10");

    CHECK(stable.IsNewerThan(older));
    CHECK(!older.IsNewerThan(stable));
    CHECK(!stable.IsNewerThan(stable));

    // Numeric identifiers must compare numerically, not lexicographically.
    CHECK(rc10.IsNewerThan(rc9, true));
    CHECK(!rc9.IsNewerThan(rc10, true));

    // A stable user is never pushed onto the pre-release channel.
    CHECK(!rc10.IsNewerThan(stable, false));
    CHECK(rc10.IsNewerThan(stable, true));

    // The final release supersedes its own release candidate.
    CHECK(ewr::Version::Parse("1.3.0").IsNewerThan(rc10));
    CHECK(!rc10.IsNewerThan(ewr::Version::Parse("1.3.0"), true));

    // Build metadata is ignored, shorter versions pad with zeros.
    CHECK(!ewr::Version::Parse("1.2.3+build.7").IsNewerThan(stable));
    CHECK(ewr::Version::Parse("1.3").IsNewerThan(stable));
}

void test_updater_release_response_parsing()
{
    std::cout << "[TEST] test_updater_release_response_parsing" << std::endl;

    const std::string releases = R"([
        {"tag_name": "v9.9.9", "draft": true, "prerelease": false},
        {"tag_name": "v2.0.0-rc.1", "draft": false, "prerelease": true},
        {"tag_name": "v1.3.0", "draft": false, "prerelease": false, "body": "Notes"}
    ])";

    // Drafts and pre-releases are skipped on the stable channel.
    const ewr::UpdateMetadata stable = ewr::update::ParseReleaseResponse(releases, "1.2.3", false);
    CHECK(stable.updateAvailable);
    CHECK(stable.latestVersion == "v1.3.0");
    CHECK(stable.releaseNotes == "Notes");

    // Opting in reaches the release candidate, but never the draft.
    const ewr::UpdateMetadata preview = ewr::update::ParseReleaseResponse(releases, "1.2.3", true);
    CHECK(preview.updateAvailable);
    CHECK(preview.latestVersion == "v2.0.0-rc.1");

    // Already up to date.
    CHECK(!ewr::update::ParseReleaseResponse(releases, "1.3.0", false).updateAvailable);

    // Malformed responses are non-events, not crashes.
    CHECK(!ewr::update::ParseReleaseResponse("<html>404</html>", "1.2.3", false).updateAvailable);
    CHECK(!ewr::update::ParseReleaseResponse("", "1.2.3", false).updateAvailable);
}

void test_updater_database_payload_validation()
{
    std::cout << "[TEST] test_updater_database_payload_validation" << std::endl;

    // The shipped database must pass the check the OTA sync applies.
    CHECK(ewr::update::ValidateDatabase("database.json", ewr::kMaxSupportedDatabaseSchema));

    const std::string path = "test_ota_payload.json";
    auto write = [&](const std::string& body) {
        std::ofstream out(path);
        out << body;
    };

    write(R"({"L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));

    // Never install a database this build cannot read.
    write(R"({"schema_version": 99, "models": {"L222": {"wkey": "Yj4"}}})");
    CHECK(!ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));

    // ...including the flat form, which is what database.json actually ships
    // as. Nesting the version check under "models" meant the refusal never ran
    // for the only shape in use, and a too-new database went straight over the
    // live file on exit.
    write(R"({"schema_version": 99, "L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(!ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));

    // A schema this build does support still installs, in either shape.
    write(R"({"schema_version": 4, "L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"schema_version": 4, "models": {"L222": {"wkey": "Yj4"}}})");
    CHECK(ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));

    // A 404 page, an empty model map, or entries without a write key are rejected.
    write("<html>Not Found</html>");
    CHECK(!ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"models": {}})");
    CHECK(!ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"L222": {"rkey": 8}})");
    CHECK(!ewr::update::ValidateDatabase(path, ewr::kMaxSupportedDatabaseSchema));

    fs::remove(path);
    CHECK(!ewr::update::ValidateDatabase("no_such_file.json", ewr::kMaxSupportedDatabaseSchema));
}
