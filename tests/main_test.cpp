#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <map>
#include "ewr/parser.h"
#include "ewr/generator.h"
#include "ewr/executor.h"
#include "ewr/status.h"
#include "ewr/deviceid.h"
#include "ewr/updater.h"
#include "ewr/end4.h"

#include "ewr/d4session.h"
#include "ewr/log.h"
#include "ewr/session.h"

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failures;                                                               \
            std::cout << "  [FAIL] " << #cond << "  (" << __FILE__ << ":" << __LINE__   \
                      << ")" << std::endl;                                              \
        }                                                                               \
    } while (0)

namespace legacy {

    std::vector<unsigned char> GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value, const std::string& wkey)
    {
        uint8_t c = 0x42;
        uint8_t not_c = ~c & 0xFF;
        uint8_t shift_c = ((c >> 1) & 0x7F) | ((c << 7) & 0x80);

        std::vector<unsigned char> inner;
        inner.push_back(rkey & 0xFF);
        inner.push_back((rkey >> 8) & 0xFF);
        inner.push_back(c);
        inner.push_back(not_c);
        inner.push_back(shift_c);
        inner.push_back(address & 0xFF);
        inner.push_back((address >> 8) & 0xFF);
        inner.push_back(value);
        inner.insert(inner.end(), wkey.begin(), wkey.end());

        std::vector<unsigned char> epson_cmd;
        epson_cmd.push_back(0x7C);
        epson_cmd.push_back(0x7C);
        uint16_t len = inner.size();
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        std::vector<unsigned char> d4;
        d4.push_back(0x02);
        d4.push_back(0x02);
        uint16_t d4_len = epson_cmd.size() + 6;
        d4.push_back((d4_len >> 8) & 0xFF);
        d4.push_back(d4_len & 0xFF);
        d4.push_back(0x00);
        d4.push_back(0x00);
        d4.insert(d4.end(), epson_cmd.begin(), epson_cmd.end());
        return d4;
    }

    std::vector<std::vector<unsigned char>> GenerateSequence(const ewr::DbPrinterModel& model)
    {
        std::vector<std::vector<unsigned char>> sequence;

        const unsigned char ejl_init[] = {
            0x00, 0x00, 0x00, 0x1B, 0x01, '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4', '.', '4', '\n',
            '@', 'E', 'J', 'L', '\n', '@', 'E', 'J', 'L', '\n'
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(ejl_init), std::end(ejl_init)));

        const unsigned char d4_init[] = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_init), std::end(d4_init)));

        const unsigned char d4_open[] = {
            0x00, 0x00, 0x00, 0x11, 0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_open), std::end(d4_open)));

        const unsigned char d4_credit_grant[] = { 0x00, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x03, 0x02, 0x02, 0x00, 0x01 };
        const unsigned char d4_credit_req[]   = { 0x00, 0x00, 0x00, 0x0D, 0x01, 0x00, 0x04, 0x02, 0x02, 0xFF, 0xFF, 0x00, 0x01 };

        auto addrs = model.GetAllAddresses();
        auto resets = model.GetAllResetValues();

        for (size_t i = 0; i < addrs.size(); ++i)
        {
            if (addrs[i] > model.mem_high)
                continue; // mirrors the generator's EEPROM bounds clamp

            sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_grant), std::end(d4_credit_grant)));
            sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_req), std::end(d4_credit_req)));
            sequence.push_back(GenerateWritePacket(model.rkey, addrs[i], resets[i], model.wkey));
        }
        return sequence;
    }

} // namespace legacy

static ewr::DbPrinterModel MakeTestModel()
{
    ewr::DbPrinterModel m;
    m.name = "TestPrinter";
    m.rkey = 0x0008;
    m.wkey = "Arkanoid";
    ewr::PadGroup pg;
    pg.description = "Main Pad Counter";
    pg.kind = "main";
    pg.addresses = { 0x0018, 0x0019 };
    pg.reset_values = { 0x00, 0x00 };
    m.pad_groups.push_back(pg);
    return m;
}

class FakeTransport final : public ewr::ITransport
{
public:
    std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> replyFor;
    std::vector<std::vector<unsigned char>> sent;
    std::vector<int> drainTimeouts;
    bool failSend = false;

    bool Send(const std::vector<unsigned char>& packet) override
    {
        sent.push_back(packet);
        if (failSend)
            return false;
        pending_ = replyFor ? replyFor(packet) : std::vector<unsigned char>{};
        return true;
    }

    std::vector<unsigned char> Drain(int timeoutMs) override
    {
        drainTimeouts.push_back(timeoutMs);
        auto reply = pending_;
        pending_.clear();
        return reply;
    }

private:
    std::vector<unsigned char> pending_;
};

static std::vector<unsigned char> OkAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'O', 'K', ';', 0x0c };
}

static std::vector<unsigned char> NgAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'N', 'G', ';', 0x0c };
}

// Byte-for-byte the reply captured from a real R220 locked by an empty
// cartridge.
static std::vector<unsigned char> NaAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'N', 'A', ';', 0x0c };
}

static std::vector<unsigned char> HandshakeAck()
{
    return { 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 0x83, 0x00, 0x02, 0x02 };
}

static std::vector<unsigned char> OpenChannelAck()
{
    return { 0x00, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x81, 0x00, 0x02, 0x02, 0x00, 0x01 };
}

static ewr::ExecutorOptions FastOptions()
{
    ewr::ExecutorOptions options;
    options.interPacketDelayMs = 0;
    options.retryDelayMs = 0;
    // The executor tests below assert the credit re-send on write retry.
    options.resendCreditOnRetry = true;
    return options;
}

static std::ofstream NullLog()
{
    return std::ofstream();
}

void test_scan_models()
{
    std::cout << "[TEST] test_scan_models" << std::endl;
    auto models = ewr::ScanModelsFolder("models");
    std::cout << "  Found " << models.size() << " replay model files in 'models/'." << std::endl;
}

void test_parser_dummy_dump()
{
    std::cout << "[TEST] test_parser_dummy_dump" << std::endl;
    std::string test_filename = "test_dummy_model.c";
    {
        std::ofstream out(test_filename);
        out << "static const char payload1[] = { 0x1b, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0xaa, 0xbb };\n";
    }

    auto packets = ewr::ParseWiresharkDump(test_filename);
    fs::remove(test_filename);

    CHECK(packets.size() == 1);
    CHECK(packets.size() == 1 && packets[0].size() == 2);
    CHECK(packets.size() == 1 && packets[0].size() == 2 && packets[0][0] == 0xAA && packets[0][1] == 0xBB);
}

void test_ack_predicates()
{
    std::cout << "[TEST] test_ack_predicates" << std::endl;

    CHECK(ewr::IsEepromWriteOkAck(OkAck()));
    CHECK(!ewr::IsEepromWriteOkAck({}));
    CHECK(!ewr::IsEepromWriteOkAck(HandshakeAck()));
    CHECK(!ewr::IsEepromWriteOkAck(NgAck()));

    CHECK(ewr::IsEepromWriteNgAck(NgAck()));
    CHECK(!ewr::IsEepromWriteNgAck(OkAck()));
    CHECK(!ewr::IsEepromWriteNgAck({}));

    CHECK(ewr::IsEepromWriteNaAck(NaAck()));
    CHECK(!ewr::IsEepromWriteNaAck(OkAck()));
    CHECK(!ewr::IsEepromWriteNaAck(NgAck()));
    CHECK(!ewr::IsEepromWriteNaAck({}));
    CHECK(!ewr::IsEepromWriteOkAck(NaAck()));
    CHECK(!ewr::IsEepromWriteNgAck(NaAck()));

    std::vector<unsigned char> channelOpenAck = { 0x00, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x81, 0x00, 0x02, 0x02, 0x00, 0x01 };
    CHECK(ewr::IsChannelOpenAck(channelOpenAck));
    CHECK(!ewr::IsChannelOpenAck({}));
    CHECK(!ewr::IsChannelOpenAck(OkAck()));

    std::vector<unsigned char> strayByte = { 0x00, 0x81, 0x00, 0x0c, 0x01, 0x00, 0x7F, 0x00 };
    CHECK(!ewr::IsChannelOpenAck(strayByte));
}

void test_write_packet_detection()
{
    std::cout << "[TEST] test_write_packet_detection" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());
    CHECK(seq.size() == 9);

    for (size_t i = 0; i < 5 && i < seq.size(); ++i)
        CHECK(!ewr::IsWritePacket(seq[i]));

    if (seq.size() == 9)
    {
        CHECK(ewr::IsWritePacket(seq[5]));
        CHECK(ewr::IsWritePacket(seq[8]));
    }

    std::vector<unsigned char> adversarial = {
        0x02, 0x02, 0x00, 0x14, 0x00, 0x00, 0x7C, 0x7C,
        0x0A, 0x00, 0x01, 0x00, 0x41, 0xBE, 0xA0, 0x42, 0x42, 0x42, 0x42, 0x42
    };
    CHECK(!ewr::IsWritePacket(adversarial));

    std::vector<unsigned char> truncated = { 0x02, 0x02, 0x00, 0x08, 0x00, 0x00, 0x7C, 0x7C };
    CHECK(!ewr::IsWritePacket(truncated));
}

void test_platen_only_detection()
{
    std::cout << "[TEST] test_platen_only_detection" << std::endl;

    ewr::DbPrinterModel platenModel;
    platenModel.name = "L8050";
    ewr::PadGroup pgPlaten;
    pgPlaten.description = "Platen Pad Counter";
    pgPlaten.kind = "platen";
    platenModel.pad_groups.push_back(pgPlaten);
    CHECK(platenModel.IsPlatenOnly());

    ewr::DbPrinterModel legacyPlaten;
    legacyPlaten.name = "L8050-legacy";
    ewr::PadGroup pgDescOnly;
    pgDescOnly.description = "Platen pad counters";
    legacyPlaten.pad_groups.push_back(pgDescOnly);
    CHECK(legacyPlaten.IsPlatenOnly());

    ewr::DbPrinterModel dualModel;
    dualModel.name = "L3150";
    ewr::PadGroup pg1, pg2;
    pg1.description = "Waste counter (main pad)";
    pg2.description = "Waste counter (platen pad)";
    dualModel.pad_groups = { pg1, pg2 };
    CHECK(!dualModel.IsPlatenOnly());

    ewr::DbPrinterModel unknownModel;
    unknownModel.name = "Mystery";
    ewr::PadGroup pgU;
    pgU.description = "Waste counters";
    ewr::PadGroup pgP;
    pgP.description = "Platen Pad Counter";
    pgP.kind = "platen";
    unknownModel.pad_groups = { pgU, pgP };
    CHECK(!unknownModel.IsPlatenOnly());

    ewr::DbPrinterModel emptyModel;
    emptyModel.name = "Empty";
    CHECK(!emptyModel.IsPlatenOnly());
}

void test_legacy_schema_loading()
{
    std::cout << "[TEST] test_legacy_schema_loading" << std::endl;

    std::string test_filename = "test_legacy_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"LegacyModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24, 25], "reset": [0, 0]}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];
        CHECK(m.pad_groups.size() == 1);
        CHECK(m.GetAllAddresses().size() == 2);
        CHECK(m.HasResettableCounters());
        CHECK(!m.IsPlatenOnly());

        auto seq = gen.GenerateSequence(m);
        CHECK(seq.size() == 9);

        size_t writes = 0;
        for (const auto& pkt : seq)
            if (ewr::IsWritePacket(pkt))
                writes++;
        CHECK(writes == 2);
    }
}

void test_superset_schema_loading()
{
    std::cout << "[TEST] test_superset_schema_loading" << std::endl;

    std::string test_filename = "test_superset_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"SupersetModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "addresses": [24, 25], "reset": [0, 0],)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];
        CHECK(m.pad_groups.size() == 1);
        CHECK(m.GetAllAddresses().size() == 2);
        CHECK(m.pad_groups[0].kind == "main");

        size_t writes = 0;
        for (const auto& pkt : gen.GenerateSequence(m))
            if (ewr::IsWritePacket(pkt))
                writes++;
        CHECK(writes == 2);
    }
}

void test_envelope_schema_loading()
{
    std::cout << "[TEST] test_envelope_schema_loading" << std::endl;

    std::string test_filename = "test_envelope_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 3, "models": {"EnvelopeModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]}}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        CHECK(models[0].name == "EnvelopeModel");
        CHECK(models[0].GetAllAddresses().size() == 2);
        CHECK(models[0].HasResettableCounters());
    }
}

void test_alias_conflict_schema_loading()
{
    std::cout << "[TEST] test_alias_conflict_schema_loading" << std::endl;

    std::string test_filename = "test_alias_conflict_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {)"
            << R"("AliasModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "aliases": ["AliasModel Series", "AM-100/101/103"], "conflict": true,)"
            << R"( "future_unknown_key": {"nested": [1, 2, 3]},)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]},)"
            << R"("PlainModel": {"rkey": 9, "wkey": "Pong",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [30], "reset": [0]}]}}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 2);

    const ewr::DbPrinterModel* aliasModel = nullptr;
    const ewr::DbPrinterModel* plainModel = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "AliasModel")
            aliasModel = &m;
        else if (m.name == "PlainModel")
            plainModel = &m;
    }

    // The additive keys parse, and an unknown future key never kills the entry.
    CHECK(aliasModel != nullptr);
    if (aliasModel)
    {
        CHECK(aliasModel->aliases.size() == 2);
        if (aliasModel->aliases.size() == 2)
        {
            CHECK(aliasModel->aliases[0] == "AliasModel Series");
            CHECK(aliasModel->aliases[1] == "AM-100/101/103");
        }
        CHECK(aliasModel->conflict);
        CHECK(aliasModel->HasResettableCounters());
    }

    // Entries without the new keys keep the safe defaults.
    CHECK(plainModel != nullptr);
    if (plainModel)
    {
        CHECK(plainModel->aliases.empty());
        CHECK(!plainModel->conflict);
    }
}

void test_future_schema_version_warning()
{
    std::cout << "[TEST] test_future_schema_version_warning" << std::endl;

    std::string test_filename = "test_future_schema_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 99, "models": {"FutureModel": {"rkey": 8, "wkey": "FutureKey",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]}}})";
    }

    // The loader reports through the default reporter now, not stderr: the
    // library must stay silent on consoles it does not own.
    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);

    ewr::log::Default().RemoveSink(sinkId);
    fs::remove(test_filename);

    CHECK(loaded);
    CHECK(captured.find("schema_version 99") != std::string::npos);
    CHECK(captured.find("https://github.com/RxNaison/Epson-Waste-Reset/releases") != std::string::npos);
}

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

    // EWR owns these two and must remove them.
    const std::string ours[] = {"database.json.staged", "database.json.tmp"};

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

    ewr::CleanupStaleTempFiles();

    for (const std::string& name : ours)
        CHECK(!fs::exists(name));

    for (const std::string& name : theirs)
    {
        CHECK(fs::exists(name));
        fs::remove(name);
    }

    CHECK(!ewr::IsEwrTempArtifact("notes.tmp"));
    CHECK(!ewr::IsEwrTempArtifact("database.json"));

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
    const ewr::UpdateMetadata stable = ewr::ParseReleaseResponse(releases, "1.2.3", false);
    CHECK(stable.updateAvailable);
    CHECK(stable.latestVersion == "v1.3.0");
    CHECK(stable.releaseNotes == "Notes");

    // Opting in reaches the release candidate, but never the draft.
    const ewr::UpdateMetadata preview = ewr::ParseReleaseResponse(releases, "1.2.3", true);
    CHECK(preview.updateAvailable);
    CHECK(preview.latestVersion == "v2.0.0-rc.1");

    // Already up to date.
    CHECK(!ewr::ParseReleaseResponse(releases, "1.3.0", false).updateAvailable);

    // Malformed responses are non-events, not crashes.
    CHECK(!ewr::ParseReleaseResponse("<html>404</html>", "1.2.3", false).updateAvailable);
    CHECK(!ewr::ParseReleaseResponse("", "1.2.3", false).updateAvailable);
}

void test_updater_database_payload_validation()
{
    std::cout << "[TEST] test_updater_database_payload_validation" << std::endl;

    // The shipped database must pass the check the OTA sync applies.
    CHECK(ewr::ValidateDatabasePayload("database.json", ewr::kMaxSupportedDatabaseSchema));

    const std::string path = "test_ota_payload.json";
    auto write = [&](const std::string& body) {
        std::ofstream out(path);
        out << body;
    };

    write(R"({"L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));

    // Never install a database this build cannot read.
    write(R"({"schema_version": 99, "models": {"L222": {"wkey": "Yj4"}}})");
    CHECK(!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));

    // A 404 page, an empty model map, or entries without a write key are rejected.
    write("<html>Not Found</html>");
    CHECK(!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"models": {}})");
    CHECK(!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"L222": {"rkey": 8}})");
    CHECK(!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));

    fs::remove(path);
    CHECK(!ewr::ValidateDatabasePayload("no_such_file.json", ewr::kMaxSupportedDatabaseSchema));
}

void test_generator_local_db()
{
    std::cout << "[TEST] test_generator_local_db" << std::endl;
    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase("database.json");
    if (!loaded)
    {
        std::cout << "  [SKIP] no database file found in current working directory." << std::endl;
        return;
    }

    CHECK(!gen.IsEmpty());
    auto available = gen.GetAvailableModels();
    CHECK(!available.empty());
    std::cout << "  Loaded " << available.size() << " models." << std::endl;
}

void test_database_integrity()
{
    std::cout << "[TEST] test_database_integrity" << std::endl;
    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.GetAvailableModels();
    CHECK(models.size() > 1400);

    size_t withCounters = 0;
    size_t withoutCounters = 0;
    bool namesOk = true;
    bool keysOk = true;
    bool arraysOk = true;

    for (const auto& m : models)
    {
        if (m.name.empty())
            namesOk = false;

        if (!m.HasResettableCounters())
        {
            withoutCounters++;
            continue;
        }

        withCounters++;
        if (m.rkey == 0 || m.wkey.empty())
            keysOk = false;
        if (m.GetAllAddresses().size() != m.GetAllResetValues().size())
            arraysOk = false;
    }

    CHECK(namesOk);
    CHECK(keysOk);
    CHECK(arraysOk);
    CHECK(withCounters > 1000);

    std::cout << "  " << withCounters << " models with resettable counters, "
              << withoutCounters << " without (hidden in CLI)." << std::endl;
}

void test_packet_structure_integrity()
{
    std::cout << "[TEST] test_packet_structure_integrity" << std::endl;
    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.GetAvailableModels();
    CHECK(!models.empty());

    size_t inspected = 0;
    for (size_t idx = 0; idx < models.size() && inspected < 20; ++idx)
    {
        const auto& m = models[idx];
        auto addrs = m.GetAllAddresses();
        if (addrs.empty())
            continue;

        inspected++;
        auto seq = gen.GenerateSequence(m);

        CHECK(seq.size() == 3 + (addrs.size() * 3));
        CHECK(seq[0].size() == 27);
        CHECK(seq[1].size() == 8);
        CHECK(seq[2].size() == 17);

        for (size_t i = 0; i < addrs.size(); ++i)
        {
            const auto& writePkt = seq[3 + (i * 3) + 2];

            // The address field is 1 or 2 bytes wide depending on 'wlen'.
            const size_t addrLen = m.WriteAddressLength();

            CHECK(writePkt.size() == 16 + addrLen + m.wkey.size());
            CHECK(writePkt[0] == ewr::EpsonD4::SOCKET_EPSON_CTRL && writePkt[1] == ewr::EpsonD4::SOCKET_EPSON_CTRL);
            CHECK(((size_t)(writePkt[2] << 8) | writePkt[3]) == writePkt.size());
            CHECK(writePkt[4] == ewr::EpsonD4::CREDIT);
            CHECK(writePkt[6] == ewr::EpsonD4::PREFIX_PIPE && writePkt[7] == ewr::EpsonD4::PREFIX_PIPE);
            CHECK(((size_t)writePkt[8] | ((size_t)writePkt[9] << 8)) == 6 + addrLen + m.wkey.size());

            CHECK(writePkt[12] == ewr::EpsonD4::CMD_EEPROM_WRITE);
            CHECK(writePkt[13] == 0xBD);
            CHECK(writePkt[14] == 0x21);

            CHECK(ewr::IsWritePacket(writePkt));
        }
    }

    CHECK(inspected > 0);
}

void test_byte_parity()
{
    std::cout << "[TEST] test_byte_parity" << std::endl;
    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.GetAvailableModels();
    CHECK(models.size() > 1400);

    size_t verifiedModels = 0;
    size_t narrowModels = 0;
    size_t totalPackets = 0;
    bool allEqual = true;
    bool narrowFormOk = true;
    bool narrowDiverged = false;

    for (const auto& m : models)
    {
        auto gotSeq = gen.GenerateSequence(m);

        // Models with a 1-byte address field (wlen == 1, e.g. R220) are the one
        // deliberate break from the legacy generator, which always wrote a
        // 2-byte address those printers reject. Everything else must stay
        // byte-for-byte identical to the hardware-verified output.
        if (m.WriteAddressLength() != 2)
        {
            narrowModels++;

            if (gotSeq != legacy::GenerateSequence(m))
                narrowDiverged = true;

            for (const auto& pkt : gotSeq)
            {
                if (!ewr::IsWritePacket(pkt))
                    continue;

                if (pkt.size() != 17 + m.wkey.size())
                    narrowFormOk = false;

                totalPackets++;
            }
            continue;
        }

        auto wantSeq = legacy::GenerateSequence(m);

        if (gotSeq.size() != wantSeq.size())
        {
            allEqual = false;
            continue;
        }

        for (size_t p = 0; p < gotSeq.size(); ++p)
        {
            if (gotSeq[p] != wantSeq[p])
                allEqual = false;
            totalPackets++;
        }
        verifiedModels++;
    }

    CHECK(allEqual);
    CHECK(narrowFormOk);
    CHECK(narrowModels > 0);
    CHECK(narrowDiverged);
    CHECK(verifiedModels + narrowModels == models.size());
    std::cout << "  Byte parity verified across " << verifiedModels << " models ("
              << totalPackets << " packets vs. hardware reference), plus "
              << narrowModels << " models on the corrected 1-byte address form." << std::endl;
}

void test_executor_success_path()
{
    std::cout << "[TEST] test_executor_success_path" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? OkAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(result.success);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(result.writesRejected == 0);
    CHECK(result.packetsSent == seq.size());
    CHECK(result.error.empty());
}

void test_executor_rejects_handshake_only_chatter()
{
    std::cout << "[TEST] test_executor_rejects_handshake_only_chatter" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesVerified == 0);
    CHECK(!result.error.empty());
}

void test_executor_fails_on_ng_reply_without_retry()
{
    std::cout << "[TEST] test_executor_fails_on_ng_reply_without_retry" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? NgAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

void test_executor_retries_missing_ack_with_credit_resend()
{
    std::cout << "[TEST] test_executor_retries_missing_ack_with_credit_resend" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    std::map<std::vector<unsigned char>, int> writeAttempts;
    t.replyFor = [&writeAttempts](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::IsWritePacket(pkt))
            return HandshakeAck();
        int attempt = ++writeAttempts[pkt];
        return attempt >= 2 ? OkAck() : std::vector<unsigned char>{};
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(result.success);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);

    CHECK(t.sent.size() == 15);

    for (const auto& entry : writeAttempts)
        CHECK(entry.second == 2);
}

void test_executor_event_stream_contract()
{
    std::cout << "[TEST] test_executor_event_stream_contract" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? OkAck() : HandshakeAck();
    };

    // The GUI contract: consume a full run as structured events - stable
    // codes, ordered progress, wire detail as Trace - with no streams at all.
    std::vector<ewr::log::Event> events;
    ewr::log::Reporter reporter;
    reporter.AddSink([&events](const ewr::log::Event& e) { events.push_back(e); });

    auto result = ewr::ExecuteSequence(t, seq, reporter, FastOptions());

    CHECK(result.success);
    CHECK(result.writesTotal == 2);

    std::vector<ewr::log::Event> info;
    std::vector<ewr::log::Event> trace;
    for (const auto& e : events)
    {
        if (e.level == ewr::log::Level::Info)
            info.push_back(e);
        else if (e.level == ewr::log::Level::Trace)
            trace.push_back(e);
    }

    // A clean run renders exactly one Info line per packet: handshake and
    // credit packets acknowledge, write packets verify. Every event carries
    // machine-readable progress against the full sequence length, in order.
    CHECK(info.size() == seq.size());

    size_t verified = 0;
    size_t acked = 0;
    bool indicesOrdered = true;
    int lastIndex = 0;
    for (const auto& e : info)
    {
        CHECK(e.stage == ewr::log::Stage::Write);
        CHECK(e.HasProgress());
        CHECK(e.total == static_cast<int>(seq.size()));

        if (e.code == "exec.write_verified")
        {
            verified++;
            CHECK(e.message.find("EEPROM write verified") != std::string::npos);
        }
        else
        {
            CHECK(e.code == "exec.packet_acked");
            acked++;
        }

        if (e.index <= lastIndex)
            indicesOrdered = false;
        lastIndex = e.index;
    }

    CHECK(verified == 2);
    CHECK(acked == seq.size() - 2);
    CHECK(indicesOrdered);

    // Every packet produces a tx and an rx trace event: the trace file's
    // hex detail is fully reconstructible from the event stream.
    size_t txCount = 0;
    size_t rxCount = 0;
    for (const auto& e : trace)
    {
        if (e.code == "exec.tx")
            txCount++;
        else if (e.code == "exec.rx")
            rxCount++;
    }

    CHECK(txCount == seq.size());
    CHECK(rxCount == seq.size());
}

void test_executor_fails_on_zero_write_sequence()
{
    std::cout << "[TEST] test_executor_fails_on_zero_write_sequence" << std::endl;

    ewr::DbPrinterModel emptyModel;
    emptyModel.name = "EmptyModel";
    emptyModel.rkey = 1;
    emptyModel.wkey = "AAAAAAAA";

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(emptyModel);
    CHECK(seq.size() == 3);

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesTotal == 0);
    CHECK(!result.error.empty());
}

void test_executor_fails_on_transport_error()
{
    std::cout << "[TEST] test_executor_fails_on_transport_error" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.failSend = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.packetsSent == 0);
    CHECK(!result.error.empty());
}

void test_executor_handshake_failfast_on_silence()
{
    std::cout << "[TEST] test_executor_handshake_failfast_on_silence" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t; // total silence: the device never replies

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(!result.handshakeConfirmed);
    // Fail fast: the 5 handshake/credit packets go out, but not a single
    // EEPROM write is blasted at a device that never opened the channel.
    CHECK(t.sent.size() == 5);
    CHECK(result.writesTotal == 0);
    CHECK(result.error.find("IEEE 1284.4") != std::string::npos);
}

void test_executor_handshake_failfast_on_chatter()
{
    std::cout << "[TEST] test_executor_handshake_failfast_on_chatter" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    // The device talks (credit-grant chatter) but never actually opens the
    // channel - the classic wrong-interface signature. Must still fail fast.
    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(!result.handshakeConfirmed);
    CHECK(t.sent.size() == 5);
    CHECK(result.writesTotal == 0);
}

void test_executor_handshake_validation_success()
{
    std::cout << "[TEST] test_executor_handshake_validation_success" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? OkAck() : OpenChannelAck();
    };

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(!result.handshakeFailed);
    CHECK(result.writesVerified == 2);
    CHECK(result.error.empty());
}

void test_executor_drain_timeout_selection()
{
    std::cout << "[TEST] test_executor_drain_timeout_selection" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? OkAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.handshakeDrainTimeoutMs = 1234;
    options.writeAckTimeoutMs = 777;
    options.drainTimeoutMs = 55;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(result.success);
    CHECK(t.drainTimeouts.size() == 9);

    if (t.drainTimeouts.size() == 9)
    {
        // Every packet before the first EEPROM write covers the EJL->D4 mode
        // switch and channel-open handshake: they get the long patience window.
        for (size_t i = 0; i < 5; ++i)
            CHECK(t.drainTimeouts[i] == 1234);

        // EEPROM writes wait for their ACK; other packets use the short settle window.
        CHECK(t.drainTimeouts[5] == 777);
        CHECK(t.drainTimeouts[6] == 55);
        CHECK(t.drainTimeouts[7] == 55);
        CHECK(t.drainTimeouts[8] == 777);
    }
}

void test_executor_retry_without_credit_resend()
{
    std::cout << "[TEST] test_executor_retry_without_credit_resend" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    std::map<std::vector<unsigned char>, int> writeAttempts;
    t.replyFor = [&writeAttempts](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::IsWritePacket(pkt))
            return HandshakeAck();
        int attempt = ++writeAttempts[pkt];
        return attempt >= 2 ? OkAck() : std::vector<unsigned char>{};
    };

    auto options = FastOptions();
    options.resendCreditOnRetry = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(result.success);
    CHECK(result.writesVerified == 2);
    // 9 sequence packets + 1 bare retry per write, with NO credit re-send.
    CHECK(t.sent.size() == 11);
}

void test_executor_replay_relaxed_verification()
{
    std::cout << "[TEST] test_executor_replay_relaxed_verification" << std::endl;

    // A replay dump with an unknown shape: no recognizable write packets.
    ewr::DbPrinterModel emptyModel;
    emptyModel.name = "ReplayModel";
    emptyModel.rkey = 1;
    emptyModel.wkey = "AAAAAAAA";

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(emptyModel); // 3 handshake packets, 0 writes

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    CHECK(result.success);
    CHECK(result.writesTotal == 0);
    CHECK(result.error.empty());
}

void test_executor_replay_silence_still_fails()
{
    std::cout << "[TEST] test_executor_replay_silence_still_fails" << std::endl;

    ewr::DbPrinterModel emptyModel;
    emptyModel.name = "ReplayModel";
    emptyModel.rkey = 1;
    emptyModel.wkey = "AAAAAAAA";

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(emptyModel);

    FakeTransport t; // device never says anything at all

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    // Even in relaxed replay mode, a dead-silent device is never a SUCCESS.
    CHECK(!result.success);
    CHECK(!result.error.empty());
}

void test_executor_replay_ng_still_fatal()
{
    std::cout << "[TEST] test_executor_replay_ng_still_fatal" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? NgAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    // An explicit :42:NG; rejection aborts immediately even in replay mode.
    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

void test_generator_respects_mem_high()
{
    std::cout << "[TEST] test_generator_respects_mem_high" << std::endl;

    ewr::DbPrinterModel m = MakeTestModel();
    m.mem_high = 0x0018; // the second address (0x0019) is now out of range

    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(m);

    ewr::log::Default().RemoveSink(sinkId);

    // 3 handshake packets + one triplet for the single in-range address.
    CHECK(seq.size() == 6);

    size_t writes = 0;
    for (const auto& pkt : seq)
        if (ewr::IsWritePacket(pkt))
            writes++;
    CHECK(writes == 1);

    if (seq.size() == 6)
    {
        CHECK(seq[5][15] == 0x18);
        CHECK(seq[5][16] == 0x00);
    }

    CHECK(captured.find("mem_high") != std::string::npos);
}

void test_database_load_skips_malformed_entries()
{
    std::cout << "[TEST] test_database_load_skips_malformed_entries" << std::endl;

    const std::string path = "test_malformed_db.json";
    {
        std::ofstream out(path);
        out << R"({"GoodModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24], "reset": [0]},)"
            << R"( "BadModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": ["not-a-number"], "reset": [0]},)"
            << R"( "GoodModel2": {"rkey": 8, "wkey": "Arkanoid", "addresses": [25], "reset": [0]}})";
    }

    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(path);

    ewr::log::Default().RemoveSink(sinkId);
    fs::remove(path);

    // One rotten entry must not poison the other 1400+ models.
    CHECK(loaded);
    CHECK(gen.GetAvailableModels().size() == 2);
    CHECK(captured.find("BadModel") != std::string::npos);
}

void test_database_reload_replaces_previous_contents()
{
    std::cout << "[TEST] test_database_reload_replaces_previous_contents" << std::endl;

    const std::string pathA = "test_reload_a.json";
    const std::string pathB = "test_reload_b.json";
    {
        std::ofstream out(pathA);
        out << R"({"ModelA": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24], "reset": [0]}})";
    }
    {
        std::ofstream out(pathB);
        out << R"({"ModelB": {"rkey": 8, "wkey": "Arkanoid", "addresses": [25], "reset": [0]}})";
    }

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase(pathA));
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1 && models[0].name == "ModelA");

    // A reload (e.g. after an OTA sync) fully replaces the old contents.
    CHECK(gen.LoadDatabase(pathB));
    models = gen.GetAvailableModels();
    CHECK(models.size() == 1 && models[0].name == "ModelB");

    // A failed reload leaves the previous database untouched.
    CHECK(!gen.LoadDatabase("no_such_db_file.json"));
    models = gen.GetAvailableModels();
    CHECK(models.size() == 1 && models[0].name == "ModelB");

    fs::remove(pathA);
    fs::remove(pathB);
}

void test_executor_na_refusal_fails_without_retry()
{
    std::cout << "[TEST] test_executor_na_refusal_fails_without_retry" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    // The printer answers the handshake normally but refuses every EEPROM
    // write with ':42:NA;' (locked by another error, e.g. empty cartridge).
    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? NaAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    // Deterministic refusal: no retries, no credit re-sends - abort at the
    // first write (5 handshake packets + 1 refused write).
    CHECK(t.sent.size() == 6);
    CHECK(result.error.find(":42:NA;") != std::string::npos);
    CHECK(result.error.find("locked") != std::string::npos);
}

void test_executor_na_refusal_fatal_in_replay_mode()
{
    std::cout << "[TEST] test_executor_na_refusal_fatal_in_replay_mode" << std::endl;

    ewr::UniversalGenerator gen;
    auto seq = gen.GenerateSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? NaAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, seq, out, log, options);

    // Even in relaxed replay mode an explicit refusal is never a SUCCESS.
    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

// ---------------------------------------------------------------------------
// Status / read-back fixtures
// ---------------------------------------------------------------------------

static std::vector<unsigned char> WrapD4Data(const std::vector<unsigned char>& payload)
{
    std::vector<unsigned char> pkt;
    const uint16_t len = static_cast<uint16_t>(payload.size() + 6);
    pkt.push_back(0x02);
    pkt.push_back(0x02);
    pkt.push_back((len >> 8) & 0xFF);
    pkt.push_back(len & 0xFF);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// A synthetic but structurally faithful '@BDC ST2' status reply:
// state ERROR, error INK OUT, maintenance box 90%, Black 42% + Yellow missing,
// serial X7A9000123.
static std::vector<unsigned char> MakeSt2Reply()
{
    std::vector<unsigned char> body;
    body.insert(body.end(), { 0x01, 0x01, 0x00 });                               // state: ERROR
    body.insert(body.end(), { 0x02, 0x01, 0x05 });                               // error: INK OUT
    body.insert(body.end(), { 0x0D, 0x01, 90 });                                 // maintenance box: 90
    body.insert(body.end(), { 0x0F, 0x07, 0x03, 0x00, 0x00, 42, 0x00, 0x03, 110 }); // inks: Black 42, Yellow missing

    const std::string serial = "X7A9000123";
    body.push_back(0x40);
    body.push_back(static_cast<unsigned char>(serial.size()));
    body.insert(body.end(), serial.begin(), serial.end());

    std::vector<unsigned char> payload;
    const std::string prefix = "@BDC ST2\r\n";
    payload.insert(payload.end(), prefix.begin(), prefix.end());
    payload.push_back(static_cast<unsigned char>(body.size() & 0xFF));
    payload.push_back(static_cast<unsigned char>((body.size() >> 8) & 0xFF));
    payload.insert(payload.end(), body.begin(), body.end());

    return WrapD4Data(payload);
}

static std::vector<unsigned char> MakeEepromReadReply(uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nrw:41:";
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

// R220-style reply: the firmware echoes the address, then the value.
static std::vector<unsigned char> MakeEepromReadReplyEE(uint8_t addr, uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nEE:";
    s += hex[(addr >> 4) & 0x0F];
    s += hex[addr & 0x0F];
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

void test_status_reply_parsing()
{
    std::cout << "[TEST] test_status_reply_parsing" << std::endl;

    const auto st = ewr::ParseStatusReply(MakeSt2Reply());

    CHECK(st.valid);
    CHECK(st.stateCode == 0x00);
    CHECK(st.stateName == "ERROR");
    CHECK(st.hasError);
    CHECK(st.errorCode == 0x05);
    CHECK(st.errorName == "INK OUT");
    CHECK(st.maintenanceBoxLevel == 90);
    CHECK(st.maintenanceBoxText == "OK");
    CHECK(st.inks.size() == 2);

    if (st.inks.size() == 2)
    {
        CHECK(st.inks[0].colorName == "Black");
        CHECK(st.inks[0].level == 42);
        CHECK(st.inks[0].statusText == "OK");
        CHECK(st.inks[1].colorName == "Yellow");
        CHECK(st.inks[1].level == -1);
        CHECK(st.inks[1].statusText == "MISSING");
    }

    CHECK(st.serial == "X7A9000123");
    CHECK(ewr::DescribePrinterCondition(st).find("INK OUT") != std::string::npos);

    // Streams that never contain '@BDC ST2' parse to valid == false.
    CHECK(!ewr::ParseStatusReply(HandshakeAck()).valid);
    CHECK(!ewr::ParseStatusReply({}).valid);
}

void test_d4_payload_extraction()
{
    std::cout << "[TEST] test_d4_payload_extraction" << std::endl;

    const std::vector<unsigned char> inner = { 'H', 'i', '!' };
    const auto pkt = WrapD4Data(inner);

    // Clean stream.
    CHECK(ewr::ExtractD4Payload(pkt) == inner);

    // Stray NAK byte before the packet (the ET-2803 mi_00 chatter): resync.
    std::vector<unsigned char> noisy = { 0x15 };
    noisy.insert(noisy.end(), pkt.begin(), pkt.end());
    CHECK(ewr::ExtractD4Payload(noisy) == inner);

    // A transaction-channel packet is skipped; the data packet after it is kept.
    std::vector<unsigned char> mixed = HandshakeAck();
    mixed.insert(mixed.end(), pkt.begin(), pkt.end());
    CHECK(ewr::ExtractD4Payload(mixed) == inner);
}

void test_eeprom_read_reply_parsing()
{
    std::cout << "[TEST] test_eeprom_read_reply_parsing" << std::endl;

    uint8_t value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReply(0x5A), value));
    CHECK(value == 0x5A);

    value = 0xFF;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReply(0x00), value));
    CHECK(value == 0x00);

    // A write ack (':42:OK;') is not a read reply.
    CHECK(!ewr::ParseEepromReadReply(OkAck(), value));
    CHECK(!ewr::ParseEepromReadReply({}, value));

    // '@BDC PS' prefix without a ':41:' echo is rejected too.
    const std::string s = "@BDC PS\r\nrw:42:OK;";
    CHECK(!ewr::ParseEepromReadReply(WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())), value));

    // R220-style 'EE:AAVV;' form: address echo plus value (seen on real
    // Stylus Photo R220 hardware, trace 2026-08-03).
    value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0x0C, 0x1C), value));
    CHECK(value == 0x1C);

    // Address echo is validated when the caller provides the expected address.
    value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0x3E, 0x0C), value, 0x3E));
    CHECK(value == 0x0C);

    // A mismatching address echo is rejected.
    CHECK(!ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0x0D, 0x39), value, 0x2B));

    // Expected addresses above one byte skip the echo comparison (form B
    // only echoes a single byte).
    value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0xFC, 0x07), value, 0x2FC));
    CHECK(value == 0x07);

    // Truncated 'EE:' bodies are rejected.
    const std::string t = "@BDC PS\r\nEE:0C";
    CHECK(!ewr::ParseEepromReadReply(WrapD4Data(std::vector<unsigned char>(t.begin(), t.end())), value));
}

// ---------------------------------------------------------------------------
// ewr::Session facade (session.h): blocker policy and the reset lifecycle
// ---------------------------------------------------------------------------

// Scripted device gateway: drives the whole Session lifecycle with no USB.
struct FakeGateway : ewr::IDeviceGateway
{
    ewr::QueryRunResult queryResult;
    ewr::ResetRunResult resetResult;
    int queryCalls = 0;
    int resetCalls = 0;
    std::vector<std::vector<unsigned char>> lastSequence;
    ewr::ExecutorOptions lastResetOptions;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        queryCalls++;
        return queryResult;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>& sequence,
                                 const ewr::ExecutorOptions& options) override
    {
        resetCalls++;
        lastSequence = sequence;
        lastResetOptions = options;
        return resetResult;
    }
};

static ewr::DbPrinterModel MakeSessionModel()
{
    ewr::DbPrinterModel model;
    model.name = "TestJet 100";
    model.rkey = 0x3B10;
    model.wkey = "McLaren";
    model.rlen = 1;
    model.wlen = 1;

    ewr::PadGroup group;
    group.description = "Waste counter";
    group.addresses = { 0x0C, 0x0D };
    group.reset_values = { 0x00, 0x00 };
    model.pad_groups.push_back(group);

    return model;
}

// A gateway whose printer answers with INK OUT (a foreign lock) and whose
// counters already read back as the reset values.
static FakeGateway MakeSessionGateway()
{
    FakeGateway gw;
    gw.queryResult.deviceFound = true;
    gw.queryResult.query.success = true;
    gw.queryResult.query.replies = {
        MakeSt2Reply(), // state ERROR, INK OUT (0x05)
        MakeEepromReadReplyEE(0x0C, 0x00),
        MakeEepromReadReplyEE(0x0D, 0x00),
    };
    gw.resetResult.deviceFound = true;
    gw.resetResult.exec.success = true;
    return gw;
}

void test_evaluate_blocker()
{
    std::cout << "[TEST] test_evaluate_blocker" << std::endl;

    ewr::PrinterStatus status;

    // Unparseable or error-free statuses never block.
    CHECK(!ewr::EvaluateBlocker(status).has_value());
    status.valid = true;
    CHECK(!ewr::EvaluateBlocker(status).has_value());

    // The waste-pad errors EWR exists to clear must never block.
    status.hasError = true;
    status.errorCode = 0x10; // SERVICE REQUEST
    status.errorName = "SERVICE REQUEST";
    CHECK(ewr::IsExpectedWastePadError(0x10));
    CHECK(!ewr::EvaluateBlocker(status).has_value());

    status.errorCode = 0x2C; // CARTRIDGE OVERFLOW
    CHECK(ewr::IsExpectedWastePadError(0x2C));
    CHECK(!ewr::EvaluateBlocker(status).has_value());

    // A foreign lock (ink out, jam, open cover) blocks and carries the error.
    status.errorCode = 0x05;
    status.errorName = "INK OUT";
    CHECK(!ewr::IsExpectedWastePadError(0x05));
    const std::optional<ewr::Blocker> blocker = ewr::EvaluateBlocker(status);
    CHECK(blocker.has_value());
    if (blocker.has_value())
    {
        CHECK(blocker->errorCode == 0x05);
        CHECK(blocker->errorName == "INK OUT");
        CHECK(!blocker->explanation.empty());
    }
}

void test_session_reset_lifecycle()
{
    std::cout << "[TEST] test_session_reset_lifecycle" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter; // no sinks: a silent host

    // 1) The host declines the blocker: aborted before any write.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        int preflights = 0;
        int decisions = 0;
        ewr::ResetHandlers handlers;
        handlers.onPreflight = [&](const ewr::StateSnapshot& s)
        {
            preflights++;
            CHECK(s.available);
            CHECK(s.status.errorCode == 0x05);
            CHECK(s.values.size() == 2);
        };
        handlers.onBlocker = [&](const ewr::Blocker& b)
        {
            decisions++;
            CHECK(b.errorName == "INK OUT");
            return false;
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(!out.success);
        CHECK(preflights == 1);
        CHECK(decisions == 1);
        CHECK(gw.resetCalls == 0); // nothing was written
        CHECK(out.before.available);
    }

    // 2) The host accepts the blocker: the reset runs and verifies clean.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        int verifies = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        handlers.onVerify = [&](const ewr::StateSnapshot& s)
        {
            verifies++;
            CHECK(s.values.size() == 2);
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(out.success);
        CHECK(out.committed); // no close_ops: nothing to commit
        CHECK(gw.resetCalls == 1);
        CHECK(!gw.lastSequence.empty());
        CHECK(gw.lastResetOptions.writeKey == "McLaren");
        CHECK(gw.lastResetOptions.verifyWrites);
        CHECK(gw.lastResetOptions.validateHandshake);
        CHECK(gw.lastResetOptions.useSessionLayer);
        CHECK(out.verificationRan);
        CHECK(out.verifyMismatches == 0);
        CHECK(out.verifyUnread == 0);
        CHECK(verifies == 1);
        CHECK(gw.queryCalls == 2); // preflight + read-back
    }

    // 3) No decision callback at all: any blocker aborts (the safe default).
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        const ewr::ResetOutcome out = session.Reset();
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(gw.resetCalls == 0);
    }

    // 4) Preflight unavailable: proceed (no blocker known), skip verification.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.queryResult.deviceFound = false;
        gw.queryResult.query.replies.clear();
        ewr::Session session(model, gw, reporter);

        int decisions = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker&) { decisions++; return false; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(out.success);
        CHECK(decisions == 0);
        CHECK(gw.resetCalls == 1);
        CHECK(!out.verificationRan);
    }

    // 5) The device disappears at write time.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.resetResult.deviceFound = false;
        ewr::Session session(model, gw, reporter);

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::DeviceNotFound);
        CHECK(!out.success);
        CHECK(!out.error.empty());
    }
}

void test_session_conflict_gate()
{
    std::cout << "[TEST] test_session_conflict_gate" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.conflict = true;
    ewr::log::Reporter reporter; // silent host

    // 1) The host tells the two blocker kinds apart and declines the
    //    conflict: aborted after the status decision, before any write.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        int statusDecisions = 0;
        int conflictDecisions = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b)
        {
            if (b.errorCode >= 0)
            {
                statusDecisions++;
                CHECK(b.errorName == "INK OUT");
                return true; // the ink error is accepted...
            }

            conflictDecisions++;
            CHECK(b.errorName == "DATABASE CONFLICT");
            CHECK(b.errorCode == -1);
            CHECK(!b.explanation.empty());
            return false; // ...but the database conflict is not.
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(!out.success);
        CHECK(statusDecisions == 1);
        CHECK(conflictDecisions == 1);
        CHECK(gw.resetCalls == 0); // nothing was written
        CHECK(!out.error.empty());
    }

    // 2) The host accepts both decisions: the reset runs as for a clean model.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(out.success);
        CHECK(gw.resetCalls == 1);
    }

    // 3) No handler and no preflight either: the conflict alone still gates
    //    the run - a silent host never writes to a flagged model.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.queryResult.deviceFound = false;
        gw.queryResult.query.replies.clear();
        ewr::Session session(model, gw, reporter);

        const ewr::ResetOutcome out = session.Reset();
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(gw.resetCalls == 0);
    }

    // 4) Flag off: no conflict decision is ever requested.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::DbPrinterModel clean = MakeSessionModel();
        ewr::Session session(clean, gw, reporter);

        int conflictDecisions = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b)
        {
            if (b.errorCode < 0)
                conflictDecisions++;
            return true;
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(conflictDecisions == 0);
    }
}

void test_query_session_failfast_on_silence()
{
    std::cout << "[TEST] test_query_session_failfast_on_silence" << std::endl;

    FakeTransport t; // never replies (the ET-2803 mi_01 behavior)

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteQuerySession(
        t,
        ewr::UniversalGenerator::GenerateHandshake(),
        { ewr::UniversalGenerator::GenerateStatusQueryPacket() },
        out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(!result.handshakeConfirmed);
    // Fail fast: only the 3 handshake packets, the query is never sent.
    CHECK(t.sent.size() == 3);
    CHECK(result.error.find("1284.4") != std::string::npos);
}

void test_query_session_happy_path()
{
    std::cout << "[TEST] test_query_session_happy_path" << std::endl;

    const auto st2 = MakeSt2Reply();
    const auto readReply = MakeEepromReadReply(0x00);

    FakeTransport t;
    t.replyFor = [&](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (pkt.size() > 6 && pkt[0] == 0x00 && pkt[6] == 0x01)
            return OpenChannelAck(); // OpenChannel -> confirm the handshake
        if (pkt.size() > 8 && pkt[0] == 0x02 && pkt[1] == 0x02)
            return pkt[6] == 's' ? st2 : readReply; // data packets: 'st' query vs EEPROM read
        return HandshakeAck(); // EJL / Init / credit traffic
    };

    const ewr::DbPrinterModel model = MakeTestModel();

    std::vector<std::vector<unsigned char>> queries;
    queries.push_back(ewr::UniversalGenerator::GenerateStatusQueryPacket());
    queries.push_back(ewr::UniversalGenerator::GenerateReadPacket(model.rkey, 0x0018));

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteQuerySession(
        t, ewr::UniversalGenerator::GenerateHandshake(), queries, out, log, FastOptions());

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(!result.handshakeFailed);
    CHECK(result.replies.size() == 2);
    // 3 handshake + (credit grant + credit request + query) x 2 = 9 packets.
    CHECK(t.sent.size() == 9);

    if (result.replies.size() == 2)
    {
        const auto st = ewr::ParseStatusReply(result.replies[0]);
        CHECK(st.valid);
        CHECK(st.hasError && st.errorCode == 0x05);

        uint8_t value = 0xFF;
        CHECK(ewr::ParseEepromReadReply(result.replies[1], value));
        CHECK(value == 0x00);
    }
}

// ---- IEEE 1284.4 session core ----

// Returns queued chunks one Drain() at a time, so tests can split one
// packet across reads or coalesce several packets into one read.
class ChunkedTransport final : public ewr::ITransport
{
public:
    std::vector<std::vector<unsigned char>> chunks;
    size_t next = 0;

    bool Send(const std::vector<unsigned char>&) override { return true; }

    std::vector<unsigned char> Drain(int) override
    {
        if (next >= chunks.size())
            return {};
        return chunks[next++];
    }
};

static std::vector<unsigned char> D4Frame(unsigned char psid, unsigned char ssid,
                                          const std::vector<unsigned char>& payload,
                                          unsigned char credit = 0x00, unsigned char control = 0x00)
{
    std::vector<unsigned char> raw;
    const size_t total = payload.size() + 6;
    raw.push_back(psid);
    raw.push_back(ssid);
    raw.push_back(static_cast<unsigned char>((total >> 8) & 0xFF));
    raw.push_back(static_cast<unsigned char>(total & 0xFF));
    raw.push_back(credit);
    raw.push_back(control);
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

// Answers the D4 session choreography like a real R220: Init, GetSocketID,
// OpenChannel (MTU to printer configurable, MTU to host 256, no initial
// credit), credit grants of 1, close/exit. Data packets go to dataReplyFor.
static std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)>
D4SessionReplier(unsigned char socket,
                 std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> dataReplyFor,
                 bool answerGetSocketId = true,
                 unsigned char mtuHi = 0x00, unsigned char mtuLo = 0x40)
{
    return [socket, dataReplyFor, answerGetSocketId, mtuHi, mtuLo](
               const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        if (sent.size() < 7)
            return {};

        // EJL enter blob: transaction header followed by the '@EJL' magic.
        if (sent[0] == 0x00 && sent[1] == 0x00 && sent[6] == '@')
            return D4Frame(0x00, 0x00, { 0xC5, 0x00 }, 0x01);

        if (sent[0] == 0x00 && sent[1] == 0x00)
        {
            switch (sent[6])
            {
                case 0x00: return D4Frame(0x00, 0x00, { 0x80, 0x00, 0x10 }, 0x01);
                case 0x09:
                    if (!answerGetSocketId)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x89, 0x00, socket }, 0x01);
                case 0x01:
                    return D4Frame(0x00, 0x00,
                                   { 0x81, 0x00, socket, socket, mtuHi, mtuLo, 0x01, 0x00, 0x00, 0x00 }, 0x01);
                case 0x03: return D4Frame(0x00, 0x00, { 0x83, 0x00, socket, socket }, 0x01);
                case 0x04: return D4Frame(0x00, 0x00, { 0x84, 0x00, socket, socket, 0x00, 0x01 }, 0x01);
                case 0x02: return D4Frame(0x00, 0x00, { 0x82, 0x00, socket, socket }, 0x01);
                case 0x08: return D4Frame(0x00, 0x00, { 0x88, 0x00 }, 0x01);
                default:   return {};
            }
        }

        return dataReplyFor ? dataReplyFor(sent) : std::vector<unsigned char>{};
    };
}

void test_d4_framer_length_framing()
{
    std::cout << "[TEST] test_d4_framer_length_framing" << std::endl;

    std::ofstream log = NullLog();

    // A reply split across two USB reads reassembles into one packet.
    {
        ChunkedTransport t;
        const auto pkt = D4Frame(0x02, 0x02, { '@', 'B', 'D', 'C', ' ', 'P', 'S' }, 0x00, 0x01);
        t.chunks.push_back(std::vector<unsigned char>(pkt.begin(), pkt.begin() + 5));
        t.chunks.push_back(std::vector<unsigned char>(pkt.begin() + 5, pkt.end()));

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(framer.ReadPacket(out, 100, log));
        CHECK(out.psid == 0x02);
        CHECK(out.control == 0x01);
        CHECK(out.payload.size() == 7);
        CHECK(!framer.HasBufferedData());
    }

    // Two packets arriving in one read come out one at a time.
    {
        ChunkedTransport t;
        auto burst = D4Frame(0x00, 0x00, { 0x83, 0x00, 0x02, 0x02 }, 0x01);
        const auto second = D4Frame(0x02, 0x02, { 'O', 'K' });
        burst.insert(burst.end(), second.begin(), second.end());
        t.chunks.push_back(burst);

        ewr::D4Framer framer(t);
        ewr::D4Packet first, next;
        CHECK(framer.ReadPacket(first, 100, log));
        CHECK(first.IsTransaction());
        CHECK(framer.HasBufferedData());
        CHECK(framer.ReadPacket(next, 0, log)); // already buffered, no wait
        CHECK(!next.IsTransaction());
        CHECK(next.payload.size() == 2);
    }

    // A corrupt header (announced length < 6) must not wedge the framer.
    {
        ChunkedTransport t;
        t.chunks.push_back({ 0x02, 0x02, 0x00, 0x03, 0x00, 0x00 });

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(!framer.ReadPacket(out, 50, log));
        CHECK(!framer.HasBufferedData());
    }
}

void test_d4_session_start_negotiates_socket_and_mtu()
{
    std::cout << "[TEST] test_d4_session_start_negotiates_socket_and_mtu" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x04, nullptr);

    std::ofstream log = NullLog();
    ewr::D4SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;

    ewr::D4Session session(t, log, options);
    CHECK(session.Start());
    CHECK(session.ChannelOpen());
    CHECK(session.Socket() == 0x04);         // GetSocketID honored, not hardcoded 2
    CHECK(session.MtuToPrinter() == 0x0040); // negotiated from the OpenChannel reply
    CHECK(session.MtuToHost() == 0x0100);
    CHECK(session.SendCredit() == 0);        // no initial credit granted

    // OpenChannel went to the resolved socket.
    bool openToSocket4 = false;
    for (const auto& sent : t.sent)
        if (sent.size() >= 9 && sent[0] == 0x00 && sent[6] == 0x01 && sent[7] == 0x04 && sent[8] == 0x04)
            openToSocket4 = true;
    CHECK(openToSocket4);
}

void test_d4_session_socketid_fallback()
{
    std::cout << "[TEST] test_d4_session_socketid_fallback" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, nullptr, /*answerGetSocketId*/ false);

    std::ofstream log = NullLog();
    ewr::D4SessionOptions options;
    options.replyTimeoutMs = 30; // GetSocketID times out fast in tests
    options.dataTimeoutMs = 30;

    ewr::D4Session session(t, log, options);
    CHECK(session.Start());
    CHECK(session.ChannelOpen());
    CHECK(session.Socket() == ewr::EpsonD4::SOCKET_EPSON_CTRL);
}

void test_d4_session_credit_gating_and_chunking()
{
    std::cout << "[TEST] test_d4_session_credit_gating_and_chunking" << std::endl;

    FakeTransport t;
    // MTU to printer = 13 bytes -> 7 payload bytes per data packet.
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) {
        return D4Frame(0x02, 0x02, { 'A', 'C', 'K' }, 0x00, 0x01);
    }, true, 0x00, 0x0D);

    std::ofstream log = NullLog();
    ewr::D4SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;

    ewr::D4Session session(t, log, options);
    CHECK(session.Start());
    CHECK(session.MtuToPrinter() == 0x000D);

    // 11-byte payload with a 7-byte budget -> exactly 2 chunks.
    std::vector<unsigned char> reply;
    CHECK(session.Exchange(std::vector<unsigned char>(11, 0xAB), reply));
    CHECK(reply.size() == 3);

    int dataPackets = 0;
    int creditRequests = 0;
    for (const auto& sent : t.sent)
    {
        if (sent.size() >= 7 && sent[0] == 0x02 && sent[1] == 0x02)
        {
            dataPackets++;
            CHECK(sent.size() <= 0x0D); // chunks respect the negotiated MTU
        }
        if (sent.size() >= 7 && sent[0] == 0x00 && sent[6] == 0x04)
            creditRequests++;
    }
    CHECK(dataPackets == 2);
    CHECK(creditRequests == 2); // zero initial credit: one grant per chunk
    CHECK(session.SendCredit() == 0);
}

void test_d4_query_session_layer()
{
    std::cout << "[TEST] test_d4_query_session_layer" << std::endl;

    const auto st2 = MakeSt2Reply();

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [st2](const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        if (sent.size() > 7 && sent[6] == 's' && sent[7] == 't')
            return st2;
        return MakeEepromReadReplyEE(0x18, 0x2A);
    });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteQuerySession(
        t,
        ewr::UniversalGenerator::GenerateHandshake(),
        { ewr::UniversalGenerator::GenerateStatusQueryPacket(),
          ewr::UniversalGenerator::GenerateReadPacket(0x0008, 0x18) },
        out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.replies.size() == 2);
    CHECK(ewr::ExtractD4Payload(result.replies[0]) == ewr::ExtractD4Payload(st2));

    uint8_t value = 0;
    CHECK(ewr::ParseEepromReadReply(result.replies[1], value, 0x18));
    CHECK(value == 0x2A);
}

void test_d4_sequence_write_verified()
{
    std::cout << "[TEST] test_d4_sequence_write_verified" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) { return OkAck(); });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(result.writesRejected == 0);
    CHECK(result.error.empty());
}

void test_d4_sequence_na_fails_fast()
{
    std::cout << "[TEST] test_d4_sequence_na_fails_fast" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) { return NaAck(); });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(!result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.writesRejected == 1);
    CHECK(result.error.find(":42:NA;") != std::string::npos);
}

void test_d4_recovery_channel_wraps_writes()
{
    std::cout << "[TEST] test_d4_recovery_channel_wraps_writes" << std::endl;

    // The fake answers the D4 choreography for every session (the recovery
    // enter, the EEPROM writes, and the recovery leave all share one
    // transport). A data packet starting with the RCMODE opcode (0x67 0x6D)
    // gets an 'OK'; any other data packet is an EEPROM write and gets ':42:OK;'.
    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        if (sent.size() > 7 && sent[6] == 0x67 && sent[7] == 0x6D)
            return D4Frame(0x02, 0x02, { 'O', 'K' }, 0x00, 0x01);
        return OkAck();
    });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;
    options.recoveryService = "fwu:ctrl";
    options.recoveryEnter = { 0x67, 0x6D, 0x01, 0x00, 0x01 };
    options.recoveryClose = { 0x67, 0x6D, 0x01, 0x00, 0x03 };
    options.recoveryReply = { 0x4F, 0x4B }; // "OK"

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    // The writes still succeed exactly as they would without recovery.
    CHECK(result.success);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);

    // Counts the data packets whose payload begins with `cmd`.
    auto countCommand = [&](const std::vector<unsigned char>& cmd) {
        int n = 0;
        for (const auto& s : t.sent)
        {
            if (s.size() >= 6 + cmd.size()
                && std::equal(cmd.begin(), cmd.end(), s.begin() + 6))
                n++;
        }
        return n;
    };

    // Recovery entered exactly once before the writes and left exactly once.
    CHECK(countCommand({ 0x67, 0x6D, 0x01, 0x00, 0x01 }) == 1);
    CHECK(countCommand({ 0x67, 0x6D, 0x01, 0x00, 0x03 }) == 1);

    // The enter was sent before the first EEPROM write; the leave after the
    // last one.
    size_t enterAt = 0, leaveAt = 0, firstWriteAt = 0, lastWriteAt = 0;
    for (size_t i = 0; i < t.sent.size(); ++i)
    {
        const auto& s = t.sent[i];
        if (s.size() > 10 && s[6] == 0x67 && s[7] == 0x6D && s[10] == 0x01)
            enterAt = i;
        else if (s.size() > 10 && s[6] == 0x67 && s[7] == 0x6D && s[10] == 0x03)
            leaveAt = i;
        else if (s.size() > 7 && s[6] == 0x7C && s[7] == 0x7C)
        {
            if (firstWriteAt == 0)
                firstWriteAt = i;
            lastWriteAt = i;
        }
    }
    CHECK(enterAt < firstWriteAt);
    CHECK(leaveAt > lastWriteAt);
}

// A model with no recovery channel must not emit any RCMODE traffic.
void test_d4_recovery_absent_is_silent()
{
    std::cout << "[TEST] test_d4_recovery_absent_is_silent" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) { return OkAck(); });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true; // no recovery* fields set

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    for (const auto& s : t.sent)
        CHECK(!(s.size() > 7 && s[6] == 0x67 && s[7] == 0x6D));
}

// L3260-style reply: models with rlen == 2 echo a 2-byte address.
static std::vector<unsigned char> MakeEepromReadReplyEE16(uint16_t addr, uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nEE:";
    s += hex[(addr >> 12) & 0x0F];
    s += hex[(addr >> 8) & 0x0F];
    s += hex[(addr >> 4) & 0x0F];
    s += hex[addr & 0x0F];
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

static bool EndsWithKey(const std::vector<unsigned char>& packet, const std::string& key)
{
    if (packet.size() < key.size())
        return false;

    const size_t offset = packet.size() - key.size();
    for (size_t i = 0; i < key.size(); ++i)
    {
        if (packet[offset + i] != static_cast<unsigned char>(key[i]))
            return false;
    }

    return true;
}

void test_address_length_framing()
{
    std::cout << "[TEST] test_address_length_framing" << std::endl;

    // 'rlen'/'wlen' select the width of the ADDRESS field, so a 1-byte model's
    // packet is one byte shorter and every field after the address shifts.
    const auto write2 = ewr::UniversalGenerator::GenerateWritePacket(0x0008, 0x0018, 0x2A, "Arkanoid", 2);
    const auto write1 = ewr::UniversalGenerator::GenerateWritePacket(0x0008, 0x0018, 0x2A, "Arkanoid", 1);

    CHECK(write2.size() == write1.size() + 1);

    // [6..7] '||', [8..9] inner length LE, [10..11] rkey LE, [12..14] triplet.
    CHECK(write2[6] == 0x7C && write2[7] == 0x7C);
    CHECK(write2[10] == 0x08 && write2[11] == 0x00);
    CHECK(write2[12] == 0x42 && write2[13] == 0xBD && write2[14] == 0x21);

    // 2-byte model: address little endian, then the value.
    CHECK(write2[15] == 0x18);
    CHECK(write2[16] == 0x00);
    CHECK(write2[17] == 0x2A);

    // 1-byte model: the high address byte is absent, value moves up one slot.
    CHECK(write1[15] == 0x18);
    CHECK(write1[16] == 0x2A);

    // The inner length field follows the shorter frame.
    CHECK(write1[8] == static_cast<unsigned char>(write2[8] - 1));

    // The keyword stays the trailing field in both widths.
    CHECK(EndsWithKey(write1, "Arkanoid"));
    CHECK(EndsWithKey(write2, "Arkanoid"));

    const auto read2 = ewr::UniversalGenerator::GenerateReadPacket(0x0008, 0x0018, 2);
    const auto read1 = ewr::UniversalGenerator::GenerateReadPacket(0x0008, 0x0018, 1);

    CHECK(read2.size() == read1.size() + 1);
    CHECK(read1.size() == 16); // D4(6) + '||'(2) + len(2) + rkey(2) + triplet(3) + addr(1)
    CHECK(read2[12] == 0x41 && read2[13] == 0xBE && read2[14] == 0xA0);
    CHECK(read1[15] == 0x18);

    // 2-byte addressing stays the default for callers that do not specify.
    CHECK(ewr::UniversalGenerator::GenerateReadPacket(0x0008, 0x0018) == read2);
}

void test_one_byte_model_address_encoding()
{
    std::cout << "[TEST] test_one_byte_model_address_encoding" << std::endl;

    ewr::DbPrinterModel m = MakeTestModel();
    m.rlen = 1;
    m.wlen = 1;
    m.pad_groups[0].addresses = { 0x0018, 0x0120 };
    m.pad_groups[0].reset_values = { 0x00, 0x00 };

    CHECK(m.ReadAddressLength() == 1);
    CHECK(m.WriteAddressLength() == 1);
    CHECK(m.CanEncodeWriteAddress(0x00FF));
    CHECK(!m.CanEncodeWriteAddress(0x0120));

    ewr::UniversalGenerator generator;
    const auto sequence = generator.GenerateSequence(m);

    // Handshake (3) + credit pair + one write: 0x120 does not fit a 1-byte
    // address field, so it is skipped rather than silently truncated to 0x20.
    CHECK(sequence.size() == 6);

    size_t writes = 0;
    for (const auto& pkt : sequence)
    {
        if (ewr::IsWritePacket(pkt))
        {
            writes++;
            CHECK(pkt[15] == 0x18); // address, then value - no high byte
            CHECK(pkt[16] == 0x00);
        }
    }
    CHECK(writes == 1);

    // A 2-byte model keeps both addresses.
    ewr::DbPrinterModel wide = m;
    wide.rlen = 2;
    wide.wlen = 2;
    CHECK(generator.GenerateSequence(wide).size() == 9);
}

void test_eeprom_read_reply_two_byte_address()
{
    std::cout << "[TEST] test_eeprom_read_reply_two_byte_address" << std::endl;

    uint8_t value = 0;

    // 6 hex digits: 2-byte address echo (big endian) followed by the value.
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE16(0x0130, 0x2A), value, 0x0130));
    CHECK(value == 0x2A);

    // A wide echo can be verified against the requested address in full.
    CHECK(!ewr::ParseEepromReadReply(MakeEepromReadReplyEE16(0x0130, 0x2A), value, 0x0131));

    value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE16(0x0002, 0x5E), value));
    CHECK(value == 0x5E);

    // The 1-byte form still parses, including the case where the echo is too
    // narrow to confirm an address above 0xFF.
    value = 0;
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0x0C, 0x1C), value, 0x0C));
    CHECK(value == 0x1C);
    CHECK(ewr::ParseEepromReadReply(MakeEepromReadReplyEE(0xFC, 0x07), value, 0x02FC));
}

void test_write_key_substitution()
{
    std::cout << "[TEST] test_write_key_substitution" << std::endl;

    const auto packet = ewr::UniversalGenerator::GenerateWritePacket(0x0008, 0x0018, 0x00, "Arkanoid", 2);

    std::vector<unsigned char> out;
    CHECK(ewr::SubstituteTrailingWriteKey(packet, "Arkanoid", "Bslbopje", out));
    CHECK(out.size() == packet.size());
    CHECK(EndsWithKey(out, "Bslbopje"));

    // Everything before the keyword is untouched, so the frame length fields
    // stay valid without recomputation.
    bool prefixIntact = true;
    for (size_t i = 0; i + 8 < packet.size(); ++i)
        prefixIntact = prefixIntact && (out[i] == packet[i]);
    CHECK(prefixIntact);

    // Refused when the keyword is absent, empty, or a different length.
    std::vector<unsigned char> ignored;
    CHECK(!ewr::SubstituteTrailingWriteKey(packet, "Wrongkey", "Bslbopje", ignored));
    CHECK(!ewr::SubstituteTrailingWriteKey(packet, "Arkanoid", "", ignored));
    CHECK(!ewr::SubstituteTrailingWriteKey(packet, "Arkanoid", "Short", ignored));
}

void test_alternate_write_key_retry()
{
    std::cout << "[TEST] test_alternate_write_key_retry" << std::endl;

    std::ofstream log = NullLog();

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::IsWritePacket(pkt))
            return HandshakeAck();
        return EndsWithKey(pkt, "Bslbopje") ? OkAck() : NgAck();
    };

    ewr::ExecutorOptions options = FastOptions();
    options.writeKey = "Arkanoid";
    options.alternateWriteKey = "Bslbopje";

    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(result.writesRejected == 0);

    // Without an alternate keyword, ':42:NG;' stays fatal on the first write.
    FakeTransport plain;
    plain.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        return ewr::IsWritePacket(pkt) ? NgAck() : HandshakeAck();
    };

    std::ostringstream plainOut;
    auto rejected = ewr::ExecuteSequence(plain, legacy::GenerateSequence(MakeTestModel()),
                                         plainOut, log, FastOptions());

    CHECK(!rejected.success);
    CHECK(!rejected.alternateKeyUsed);
    CHECK(rejected.writesRejected == 1);
    CHECK(rejected.error.find(":42:NG;") != std::string::npos);
}

void test_alternate_write_key_retry_d4()
{
    std::cout << "[TEST] test_alternate_write_key_retry_d4" << std::endl;

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        return EndsWithKey(sent, "Bslbopje") ? OkAck() : NgAck();
    });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;
    options.writeKey = "Arkanoid";
    options.alternateWriteKey = "Bslbopje";

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesVerified == 2);
}

void test_schema4_counter_specs_loading()
{
    std::cout << "[TEST] test_schema4_counter_specs_loading" << std::endl;

    // Shaped after a real L3260 entry: two pads whose overflow nibbles share
    // one byte (0x2F), each with its own service limit.
    std::string test_filename = "test_schema4_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {"SchemaFourModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main",)"
            << R"( "addresses": [48, 49, 50, 51], "reset": [0, 0, 0, 0],)"
            << R"( "counters": [)"
            << R"( {"desc": "Main pad", "max": 6346, "bytes": [48, 49, {"addr": 47, "mask": 15, "weight": 254}]},)"
            << R"( {"desc": "Second pad", "max": 3416, "bytes": [50, 51, {"addr": 47, "mask": 240, "weight": 379}]})"
            << R"( ]}],)"
            << R"( "close": [{"addr": 256, "and": 254}, {"addr": 300, "and": 255, "or": 0}]}}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1);
    if (models.size() != 1)
        return;

    const auto& m = models[0];
    const auto counters = m.GetAllCounters();
    CHECK(counters.size() == 2);
    if (counters.size() != 2)
        return;

    // A bare address list is a plain little-endian integer: weights 1, 256.
    CHECK(counters[0].bytes.size() == 3);
    CHECK(counters[0].bytes[0].address == 48 && counters[0].bytes[0].weight == 1);
    CHECK(counters[0].bytes[1].address == 49 && counters[0].bytes[1].weight == 256);
    CHECK(counters[0].bytes[2].address == 47 && counters[0].bytes[2].weight == 254);
    CHECK(counters[0].bytes[2].mask == 0x0F);
    CHECK(counters[0].bytes[2].Shift() == 0);
    CHECK(counters[1].bytes[2].mask == 0xF0);
    CHECK(counters[1].bytes[2].Shift() == 4);
    CHECK(counters[0].HasLimit() && counters[0].max_value == 6346);

    // 0x2F is read but never written, so it only shows up in the read list.
    const auto writeAddrs = m.GetAllAddresses();
    const auto readAddrs = m.GetReadAddresses();
    CHECK(writeAddrs.size() == 4);
    CHECK(readAddrs.size() == 5);
    CHECK(readAddrs[4] == 47);

    // 0x30 = 0x10, 0x31 = 0x01, low nibble of 0x2F = 5.
    const std::vector<std::pair<uint16_t, int>> values = {
        { 48, 0x10 }, { 49, 0x01 }, { 50, 0x00 }, { 51, 0x00 }, { 47, 0x35 }
    };

    const auto main = ewr::EvaluateCounter(counters[0], values);
    CHECK(main.complete);
    CHECK(main.value == 16 + 256 + (5 * 254)); // 1542
    CHECK(main.Percent() == 24);

    // The high nibble of the shared byte belongs to the second pad.
    const auto second = ewr::EvaluateCounter(counters[1], values);
    CHECK(second.complete);
    CHECK(second.value == 3 * 379); // 1137
    CHECK(second.Percent() == 33);

    // A byte that did not answer makes the whole reading incomplete rather
    // than silently reporting a too-low value.
    const std::vector<std::pair<uint16_t, int>> partial = {
        { 48, 0x10 }, { 49, 0x01 }, { 47, -1 }
    };
    const auto incomplete = ewr::EvaluateCounter(counters[0], partial);
    CHECK(!incomplete.complete);
    CHECK(incomplete.Percent() == -1);

    // Past the service limit the percentage is reported as-is, not clamped.
    ewr::CounterSpec small;
    small.max_value = 100;
    ewr::CounterByte only;
    only.address = 48;
    small.bytes.push_back(only);
    const auto over = ewr::EvaluateCounter(small, { { 48, 200 } });
    CHECK(over.Percent() == 200);

    // Without a limit there is nothing to turn into a percentage.
    ewr::CounterSpec unlimited;
    unlimited.bytes.push_back(only);
    CHECK(!unlimited.HasLimit());
    CHECK(ewr::EvaluateCounter(unlimited, { { 48, 200 } }).Percent() == -1);
}

void test_schema4_close_ops()
{
    std::cout << "[TEST] test_schema4_close_ops" << std::endl;

    std::string test_filename = "test_schema4_close_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {)"
            << R"( "CommitModel": {"rkey": 8, "wkey": "Arkanoid", "mem_high": 4095,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}],)"
            << R"( "close": [{"addr": 256, "and": 254}, {"addr": 300, "and": 255, "or": 0}]},)"
            << R"( "PlainModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}]})"
            << R"( }})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 2);

    const ewr::DbPrinterModel* commit = nullptr;
    const ewr::DbPrinterModel* plain = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "CommitModel")
            commit = &m;
        else if (m.name == "PlainModel")
            plain = &m;
    }

    CHECK(commit != nullptr);
    CHECK(plain != nullptr);
    if (!commit || !plain)
        return;

    // The second entry changes nothing, so it must not cost an EEPROM write.
    CHECK(commit->HasCloseOps());
    CHECK(commit->close_ops.size() == 1);
    CHECK(commit->close_ops[0].address == 0x100);
    CHECK(commit->close_ops[0].and_mask == 0xFE);
    CHECK(commit->close_ops[0].or_mask == 0x00);

    // Read-modify-write: clear bit 0, leave everything else alone.
    CHECK(commit->close_ops[0].Apply(0x01) == 0x00);
    CHECK(commit->close_ops[0].Apply(0xFF) == 0xFE);
    CHECK(commit->close_ops[0].Apply(0x2A) == 0x2A); // already committed

    // Models without a commit step keep the plain reset flow.
    CHECK(!plain->HasCloseOps());
    CHECK(plain->GetAllCounters().empty());

    // The commit write is a normal factory write, so a 1-byte model could not
    // even address 0x100 - that guard is shared with the counter writes.
    ewr::DbPrinterModel narrow = *commit;
    narrow.wlen = 1;
    CHECK(!narrow.CanEncodeWriteAddress(narrow.close_ops[0].address));

    // Schema 4 is what this build advertises to the OTA endpoint.
    CHECK(ewr::kMaxSupportedDatabaseSchema == 4);
}

void test_schema4_spec_groups()
{
    std::cout << "[TEST] test_schema4_spec_groups" << std::endl;

    std::string test_filename = "test_schema4_specs_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4,)"
            << R"( "specs": {"L-family": {"rkey": 8, "wkey": "Arkanoid", "wkey1": "Bslbopje", "mem_high": 4095,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48, 49], "reset": [0, 0],)"
            << R"( "counters": [{"desc": "Main pad", "max": 6346, "bytes": [48, 49]}]}],)"
            << R"( "close": [{"addr": 256, "and": 254}]}},)"
            << R"( "models": {)"
            << R"( "Inherited": {"spec": "L-family"},)"
            << R"( "Overridden": {"spec": "L-family", "rkey": 9, "mem_high": 2047},)"
            << R"( "Orphan": {"spec": "Missing-family", "rkey": 10, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Platen Pad Counter", "kind": "platen", "addresses": [24], "reset": [0]}]})"
            << R"( }})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);

    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 3);

    const ewr::DbPrinterModel* inherited = nullptr;
    const ewr::DbPrinterModel* overridden = nullptr;
    const ewr::DbPrinterModel* orphan = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "Inherited")
            inherited = &m;
        else if (m.name == "Overridden")
            overridden = &m;
        else if (m.name == "Orphan")
            orphan = &m;
    }

    CHECK(inherited != nullptr);
    CHECK(overridden != nullptr);
    CHECK(orphan != nullptr);
    if (!inherited || !overridden || !orphan)
        return;

    // Everything comes from the shared group, including schema 4 extras.
    CHECK(inherited->rkey == 8);
    CHECK(inherited->wkey == "Arkanoid");
    CHECK(inherited->wkey1 == "Bslbopje");
    CHECK(inherited->mem_high == 4095);
    CHECK(inherited->GetAllAddresses().size() == 2);
    CHECK(inherited->GetAllCounters().size() == 1);
    CHECK(inherited->HasCloseOps());
    CHECK(inherited->close_ops[0].address == 0x100);

    size_t writes = 0;
    for (const auto& pkt : gen.GenerateSequence(*inherited))
        if (ewr::IsWritePacket(pkt))
            writes++;
    CHECK(writes == 2);

    // Own keys win over the inherited ones; the rest is still inherited.
    CHECK(overridden->rkey == 9);
    CHECK(overridden->mem_high == 2047);
    CHECK(overridden->wkey == "Arkanoid");
    CHECK(overridden->GetAllAddresses().size() == 2);
    CHECK(overridden->HasCloseOps());

    // An unknown group must not lose the model: it loads from its own fields.
    CHECK(orphan->rkey == 10);
    CHECK(orphan->IsPlatenOnly());
    CHECK(orphan->GetAllAddresses().size() == 1);
    CHECK(!orphan->HasCloseOps());

    // In the flat form 'specs' is a reserved root key, not a printer name.
    std::string flat_filename = "test_flat_specs_db.json";
    {
        std::ofstream out(flat_filename);
        out << R"({"schema_version": 4,)"
            << R"( "specs": {"L-family": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}]}},)"
            << R"( "FlatModel": {"spec": "L-family"}})";
    }

    ewr::UniversalGenerator flatGen;
    bool flatLoaded = flatGen.LoadDatabase(flat_filename);
    fs::remove(flat_filename);

    CHECK(flatLoaded);

    auto flatModels = flatGen.GetAvailableModels();
    CHECK(flatModels.size() == 1);
    if (flatModels.size() == 1)
    {
        CHECK(flatModels[0].name == "FlatModel");
        CHECK(flatModels[0].wkey == "Arkanoid");
        CHECK(flatModels[0].GetAllAddresses().size() == 1);
    }
}

void test_log_reporter_and_database_events()
{
    std::cout << "[TEST] test_log_reporter_and_database_events" << std::endl;

    // Reporter mechanics: fan-out, removal, silence without sinks.
    ewr::log::Reporter reporter;
    CHECK(!reporter.HasSinks());
    CHECK(reporter.AddSink(nullptr) == 0);

    std::vector<ewr::log::Event> seen;
    const int sinkId = reporter.AddSink([&](const ewr::log::Event& e) { seen.push_back(e); });
    CHECK(sinkId != 0);
    CHECK(reporter.HasSinks());

    reporter.Log(ewr::log::Level::Warning, ewr::log::Stage::Database, "test.code", "message one");
    CHECK(seen.size() == 1);
    CHECK(seen[0].code == "test.code");
    CHECK(seen[0].level == ewr::log::Level::Warning);
    CHECK(seen[0].stage == ewr::log::Stage::Database);
    CHECK(seen[0].message == "message one");
    CHECK(!seen[0].HasProgress());

    reporter.RemoveSink(sinkId);
    CHECK(!reporter.HasSinks());
    reporter.Log(ewr::log::Level::Error, ewr::log::Stage::General, "test.gone", "not seen");
    CHECK(seen.size() == 1);

    // The database loader reports through the default reporter now: an entry
    // referencing an unknown spec group must produce db.unknown_spec, and the
    // model must still load from its own fields.
    std::vector<std::string> codes;
    const int defaultId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { codes.push_back(e.code); });

    const std::string test_filename = "test_log_events_db.json";
    fs::remove(test_filename);
    {
        std::ofstream f(test_filename);
        f << R"({
            "schema_version": 4,
            "specs": {
                "known-family": { "rlen": 2, "wlen": 2 }
            },
            "models": {
                "GhostModel": {
                    "spec": "NoSuchSpec",
                    "rkey": 8, "wkey": "Arkanoid",
                    "addresses": [24, 25], "reset": [0, 0]
                }
            }
        })";
    }

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase(test_filename));
    fs::remove(test_filename);

    ewr::log::Default().RemoveSink(defaultId);

    bool sawUnknownSpec = false;
    for (const auto& code : codes)
    {
        if (code == "db.unknown_spec")
            sawUnknownSpec = true;
    }
    CHECK(sawUnknownSpec);
}

void test_device_id_extraction_and_parsing()
{
    std::cout << "[TEST] test_device_id_extraction_and_parsing" << std::endl;

    const std::string id = "MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:ET-2800 Series;CLS:PRINTER;DES:EPSON ET-2800 Series;";

    // With the IEEE 1284 two-byte big-endian length prefix.
    std::vector<unsigned char> prefixed;
    prefixed.push_back(0x00);
    prefixed.push_back(static_cast<unsigned char>(id.size() + 2));
    prefixed.insert(prefixed.end(), id.begin(), id.end());
    CHECK(ewr::ExtractDeviceIdString(prefixed.data(), prefixed.size()) == id);

    // Without the prefix (driver already stripped it) and with a trailing NUL;
    // garbage after the NUL must be ignored.
    std::vector<unsigned char> bare(id.begin(), id.end());
    bare.push_back('\0');
    bare.push_back('X');
    CHECK(ewr::ExtractDeviceIdString(bare.data(), bare.size()) == id);

    CHECK(ewr::ExtractDeviceIdString(nullptr, 0).empty());

    const ewr::DeviceIdInfo info = ewr::ParseIeee1284DeviceId(id);
    CHECK(info.manufacturer == "EPSON");
    CHECK(info.model == "ET-2800 Series");
    CHECK(info.commandSet == "ESCPL2,BDC,D4");

    // Long-form keys and stray whitespace.
    const ewr::DeviceIdInfo longForm = ewr::ParseIeee1284DeviceId(
        "MANUFACTURER: Seiko Epson ; MODEL: Stylus Photo R220 ;");
    CHECK(longForm.manufacturer == "Seiko Epson");
    CHECK(longForm.model == "Stylus Photo R220");
}

void test_device_id_model_matching()
{
    std::cout << "[TEST] test_device_id_model_matching" << std::endl;

    const std::vector<std::string> known = { "ET-2800", "ET-2850", "L3150", "R220", "XP-2200" };

    // Exact family match: the "Series" suffix and punctuation are ignored.
    auto m = ewr::MatchModelNames("ET-2800 Series", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "ET-2800");

    // An "EPSON" prefix is ignored too.
    m = ewr::MatchModelNames("EPSON ET-2850 Series", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "ET-2850");

    // Whole-word partial match inside a marketing name.
    m = ewr::MatchModelNames("Stylus Photo R220", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "R220");

    // No cross-family bleed: ET-2850 must never suggest ET-2800.
    m = ewr::MatchModelNames("ET-2850 Series", known);
    for (const auto& name : m)
        CHECK(name != "ET-2800");

    // No match at all.
    CHECK(ewr::MatchModelNames("WF-7840 Series", known).empty());
    CHECK(ewr::MatchModelNames("", known).empty());

    // ---- Alias-aware matching: the database owns the name table now.
    std::vector<ewr::ModelNameEntry> entries;
    entries.push_back({ "ET-2800", { "ET-2800 Series", "ET-2803", "L3260 Series", "L3260" } });
    entries.push_back({ "ET-2850", { "ET-2850 Series" } });
    entries.push_back({ "R220", { "Stylus Photo R220" } });

    // The issue-#16 device: an alias hit resolves to the owning entry.
    auto e = ewr::MatchModelEntries("ET-2803", entries);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "ET-2800");

    // Marketing decoration still normalizes away on aliases.
    e = ewr::MatchModelEntries("EPSON L3260 Series", entries);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "ET-2800");

    // Several aliases of one entry hitting at once yield the entry once.
    e = ewr::MatchModelEntries("ET-2800 Series", entries);
    size_t owners = 0;
    for (const auto& name : e)
    {
        if (name == "ET-2800")
            owners++;
    }
    CHECK(owners == 1);

    // An entry whose own name matches outranks an alias-only match, so an
    // alias never shadows a real model.
    std::vector<ewr::ModelNameEntry> shadow;
    shadow.push_back({ "ET-2800", { "L3260" } });
    shadow.push_back({ "L3260", {} });
    e = ewr::MatchModelEntries("L3260", shadow);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "L3260");

    // Aliases join the no-match rule, not replace it.
    CHECK(ewr::MatchModelEntries("WF-7840 Series", entries).empty());
}

// ---------------------------------------------------------------------------
// --interface pin: ExecutorOptions.interfaceCandidate must survive the
// Session facade's option rebuild on its way to every device call.
// ---------------------------------------------------------------------------

void test_interface_pin_option_threading()
{
    std::cout << "[TEST] test_interface_pin_option_threading" << std::endl;

    // Automatic selection is the default everywhere.
    CHECK(ewr::ExecutorOptions{}.interfaceCandidate == 0);
    CHECK(ewr::DefaultQueryOptions().interfaceCandidate == 0);

    const ewr::DbPrinterModel model = MakeSessionModel();
    FakeGateway gw = MakeSessionGateway();

    ewr::ExecutorOptions pinnedOptions = ewr::DefaultQueryOptions();
    pinnedOptions.interfaceCandidate = 2;

    ewr::Session session(model, gw, ewr::log::Default(), pinnedOptions);

    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; }; // past INK OUT

    const ewr::ResetOutcome outcome = session.Reset(handlers);

    CHECK(outcome.phase == ewr::ResetPhase::Done);
    CHECK(gw.resetCalls == 1);

    // BuildWriteOptions starts from the query options: the pin is intact...
    CHECK(gw.lastResetOptions.interfaceCandidate == 2);

    // ...and the non-negotiable write safeguards are still forced on.
    CHECK(gw.lastResetOptions.validateHandshake);
    CHECK(gw.lastResetOptions.verifyWrites);
    CHECK(gw.lastResetOptions.useSessionLayer);
}

void test_session_recovery_option_threading()
{
    std::cout << "[TEST] test_session_recovery_option_threading" << std::endl;

    // A model that carries an RCMODE recovery channel...
    ewr::DbPrinterModel model = MakeSessionModel();
    model.recovery.service = "fwu:ctrl";
    model.recovery.enter = { 0x67, 0x6D, 0x01, 0x00, 0x01 };
    model.recovery.close = { 0x67, 0x6D, 0x01, 0x00, 0x03 };
    model.recovery.reply = { 0x4F, 0x4B };
    CHECK(model.HasRecoveryChannel());

    FakeGateway gw = MakeSessionGateway();
    ewr::Session session(model, gw, ewr::log::Default(), ewr::DefaultQueryOptions());

    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; }; // past INK OUT

    const ewr::ResetOutcome outcome = session.Reset(handlers);
    CHECK(outcome.phase == ewr::ResetPhase::Done);
    CHECK(gw.resetCalls == 1);

    // ...has that channel threaded into the write options the gateway sees.
    CHECK(gw.lastResetOptions.recoveryService == "fwu:ctrl");
    CHECK(gw.lastResetOptions.recoveryEnter == std::vector<unsigned char>({ 0x67, 0x6D, 0x01, 0x00, 0x01 }));
    CHECK(gw.lastResetOptions.recoveryClose == std::vector<unsigned char>({ 0x67, 0x6D, 0x01, 0x00, 0x03 }));
    CHECK(gw.lastResetOptions.recoveryReply == std::vector<unsigned char>({ 0x4F, 0x4B }));

    // A model with no recovery channel leaves the write options empty.
    ewr::DbPrinterModel plain = MakeSessionModel();
    CHECK(!plain.HasRecoveryChannel());
    FakeGateway gw2 = MakeSessionGateway();
    ewr::Session plainSession(plain, gw2, ewr::log::Default(), ewr::DefaultQueryOptions());
    plainSession.Reset(handlers);
    CHECK(gw2.lastResetOptions.recoveryEnter.empty());
    CHECK(gw2.lastResetOptions.recoveryService.empty());
}

void test_evaluate_ink_blocker()
{
    std::cout << "[TEST] test_evaluate_ink_blocker" << std::endl;

    ewr::PrinterStatus status;

    // No parse / no error: never a blocker, exactly like the waste policy.
    CHECK(!ewr::EvaluateInkBlocker(status).has_value());
    status.valid = true;
    CHECK(!ewr::EvaluateInkBlocker(status).has_value());

    // INK OUT gates the waste reset but must never gate the ink reset:
    // an empty cartridge is the state the ink reset exists to clear.
    status.hasError = true;
    status.errorCode = 0x05;
    status.errorName = "INK OUT";
    CHECK(ewr::EvaluateBlocker(status).has_value());
    CHECK(!ewr::EvaluateInkBlocker(status).has_value());

    // The waste-pad errors stay expected for both flavors.
    status.errorCode = 0x10; // SERVICE REQUEST
    CHECK(!ewr::EvaluateInkBlocker(status).has_value());
    status.errorCode = 0x2C; // CARTRIDGE OVERFLOW
    CHECK(!ewr::EvaluateInkBlocker(status).has_value());

    // A genuinely foreign lock still blocks the ink reset too.
    status.errorCode = 0x06;
    status.errorName = "PAPER JAM";
    const std::optional<ewr::Blocker> blocker = ewr::EvaluateInkBlocker(status);
    CHECK(blocker.has_value());
    if (blocker.has_value())
        CHECK(blocker->errorName == "PAPER JAM");
}

// The session model with a cartridge ink map: two colors, one counter
// byte each, plus a close op that must NOT run on the ink path (ink
// persistence comes from the power cycle, never from a commit).
static ewr::DbPrinterModel MakeInkSessionModel()
{
    ewr::DbPrinterModel model = MakeSessionModel();

    ewr::InkGroup black;
    black.color = "black";
    black.addresses = { 0x84 };
    black.reset_values = { 0x00 };
    model.ink_groups.push_back(black);

    ewr::InkGroup lightmagenta;
    lightmagenta.color = "lightmagenta";
    lightmagenta.addresses = { 0x98 };
    lightmagenta.reset_values = { 0x00 };
    model.ink_groups.push_back(lightmagenta);

    ewr::CloseOp close;
    close.address = 0x0C;
    close.and_mask = 0xFF;
    close.or_mask = 0x80;
    model.close_ops.push_back(close);

    return model;
}

// A gateway whose printer answers INK OUT and whose ink counter bytes
// already read back zeroed (the post-write mirror state).
static FakeGateway MakeInkSessionGateway()
{
    FakeGateway gw;
    gw.queryResult.deviceFound = true;
    gw.queryResult.query.success = true;
    gw.queryResult.query.replies = {
        MakeSt2Reply(), // state ERROR, INK OUT (0x05)
        MakeEepromReadReplyEE(0x84, 0x00),
        MakeEepromReadReplyEE(0x98, 0x00),
    };
    gw.resetResult.deviceFound = true;
    gw.resetResult.exec.success = true;
    return gw;
}

void test_session_ink_reset()
{
    std::cout << "[TEST] test_session_ink_reset" << std::endl;

    const ewr::DbPrinterModel model = MakeInkSessionModel();
    ewr::log::Reporter reporter; // no sinks: a silent host

    // 1) INK OUT does not gate the ink reset: no decision is requested, the
    //    writes run, and the read-back verifies against the ink map.
    {
        FakeGateway gw = MakeInkSessionGateway();
        ewr::Session session(model, gw, reporter);

        int preflights = 0;
        int decisions = 0;
        int verifies = 0;
        ewr::ResetHandlers handlers;
        handlers.onPreflight = [&](const ewr::StateSnapshot& s)
        {
            preflights++;
            CHECK(s.available);
            CHECK(s.status.errorCode == 0x05);
            CHECK(s.values.size() == 2); // the two ink bytes, not the waste pads
            if (s.values.size() == 2)
            {
                CHECK(s.values[0].first == 0x84);
                CHECK(s.values[1].first == 0x98);
            }
        };
        handlers.onBlocker = [&](const ewr::Blocker&) { decisions++; return false; };
        handlers.onVerify = [&](const ewr::StateSnapshot& s)
        {
            verifies++;
            CHECK(s.values.size() == 2);
        };

        const ewr::ResetOutcome out = session.ResetInk(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(out.success);
        CHECK(preflights == 1);
        CHECK(decisions == 0); // INK OUT never asked for a decision
        CHECK(verifies == 1);
        CHECK(out.verificationRan);
        CHECK(out.verifyMismatches == 0);
        CHECK(out.verifyUnread == 0);
        CHECK(out.committed);      // the close op did NOT run...
        CHECK(gw.resetCalls == 1); // ...one write session, no commit session
        CHECK(gw.queryCalls == 2); // preflight + read-back

        // The write session carries exactly the ink writes, with the model's
        // write keyword and every protocol safeguard on.
        size_t writePackets = 0;
        for (const auto& packet : gw.lastSequence)
            writePackets += ewr::IsWritePacket(packet) ? 1 : 0;
        CHECK(writePackets == 2);
        CHECK(gw.lastResetOptions.writeKey == "McLaren");
        CHECK(gw.lastResetOptions.verifyWrites);
        CHECK(gw.lastResetOptions.validateHandshake);
        CHECK(gw.lastResetOptions.useSessionLayer);
    }

    // 2) A model without an ink map refuses cleanly: structured error, no
    //    device traffic at all.
    {
        FakeGateway gw = MakeInkSessionGateway();
        const ewr::DbPrinterModel plain = MakeSessionModel();
        ewr::Session session(plain, gw, reporter);

        const ewr::ResetOutcome out = session.ResetInk();
        CHECK(out.phase == ewr::ResetPhase::NotStarted);
        CHECK(!out.success);
        CHECK(!out.error.empty());
        CHECK(gw.queryCalls == 0);
        CHECK(gw.resetCalls == 0);
    }

    // 3) The waste path is untouched by the ink map: Reset() on the same
    //    model still reads the waste pads and still asks about INK OUT.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        int decisions = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b)
        {
            decisions++;
            CHECK(b.errorCode == 0x05); // INK OUT still gates the WASTE reset
            return false;
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(decisions == 1);
        CHECK(gw.resetCalls == 0);
    }
}

void test_ink_groups_loading()
{
    std::cout << "[TEST] test_ink_groups_loading" << std::endl;

    const std::string test_filename = "test_ink_groups_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {"InkModel": {"rkey": 8, "wkey": "Arkanoid", "rlen": 1, "wlen": 1,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [26, 27], "reset": [0, 0]}],)"
            << R"( "ink_groups": [)"
            << R"({"color": "black", "addresses": [2, 3, 4, 5], "reset": [0, 0, 0, 0]},)"
            << R"({"color": "cyan", "addresses": [14, 15, 16, 17], "reset": [0, 0, 0, 0]})"
            << R"(]}}})";
    }

    ewr::UniversalGenerator gen;
    bool loaded = gen.LoadDatabase(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.GetAvailableModels();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];

        // Waste pads and cartridge ink are independent maps: both parse, and
        // the ink map never bleeds into the waste addresses (a waste reset
        // must never touch ink counters, and the reverse).
        CHECK(m.HasResettableCounters());
        CHECK(m.HasInkReset());
        CHECK(m.ink_groups.size() == 2);
        CHECK(m.GetAllAddresses().size() == 2); // waste pads only: 26, 27

        const std::vector<uint16_t> inkAddrs = m.GetInkAddresses();
        CHECK(inkAddrs.size() == 8);
        if (inkAddrs.size() == 8)
        {
            CHECK(inkAddrs[0] == 2);
            CHECK(inkAddrs[4] == 14);
        }

        const std::vector<uint8_t> inkResets = m.GetInkResetValues();
        CHECK(inkResets.size() == 8);
        bool allZero = true;
        for (uint8_t v : inkResets)
            allZero = allZero && (v == 0x00);
        CHECK(allZero);

        CHECK(m.ink_groups[0].color == "black");
        CHECK(m.ink_groups[1].color == "cyan");
    }

    // The common case: a model with no ink_groups reports no ink reset and
    // yields no ink addresses.
    ewr::DbPrinterModel plain;
    CHECK(!plain.HasInkReset());
    CHECK(plain.GetInkAddresses().empty());
}

void test_session_read_addresses()
{
    std::cout << "[TEST] test_session_read_addresses" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    FakeGateway gw = MakeSessionGateway(); // status + EEPROM reads at 0x0C, 0x0D

    ewr::Session session(model, gw, ewr::log::Default(), ewr::DefaultQueryOptions());

    // An explicit address list is read read-only, in order, with no writes -
    // exactly what --dump leans on.
    const ewr::StateSnapshot snap = session.ReadAddresses({ 0x0C, 0x0D });

    CHECK(snap.available);
    CHECK(gw.resetCalls == 0);
    CHECK(snap.values.size() == 2);
    if (snap.values.size() == 2)
    {
        CHECK(snap.values[0].first == 0x0C);
        CHECK(snap.values[1].first == 0x0D);
    }
}

// A BUSY status with no error entry can be hiding a real blocker (the R220
// omits INK OUT from ST2 while warming up), so both classifiers must demand
// a decision instead of treating it as a green light.
void test_busy_status_blocker()
{
    std::cout << "[TEST] test_busy_status_blocker" << std::endl;

    ewr::PrinterStatus busy;
    busy.valid = true;
    busy.hasError = false;
    busy.stateCode = 0x02;
    busy.stateName = "BUSY";

    const std::optional<ewr::Blocker> waste = ewr::EvaluateBlocker(busy);
    CHECK(waste.has_value());
    if (waste.has_value())
    {
        CHECK(waste->errorCode == -1);
        CHECK(waste->errorName == "PRINTER BUSY");
        CHECK(!waste->explanation.empty());
    }
    CHECK(ewr::EvaluateInkBlocker(busy).has_value());

    // Idle with no error stays clean - the gate is BUSY-specific.
    ewr::PrinterStatus idle = busy;
    idle.stateCode = 0x04;
    idle.stateName = "IDLE";
    CHECK(!ewr::EvaluateBlocker(idle).has_value());
    CHECK(!ewr::EvaluateInkBlocker(idle).has_value());

    // A BUSY report that DOES carry an error keeps the specific policy:
    // expected states pass, and INK OUT still splits the two paths.
    ewr::PrinterStatus busyService = busy;
    busyService.hasError = true;
    busyService.errorCode = 0x10; // SERVICE REQUEST
    busyService.errorName = "SERVICE REQUEST";
    CHECK(!ewr::EvaluateBlocker(busyService).has_value());

    ewr::PrinterStatus busyInkOut = busy;
    busyInkOut.hasError = true;
    busyInkOut.errorCode = 0x05; // INK OUT
    busyInkOut.errorName = "INK OUT";
    CHECK(ewr::EvaluateBlocker(busyInkOut).has_value());
    CHECK(!ewr::EvaluateInkBlocker(busyInkOut).has_value());
}

// The unconditional ask-once gate: fires after every conditional gate, right
// before the first EEPROM write, and a decline leaves the device untouched.
void test_confirm_write_gate()
{
    std::cout << "[TEST] test_confirm_write_gate" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter; // no sinks: a silent host

    // 1) Declining the final confirmation aborts with zero device writes,
    //    even after an accepted blocker said "go".
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        int confirms = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        handlers.confirmWrite = [&](const ewr::StateSnapshot& before)
        {
            confirms++;
            CHECK(before.available);
            return false;
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(!out.success);
        CHECK(confirms == 1);
        CHECK(gw.resetCalls == 0); // nothing was written
    }

    // 2) Accepting it proceeds exactly as before.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        handlers.confirmWrite = [](const ewr::StateSnapshot&) { return true; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(out.success);
        CHECK(gw.resetCalls == 1);
    }

    // 3) It fires even when the preflight is unavailable: the runs that
    //    could not be checked up front need the gate the most.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.queryResult.deviceFound = false;
        gw.queryResult.query.success = false;
        gw.queryResult.query.replies.clear();
        ewr::Session session(model, gw, reporter);

        int confirms = 0;
        ewr::ResetHandlers handlers;
        handlers.confirmWrite = [&](const ewr::StateSnapshot& before)
        {
            confirms++;
            CHECK(!before.available);
            return false;
        };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(confirms == 1);
        CHECK(gw.resetCalls == 0);
    }
}

// The ink write direction is unproven and read-back is a run's only proof:
// with no preflight answer the ink reset must refuse to run blind. (The
// waste path keeps its legacy continue-without-preflight behavior.)
void test_ink_reset_requires_preflight()
{
    std::cout << "[TEST] test_ink_reset_requires_preflight" << std::endl;

    FakeGateway gw = MakeInkSessionGateway();
    gw.queryResult.deviceFound = false;
    gw.queryResult.query.success = false;
    gw.queryResult.query.replies.clear();

    const ewr::DbPrinterModel model = MakeInkSessionModel();
    ewr::log::Reporter reporter;
    ewr::Session session(model, gw, reporter);

    const ewr::ResetOutcome out = session.ResetInk();
    CHECK(out.phase == ewr::ResetPhase::Aborted);
    CHECK(!out.success);
    CHECK(!out.error.empty());
    CHECK(gw.queryCalls == 1);  // the preflight attempt
    CHECK(gw.resetCalls == 0);  // nothing was written
}

void test_end4_framing()
{
    std::cout << "[TEST] test_end4_framing" << std::endl;

    const std::vector<unsigned char> cmd = { 's', 't', 0x01 };
    const std::vector<unsigned char> end4Pkt = ewr::end4::BuildEnd4Packet(cmd);

    CHECK(end4Pkt.size() == 14 + cmd.size());
    CHECK(end4Pkt[0] == 'E' && end4Pkt[1] == 'N' && end4Pkt[2] == 'D' && end4Pkt[3] == '4');
    CHECK(end4Pkt[9] == static_cast<uint8_t>(14 + cmd.size()));

    const std::vector<unsigned char> escPkt = ewr::end4::BuildEscRemotePacket(cmd);
    CHECK(escPkt.size() > cmd.size());
    CHECK(escPkt[0] == 0x1b && escPkt[1] == '@');

    std::vector<unsigned char> rawReply = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x0E,
        '@', 'B', 'D', 'C'
    };
    std::vector<unsigned char> payload;
    bool parsed = ewr::end4::ParseEnd4Response(rawReply, payload);
    CHECK(parsed);
    CHECK(payload.size() == 4);
    if (payload.size() == 4)
    {
        CHECK(payload[0] == '@');
        CHECK(payload[1] == 'B');
    }
}

// Wrap a factory-control inner payload in the 10-byte END4 reply framing a real
// printer returns, so FakeTransport can hand it back byte-shaped.
static std::vector<unsigned char> End4Wrap(const std::vector<unsigned char>& inner)
{
    std::vector<unsigned char> reply = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00
    };
    reply.insert(reply.end(), inner.begin(), inner.end());
    reply[9] = static_cast<unsigned char>(reply.size() & 0xFF);
    return reply;
}

void test_end4_factory_command_extraction()
{
    std::cout << "[TEST] test_end4_factory_command_extraction" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel(); // 2 write addresses
    const std::vector<std::vector<unsigned char>> sequence = legacy::GenerateSequence(model);

    // The raw '||' factory commands END4 carries are each write packet with the
    // 6-byte D4 data header stripped; handshake/credit packets are dropped.
    std::vector<std::vector<unsigned char>> expected;
    for (const auto& pkt : sequence)
    {
        if (ewr::IsWritePacket(pkt))
            expected.emplace_back(pkt.begin() + 6, pkt.end());
    }

    const std::vector<std::vector<unsigned char>> commands = ewr::ExtractFactoryWriteCommands(sequence);

    CHECK(commands.size() == 2);
    CHECK(commands.size() == expected.size());
    CHECK(commands == expected);
    if (!commands.empty())
    {
        CHECK(commands[0].size() >= 2);
        CHECK(commands[0][0] == 0x7c && commands[0][1] == 0x7c);
    }
}

void test_end4_dds_parsing()
{
    std::cout << "[TEST] test_end4_dds_parsing" << std::endl;

    // DDS is hexadecimal: "022500" -> 0x022500 == 140544.
    CHECK(ewr::end4::ParseDdsFlushLength("MFG:EPSON;CMD:ESCPL2,D4,END4;DDS:022500;SN:X;") == static_cast<std::size_t>(0x022500));
    CHECK(ewr::end4::ParseDdsFlushLength("DDS:0100;") == static_cast<std::size_t>(0x0100));
    CHECK(ewr::end4::ParseDdsFlushLength("DDS:0;") == static_cast<std::size_t>(0));
    // Absent or malformed -> 0, so the caller simply skips the flush.
    CHECK(ewr::end4::ParseDdsFlushLength("MFG:EPSON;MDL:ET-2800;") == static_cast<std::size_t>(0));
    CHECK(ewr::end4::ParseDdsFlushLength("DDS:;") == static_cast<std::size_t>(0));
    CHECK(ewr::end4::ParseDdsFlushLength("DDS:zzzz;") == static_cast<std::size_t>(0));
}

void test_end4_sequence_verified()
{
    std::cout << "[TEST] test_end4_sequence_verified" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));
    CHECK(commands.size() == 2);

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (pkt.size() >= 4 && pkt[0] == 'E' && pkt[1] == 'N' && pkt[2] == 'D' && pkt[3] == '4')
            return End4Wrap({ 0x7c, 0x7c, ':', '4', '2', ':', 'O', 'K', ';' });
        return {};
    };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEnd4Sequence(transport, "MFG:EPSON;DDS:0020;", commands, reporter, options);

    CHECK(result.success);
    CHECK(result.anyReply);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);

    // The ExitPacketMode2 preamble is always the first thing on the wire.
    CHECK(!transport.sent.empty());
    if (!transport.sent.empty())
        CHECK(transport.sent.front() == ewr::end4::kExitPacketMode2);

    // A 0x20-byte DDS field means exactly one 0x11 flush packet of 32 bytes.
    bool sawFlush = false;
    for (const auto& pkt : transport.sent)
    {
        if (pkt.size() == 0x20 && !pkt.empty() && pkt.front() == 0x11)
            sawFlush = true;
    }
    CHECK(sawFlush);
}

void test_end4_sequence_silent_fails()
{
    std::cout << "[TEST] test_end4_sequence_silent_fails" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport; // no replyFor -> the printer never answers

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100; // keep the bounded give-up quick for the test

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

    // Honest failure: no false success, and silence is reported as silence.
    CHECK(!result.success);
    CHECK(!result.anyReply);
    CHECK(!result.anyBytes);
    CHECK(result.writesVerified == 0);
    CHECK(!result.error.empty());
}

// The ET-2xxx case from issue #16: the printer answers the packet-mode flush
// with a substantial non-END4 reply and then ignores the END4 writes. That is a
// working transport with unanswered framing, not a filtered one, and the run
// must not report it as silence.
void test_end4_sequence_reports_unframed_bytes()
{
    std::cout << "[TEST] test_end4_sequence_reports_unframed_bytes" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        // Only the packet-mode flush draws anything, and it is not END4-framed.
        if (!pkt.empty() && pkt.front() == 0x11)
        {
            const std::string status = "@BDC PS\r\nST:04;";
            return std::vector<unsigned char>(status.begin(), status.end());
        }
        return {};
    };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    std::vector<ewr::log::Event> events;
    ewr::log::Reporter reporter;
    reporter.AddSink([&events](const ewr::log::Event& e) { events.push_back(e); });

    const ewr::End4Result result =
        ewr::ExecuteEnd4Sequence(transport, "DDS:0020;", commands, reporter, options);

    CHECK(!result.success);
    CHECK(!result.anyReply);      // nothing carried the 'END4' marker
    CHECK(result.anyBytes);       // but bytes did arrive
    CHECK(result.writesVerified == 0);

    // A count alone made this indistinguishable from a mute printer, so the
    // bytes themselves have to reach the trace.
    bool dumped = false;
    for (const auto& e : events)
    {
        if (e.code != "end4.post_flush")
            continue;

        CHECK(e.level == ewr::log::Level::Trace);
        CHECK(e.message.find("15 bytes") != std::string::npos);
        CHECK(e.message.find("40 42 44 43") != std::string::npos); // "@BDC" in hex
        CHECK(e.message.find("@BDC PS") != std::string::npos);     // ASCII column
        dumped = true;
    }
    CHECK(dumped);

    // The old wording blamed the driver on this exact evidence. It must not.
    CHECK(result.error.find("usbprint.sys") == std::string::npos);
    CHECK(result.error.find("never answered") == std::string::npos);
}

void test_hex_dump_capping()
{
    std::cout << "[TEST] test_hex_dump_capping" << std::endl;

    const std::vector<unsigned char> eight = { 0, 1, 2, 3, 4, 5, 6, 7 };

    CHECK(ewr::HexDumpCapped(eight.data(), eight.size(), 8) == ewr::HexDump(eight.data(), eight.size()));
    CHECK(ewr::HexDumpCapped(eight.data(), eight.size(), 64) == ewr::HexDump(eight.data(), eight.size()));

    // Over the cap: head kept, remainder counted rather than silently dropped.
    const std::string capped = ewr::HexDumpCapped(eight.data(), eight.size(), 4);
    CHECK(capped.find("... 4 further byte(s) not shown") != std::string::npos);
    CHECK(capped.find(ewr::HexDump(eight.data(), 4)) == 0);

    CHECK(ewr::HexDumpCapped(eight.data(), 0, 16) == ewr::HexDump(eight.data(), 0));
}

void test_end4_sequence_alternate_key()
{
    std::cout << "[TEST] test_end4_sequence_alternate_key" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel(); // writes end with wkey "Arkanoid"
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));
    CHECK(commands.size() == 2);

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        using Diff = std::vector<unsigned char>::difference_type;
        if (!(pkt.size() >= 4 && pkt[0] == 'E' && pkt[1] == 'N' && pkt[2] == 'D' && pkt[3] == '4'))
            return {};

        const std::string primary = "Arkanoid";
        const bool endsWithPrimary = pkt.size() >= primary.size()
            && std::equal(primary.begin(), primary.end(),
                          pkt.begin() + static_cast<Diff>(pkt.size() - primary.size()));

        // Primary key -> NG; alternate key -> OK.
        return endsWithPrimary
            ? End4Wrap({ 0x7c, 0x7c, ':', '4', '2', ':', 'N', 'G', ';' })
            : End4Wrap({ 0x7c, 0x7c, ':', '4', '2', ':', 'O', 'K', ';' });
    };

    ewr::ExecutorOptions options;
    options.writeKey = "Arkanoid";
    options.alternateWriteKey = "Breakout"; // same length, so the tail substitution is legal
    options.interPacketDelayMs = 0;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesVerified == 2);
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "       EWR Unit Test Suite              " << std::endl;
    std::cout << "========================================" << std::endl;

    test_scan_models();
    test_parser_dummy_dump();
    test_ack_predicates();
    test_write_packet_detection();
    test_platen_only_detection();
    test_legacy_schema_loading();
    test_superset_schema_loading();
    test_envelope_schema_loading();
    test_alias_conflict_schema_loading();
    test_future_schema_version_warning();
    test_updater_version_parsing();
    test_stale_temp_file_cleanup();
    test_updater_prerelease_ordering();
    test_updater_release_response_parsing();
    test_updater_database_payload_validation();
    test_generator_local_db();
    test_database_integrity();
    test_packet_structure_integrity();
    test_byte_parity();
    test_executor_success_path();
    test_executor_rejects_handshake_only_chatter();
    test_executor_fails_on_ng_reply_without_retry();
    test_executor_retries_missing_ack_with_credit_resend();
    test_executor_fails_on_zero_write_sequence();
    test_executor_fails_on_transport_error();
    test_executor_handshake_failfast_on_silence();
    test_executor_handshake_failfast_on_chatter();
    test_executor_handshake_validation_success();
    test_executor_drain_timeout_selection();
    test_executor_retry_without_credit_resend();
    test_executor_replay_relaxed_verification();
    test_executor_replay_silence_still_fails();
    test_executor_replay_ng_still_fatal();
    test_executor_na_refusal_fails_without_retry();
    test_executor_na_refusal_fatal_in_replay_mode();
    test_generator_respects_mem_high();
    test_database_load_skips_malformed_entries();
    test_database_reload_replaces_previous_contents();
    test_status_reply_parsing();
    test_d4_payload_extraction();
    test_eeprom_read_reply_parsing();
    test_evaluate_blocker();
    test_evaluate_ink_blocker();
    test_session_reset_lifecycle();
    test_session_ink_reset();
    test_busy_status_blocker();
    test_confirm_write_gate();
    test_ink_reset_requires_preflight();
    test_session_conflict_gate();
    test_interface_pin_option_threading();
    test_session_recovery_option_threading();
    test_query_session_failfast_on_silence();
    test_query_session_happy_path();
    test_d4_framer_length_framing();
    test_d4_session_start_negotiates_socket_and_mtu();
    test_d4_session_socketid_fallback();
    test_d4_session_credit_gating_and_chunking();
    test_d4_query_session_layer();
    test_d4_sequence_write_verified();
    test_d4_sequence_na_fails_fast();
    test_d4_recovery_channel_wraps_writes();
    test_d4_recovery_absent_is_silent();
    test_address_length_framing();
    test_one_byte_model_address_encoding();
    test_eeprom_read_reply_two_byte_address();
    test_write_key_substitution();
    test_alternate_write_key_retry();
    test_alternate_write_key_retry_d4();
    test_schema4_counter_specs_loading();
    test_schema4_close_ops();
    test_ink_groups_loading();
    test_session_read_addresses();
    test_log_reporter_and_database_events();
    test_executor_event_stream_contract();
    test_schema4_spec_groups();
    test_device_id_extraction_and_parsing();
    test_device_id_model_matching();
    test_end4_framing();
    test_end4_factory_command_extraction();
    test_end4_dds_parsing();
    test_end4_sequence_reports_unframed_bytes();
    test_hex_dump_capping();
    test_end4_sequence_verified();
    test_end4_sequence_silent_fails();
    test_end4_sequence_alternate_key();

    std::cout << "\n----------------------------------------" << std::endl;
    if (g_failures == 0)
    {
        std::cout << "[ALL TESTS PASSED] " << g_checks << " checks." << std::endl;
        return 0;
    }

    std::cout << "[TESTS FAILED] " << g_failures << " of " << g_checks << " checks failed." << std::endl;
    return 1;
}
