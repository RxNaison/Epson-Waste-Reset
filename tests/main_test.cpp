#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <iterator>
#include <map>
#include <thread>
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
#include "ewr/usb_backend.h"
#include "ewr/run_lock.h"
#include "ewr/json_out.h"
#include "ewr/ewr_c.h"
#include "ewr/version.h"
#include "ewr/discover.h"
#include "ewr/snmp.h"
#include "ewr/snmp_gateway.h"
#include "../cli/console.h"

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
    // Fails only the sends this says to, so a transport can break mid-session
    // and recover - which is what the close-and-recover paths need.
    std::function<bool(const std::vector<unsigned char>&)> failSendIf;

    bool Send(const std::vector<unsigned char>& packet) override
    {
        sent.push_back(packet);
        if (failSend || (failSendIf && failSendIf(packet)))
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

    // ...including the flat form, which is what database.json actually ships
    // as. Nesting the version check under "models" meant the refusal never ran
    // for the only shape in use, and a too-new database went straight over the
    // live file on exit.
    write(R"({"schema_version": 99, "L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(!ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));

    // A schema this build does support still installs, in either shape.
    write(R"({"schema_version": 4, "L222": {"rkey": 8, "wkey": "Yj4", "addresses": [24], "reset": [0]}})");
    CHECK(ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));
    write(R"({"schema_version": 4, "models": {"L222": {"wkey": "Yj4"}}})");
    CHECK(ewr::ValidateDatabasePayload(path, ewr::kMaxSupportedDatabaseSchema));

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

// v1.3.0-v1.4.0 read database.json from main and, for any model carrying a
// "recovery" key, send its enter command before writing: ez-reset's firmware-
// updater data, which left an L565 on "Preparing to update". Current builds
// ignore the key, so only this check protects the installs that still read it.
void test_database_carries_no_recovery_channel()
{
    std::cout << "[TEST] test_database_carries_no_recovery_channel" << std::endl;

    std::ifstream in("database.json", std::ios::binary);
    CHECK(in.good());

    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty());
    CHECK(text.find("\"recovery\"") == std::string::npos);
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

// GetSocketID can hand back a socket other than the well-known 2, and the
// session frames its replies with whatever it negotiated.
static std::vector<unsigned char> WrapD4DataOnSocket(const std::vector<unsigned char>& payload,
                                                     unsigned char socket)
{
    std::vector<unsigned char> pkt;
    const uint16_t len = static_cast<uint16_t>(payload.size() + 6);
    pkt.push_back(socket);
    pkt.push_back(socket);
    pkt.push_back((len >> 8) & 0xFF);
    pkt.push_back(len & 0xFF);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

static std::vector<unsigned char> WrapD4Data(const std::vector<unsigned char>& payload)
{
    return WrapD4DataOnSocket(payload, ewr::EpsonD4::SOCKET_EPSON_CTRL);
}

// '@BDC ST2', a 2-byte little-endian body length, then the field block.
// `declaredLength` overrides that length so a report can announce more than it
// delivers, the way a reply cut short by the transport does.
static std::vector<unsigned char> WrapSt2Body(const std::vector<unsigned char>& body,
                                              unsigned char socket = ewr::EpsonD4::SOCKET_EPSON_CTRL,
                                              int declaredLength = -1)
{
    const size_t declared = (declaredLength >= 0) ? static_cast<size_t>(declaredLength) : body.size();

    std::vector<unsigned char> payload;
    const std::string prefix = "@BDC ST2\r\n";
    payload.insert(payload.end(), prefix.begin(), prefix.end());
    payload.push_back(static_cast<unsigned char>(declared & 0xFF));
    payload.push_back(static_cast<unsigned char>((declared >> 8) & 0xFF));
    payload.insert(payload.end(), body.begin(), body.end());

    return WrapD4DataOnSocket(payload, socket);
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

    return WrapSt2Body(body);
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

    CHECK(!st.truncated);

    // Streams that never contain '@BDC ST2' parse to valid == false.
    CHECK(!ewr::ParseStatusReply(HandshakeAck()).valid);
    CHECK(!ewr::ParseStatusReply({}).valid);
}

// An '@BDC ST2' prefix and a length field are not a status report on their own.
// Reporting one as valid tells the blocker gate the printer is clear when
// nothing about the printer was ever read.
void test_status_reply_without_fields_is_invalid()
{
    std::cout << "[TEST] test_status_reply_without_fields_is_invalid" << std::endl;

    // Declared length 0: the prefix arrived, the body did not.
    const auto empty = ewr::ParseStatusReply(WrapSt2Body({}));
    CHECK(!empty.valid);
    CHECK(!empty.hasError);
    CHECK(empty.stateCode == -1);
    CHECK(ewr::DescribePrinterCondition(empty) == "status unavailable");

    // A body of headers this build does not decode is no evidence either.
    const std::vector<unsigned char> unknownOnly = { 0x71, 0x01, 0x00, 0x72, 0x01, 0x00 };
    CHECK(!ewr::ParseStatusReply(WrapSt2Body(unknownOnly)).valid);

    // A recognized header with an empty body decodes nothing.
    const std::vector<unsigned char> emptyField = { 0x01, 0x00 };
    CHECK(!ewr::ParseStatusReply(WrapSt2Body(emptyField)).valid);
}

// A report that stops mid-field parses what arrived, but the error entry may be
// part of what did not - so the truncation has to survive into the snapshot.
void test_status_reply_truncation_is_recorded()
{
    std::cout << "[TEST] test_status_reply_truncation_is_recorded" << std::endl;

    // State IDLE, then a serial field announcing 32 bytes that never arrive.
    std::vector<unsigned char> body = { 0x01, 0x01, 0x04, 0x40, 0x20, 'X', '7' };

    // The length field counts the bytes the report claims, not the ones sent.
    const auto st = ewr::ParseStatusReply(WrapSt2Body(body, ewr::EpsonD4::SOCKET_EPSON_CTRL, 0x25));

    CHECK(st.valid);        // the state field did arrive
    CHECK(st.truncated);    // the serial field did not
    CHECK(st.stateCode == 0x04);
    CHECK(st.stateName == "IDLE");
    CHECK(!st.hasError);

    // A complete report is not flagged.
    CHECK(!ewr::ParseStatusReply(WrapSt2Body({ 0x01, 0x01, 0x04 })).truncated);
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

// GetSocketID answers with the socket the firmware chose, and the session
// frames every reply with that one. Parsing only socket 2 would drop the
// payload of every printer that picks anything else - silently, because an
// unparsed reply looks exactly like a printer that answered nothing.
void test_d4_payload_extraction_on_negotiated_socket()
{
    std::cout << "[TEST] test_d4_payload_extraction_on_negotiated_socket" << std::endl;

    const std::vector<unsigned char> inner = { 'H', 'i', '!' };

    for (unsigned char socket : { 0x01, 0x02, 0x04, 0x7F, 0xFE })
    {
        const auto pkt = WrapD4DataOnSocket(inner, socket);
        CHECK(ewr::ExtractD4Payload(pkt) == inner);

        // Transaction traffic ahead of it is still skipped, not merged in.
        std::vector<unsigned char> mixed = HandshakeAck();
        mixed.insert(mixed.end(), pkt.begin(), pkt.end());
        CHECK(ewr::ExtractD4Payload(mixed) == inner);
    }

    // Mismatched psid/ssid is not a data packet: resync rather than accept it.
    std::vector<unsigned char> mismatched = WrapD4DataOnSocket(inner, 0x04);
    mismatched[1] = 0x05;
    CHECK(ewr::ExtractD4Payload(mismatched) != inner);

    // The whole status and read-back path has to survive the other socket, not
    // just the extractor.
    const auto st = ewr::ParseStatusReply(WrapSt2Body({ 0x01, 0x01, 0x02 }, 0x04));
    CHECK(st.valid);
    CHECK(st.stateCode == 0x02);

    const std::string ps = "@BDC PS\r\nrw:41:5A;";
    uint8_t value = 0;
    CHECK(ewr::ParseEepromReadReply(
        WrapD4DataOnSocket(std::vector<unsigned char>(ps.begin(), ps.end()), 0x04), value));
    CHECK(value == 0x5A);
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
    ewr::ExecutorOptions lastQueryOptions;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions& options) override
    {
        queryCalls++;
        lastQueryOptions = options;
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

        // Written past an objection, so the result says which one.
        CHECK(out.overrides.size() == 1);
        if (out.overrides.size() == 1)
        {
            CHECK(out.overrides[0].gate == "blocker");
            CHECK(out.overrides[0].detail == "INK OUT");
            CHECK(out.overrides[0].errorCode >= 0);
        }
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
        CHECK(out.overrides.size() == 2);
        if (out.overrides.size() == 2)
        {
            CHECK(out.overrides[0].gate == "blocker");
            CHECK(out.overrides[1].gate == "db_conflict");
        }
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

// The length-is-truth rule needs a header worth trusting. A corrupt header
// announcing a plausible-but-wrong length used to buffer forever: the packet
// never completed, nothing was ever discarded, and every later read on the
// session re-evaluated the same bytes and timed out the same way - so the
// status query, every EEPROM read and every write acknowledgement came back
// empty and the run reported that the printer never answered.
void test_d4_framer_resyncs_on_bogus_length()
{
    std::cout << "[TEST] test_d4_framer_resyncs_on_bogus_length" << std::endl;

    std::ofstream log = NullLog();

    // A header announcing 65535 bytes that never arrive must not wedge the
    // framer: the next read has to start clean.
    {
        ChunkedTransport t;
        t.chunks.push_back({ 0x02, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0xDE, 0xAD });

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(!framer.ReadPacket(out, 20, log));
        CHECK(!framer.HasBufferedData());

        // Second read on the same framer: a real packet now gets through.
        t.chunks.push_back(D4Frame(0x02, 0x02, { 'O', 'K' }, 0x00, 0x01));
        CHECK(framer.ReadPacket(out, 50, log));
        CHECK(out.payload.size() == 2);
    }

    // A stray byte ahead of a real packet (the ET-2803 mi_00 chatter) resyncs
    // to the packet instead of reading a length out of the junk.
    {
        ChunkedTransport t;
        std::vector<unsigned char> noisy = { 0x15 };
        const auto pkt = D4Frame(0x02, 0x02, { '@', 'B', 'D', 'C' }, 0x00, 0x01);
        noisy.insert(noisy.end(), pkt.begin(), pkt.end());
        t.chunks.push_back(noisy);

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(framer.ReadPacket(out, 50, log));
        CHECK(out.psid == 0x02);
        CHECK(out.payload.size() == 4);
        CHECK(!framer.HasBufferedData());
    }

    // Mismatched socket ids are not a header either, however plausible the
    // length behind them looks.
    {
        ChunkedTransport t;
        t.chunks.push_back({ 0x02, 0x05, 0x00, 0x40, 0x00, 0x00 });

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(!framer.ReadPacket(out, 20, log));
        CHECK(!framer.HasBufferedData());
    }

    // A header split across two reads still reassembles - resync must not eat
    // a partial header while the rest is still in flight.
    {
        ChunkedTransport t;
        const auto pkt = D4Frame(0x02, 0x02, { 'H', 'i' }, 0x00, 0x01);
        t.chunks.push_back(std::vector<unsigned char>(pkt.begin(), pkt.begin() + 2));
        t.chunks.push_back(std::vector<unsigned char>(pkt.begin() + 2, pkt.end()));

        ewr::D4Framer framer(t);
        ewr::D4Packet out;
        CHECK(framer.ReadPacket(out, 100, log));
        CHECK(out.payload.size() == 2);
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

// A D4 printer that keeps real credit books on both sides (issue #39). A reply
// goes out only against credit the host granted, a data packet is accepted
// only against credit the printer granted, and Init resets both - so a reply
// that vanishes on the way, the way one does when another process reads the
// shared usbprint handle first, leaves the books as wrong as on hardware.
// Like an R220, Init does not drop a reply still waiting for credit: the next
// session that grants some gets it.
class CreditBookPrinter
{
public:
    // A reply left over from an earlier session, waiting for credit.
    void HoldOver(std::vector<unsigned char> reply) { held_.push_back(std::move(reply)); }

    std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> answer;
    // 1-based counts of data replies / credit-request replies that the
    // printer sends but the host never receives.
    std::vector<int> lostDataReplies;
    std::vector<int> lostCreditReplies;
    // After this many data replies the printer never answers data again,
    // not even on a fresh session. 0 = never.
    int silentAfter = 0;
    // Sessions after the first whose GetSocketID and OpenChannel go
    // unanswered: the L365's control service busy while its D4 core still
    // answers Init and Exit.
    int sessionsWithSilentService = 0;

    int inits = 0;
    int dataReplies = 0;
    int creditReplies = 0;

    std::vector<unsigned char> operator()(const std::vector<unsigned char>& sent)
    {
        if (sent.size() < 7)
            return {};

        if (sent[0] == 0x00 && sent[1] == 0x00 && sent[6] == '@')
            return D4Frame(0x00, 0x00, { 0xC5, 0x00 }, 0x01);

        if (sent[0] == 0x00 && sent[1] == 0x00)
        {
            switch (sent[6])
            {
                case 0x00:
                    ++inits;
                    printerCredit_ = 0;
                    hostCredit_ = 0;
                    serviceSilent_ = (inits > 1) && (sessionsWithSilentService-- > 0);
                    return D4Frame(0x00, 0x00, { 0x80, 0x00, 0x10 }, 0x01);
                case 0x09:
                    if (serviceSilent_)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x89, 0x00, 0x02 }, 0x01);
                case 0x01:
                    if (serviceSilent_)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x81, 0x00, 0x02, 0x02, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00 }, 0x01);
                case 0x03:
                {
                    printerCredit_ += (sent.size() >= 11) ? ((sent[9] << 8) | sent[10]) : 0;
                    std::vector<unsigned char> out = D4Frame(0x00, 0x00, { 0x83, 0x00, 0x02, 0x02 }, 0x01);
                    SendHeld(out);
                    return out;
                }
                case 0x04:
                {
                    // The host asks with a maximum outstanding credit of one.
                    const unsigned char granted = (hostCredit_ < 1) ? 1 : 0;
                    hostCredit_ += granted;
                    ++creditReplies;
                    if (Contains(lostCreditReplies, creditReplies))
                        return {};
                    return D4Frame(0x00, 0x00, { 0x84, 0x00, 0x02, 0x02, 0x00, granted }, 0x01);
                }
                case 0x02: return D4Frame(0x00, 0x00, { 0x82, 0x00, 0x02, 0x02 }, 0x01);
                case 0x08: return D4Frame(0x00, 0x00, { 0x88, 0x00 }, 0x01);
                default:   return {};
            }
        }

        if (hostCredit_ <= 0)
            return {};
        --hostCredit_;

        if (silentAfter > 0 && dataReplies >= silentAfter)
            return {};

        held_.push_back(answer(sent));
        std::vector<unsigned char> out;
        SendHeld(out);
        return out;
    }

private:
    static bool Contains(const std::vector<int>& list, int n)
    {
        return std::find(list.begin(), list.end(), n) != list.end();
    }

    void SendHeld(std::vector<unsigned char>& out)
    {
        while (!held_.empty() && printerCredit_ > 0)
        {
            --printerCredit_;
            ++dataReplies;
            if (!Contains(lostDataReplies, dataReplies))
                out.insert(out.end(), held_.front().begin(), held_.front().end());
            held_.erase(held_.begin());
        }
    }

    bool serviceSilent_ = false;
    int printerCredit_ = 0; // granted by the host, spent on replies
    int hostCredit_ = 0;    // granted to the host, spent on its data
    std::vector<std::vector<unsigned char>> held_;
};

// Reads of 0x10..0x15 on a 1-byte-address model, each answered with its own
// address echoed and value 0x40 + address.
static std::vector<std::vector<unsigned char>> LostReplyQueries()
{
    std::vector<std::vector<unsigned char>> queries;
    for (uint8_t addr = 0x10; addr <= 0x15; ++addr)
        queries.push_back(ewr::UniversalGenerator::GenerateReadPacket(0x0008, addr, 1));
    return queries;
}

static std::vector<unsigned char> EchoReadReply(const std::vector<unsigned char>& sent)
{
    const uint8_t addr = sent.back();
    return MakeEepromReadReplyEE(addr, static_cast<uint8_t>(0x40 + addr));
}

static ewr::ExecutorOptions LostReplyOptions()
{
    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;
    options.handshakeDrainTimeoutMs = 40;
    options.writeAckTimeoutMs = 20;
    options.sessionRestartBackoffMs = 1; // the wait itself is not what is under test
    return options;
}

static int CountAnsweredReads(const ewr::QuerySessionResult& result)
{
    int answered = 0;
    for (size_t i = 0; i < result.replies.size(); ++i)
    {
        uint8_t value = 0;
        const int addr = 0x10 + static_cast<int>(i);
        if (ewr::ParseEepromReadReply(result.replies[i], value, addr) && value == 0x40 + addr)
            ++answered;
    }
    return answered;
}

// One lost reply used to end every read after it: the host still counted the
// credit the printer had spent on it and never granted another, so the printer
// could not answer again. That is the contiguous-then-'--' dump of issue #39.
void test_d4_query_session_survives_a_lost_reply()
{
    std::cout << "[TEST] test_d4_query_session_survives_a_lost_reply" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.lostDataReplies = { 3 };

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, LostReplyOptions());

    CHECK(result.success);
    CHECK(result.replies.size() == 6);
    CHECK(CountAnsweredReads(result) == 6);
    CHECK(printer.inits == 2); // the lost reply cost one fresh session, no more
}

// The trace in issue #39 itself: credit-request replies lost as well, after
// which the printer answered every request with zero credit - it counted the
// host as already holding the credit it had granted.
void test_d4_query_session_survives_a_lost_credit_reply()
{
    std::cout << "[TEST] test_d4_query_session_survives_a_lost_credit_reply" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.lostCreditReplies = { 3 };

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, LostReplyOptions());

    CHECK(result.success);
    CHECK(CountAnsweredReads(result) == 6);
    CHECK(printer.inits == 2);
}

// A printer that stays silent even on a fresh session has not lost a reply, it
// has stopped answering. The run ends there instead of spending three sessions
// on every address left, and the replies stay one-for-one with the queries.
void test_d4_query_session_stops_when_a_fresh_session_stays_silent()
{
    std::cout << "[TEST] test_d4_query_session_stops_when_a_fresh_session_stays_silent" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.silentAfter = 2;

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, LostReplyOptions());

    CHECK(!result.success);
    CHECK(!result.error.empty());
    CHECK(result.replies.size() == 6);
    CHECK(CountAnsweredReads(result) == 2);
    for (size_t i = 2; i < result.replies.size(); ++i)
        CHECK(result.replies[i].empty());

    // Nothing was asked past the address that went silent.
    int laterReads = 0;
    for (const auto& sent : t.sent)
    {
        if (sent.size() > 7 && sent[0] == 0x02 && sent[6] == 0x7c && sent.back() > 0x12)
            ++laterReads;
    }
    CHECK(laterReads == 0);
}

// A reply held over from an earlier session - a deadlocked one, or a killed
// run - used to answer the next session's first query and shift every answer
// after it by one, which the address echo then rejected as '--'.
void test_d4_session_discards_replies_held_from_an_earlier_session()
{
    std::cout << "[TEST] test_d4_session_discards_replies_held_from_an_earlier_session" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.HoldOver(MakeEepromReadReplyEE(0x2B, 0x00));
    printer.HoldOver(MakeEepromReadReplyEE(0x3E, 0x00));

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, LostReplyOptions());

    CHECK(result.success);
    CHECK(CountAnsweredReads(result) == 6);
    CHECK(printer.inits == 1);
}

// The printer that lost the reply may still be busy: an L365 answered Exit,
// EJL and Init while ignoring GetSocketID and OpenChannel for over eight
// seconds, then came back by itself. One immediate attempt gave up on it.
void test_d4_session_restart_waits_for_a_busy_control_service()
{
    std::cout << "[TEST] test_d4_session_restart_waits_for_a_busy_control_service" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.lostDataReplies = { 3 };
    printer.sessionsWithSilentService = 2; // two fresh sessions refused, the third opens

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, LostReplyOptions());

    CHECK(result.success);
    CHECK(CountAnsweredReads(result) == 6);
    CHECK(printer.inits == 4); // the first session plus three restart attempts
}

// Giving up is still bounded: a control service that never comes back ends the
// run after the configured attempts rather than hanging on it.
void test_d4_session_restart_gives_up_after_its_attempts()
{
    std::cout << "[TEST] test_d4_session_restart_gives_up_after_its_attempts" << std::endl;

    CreditBookPrinter printer;
    printer.answer = EchoReadReply;
    printer.lostDataReplies = { 3 };
    printer.sessionsWithSilentService = 99;

    FakeTransport t;
    t.replyFor = std::ref(printer);

    ewr::ExecutorOptions options = LostReplyOptions();
    options.sessionRestartAttempts = 3;

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(t, LostReplyQueries(), reporter, options);

    CHECK(!result.success);
    CHECK(result.error.find("would not open a fresh D4 session") != std::string::npos);
    CHECK(result.replies.size() == 6);
    CHECK(CountAnsweredReads(result) == 2);
    CHECK(printer.inits == 4); // the first session plus three refused attempts
}

// The same loss on a write acknowledgement. A write is idempotent, so it is
// sent again on the fresh session and every write still ends up verified.
void test_d4_sequence_survives_a_lost_write_ack()
{
    std::cout << "[TEST] test_d4_sequence_survives_a_lost_write_ack" << std::endl;

    CreditBookPrinter printer;
    printer.answer = [](const std::vector<unsigned char>&) { return OkAck(); };
    printer.lostDataReplies = { 1 };

    FakeTransport t;
    t.replyFor = std::ref(printer);

    std::ofstream log = NullLog();
    std::ostringstream out;
    const ewr::ExecutionResult result =
        ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, LostReplyOptions());

    CHECK(result.success);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(printer.inits == 2);
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

// Every other exit from the write loop closed the channel; the transport-
// failure return did not, leaving it open on the printer after a failed run.
void test_d4_transport_failure_closes_channel()
{
    std::cout << "[TEST] test_d4_transport_failure_closes_channel" << std::endl;

    // One EEPROM write fails to send; everything after it works again, so the
    // close is observable.
    bool failedOnce = false;

    FakeTransport t;
    t.failSendIf = [&failedOnce](const std::vector<unsigned char>& pkt) {
        if (!failedOnce && pkt.size() > 7 && pkt[6] == 0x7C && pkt[7] == 0x7C)
        {
            failedOnce = true;
            return true;
        }
        return false;
    };
    t.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) { return OkAck(); });

    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    const auto result = ewr::ExecuteSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(!result.success);
    CHECK(result.error.find("Transport failure") != std::string::npos);
    CHECK(failedOnce);

    const size_t none = t.sent.size();
    size_t writeAt = none, closeAfterWrite = none;

    for (size_t i = 0; i < t.sent.size(); ++i)
    {
        const auto& s = t.sent[i];
        const bool isTxn = s.size() > 6 && s[0] == 0x00 && s[1] == 0x00;

        if (writeAt == none && s.size() > 7 && s[6] == 0x7C && s[7] == 0x7C)
            writeAt = i;
        else if (writeAt != none && closeAfterWrite == none && isTxn && s[6] == 0x02)
            closeAfterWrite = i; // CloseChannel
    }

    CHECK(writeAt != none);
    CHECK(closeAfterWrite != none);
}

// One credit buys one packet, so a reply longer than the channel's payload
// capacity arrives in pieces. Reading only the first left the rest in the
// framer, to be handed back as the answer to the NEXT query - and callers match
// replies to addresses positionally, so one truncation shifts every value after
// it onto the wrong EEPROM address.
void test_d4_reassembles_a_fragmented_reply()
{
    std::cout << "[TEST] test_d4_reassembles_a_fragmented_reply" << std::endl;

    // MTU to host is 256, so 250 payload bytes per packet. A first fragment
    // that fills it without the end-of-message bit means more is coming.
    const std::vector<unsigned char> head(250, 'A');
    const std::vector<unsigned char> tail = { 'B', 'C', 'D' };

    FakeTransport t;
    t.replyFor = D4SessionReplier(0x02, [&](const std::vector<unsigned char>&) {
        std::vector<unsigned char> both = D4Frame(0x02, 0x02, head, 0x00, 0x00); // no EOM
        const auto second = D4Frame(0x02, 0x02, tail, 0x00, 0x01);               // EOM
        both.insert(both.end(), second.begin(), second.end());
        return both;
    });

    ewr::D4SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;
    options.fragmentTimeoutMs = 100;

    std::ofstream log = NullLog();
    ewr::D4Session session(t, log, options);
    CHECK(session.Start());
    CHECK(session.MtuToHost() == 0x0100);

    std::vector<unsigned char> reply;
    CHECK(session.Exchange({ 's', 't' }, reply));
    CHECK(reply.size() == head.size() + tail.size());

    if (reply.size() == head.size() + tail.size())
    {
        CHECK(reply.front() == 'A');
        CHECK(reply[249] == 'A');
        CHECK(reply[250] == 'B');
        CHECK(reply.back() == 'D');
    }

    session.Close();
}

// The reassembly must stay free on the firmware everybody actually has: a reply
// shorter than the MTU cannot have been cut off by it, so it ends the exchange
// whatever the control byte says - no extra credit packet, no extra wait.
void test_d4_short_reply_costs_no_extra_round_trip()
{
    std::cout << "[TEST] test_d4_short_reply_costs_no_extra_round_trip" << std::endl;

    for (unsigned char control : { 0x00, 0x01 })
    {
        FakeTransport t;
        t.replyFor = D4SessionReplier(0x02, [control](const std::vector<unsigned char>&) {
            return D4Frame(0x02, 0x02, { ':', '4', '2', ':', 'O', 'K', ';' }, 0x00, control);
        });

        ewr::D4SessionOptions options;
        options.replyTimeoutMs = 200;
        options.dataTimeoutMs = 200;
        options.fragmentTimeoutMs = 100;

        std::ofstream log = NullLog();
        ewr::D4Session session(t, log, options);
        CHECK(session.Start());

        const size_t afterStart = t.sent.size();

        std::vector<unsigned char> reply;
        CHECK(session.Exchange({ 's', 't' }, reply));
        CHECK(reply.size() == 7);

        // The credit grant already went out in Start (DiscardHeldReplies):
        // the credit request, the data packet - and nothing more.
        CHECK(t.sent.size() - afterStart == 2);

        session.Close();
    }
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

    // Both keywords rejected: the flag says the alternate one WORKED, and a
    // host turns it into "please report this model" - so a database entry
    // whose keys are both wrong must not be reported as one that was fixed.
    FakeTransport bothBad;
    bothBad.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        return ewr::IsWritePacket(pkt) ? NgAck() : HandshakeAck();
    };

    std::ostringstream bothOut;
    auto bothRejected = ewr::ExecuteSequence(bothBad, legacy::GenerateSequence(MakeTestModel()),
                                             bothOut, log, options);

    CHECK(!bothRejected.success);
    CHECK(!bothRejected.alternateKeyUsed);
    CHECK(bothRejected.writesRejected == 1);

    // The alternate keyword was still tried - this is about what gets reported
    // afterwards, not about giving up early.
    bool sawAlternate = false;
    for (const auto& s : bothBad.sent)
        sawAlternate = sawAlternate || EndsWithKey(s, "Bslbopje");
    CHECK(sawAlternate);
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

    // Same on the session path: both keywords rejected is a bad database
    // entry, not a fixed one.
    FakeTransport bothBad;
    bothBad.replyFor = D4SessionReplier(0x02, [](const std::vector<unsigned char>&) {
        return NgAck();
    });

    std::ostringstream bothOut;
    auto bothRejected = ewr::ExecuteSequence(bothBad, legacy::GenerateSequence(MakeTestModel()),
                                             bothOut, log, options);

    CHECK(!bothRejected.success);
    CHECK(!bothRejected.alternateKeyUsed);
    CHECK(bothRejected.writesRejected == 1);
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

// A trace sent in by email arrives without the console output, so the banner
// has to name the printer and the entry written - and say so when they differ.
void test_trace_context_description()
{
    std::cout << "[TEST] test_trace_context_description" << std::endl;

    ewr::ExecutorOptions::TraceContext context;
    context.printerReports = "L565 Series";
    context.detectedEntry = "L565";
    context.selectedEntry = "L565";
    context.selectedBy = "--yes: the detected model";

    const std::string same = ewr::DescribeTraceContext(context);
    CHECK(same.find("Printer reports: L565 Series\n") != std::string::npos);
    CHECK(same.find("Detected entry:  L565\n") != std::string::npos);
    CHECK(same.find("Selected entry:  L565 (--yes: the detected model)\n") != std::string::npos);
    CHECK(same.find("[!]") == std::string::npos);

    context.selectedEntry = "L3110";
    context.selectedBy = "--model \"L3110\"";
    CHECK(ewr::DescribeTraceContext(context).find("[!] The selected entry is not the one the printer matches.")
          != std::string::npos);

    // Nothing known: every line still printed, none claiming a mismatch.
    const std::string empty = ewr::DescribeTraceContext({});
    CHECK(empty.find("Printer reports: (unknown)\n") != std::string::npos);
    CHECK(empty.find("Detected entry:  (none)\n") != std::string::npos);
    CHECK(empty.find("Selected entry:  (unknown)\n") != std::string::npos);
    CHECK(empty.find("[!]") == std::string::npos);

    // A printer that matches nothing is not a mismatch either.
    ewr::ExecutorOptions::TraceContext unmatched;
    unmatched.printerReports = "EPSON Mystery 9000";
    unmatched.selectedEntry = "L3110";
    CHECK(ewr::DescribeTraceContext(unmatched).find("[!]") == std::string::npos);

    // A run with no entry on purpose says why, and is no mismatch: --find-key
    // on a printer the database does know selects nothing.
    ewr::ExecutorOptions::TraceContext searching;
    searching.printerReports = "Stylus Photo R220";
    searching.detectedEntry = "R220";
    searching.selectedBy = "--find-key";
    const std::string search = ewr::DescribeTraceContext(searching);
    CHECK(search.find("Selected entry:  (none: --find-key)\n") != std::string::npos);
    CHECK(search.find("[!]") == std::string::npos);
}

// Every host gets the entry into the trace, the C API included, without
// having to say it: the Session names the model it runs.
void test_session_names_its_model_in_the_trace_context()
{
    std::cout << "[TEST] test_session_names_its_model_in_the_trace_context" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();

    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; }; // past INK OUT

    FakeGateway quiet = MakeSessionGateway();
    ewr::Session unnamed(model, quiet, ewr::log::Default(), ewr::DefaultQueryOptions());
    CHECK(unnamed.Reset(handlers).phase == ewr::ResetPhase::Done);
    CHECK(quiet.queryCalls > 0);
    CHECK(quiet.lastQueryOptions.trace.selectedEntry == model.name);
    CHECK(quiet.lastResetOptions.trace.selectedEntry == model.name);

    // What a host says is kept, not overwritten.
    ewr::ExecutorOptions named = ewr::DefaultQueryOptions();
    named.trace.printerReports = "TestJet 100 Series";
    named.trace.detectedEntry = "TestJet 100";
    named.trace.selectedEntry = "TestJet 100";
    named.trace.selectedBy = "--model \"testjet\"";

    FakeGateway told = MakeSessionGateway();
    ewr::Session session(model, told, ewr::log::Default(), named);
    CHECK(session.Reset(handlers).phase == ewr::ResetPhase::Done);
    CHECK(told.lastResetOptions.trace.printerReports == "TestJet 100 Series");
    CHECK(told.lastResetOptions.trace.detectedEntry == "TestJet 100");
    CHECK(told.lastResetOptions.trace.selectedBy == "--model \"testjet\"");
}

// The refusals are different evidence, so they must not collapse into one:
// ':41:NA;' turns a known read down, a bare '||:;' never echoes the action
// code at all (an SX110 answered every read that way).
void test_classify_eeprom_read_reply()
{
    std::cout << "[TEST] test_classify_eeprom_read_reply" << std::endl;

    auto text = [](const std::string& s) { return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())); };

    uint8_t value = 0;
    CHECK(ewr::ClassifyEepromReadReply({}, value, 0) == ewr::ReadAnswer::Silent);

    CHECK(ewr::ClassifyEepromReadReply(MakeEepromReadReplyEE16(0, 0x5E), value, 0) == ewr::ReadAnswer::Value);
    CHECK(value == 0x5E);
    CHECK(ewr::ClassifyEepromReadReply(MakeEepromReadReplyEE(0, 0x07), value, 0) == ewr::ReadAnswer::Value);
    CHECK(value == 0x07);

    CHECK(ewr::ClassifyEepromReadReply(text("||:41:NA;\f"), value, 0) == ewr::ReadAnswer::Refused);
    CHECK(ewr::ClassifyEepromReadReply(text("||:;\f"), value, 0) == ewr::ReadAnswer::Empty);
    CHECK(ewr::ClassifyEepromReadReply(text("||:;\r\n\f"), value, 0) == ewr::ReadAnswer::Empty);

    // Neither a value nor a known refusal: reported, never guessed at.
    CHECK(ewr::ClassifyEepromReadReply(text("zz:;\f"), value, 0) == ewr::ReadAnswer::Other);
    // A value for another address is not an answer to this read.
    CHECK(ewr::ClassifyEepromReadReply(MakeEepromReadReplyEE16(0x18, 1), value, 0) == ewr::ReadAnswer::Other);
}

void test_collect_read_key_candidates()
{
    std::cout << "[TEST] test_collect_read_key_candidates" << std::endl;

    auto model = [](const char* name, uint16_t rkey, uint16_t rlen) {
        ewr::DbPrinterModel m;
        m.name = name;
        m.rkey = rkey;
        m.rlen = rlen;
        return m;
    };

    // The same key at another address width is another candidate: the width
    // changes the packet.
    const std::vector<ewr::ReadKeyCandidate> got = ewr::CollectReadKeyCandidates(
        { model("A", 1, 2), model("D", 9, 2), model("B", 1, 2), model("C", 1, 1), model("E", 1, 2) });

    CHECK(got.size() == 3);
    if (got.size() == 3)
    {
        // Most shared first, then (key, width) order.
        CHECK(got[0].rkey == 1 && got[0].addressLength == 2);
        CHECK(got[0].models == std::vector<std::string>({ "A", "B", "E" }));
        CHECK(got[1].rkey == 1 && got[1].addressLength == 1);
        CHECK(got[2].rkey == 9 && got[2].addressLength == 2);
    }

    // The real database: every model lands in exactly one candidate.
    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));
    const auto models = gen.GetAvailableModels();
    const auto real = ewr::CollectReadKeyCandidates(models);

    size_t covered = 0;
    std::map<std::pair<uint16_t, uint8_t>, int> seen;
    for (const auto& candidate : real)
    {
        covered += candidate.models.size();
        seen[{ candidate.rkey, candidate.addressLength }]++;
        CHECK(candidate.addressLength == 1 || candidate.addressLength == 2);
    }
    CHECK(covered == models.size());
    CHECK(seen.size() == real.size());
    CHECK(real.size() > 50);
    for (size_t i = 1; i < real.size(); ++i)
        CHECK(real[i - 1].models.size() >= real[i].models.size());
}

// Answers each read by its key, the way a printer does, and ends the session
// at the first read it ignores - which is what the real query session does.
struct KeyGateway : ewr::IDeviceGateway
{
    std::function<std::vector<unsigned char>(uint16_t, int)> answer;
    bool deviceFound = true;
    bool handshakeFailed = false;
    int sessions = 0;
    int resets = 0;
    int nonReads = 0;
    std::vector<uint16_t> asked;
    ewr::ExecutorOptions lastOptions;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>& queries,
                                 const ewr::ExecutorOptions& options) override
    {
        sessions++;
        lastOptions = options;

        ewr::QueryRunResult run;
        run.deviceFound = deviceFound;
        if (!deviceFound)
            return run;

        if (handshakeFailed)
        {
            run.query.handshakeFailed = true;
            run.query.error = "no handshake";
            return run;
        }

        run.query.handshakeConfirmed = true;
        for (const auto& q : queries)
        {
            // D4 header (6), '||', length (2), key (2), action 0x41 + 0xBE.
            if (q.size() < 16 || q[12] != 0x41 || q[13] != 0xBE)
                nonReads++;

            const uint16_t rkey = static_cast<uint16_t>(q[10] | (q[11] << 8));
            asked.push_back(rkey);
            run.query.packetsSent++;

            std::vector<unsigned char> reply = answer(rkey, q[8] - 5);
            if (reply.empty())
            {
                run.query.error = "The printer stopped answering";
                run.query.replies.resize(queries.size());
                return run;
            }

            run.query.replies.push_back(std::move(reply));
        }

        run.query.success = true;
        return run;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        resets++;
        return {};
    }
};

void test_search_read_key()
{
    std::cout << "[TEST] test_search_read_key" << std::endl;

    auto text = [](const std::string& s) { return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())); };
    const std::vector<unsigned char> refused = text("||:41:NA;\f");
    const std::vector<unsigned char> empty = text("||:;\f");

    // 40 keys: three batches when nothing goes wrong.
    std::vector<ewr::ReadKeyCandidate> candidates;
    for (uint16_t rkey = 100; rkey < 140; ++rkey)
        candidates.push_back({ rkey, 2, { "Model " + std::to_string(rkey) } });

    // One key fits.
    {
        KeyGateway gw;
        gw.answer = [&](uint16_t rkey, int) { return rkey == 123 ? MakeEepromReadReplyEE16(0, 0x5E) : refused; };

        size_t lastTried = 0, lastTotal = 0;
        ewr::ExecutorOptions options = ewr::DefaultQueryOptions();
        options.interfaceCandidate = 2;
        options.trace.selectedBy = "--find-key";

        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates, options,
            [&](size_t tried, size_t total) { lastTried = tried; lastTotal = total; });

        CHECK(search.deviceFound);
        CHECK(search.completed);
        CHECK(search.error.empty());
        CHECK(search.probes.size() == 40);
        CHECK(search.Count(ewr::ReadAnswer::Value) == 1);
        CHECK(search.Count(ewr::ReadAnswer::Refused) == 39);
        CHECK(search.probes[23].candidate.rkey == 123);
        CHECK(search.probes[23].answer == ewr::ReadAnswer::Value);
        CHECK(search.probes[23].value == 0x5E);

        CHECK(gw.sessions == 3);
        CHECK(lastTried == 40 && lastTotal == 40);

        // A search for a key never writes, and never sends anything but reads.
        CHECK(gw.resets == 0);
        CHECK(gw.nonReads == 0);

        // The host's options travel; retries on an ignored key do not.
        CHECK(gw.lastOptions.interfaceCandidate == 2);
        CHECK(gw.lastOptions.trace.selectedBy == "--find-key");
        CHECK(gw.lastOptions.maxWriteAttempts == 1);
    }

    // Every key, the same empty answer - what an SX110's one reply suggests.
    {
        KeyGateway gw;
        gw.answer = [&](uint16_t, int) { return empty; };

        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates);
        CHECK(search.completed);
        CHECK(search.Count(ewr::ReadAnswer::Empty) == 40);
        CHECK(search.Count(ewr::ReadAnswer::Value) == 0);
    }

    // A printer that ignores a key ends that session. The search resumes
    // right after it, so no key is skipped, and asks the ignored ones again
    // in later passes before calling them silent.
    {
        KeyGateway gw;
        gw.answer = [&](uint16_t rkey, int) {
            return (rkey == 105 || rkey == 120) ? std::vector<unsigned char>{} : refused;
        };

        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates);
        CHECK(search.completed);
        CHECK(search.probes.size() == 40);
        CHECK(search.Count(ewr::ReadAnswer::Silent) == 2);
        CHECK(search.Count(ewr::ReadAnswer::Refused) == 38);

        // Reported in candidate order, whatever order they were settled in.
        for (size_t i = 0; i < search.probes.size(); ++i)
            CHECK(search.probes[i].candidate.rkey == 100 + i);
        CHECK(search.probes[5].answer == ewr::ReadAnswer::Silent);
        CHECK(search.probes[6].answer == ewr::ReadAnswer::Refused);
        CHECK(search.probes[20].answer == ewr::ReadAnswer::Silent);

        // Every key once, the two ignored ones three times.
        std::map<uint16_t, int> times;
        for (uint16_t rkey : gw.asked)
            times[rkey]++;
        CHECK(times.size() == 40);
        CHECK(times[105] == 3 && times[120] == 3);
        CHECK(gw.asked.size() == 44);

        // Pass 1: 100-105, 106-120, 121-136, 137-139. Passes 2 and 3: two each.
        CHECK(gw.sessions == 8);
    }

    // The SX110 case: the session failed before the read was even sent (a
    // refused D4 credit), so the key was never tried. It must be asked again,
    // not written off - one of the four keys lost that way was the closest
    // sibling's.
    {
        KeyGateway gw;
        std::map<uint16_t, int> failuresLeft = { { 110, 1 }, { 130, 2 } };
        gw.answer = [&](uint16_t rkey, int) {
            auto it = failuresLeft.find(rkey);
            if (it != failuresLeft.end() && it->second > 0)
            {
                it->second--;
                return std::vector<unsigned char>{};
            }
            return rkey == 110 ? MakeEepromReadReplyEE16(0, 0x2A) : empty;
        };

        size_t lastTried = 0, lastTotal = 0;
        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates, ewr::DefaultQueryOptions(),
            [&](size_t tried, size_t total) { lastTried = tried; lastTotal = total; });

        CHECK(search.completed);
        CHECK(search.probes.size() == 40);
        CHECK(search.Count(ewr::ReadAnswer::Silent) == 0);
        CHECK(search.Count(ewr::ReadAnswer::Value) == 1);
        CHECK(search.Count(ewr::ReadAnswer::Empty) == 39);
        CHECK(search.probes[10].candidate.rkey == 110);
        CHECK(search.probes[10].answer == ewr::ReadAnswer::Value);
        CHECK(search.probes[10].value == 0x2A);
        CHECK(search.probes[30].answer == ewr::ReadAnswer::Empty);
        CHECK(lastTried == 40 && lastTotal == 40);
    }

    // No printer, and a printer that will not open a session: both stop the
    // search and say why, without pretending any key was tried.
    {
        KeyGateway gw;
        gw.deviceFound = false;
        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates);
        CHECK(!search.deviceFound);
        CHECK(!search.completed);
        CHECK(search.probes.empty());
        CHECK(!search.error.empty());
    }
    {
        KeyGateway gw;
        gw.handshakeFailed = true;
        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, candidates);
        CHECK(search.deviceFound);
        CHECK(!search.completed);
        CHECK(search.probes.empty());
        CHECK(search.error == "no handshake");
        CHECK(gw.sessions == 1);
    }

    // Nothing to try is a finished search, with no device session opened.
    {
        KeyGateway gw;
        const ewr::ReadKeySearch search = ewr::SearchReadKey(gw, {});
        CHECK(search.completed);
        CHECK(gw.sessions == 0);
    }
}

void test_json_key_search_data()
{
    std::cout << "[TEST] test_json_key_search_data" << std::endl;

    ewr::ReadKeySearch search;
    search.deviceFound = true;
    search.completed = true;
    search.probes.push_back({ { 15120, 1, { "R200", "R220" } }, ewr::ReadAnswer::Value, 0x07 });
    search.probes.push_back({ { 373, 2, { "SX210" } }, ewr::ReadAnswer::Empty, 0 });
    search.probes.push_back({ { 1609, 2, { "SX200" } }, ewr::ReadAnswer::Refused, 0 });

    const nlohmann::json data = ewr::JsonKeySearchData("Stylus Photo R220", search);
    CHECK(data["printer"] == "Stylus Photo R220");
    CHECK(data["completed"] == true);
    CHECK(data["tried"] == 3);
    CHECK(data["answers"]["value"] == 1);
    CHECK(data["answers"]["refused"] == 1);
    CHECK(data["answers"]["empty"] == 1);
    CHECK(data["answers"]["silent"] == 0);
    CHECK(data["answers"]["other"] == 0);

    // Only the keys that work are listed.
    CHECK(data["keys"].size() == 1);
    CHECK(data["keys"][0]["read_key"] == 15120);
    CHECK(data["keys"][0]["address_length"] == 1);
    CHECK(data["keys"][0]["value"] == 7);
    CHECK(data["keys"][0]["models"] == nlohmann::json({ "R200", "R220" }));

    // A printer that named nothing is null, never an empty string.
    const nlohmann::json unnamed = ewr::JsonKeySearchData("", ewr::ReadKeySearch{});
    CHECK(unnamed["printer"].is_null());
    CHECK(unnamed["keys"].is_array() && unnamed["keys"].empty());
    CHECK(unnamed["completed"] == false);
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

// Both executor paths pad `replies` one-for-one with the queries, pushing an
// empty vector for every query that drew no answer. A snapshot that reads that
// padding as "the preflight ran" hands the blocker gate an empty status and
// calls the printer clear without ever having read it.
void test_snapshot_requires_a_reply()
{
    std::cout << "[TEST] test_snapshot_requires_a_reply" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();

    // Handshake fine, device found, every query unanswered.
    FakeGateway silent = MakeSessionGateway();
    for (auto& reply : silent.queryResult.query.replies)
        reply.clear();
    silent.queryResult.query.success = false;

    ewr::Session session(model, silent, ewr::log::Default(), ewr::DefaultQueryOptions());
    const ewr::StateSnapshot snap = session.ReadState();
    CHECK(!snap.available);
    CHECK(!snap.status.valid);

    // The model-free read is gated the same way.
    FakeGateway silentStatus = silent;
    CHECK(!ewr::ReadPrinterStatus(silentStatus, ewr::DefaultQueryOptions()).available);

    // One answered query is enough: a printer that answers the counter reads
    // but not the status query is still readable.
    FakeGateway partial = MakeSessionGateway();
    partial.queryResult.query.replies[0].clear();
    ewr::Session partialSession(model, partial, ewr::log::Default(), ewr::DefaultQueryOptions());
    const ewr::StateSnapshot partialSnap = partialSession.ReadState();
    CHECK(partialSnap.available);
    CHECK(!partialSnap.status.valid);

    // And the reset that refuses to run blind must refuse on silence, not just
    // on a missing device.
    FakeGateway silentInk = MakeInkSessionGateway();
    for (auto& reply : silentInk.queryResult.query.replies)
        reply.clear();
    silentInk.queryResult.query.success = false;

    const ewr::DbPrinterModel inkModel = MakeInkSessionModel();
    ewr::log::Reporter reporter;
    ewr::Session inkSession(inkModel, silentInk, reporter);

    const ewr::ResetOutcome out = inkSession.ResetInk();
    CHECK(out.phase == ewr::ResetPhase::Aborted);
    CHECK(!out.success);
    CHECK(silentInk.resetCalls == 0); // nothing was written
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

// Same reasoning as BUSY: a report that ran out mid-field may have been cut off
// right before its error entry, so its silence about errors proves nothing.
void test_truncated_status_blocker()
{
    std::cout << "[TEST] test_truncated_status_blocker" << std::endl;

    ewr::PrinterStatus partial;
    partial.valid = true;
    partial.truncated = true;
    partial.hasError = false;
    partial.stateCode = 0x04; // IDLE - the reassuring case
    partial.stateName = "IDLE";

    const std::optional<ewr::Blocker> waste = ewr::EvaluateBlocker(partial);
    CHECK(waste.has_value());
    if (waste.has_value())
    {
        CHECK(waste->errorCode == -1);
        CHECK(waste->errorName == "INCOMPLETE STATUS REPORT");
        CHECK(!waste->explanation.empty());
    }
    CHECK(ewr::EvaluateInkBlocker(partial).has_value());

    // A truncated report that still carried its error entry is classified on
    // the error, not on the truncation: the waste-pad errors stay expected.
    ewr::PrinterStatus partialService = partial;
    partialService.hasError = true;
    partialService.errorCode = 0x10; // SERVICE REQUEST
    partialService.errorName = "SERVICE REQUEST";
    CHECK(!ewr::EvaluateBlocker(partialService).has_value());

    ewr::PrinterStatus partialJam = partial;
    partialJam.hasError = true;
    partialJam.errorCode = 0x04; // PAPER JAM
    partialJam.errorName = "PAPER JAM";
    const std::optional<ewr::Blocker> jam = ewr::EvaluateBlocker(partialJam);
    CHECK(jam.has_value());
    if (jam.has_value())
        CHECK(jam->errorName == "PAPER JAM");

    // A complete report is untouched by the new gate.
    ewr::PrinterStatus complete = partial;
    complete.truncated = false;
    CHECK(!ewr::EvaluateBlocker(complete).has_value());
    CHECK(!ewr::EvaluateInkBlocker(complete).has_value());
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

// A stand-in for what an ESC/P Remote capable printer answers a factory write.
// No END4 envelope: the '||' verdict arrives bare.
static std::vector<unsigned char> BareOkAck()
{
    const std::string ack = "||:42:OK;";
    return std::vector<unsigned char>(ack.begin(), ack.end());
}

void test_esc_remote_sequence_verified()
{
    std::cout << "[TEST] test_esc_remote_sequence_verified" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));
    CHECK(commands.size() == 2);

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (pkt.size() >= 2 && pkt[0] == 0x1b && pkt[1] == '@')
            return BareOkAck();
        return {};
    };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEscRemoteSequence(transport, commands, reporter, options);

    CHECK(result.success);
    CHECK(result.anyReply);
    CHECK(result.anyBytes);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(!result.alternateKeyUsed);

    // One self-contained block per write, and nothing else. The point of this
    // path is that it sends no handshake, opens no session and floods no
    // packet-mode filler, so a run that emitted any of those is not this path.
    CHECK(transport.sent.size() == commands.size());
    for (const auto& pkt : transport.sent)
    {
        CHECK(pkt.size() >= 2 && pkt[0] == 0x1b && pkt[1] == '@');
        CHECK(pkt.front() != 0x11);
        CHECK(pkt != ewr::end4::kExitPacketMode2);
    }
}

void test_esc_remote_sequence_silent_fails()
{
    std::cout << "[TEST] test_esc_remote_sequence_silent_fails" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport; // no replyFor -> the printer never answers

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEscRemoteSequence(transport, commands, reporter, options);

    CHECK(!result.success);
    CHECK(!result.anyReply);
    CHECK(!result.anyBytes);
    CHECK(result.writesVerified == 0);
    CHECK(!result.error.empty());
}

// ':42:NG;' is the write key being wrong, not the transport failing, so the
// alternate keyword gets one try - and the flag only claims the alternate
// worked when the retry actually came back OK.
void test_esc_remote_alternate_key()
{
    std::cout << "[TEST] test_esc_remote_alternate_key" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel(); // writes end with wkey "Arkanoid"
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    const std::string alternate = "Tetris01"; // same length, so the tail swap applies

    FakeTransport transport;
    transport.replyFor = [&alternate](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        const bool carriesAlternate = pkt.size() >= alternate.size()
            && std::equal(alternate.begin(), alternate.end(),
                          pkt.end() - static_cast<long>(alternate.size()) - 6);
        const std::string ng = "||:42:NG;";
        return carriesAlternate ? BareOkAck() : std::vector<unsigned char>(ng.begin(), ng.end());
    };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.alternateWriteKey = alternate;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEscRemoteSequence(transport, commands, reporter, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesVerified == result.writesTotal);
}

// Both keywords rejected is a bad database entry, not a fixed one, so the
// alternate-key flag must stay false - hosts surface it as a fix worth
// reporting upstream.
void test_esc_remote_both_keys_rejected_claims_nothing()
{
    std::cout << "[TEST] test_esc_remote_both_keys_rejected_claims_nothing" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) -> std::vector<unsigned char> {
        const std::string ng = "||:42:NG;";
        return std::vector<unsigned char>(ng.begin(), ng.end());
    };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.alternateWriteKey = "Tetris01";
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEscRemoteSequence(transport, commands, reporter, options);

    CHECK(!result.success);
    CHECK(!result.alternateKeyUsed);
    CHECK(result.writesRejected == 1);
}

void test_end4_framing()
{
    std::cout << "[TEST] test_end4_framing" << std::endl;

    const std::vector<unsigned char> cmd = { 's', 't', 0x01 };
    const std::vector<unsigned char> end4Pkt = ewr::end4::BuildEnd4Packet(cmd);

    CHECK(end4Pkt.size() == 14 + cmd.size());
    CHECK(end4Pkt[0] == 'E' && end4Pkt[1] == 'N' && end4Pkt[2] == 'D' && end4Pkt[3] == '4');
    CHECK(end4Pkt[9] == static_cast<uint8_t>(14 + cmd.size()));

    // ESC @ ESC @ | ESC ( R len=8 | \0 REMOTE1 | <cmd> | ESC 0 0 0 | ESC @
    const std::vector<unsigned char> escPkt = ewr::end4::BuildEscRemotePacket(cmd);
    CHECK(escPkt.size() == 23 + cmd.size());
    CHECK(escPkt[0] == 0x1b && escPkt[1] == '@');
    CHECK(escPkt[2] == 0x1b && escPkt[3] == '@');
    CHECK(escPkt[4] == 0x1b && escPkt[5] == '(' && escPkt[6] == 'R');

    // The declared parameter length has to match the bytes actually supplied.
    // Counting only "REMOTE1" leaves it one short and the printer eats the
    // leading '|' of the command behind it.
    const size_t declared = static_cast<size_t>(escPkt[7]) | (static_cast<size_t>(escPkt[8]) << 8);
    CHECK(declared == 8);

    const std::vector<unsigned char> expectedParam = { 0x00, 'R', 'E', 'M', 'O', 'T', 'E', '1' };
    CHECK(std::equal(expectedParam.begin(), expectedParam.end(), escPkt.begin() + 9));

    // The command starts immediately after the declared parameter.
    CHECK(std::equal(cmd.begin(), cmd.end(), escPkt.begin() + 9 + static_cast<long>(declared)));

    const std::vector<unsigned char> tail(escPkt.end() - 6, escPkt.end());
    const std::vector<unsigned char> expectedTail = { 0x1b, 0x00, 0x00, 0x00, 0x1b, '@' };
    CHECK(tail == expectedTail);

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

// The END4 path hands ParseEnd4Response an arbitrary suffix found by searching
// for the literal 'END4' anywhere in a raw drain, prefix junk included, so a
// length byte that makes no sense is the expected case rather than the
// exceptional one. A declared total below the header's own size would run the
// body's end iterator backwards past its start.
void test_end4_response_rejects_short_declared_length()
{
    std::cout << "[TEST] test_end4_response_rejects_short_declared_length" << std::endl;

    for (unsigned char declared : { 0x00, 0x01, 0x05, 0x09 })
    {
        const std::vector<unsigned char> malformed = {
            'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, declared,
            '@', 'B', 'D', 'C'
        };

        std::vector<unsigned char> body = { 0xFF }; // must be cleared, not kept
        CHECK(!ewr::end4::ParseEnd4Response(malformed, body));
        CHECK(body.empty());
    }

    // Exactly the header size is a well-formed frame with an empty body.
    const std::vector<unsigned char> headerOnly = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x0A,
        '@', 'B', 'D', 'C'
    };

    std::vector<unsigned char> emptyBody = { 0xFF };
    CHECK(ewr::end4::ParseEnd4Response(headerOnly, emptyBody));
    CHECK(emptyBody.empty());

    // A frame that announces more than arrived still hands back the partial
    // body, which is what lets the caller keep draining.
    const std::vector<unsigned char> short_ = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x20,
        '@', 'B', 'D', 'C'
    };

    std::vector<unsigned char> partial;
    CHECK(!ewr::end4::ParseEnd4Response(short_, partial));
    CHECK(partial.size() == 4);
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

// executor.h caps trace dumps because a single drain may return kMaxDrainBytes.
// The backends honour that cap; the executor and the D4 session used to format
// the same bytes a second time, uncapped - hundreds of KB of hex per run, built
// eagerly as a function argument before the reporter has even looked for a
// trace sink, and written into the one artifact users attach to bug reports.
void test_executor_caps_inbound_trace_dumps()
{
    std::cout << "[TEST] test_executor_caps_inbound_trace_dumps" << std::endl;

    ewr::UniversalGenerator gen;
    const auto seq = gen.GenerateSequence(MakeTestModel());

    // A channel-open ack trailed by a burst the size of a full drain window.
    std::vector<unsigned char> flood = HandshakeAck();
    flood.resize(16 * 1024, 0xA5);

    FakeTransport t;
    t.replyFor = [&flood](const std::vector<unsigned char>& pkt) {
        return ewr::IsWritePacket(pkt) ? OkAck() : flood;
    };

    std::ostringstream out;
    std::ostringstream log;
    ewr::ExecuteSequence(t, seq, out, log, FastOptions());

    const std::string trace = log.str();

    // The burst is reported, not silently dropped...
    CHECK(trace.find("16384 bytes") != std::string::npos);
    CHECK(trace.find("further byte(s) not shown") != std::string::npos);

    // ...and 16 KB of hex (~70 KB of text) never reaches the file. Several
    // packets draw the flood, so uncapped this runs to hundreds of KB.
    CHECK(trace.size() < 64 * 1024);
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

// A caller reads pads by key, never by label: "Platen Pad Counter" and "Main
// Pad Counter" are one typo apart in English and worlds apart in meaning
// (#35), and a wrapper acting on the wrong one is the failure #42 asked us to
// make impossible. The kind survives the flattening into CounterSpecs, whether
// the database states it or only the description implies it.
void test_json_pads_carry_a_kind_not_just_a_label()
{
    std::cout << "[TEST] test_json_pads_carry_a_kind_not_just_a_label" << std::endl;

    ewr::DbPrinterModel model;
    model.name = "TwoPads";

    ewr::PadGroup main;
    main.description = "Main Pad Counter";
    main.kind = "main";
    main.addresses = { 0x10 };
    ewr::CounterSpec mainCounter;
    mainCounter.description = "Main Pad Counter";
    mainCounter.max_value = 100;
    mainCounter.bytes = { ewr::CounterByte{ 0x10, 0xFF, 1 } };
    main.counters.push_back(mainCounter);
    model.pad_groups.push_back(main);

    // No kind in the database: it has to come from the description, which is
    // the path most entries take.
    ewr::PadGroup platen;
    platen.description = "Platen Pad Counter";
    platen.addresses = { 0x11 };
    ewr::CounterSpec platenCounter;
    platenCounter.description = "Platen Pad Counter";
    platenCounter.max_value = 50;
    platenCounter.bytes = { ewr::CounterByte{ 0x11, 0xFF, 1 } };
    platen.counters.push_back(platenCounter);
    model.pad_groups.push_back(platen);

    const std::vector<std::pair<uint16_t, int>> values = { { 0x10, 37 }, { 0x11, 25 } };
    const nlohmann::json pads = ewr::JsonPadUsage(model, values);

    CHECK(pads.size() == 2);
    CHECK(pads[0]["kind"] == "main");
    CHECK(pads[0]["name"] == "Main Pad Counter");
    CHECK(pads[0]["used"] == 37);
    CHECK(pads[0]["max"] == 100);
    CHECK(pads[0]["percent"] == 37);
    CHECK(pads[1]["kind"] == "platen");
    CHECK(pads[1]["used"] == 25);
    CHECK(pads[1]["percent"] == 50);

    // A model that says neither reports null rather than guessing a pad.
    ewr::DbPrinterModel vague;
    vague.name = "Vague";
    ewr::PadGroup group;
    group.description = "Counter";
    group.addresses = { 0x20 };
    ewr::CounterSpec spec;
    spec.description = "Counter";
    spec.max_value = 10;
    spec.bytes = { ewr::CounterByte{ 0x20, 0xFF, 1 } };
    group.counters.push_back(spec);
    vague.pad_groups.push_back(group);

    const nlohmann::json vaguePads = ewr::JsonPadUsage(vague, { { 0x20, 5 } });
    CHECK(vaguePads.size() == 1);
    CHECK(vaguePads[0]["kind"].is_null());
}

// "not reported" and "zero" must not look alike: an ink the printer says
// nothing about reports a null level beside its own status text, and a
// maintenance box can report a condition without a level.
void test_json_status_separates_unknown_from_zero()
{
    std::cout << "[TEST] test_json_status_separates_unknown_from_zero" << std::endl;

    ewr::PrinterStatus status;
    status.valid = true;
    status.stateCode = 0x04;
    status.stateName = "IDLE";
    status.serial = "VHEK034809";
    status.maintenanceBoxLevel = -1;
    status.maintenanceBoxText = "NOT INSTALLED";
    status.inks = {
        ewr::InkReading{ 0, "Black", 43, "OK" },
        ewr::InkReading{ 1, "Cyan", -1, "UNKNOWN" },
        ewr::InkReading{ 2, "Magenta", 0, "EMPTY" },
    };

    const nlohmann::json out = ewr::JsonPrinterStatus(status);

    CHECK(out["state"] == "IDLE");
    CHECK(out["state_code"] == 4);
    CHECK(out["error"].is_null());
    CHECK(out["error_code"].is_null());
    CHECK(out["serial"] == "VHEK034809");

    CHECK(out["inks"][0]["level"] == 43);
    // Not reported: null, and never confused with the empty cartridge below.
    CHECK(out["inks"][1]["level"].is_null());
    CHECK(out["inks"][1]["status"] == "UNKNOWN");
    CHECK(out["inks"][2]["level"] == 0);

    CHECK(out["maintenance_box"].is_null());
    CHECK(out["maintenance_box_status"] == "NOT INSTALLED");

    // Nothing parseable at all is null, not an object of empty strings.
    ewr::PrinterStatus silent;
    CHECK(ewr::JsonPrinterStatus(silent).is_null());
}

// A host that ran detection fills detected_model; the library cannot invent
// it, and null has to mean "not asked" rather than "no match".
void test_json_state_data_reports_detection_as_unknown()
{
    std::cout << "[TEST] test_json_state_data_reports_detection_as_unknown" << std::endl;

    ewr::StateSnapshot state;
    state.available = true;
    state.status.valid = true;
    state.status.stateName = "IDLE";
    state.values = { { 0x18, 255 }, { 0x19, -1 } };

    const nlohmann::json data = ewr::JsonStateData(MakeTestModel(), state);

    CHECK(data["model"] == "TestPrinter");
    CHECK(data.contains("detected_model"));
    CHECK(data["detected_model"].is_null());
    CHECK(data["counters"][0]["address"] == 0x18);
    CHECK(data["counters"][0]["value"] == 255);
    // An unread byte is null, not 0 and not absent.
    CHECK(data["counters"][1]["value"].is_null());
}

// A counter's kind is what the counter says about itself, and nothing else.
// A pad group is a set of bytes reset together, not one pad: 489 entries file
// a "Platen Pad Counter" under a group marked main, and 87 counters named only
// by position ("Waste Counter 3") sit in groups marked main, 6 of them alone in
// one. Taking the group's kind labelled the first set wrongly and the second
// set with a guess the database build explicitly refuses to make (#35).
// Asserted over the real database rather than a few models, so the rule holds
// whatever build_db.py regenerates.
void test_every_counter_kind_matches_its_own_description()
{
    std::cout << "[TEST] test_every_counter_kind_matches_its_own_description" << std::endl;

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));

    size_t named = 0;
    size_t platen = 0;
    size_t unnamed = 0;

    for (const auto& model : gen.GetAvailableModels())
    {
        for (const auto& counter : model.GetAllCounters())
        {
            const std::string own = ewr::PadKindFromDescription(counter.description);

            if (counter.kind != own)
            {
                std::cout << "  [detail] " << model.name << ": \"" << counter.description
                          << "\" carries kind \"" << counter.kind << "\"" << std::endl;
            }
            CHECK(counter.kind == own);

            if (own.empty())
                ++unnamed;
            else
                ++named;

            if (own == "platen")
                ++platen;
        }
    }

    // Each branch has to have been exercised, or the test passes while
    // checking nothing: named counters, platen ones among them, and counters
    // whose pad nobody knows.
    CHECK(named > 100);
    CHECK(platen > 0);
    CHECK(unnamed > 0);
}

// The shapes from the database, as a caller sees them: a counter named only by
// position reports no kind, whether it shares its group (E-300) or sits in it
// alone (PM-A920) - and whatever the group is marked.
void test_unnamed_counter_reports_no_kind()
{
    std::cout << "[TEST] test_unnamed_counter_reports_no_kind" << std::endl;

    auto counter = [](const char* description, uint16_t address, uint32_t max)
    {
        ewr::CounterSpec spec;
        spec.description = description;
        spec.max_value = max;
        spec.bytes = { ewr::CounterByte{ address, 0xFF, 1 } };
        return spec;
    };

    // E-300: main, platen and a third counter in one group marked main.
    ewr::DbPrinterModel shared;
    shared.name = "SharedGroup";
    ewr::PadGroup group;
    group.description = "Main Pad Counter";
    group.kind = "main";
    group.addresses = { 0x10, 0x28, 0x26 };
    group.counters = { counter("Main Pad Counter", 0x10, 60000),
                       counter("Platen Pad Counter", 0x28, 4177),
                       counter("Waste Counter 3", 0x26, 600) };
    shared.pad_groups.push_back(group);

    const nlohmann::json pads = ewr::JsonPadUsage(shared, { { 0x10, 1 }, { 0x28, 2 }, { 0x26, 3 } });
    CHECK(pads.size() == 3);
    CHECK(pads[0]["kind"] == "main");
    CHECK(pads[1]["kind"] == "platen");
    CHECK(pads[2]["kind"].is_null());
    // Unknown pad, but still a counter with a limit: it stays in `pads`.
    CHECK(pads[2]["max"] == 600);

    // PM-A920: the unnamed counter alone in a group marked main.
    ewr::DbPrinterModel lone;
    lone.name = "LoneCounter";
    ewr::PadGroup single;
    single.description = "Main Pad Counter";
    single.kind = "main";
    single.addresses = { 0x30 };
    single.counters = { counter("Waste Counter 3", 0x30, 3260) };
    lone.pad_groups.push_back(single);

    const nlohmann::json lonePads = ewr::JsonPadUsage(lone, { { 0x30, 7 } });
    CHECK(lonePads.size() == 1);
    CHECK(lonePads[0]["kind"].is_null());
}

// `pads` says how full the readable pads are; `reset_covers` says which pads a
// reset would clear, readable or not. Without it, 809 of the 1337 resettable
// models hid a pad the reset clears: 669 have no counter at all (an L6490's
// only pad), and 140 reset a pad they cannot read beside one they can (an
// R220's platen pad). The invariants, over every model in the real database:
//   - a model the reset writes to never reports an empty list;
//   - a known pad appears once, however many groups it turns up in;
//   - the readable entries are exactly the pads `pads` can report;
//   - bytes the reset writes that no counter reads show up as their group's pad.
void test_reset_coverage_over_the_database()
{
    std::cout << "[TEST] test_reset_coverage_over_the_database" << std::endl;

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));

    size_t resettable = 0;
    size_t withUnreadable = 0;

    for (const auto& model : gen.GetAvailableModels())
    {
        std::vector<uint16_t> written;
        std::vector<uint16_t> read;
        for (const auto& group : model.pad_groups)
        {
            written.insert(written.end(), group.addresses.begin(), group.addresses.end());
            for (const auto& counter : group.counters)
                for (const auto& part : counter.bytes)
                    read.push_back(part.address);
        }
        if (written.empty())
            continue;

        ++resettable;
        const std::vector<ewr::PadCoverage> covers = model.GetResetCoverage();

        if (covers.empty())
            std::cout << "  [detail] " << model.name << ": resettable, but covers nothing" << std::endl;
        CHECK(!covers.empty());

        size_t mains = 0, platens = 0, unnamedReadable = 0;
        bool anyUnreadable = false;
        for (const auto& pad : covers)
        {
            mains += (pad.kind == "main") ? 1 : 0;
            platens += (pad.kind == "platen") ? 1 : 0;
            unnamedReadable += (pad.kind.empty() && pad.readable) ? 1 : 0;
            anyUnreadable = anyUnreadable || !pad.readable;
        }
        CHECK(mains <= 1);
        CHECK(platens <= 1);
        withUnreadable += anyUnreadable ? 1 : 0;

        // Readable entries against the counters themselves.
        bool mainCounter = false, platenCounter = false;
        size_t unnamedCounters = 0;
        for (const auto& counter : model.GetAllCounters())
        {
            mainCounter = mainCounter || counter.kind == "main";
            platenCounter = platenCounter || counter.kind == "platen";
            unnamedCounters += counter.kind.empty() ? 1 : 0;
        }

        auto readableKind = [&](const char* kind)
        {
            for (const auto& pad : covers)
                if (pad.kind == kind && pad.readable)
                    return true;
            return false;
        };
        CHECK(readableKind("main") == mainCounter);
        CHECK(readableKind("platen") == platenCounter);
        CHECK(unnamedReadable == unnamedCounters);

        // A group whose written bytes no counter reads is still covered.
        for (const auto& group : model.pad_groups)
        {
            bool unread = false;
            for (uint16_t addr : group.addresses)
                unread = unread || std::find(read.begin(), read.end(), addr) == read.end();

            const std::string kind = group.EffectiveKind();
            if (!unread || kind.empty())
                continue;

            bool present = false;
            for (const auto& pad : covers)
                present = present || pad.kind == kind;
            if (!present)
                std::cout << "  [detail] " << model.name << ": group \"" << group.description
                          << "\" has unread bytes but no " << kind << " entry" << std::endl;
            CHECK(present);
        }
    }

    // Both halves of the reason this exists have to be in the data, or the
    // test checks nothing that matters.
    CHECK(resettable > 1000);
    CHECK(withUnreadable > 500);
}

// The shapes each rule came from, as a caller sees them.
void test_reset_coverage_shapes()
{
    std::cout << "[TEST] test_reset_coverage_shapes" << std::endl;

    // A missing entry has to fail its check, not read past the end of the
    // vector and take the rest of the suite down with it.
    auto at = [](const std::vector<ewr::PadCoverage>& covers, size_t i)
    {
        return (i < covers.size()) ? covers[i] : ewr::PadCoverage{ "<missing>", "", false };
    };

    auto counter = [](const char* description, uint16_t address)
    {
        ewr::CounterSpec spec;
        spec.description = description;
        spec.max_value = 100;
        spec.bytes = { ewr::CounterByte{ address, 0xFF, 1 } };
        return spec;
    };
    auto group = [](const char* description, const char* kind, std::vector<uint16_t> addresses)
    {
        ewr::PadGroup g;
        g.description = description;
        g.kind = kind;
        g.addresses = std::move(addresses);
        return g;
    };

    // L6490: its only pad has bytes to reset and no counter to read them.
    {
        ewr::DbPrinterModel m;
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28, 47, 50 }));
        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 1);
        CHECK(at(covers, 0).kind == "platen");
        CHECK(!at(covers, 0).readable);
        CHECK(m.GetAllCounters().empty()); // so `pads_total` is 0
    }

    // R220: a readable main pad, and a platen pad the reset clears blind.
    {
        ewr::DbPrinterModel m;
        auto main = group("Main Pad Counter", "main", { 12, 13 });
        main.counters = { counter("Main Pad Counter", 12) };
        main.counters[0].bytes.push_back(ewr::CounterByte{ 13, 0xFF, 256 });
        m.pad_groups.push_back(main);
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 43, 62, 63 }));

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 2);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && !at(covers, 1).readable);
    }

    // E-300: one group marked main holds all three counters. Its label alone
    // would say the reset covers only main.
    {
        ewr::DbPrinterModel m;
        auto g = group("Main Pad Counter", "main", { 16, 38, 40 });
        g.counters = { counter("Main Pad Counter", 16),
                       counter("Platen Pad Counter", 40),
                       counter("Waste Counter 3", 38) };
        m.pad_groups.push_back(g);

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 3);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && at(covers, 1).readable);
        CHECK(at(covers, 2).kind.empty() && at(covers, 2).readable);
        CHECK(at(covers, 2).name == "Waste Counter 3");
    }

    // BX305FW: the platen counter lives in the main group and the platen group
    // holds more platen bytes nobody reads. Still one platen pad, and readable.
    {
        ewr::DbPrinterModel m;
        auto first = group("Main Pad Counter", "main", { 24, 26 });
        first.counters = { counter("Main Pad Counter", 24), counter("Platen Pad Counter", 26) };
        m.pad_groups.push_back(first);
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28, 46 }));

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 2);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && at(covers, 1).readable);
    }

    // What a caller gets: the key is there whether or not the printer answered,
    // because it comes from the database, not from the reading.
    {
        ewr::DbPrinterModel m;
        m.name = "BlindPlaten";
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28 }));

        ewr::StateSnapshot silent;
        const nlohmann::json data = ewr::JsonStateData(m, silent);
        CHECK(data["pads_total"] == 0);
        CHECK(data["pads"].empty());
        CHECK(data["reset_covers"].size() == 1);
        if (data["reset_covers"].size() == 1)
        {
            CHECK(data["reset_covers"][0]["kind"] == "platen");
            CHECK(data["reset_covers"][0]["readable"] == false);
        }
    }
}

// The C API's device-free plan says which pads a reset clears, and an ink reset
// clears none.
void test_c_abi_plan_reports_reset_coverage()
{
    std::cout << "[TEST] test_c_abi_plan_reports_reset_coverage" << std::endl;

    ewr_session* session = nullptr;
    const int opened = ewr_session_open("database.json", &session);
    if (opened == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }
    CHECK(opened == EWR_OK);

    char* json = nullptr;
    CHECK(ewr_plan(session, "R220", 0, &json) == EWR_OK);
    const nlohmann::json waste = nlohmann::json::parse(json);
    ewr_string_free(json);
    CHECK(waste["reset_covers"].size() == 2);
    if (waste["reset_covers"].size() == 2)
    {
        CHECK(waste["reset_covers"][0]["kind"] == "main");
        CHECK(waste["reset_covers"][0]["readable"] == true);
        CHECK(waste["reset_covers"][1]["kind"] == "platen");
        CHECK(waste["reset_covers"][1]["readable"] == false);
    }

    ewr_session_close(session);
}

// An empty `pads` used to mean two different things: a model with no pads, and
// a model whose pads all went unread. `pads_total` is what tells them apart -
// a caller seeing 0 of 2 knows to retry rather than to conclude there is
// nothing to reset.
void test_json_state_data_counts_pads_it_could_not_read()
{
    std::cout << "[TEST] test_json_state_data_counts_pads_it_could_not_read" << std::endl;

    ewr::DbPrinterModel model;
    model.name = "TwoPads";
    for (int i = 0; i < 2; ++i)
    {
        ewr::PadGroup group;
        group.description = (i == 0) ? "Main Pad Counter" : "Platen Pad Counter";
        group.addresses = { static_cast<uint16_t>(0x30 + i) };
        ewr::CounterSpec spec;
        spec.description = group.description;
        spec.max_value = 100;
        spec.bytes = { ewr::CounterByte{ static_cast<uint16_t>(0x30 + i), 0xFF, 1 } };
        group.counters.push_back(spec);
        model.pad_groups.push_back(group);
    }

    // One pad answered, the other did not.
    ewr::StateSnapshot half;
    half.available = true;
    half.status.valid = true;
    half.values = { { 0x30, 12 }, { 0x31, -1 } };

    const nlohmann::json partial = ewr::JsonStateData(model, half);
    CHECK(partial["pads_total"] == 2);
    CHECK(partial["pads"].size() == 1);
    CHECK(partial["pads"][0]["kind"] == "main");

    // Nothing answered at all: still two pads on this model.
    ewr::StateSnapshot silent;
    const nlohmann::json none = ewr::JsonStateData(model, silent);
    CHECK(none["pads_total"] == 2);
    CHECK(none["pads"].empty());
    CHECK(none["counters"].empty());
    CHECK(none["printer"].is_null());

    // A model with no pad groups reports zero, which is the other meaning.
    ewr::DbPrinterModel bare;
    bare.name = "NoPads";
    const nlohmann::json empty = ewr::JsonStateData(bare, half);
    CHECK(empty["pads_total"] == 0);
    CHECK(empty["pads"].empty());
}

// The C ABI is what another language links against, so the test is the
// promise: codes keep their numbers, an unknown one still answers, and the
// parts that need no printer work without one.
void test_c_abi_constants_and_status_names()
{
    std::cout << "[TEST] test_c_abi_constants_and_status_names" << std::endl;

    // 2 added ewr_session_open_network.
    CHECK(ewr_abi_version() == 2);
    CHECK(ewr_json_contract_version() == ewr::JsonEmitter::kContractVersion);
    CHECK(std::string(ewr_version()) == EWR_VERSION);

    // Renumbering these would silently change what a caller's constant means.
    CHECK(EWR_OK == 0);
    CHECK(EWR_ERR_FAILED == 1);
    CHECK(EWR_ERR_INVALID_ARGUMENT == 2);
    CHECK(EWR_ERR_ANOTHER_RUN == 3);
    CHECK(EWR_ERR_DEVICE_NOT_FOUND == 4);
    CHECK(EWR_ERR_BLOCKED == 10);
    CHECK(EWR_ERR_WRITE_UNVERIFIED == 12);

    CHECK(std::string(ewr_status_name(EWR_OK)) == "ok");
    CHECK(std::string(ewr_status_name(EWR_ERR_BLOCKED)) == "blocked");
    CHECK(std::string(ewr_status_name(EWR_ERR_INCOMPLETE_DUMP)) == "incomplete_dump");
    CHECK(std::string(ewr_status_name(EWR_ERR_MODEL_MISMATCH)) == "model_mismatch");
    // A caller compiled against a newer header must still get an answer.
    CHECK(std::string(ewr_status_name(9999)) == "unknown");

    // Freeing nothing is not an error, so a caller need not guard its cleanup.
    ewr_string_free(nullptr);

    // A null out-parameter is refused rather than dereferenced.
    CHECK(ewr_session_open("database.json", nullptr) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(ewr_list_models(nullptr, nullptr) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(std::string(ewr_session_last_error(nullptr)).empty());
}

// The database-only calls: no printer, no hardware, still useful - this is
// what a host uses to populate a model list and show what a reset would do.
void test_c_abi_database_calls_need_no_printer()
{
    std::cout << "[TEST] test_c_abi_database_calls_need_no_printer" << std::endl;

    ewr_session* session = nullptr;
    const int opened = ewr_session_open("database.json", &session);

    // Another EWR holding the printer is not a test failure, just nothing to
    // test against here.
    if (opened == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }

    CHECK(opened == EWR_OK);
    CHECK(session != nullptr);
    CHECK(std::string(ewr_session_last_error(session)).empty());

    char* json = nullptr;
    CHECK(ewr_list_models(session, &json) == EWR_OK);
    const nlohmann::json models = nlohmann::json::parse(json);
    ewr_string_free(json);
    CHECK(models["models"].is_array());
    CHECK(models["models"].size() > 1000);
    CHECK(models["models"][0].contains("name"));
    CHECK(models["models"][0].contains("resettable"));

    json = nullptr;
    CHECK(ewr_plan(session, "R220", 0, &json) == EWR_OK);
    const nlohmann::json plan = nlohmann::json::parse(json);
    ewr_string_free(json);
    CHECK(plan["model"] == "R220");
    CHECK(plan["target"] == "waste");
    CHECK(plan["planned_writes"].size() == 5);
    // Numbers, not "0x0C" strings: the contract's rule.
    CHECK(plan["planned_writes"][0]["address"].is_number());
    CHECK(plan["planned_writes"][0]["value"].is_number());

    json = nullptr;
    CHECK(ewr_plan(session, "no such printer", 0, &json) == EWR_ERR_MODEL_UNKNOWN);
    CHECK(json == nullptr);
    CHECK(!std::string(ewr_session_last_error(session)).empty());

    // No name is the caller's mistake, not a model the database lacks.
    CHECK(ewr_plan(session, nullptr, 0, &json) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(ewr_plan(session, "", 0, &json) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(json == nullptr);

    ewr_session_close(session);
}

// A database that is not there fails as a database problem, with the handle
// still returned so the caller can read why.
void test_c_abi_reports_a_missing_database()
{
    std::cout << "[TEST] test_c_abi_reports_a_missing_database" << std::endl;

    ewr_session* session = nullptr;
    const int opened = ewr_session_open("no-such-database.json", &session);

    if (opened == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }

    CHECK(opened == EWR_ERR_DATABASE);
    CHECK(session != nullptr);
    CHECK(!std::string(ewr_session_last_error(session)).empty());

    char* json = nullptr;
    CHECK(ewr_list_models(session, &json) == EWR_ERR_DATABASE);
    CHECK(json == nullptr);

    ewr_session_close(session);
}

// A network session is chosen at open and keeps to the CLI's --ip scope: what
// was never tried over the network is refused, not attempted. Nothing here
// sends a datagram - 192.0.2.1 is TEST-NET-1 and is only ever pointed at.
void test_c_abi_network_session()
{
    std::cout << "[TEST] test_c_abi_network_session" << std::endl;

    // No host is the caller's mistake, never a USB session in its place.
    ewr_session* session = nullptr;
    CHECK(ewr_session_open_network("database.json", nullptr, &session) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(ewr_session_open_network("database.json", "", &session) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(session == nullptr);
    CHECK(ewr_session_open_network("database.json", "192.0.2.1", nullptr) == EWR_ERR_INVALID_ARGUMENT);

    // One lock, whichever way a run reaches its printer.
    ewr_session* usb = nullptr;
    const int usbOpened = ewr_session_open("database.json", &usb);
    if (usbOpened == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(usb);
        return;
    }
    CHECK(usbOpened == EWR_OK);
    CHECK(ewr_session_open_network("database.json", "192.0.2.1", &session) == EWR_ERR_ANOTHER_RUN);
    CHECK(session == nullptr);
    ewr_session_close(usb);

    auto readTrace = []() -> std::string
    {
        std::ifstream in("ewr_trace.log", std::ios::binary);
        return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string("(absent)");
    };
    const std::string traceBefore = readTrace();

    CHECK(ewr_session_open_network("database.json", "192.0.2.1", &session) == EWR_OK);
    CHECK(session != nullptr);

    char* json = nullptr;

    // The search is refused before it listens, not after.
    CHECK(ewr_discover_network(nullptr, &json) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(ewr_discover_network(session, nullptr) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(json == nullptr);
    CHECK(ewr_list_models(session, &json) == EWR_OK);
    ewr_string_free(json);
    json = nullptr;
    CHECK(ewr_plan(session, "R220", 0, &json) == EWR_OK);
    ewr_string_free(json);
    json = nullptr;

    CHECK(ewr_list_interfaces(session, &json) == EWR_ERR_NOT_SUPPORTED);
    CHECK(ewr_dump(session, "R220", &json) == EWR_ERR_NOT_SUPPORTED);
    CHECK(ewr_reset(session, "R220", 1, &json) == EWR_ERR_NOT_SUPPORTED);
    CHECK(json == nullptr);
    CHECK(std::string(ewr_session_last_error(session)).find("network") != std::string::npos);

    // Opened and refused without a device call: the last run's trace stands.
    CHECK(readTrace() == traceBefore);
    ewr_session_close(session);

    // A host that does not resolve is known at open, with the session there
    // to say so and the database calls still working.
    session = nullptr;
    CHECK(ewr_session_open_network("database.json", "no-such-printer.invalid", &session) == EWR_ERR_DEVICE_NOT_FOUND);
    CHECK(session != nullptr);
    CHECK(std::string(ewr_session_last_error(session)).find("no-such-printer.invalid") != std::string::npos);
    CHECK(ewr_list_models(session, &json) == EWR_OK);
    ewr_string_free(json);
    ewr_session_close(session);
}

// The --json contract (docs/json-output.md) is what a caller's program is
// written against, so the shape is the test: an envelope on every line, one
// hello first, one result last, and a line that stays one line whatever the
// message contains.
void test_json_contract_envelope_and_order()
{
    std::cout << "[TEST] test_json_contract_envelope_and_order" << std::endl;

    std::ostringstream out;
    {
        ewr::JsonEmitter emitter(out);
        emitter.Hello("1.2.3", "linux", "status", nullptr, nlohmann::json::object());

        ewr::log::Event event;
        event.level = ewr::log::Level::Info;
        event.stage = ewr::log::Stage::Write;
        event.code = "exec.write_verified";
        event.message = "wrote\n\"0x2B\"\ttab";  // newline, quote and tab
        event.index = 3;
        event.total = 5;
        event.fields = { { "address", "0x2B" } };
        emitter.Event(event);

        // Trace belongs to ewr_trace.log, not to the contract.
        ewr::log::Event trace;
        trace.level = ewr::log::Level::Trace;
        trace.code = "d4.tx";
        emitter.Event(trace);

        emitter.Result("status", true, 0, nullptr, nullptr, { { "counters", nlohmann::json::array() } });
        // A second verdict would end the stream twice.
        emitter.Result("status", false, 1, "failed", "ignored", nlohmann::json::object());
    }

    std::vector<nlohmann::json> lines;
    std::istringstream reader(out.str());
    for (std::string line; std::getline(reader, line); )
    {
        CHECK(line.find('\n') == std::string::npos);
        lines.push_back(nlohmann::json::parse(line));
    }

    CHECK(lines.size() == 3); // hello, one event, one result - the trace dropped

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        CHECK(lines[i]["v"] == ewr::JsonEmitter::kContractVersion);
        CHECK(lines[i]["seq"] == static_cast<int>(i));
        CHECK(lines[i].contains("t"));
    }

    CHECK(lines[0]["type"] == "hello");
    CHECK(lines[0]["command"] == "status");
    CHECK(lines[0]["model"].is_null());

    CHECK(lines[1]["type"] == "event");
    CHECK(lines[1]["code"] == "exec.write_verified");
    CHECK(lines[1]["level"] == "info");
    CHECK(lines[1]["stage"] == "write");
    CHECK(lines[1]["index"] == 3);
    CHECK(lines[1]["total"] == 5);
    CHECK(lines[1]["fields"]["address"] == "0x2B");
    // The escaping survived the round trip rather than breaking the line.
    CHECK(lines[1]["message"] == "wrote\n\"0x2B\"\ttab");

    CHECK(lines[2]["type"] == "result");
    CHECK(lines[2]["ok"] == true);
    CHECK(lines[2]["exit"] == 0);
    CHECK(lines[2]["error_code"].is_null());
    CHECK(lines[2]["data"]["counters"].is_array());
}

// An event with no progress reports null rather than the -1 the struct uses,
// so a caller can test for absence without knowing EWR's sentinel.
void test_json_contract_reports_absent_progress_as_null()
{
    std::cout << "[TEST] test_json_contract_reports_absent_progress_as_null" << std::endl;

    std::ostringstream out;
    ewr::JsonEmitter emitter(out);

    ewr::log::Event event;
    event.level = ewr::log::Level::Warning;
    event.stage = ewr::log::Stage::Detect;
    event.code = "usb.another_run";
    emitter.Event(event);

    const nlohmann::json line = nlohmann::json::parse(out.str());
    CHECK(line["index"].is_null());
    CHECK(line["total"].is_null());
    CHECK(line["level"] == "warning");
    CHECK(line["stage"] == "detect");
    CHECK(line["fields"].is_object());
}

// Two EWR runs on one printer take each other's replies, and the one that
// loses a reply may be the one mid-write (issue #39). The second run is meant
// to find the lock taken and stop, and a run that ends must leave it free -
// including one that was killed, which is why it is an OS lock and not a file
// anybody has to clean up.
void test_run_lock_admits_one_run_at_a_time()
{
    std::cout << "[TEST] test_run_lock_admits_one_run_at_a_time" << std::endl;

    {
        ewr::RunLock first;
        CHECK(first.Held());

        ewr::RunLock second;
        CHECK(!second.Held());
    }

    // Both are gone: the next run gets it.
    ewr::RunLock afterwards;
    CHECK(afterwards.Held());
}

// ---------------------------------------------------------------------------
// Composite backend (usb_composite.cpp)
// ---------------------------------------------------------------------------

// Describe(), QueryDeviceId() and AttemptsPerCandidate() all echo the ordinal
// they were handed, so a mistranslated ordinal fails loudly instead of
// looking like a pass.
class FakeBackend final : public ewr::UsbBackend
{
public:
    FakeBackend(std::string name, std::vector<ewr::UsbCandidate> candidates, std::string initError = "")
        : name_(std::move(name)), candidates_(std::move(candidates)), initError_(std::move(initError))
    {
        for (std::size_t i = 0; i < candidates_.size(); ++i)
            candidates_[i].ordinal = i;
    }

    const char* PlatformName() const override { return name_.c_str(); }
    const std::string& InitError() const override { return initError_; }

    std::vector<ewr::UsbCandidate> Enumerate() override
    {
        ++enumerations;
        return candidates_;
    }

    std::string Describe(std::size_t ordinal) const override
    {
        return name_ + " candidate " + std::to_string(ordinal);
    }

    ewr::ITransport* Open(std::size_t ordinal) override
    {
        opened.push_back(ordinal);
        return openSucceeds ? &transport : nullptr;
    }

    void Close() override { ++closes; }

    bool SoftReset() override
    {
        ++softResets;
        return softResetSucceeds;
    }

    int AttemptsPerCandidate(std::size_t ordinal) const override
    {
        return attemptsBase + static_cast<int>(ordinal);
    }

    std::string QueryDeviceId(std::size_t ordinal) override
    {
        return name_ + " id " + std::to_string(ordinal);
    }

    std::string DescribeOpenFailure(bool) override { return name_ + " could not open."; }

    FakeTransport transport;
    std::vector<std::size_t> opened;
    int closes = 0;
    int softResets = 0;
    int enumerations = 0;
    int attemptsBase = 1;
    bool openSucceeds = true;
    bool softResetSucceeds = true;

private:
    std::string name_;
    std::vector<ewr::UsbCandidate> candidates_;
    std::string initError_;
};

static ewr::UsbCandidate MakeCandidate(const std::string& className, int interfaceNumber,
                                       const std::string& pid = "1187")
{
    ewr::UsbCandidate cand;
    cand.className = className;
    cand.interfaceNumber = interfaceNumber;
    cand.path = className + "#if_" + std::to_string(interfaceNumber);
    cand.pid = pid;
    return cand;
}

static ewr::UsbBackendMember Member(std::unique_ptr<ewr::UsbBackend> backend, std::string tag)
{
    ewr::UsbBackendMember member;
    member.backend = std::move(backend);
    member.tag = std::move(tag);
    return member;
}

void test_composite_merges_candidates_in_member_order()
{
    std::cout << "[TEST] test_composite_merges_candidates_in_member_order" << std::endl;

    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", 1), MakeCandidate("IMAGE", 0) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("VENDOR", 2), MakeCandidate("VENDOR", 3) });

    std::vector<ewr::UsbBackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::UsbBackend> composite = ewr::CreateCompositeUsbBackend(std::move(members), trace);

    CHECK(std::string(composite->PlatformName()) == "usbprint + libusb");
    CHECK(composite->InitError().empty());

    const std::vector<ewr::UsbCandidate> merged = composite->Enumerate();
    CHECK(merged.size() == 4);

    // Order is the guarantee: a printer that already worked never reaches
    // libusb.
    CHECK(merged[0].className == "USBPRINT");
    CHECK(merged[1].className == "IMAGE");
    CHECK(merged[2].className == "libusb:VENDOR");
    CHECK(merged[3].className == "libusb:VENDOR");

    // --interface N and the fallback ladder both address candidates by this.
    for (std::size_t i = 0; i < merged.size(); ++i)
        CHECK(merged[i].ordinal == i);

    CHECK(merged[2].interfaceNumber == 2);
    CHECK(merged[2].pid == "1187");
    CHECK(merged[0].path == "USBPRINT#if_1");
}

void test_composite_identifies_a_shared_interface_by_number()
{
    std::cout << "[TEST] test_composite_identifies_a_shared_interface_by_number" << std::endl;

    // mi_01 is bound to usbprint.sys and still visible to libusb; listing it
    // twice only spends a failed claim on a handle usbprint already holds.
    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", 1) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{
            MakeCandidate("PRINTER", 1),              // the same interface
            MakeCandidate("VENDOR", 1, "1234"),       // same number, different device
            MakeCandidate("VENDOR", 2) });

    std::vector<ewr::UsbBackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::UsbBackend> composite = ewr::CreateCompositeUsbBackend(std::move(members), trace);

    const std::vector<ewr::UsbCandidate> merged = composite->Enumerate();
    CHECK(merged.size() == 3);
    CHECK(merged[0].className == "USBPRINT");
    CHECK(merged[1].className == "libusb:VENDOR" && merged[1].pid == "1234");
    CHECK(merged[2].className == "libusb:VENDOR" && merged[2].interfaceNumber == 2);

    for (std::size_t i = 0; i < merged.size(); ++i)
        CHECK(merged[i].ordinal == i);

    // No mi_XX means non-composite: one function, so libusb's interface 0 on
    // that PID is the same one.
    auto lonePrimary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", -1) });
    auto loneSecondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("PRINTER", 0) });

    std::vector<ewr::UsbBackendMember> loneMembers;
    loneMembers.push_back(Member(std::move(lonePrimary), ""));
    loneMembers.push_back(Member(std::move(loneSecondary), "libusb"));

    std::unique_ptr<ewr::UsbBackend> lone = ewr::CreateCompositeUsbBackend(std::move(loneMembers), trace);
    CHECK(lone->Enumerate().size() == 1);
    CHECK(lone->Enumerate()[0].className == "USBPRINT");

    // Two of the same printer share a PID and an interface number and are
    // still two printers.
    auto twins = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", 1), MakeCandidate("USBPRINT", 1) });

    std::vector<ewr::UsbBackendMember> twinMembers;
    twinMembers.push_back(Member(std::move(twins), ""));

    std::unique_ptr<ewr::UsbBackend> pair = ewr::CreateCompositeUsbBackend(std::move(twinMembers), trace);
    CHECK(pair->Enumerate().size() == 2);
}

void test_composite_routes_every_call_to_the_owning_member()
{
    std::cout << "[TEST] test_composite_routes_every_call_to_the_owning_member" << std::endl;

    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", 1), MakeCandidate("IMAGE", 0) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("VENDOR", 2), MakeCandidate("VENDOR", 3) });

    FakeBackend* primaryRaw = primary.get();
    FakeBackend* secondaryRaw = secondary.get();

    // usbprint retries a silent handshake on a fresh handle; libusb does not.
    primaryRaw->attemptsBase = 2;
    secondaryRaw->attemptsBase = 1;

    std::vector<ewr::UsbBackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::UsbBackend> composite = ewr::CreateCompositeUsbBackend(std::move(members), trace);
    composite->Enumerate();

    // Composite ordinal 3 is libusb's own ordinal 1.
    CHECK(composite->Describe(3) == "[libusb] libusb candidate 1");
    CHECK(composite->QueryDeviceId(3) == "libusb id 1");
    CHECK(composite->AttemptsPerCandidate(3) == 2); // 1 + local ordinal 1
    CHECK(composite->AttemptsPerCandidate(2) == 1); // 1 + local ordinal 0
    CHECK(composite->AttemptsPerCandidate(1) == 3); // 2 + local ordinal 1

    CHECK(composite->Open(3) == &secondaryRaw->transport);
    CHECK(secondaryRaw->opened == std::vector<std::size_t>{ 1 });
    CHECK(primaryRaw->opened.empty());

    // The reset goes to the transport holding the handle, and its "nothing
    // was reset" answer comes back unchanged.
    secondaryRaw->softResetSucceeds = false;
    CHECK(!composite->SoftReset());
    CHECK(secondaryRaw->softResets == 1);
    CHECK(primaryRaw->softResets == 0);

    composite->Close();
    CHECK(secondaryRaw->closes == 1);
    CHECK(primaryRaw->closes == 0);

    // Refused, not routed to member zero.
    CHECK(composite->Open(99) == nullptr);
    CHECK(composite->Describe(99) == "<invalid candidate>");
    CHECK(composite->QueryDeviceId(99).empty());
    CHECK(secondaryRaw->opened.size() == 1);

    // A failed open still leaves the diagnosis with the transport that tried.
    secondaryRaw->openSucceeds = false;
    CHECK(composite->Open(2) == nullptr);
    CHECK(composite->DescribeOpenFailure(true) == "libusb could not open.");
}

void test_composite_runs_on_the_transports_that_came_up()
{
    std::cout << "[TEST] test_composite_runs_on_the_transports_that_came_up" << std::endl;

    // libusb failing to initialise must not stop a run usbprint can finish.
    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("USBPRINT", 1) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{ MakeCandidate("VENDOR", 2) }, "Failed to initialize libusb.");

    FakeBackend* secondaryRaw = secondary.get();

    std::vector<ewr::UsbBackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::UsbBackend> composite = ewr::CreateCompositeUsbBackend(std::move(members), trace);

    CHECK(composite->InitError().empty());
    CHECK(std::string(composite->PlatformName()) == "usbprint");
    CHECK(composite->Enumerate().size() == 1);
    CHECK(secondaryRaw->enumerations == 0);
    CHECK(trace.str().find("Failed to initialize libusb.") != std::string::npos);

    // Every transport down is the only fatal case, and the reason survives.
    std::vector<ewr::UsbBackendMember> deadMembers;
    deadMembers.push_back(Member(std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::UsbCandidate>{}, "SetupAPI enumeration failed."), ""));
    deadMembers.push_back(Member(std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::UsbCandidate>{}, "Failed to initialize libusb."), "libusb"));

    std::unique_ptr<ewr::UsbBackend> dead = ewr::CreateCompositeUsbBackend(std::move(deadMembers), trace);
    CHECK(dead->InitError() == "SetupAPI enumeration failed.");
    CHECK(dead->Enumerate().empty());
}

// The exact 57 bytes topdeckg's ET-2800 returned to the END4 packet-mode
// flush in issue #16.
static std::vector<unsigned char> Http500Reply()
{
    const std::string body = "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    return std::vector<unsigned char>(body.begin(), body.end());
}

void test_http_personality_detection()
{
    std::cout << "[TEST] test_http_personality_detection" << std::endl;

    const std::vector<unsigned char> http = Http500Reply();
    CHECK(http.size() == 57);
    CHECK(ewr::LooksLikeHttpReply(http.data(), http.size()));

    const std::string ok = "HTTP/1.1 200 OK\r\n";
    CHECK(ewr::LooksLikeHttpReply(reinterpret_cast<const unsigned char*>(ok.data()), ok.size()));

    // Only a leading status line counts. A D4 packet that happens to carry the
    // bytes further in is still a D4 packet.
    const std::vector<unsigned char> d4 = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x80, 0x00 };
    CHECK(!ewr::LooksLikeHttpReply(d4.data(), d4.size()));

    std::vector<unsigned char> buried = { 0x00, 0x00 };
    buried.insert(buried.end(), http.begin(), http.end());
    CHECK(!ewr::LooksLikeHttpReply(buried.data(), buried.size()));

    // Short and empty buffers must not read past the end.
    const std::vector<unsigned char> shortBuf = { 'H', 'T', 'T' };
    CHECK(!ewr::LooksLikeHttpReply(shortBuf.data(), shortBuf.size()));
    CHECK(!ewr::LooksLikeHttpReply(nullptr, 0));
}

// The four-week bug: the run reported "returned data but never an END4 reply",
// which is true and tells nobody which interface to look at instead.
void test_end4_reports_the_http_personality()
{
    std::cout << "[TEST] test_end4_reports_the_http_personality" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) { return Http500Reply(); };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

    CHECK(!result.success);
    CHECK(result.httpReply);
    CHECK(result.anyBytes);
    CHECK(result.error == ewr::kHttpPersonalityError);
}

void test_esc_remote_reports_the_http_personality()
{
    std::cout << "[TEST] test_esc_remote_reports_the_http_personality" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ExtractFactoryWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) { return Http500Reply(); };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::End4Result result =
        ewr::ExecuteEscRemoteSequence(transport, commands, reporter, options);

    CHECK(!result.success);
    CHECK(result.httpReply);
    CHECK(result.error == ewr::kHttpPersonalityError);
}

// The D4 framer sees the status line before its resync discards it as junk.
void test_d4_session_reports_the_http_personality()
{
    std::cout << "[TEST] test_d4_session_reports_the_http_personality" << std::endl;

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) { return Http500Reply(); };

    ewr::ExecutorOptions options;
    options.handshakeDrainTimeoutMs = 60;
    options.writeAckTimeoutMs = 20;
    options.interPacketDelayMs = 0;

    ewr::log::Reporter reporter;
    const ewr::QuerySessionResult result =
        ewr::ExecuteQuerySessionD4(transport, {}, reporter, options);

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(result.error == ewr::kHttpPersonalityError);
}

// ---------------------------------------------------------------------------
// Address discovery (discover.cpp)
// ---------------------------------------------------------------------------

static ewr::EepromSnapshot Snap(std::initializer_list<std::pair<uint16_t, int>> v)
{
    return ewr::EepromSnapshot(v);
}

void test_discovery_needs_three_passes()
{
    std::cout << "[TEST] test_discovery_needs_three_passes" << std::endl;

    // Two passes give one interval, and one interval cannot tell a counter
    // from a byte that happened to move once.
    const std::vector<ewr::EepromSnapshot> two = {
        Snap({ {0x30, 10} }),
        Snap({ {0x30, 11} }),
    };
    CHECK(ewr::FindTrendingBytes(two).empty());
    CHECK(ewr::FindTrendingBytes({}).empty());
}

void test_discovery_keeps_only_what_moves_one_way_every_time()
{
    std::cout << "[TEST] test_discovery_keeps_only_what_moves_one_way_every_time" << std::endl;

    const std::vector<ewr::EepromSnapshot> passes = {
        Snap({ {0x30, 10}, {0x40, 5}, {0x50, 7}, {0x60, 9}, {0x70, 90} }),
        Snap({ {0x30, 14}, {0x40, 9}, {0x50, 7}, {0x60, 3}, {0x70, 80} }),
        Snap({ {0x30, 21}, {0x40, 9}, {0x50, 7}, {0x60, 8}, {0x70, 55} }),
    };

    const std::vector<ewr::ByteTrend> found = ewr::FindTrendingBytes(passes);

    // 0x30 rises twice and 0x70 falls twice: both are trends. 0x40 rises then
    // stalls, 0x50 never moves, 0x60 falls then rises - the noise to reject.
    CHECK(found.size() == 2);

    CHECK(found[0].addresses == std::vector<uint16_t>{ 0x30 });
    CHECK(found[0].rising);
    CHECK(found[0].values == std::vector<uint32_t>({ 10, 14, 21 }));
    CHECK(found[0].deltas == std::vector<int64_t>({ 4, 7 }));

    // A falling byte is reported too - it is not a waste counter, and saying
    // so is the reader's job, not this function's.
    CHECK(found[1].addresses == std::vector<uint16_t>{ 0x70 });
    CHECK(!found[1].rising);
    CHECK(found[1].values == std::vector<uint32_t>({ 90, 80, 55 }));
    CHECK(found[1].deltas == std::vector<int64_t>({ -10, -25 }));
}

void test_discovery_reads_a_wrapping_low_byte_as_a_pair()
{
    std::cout << "[TEST] test_discovery_reads_a_wrapping_low_byte_as_a_pair" << std::endl;

    // 0x0130 -> 0x0202 -> 0x02FE. The low byte goes 0x30, 0x02, 0xFE: it breaks
    // its own run at the first interval. Judged alone it is discarded; judged
    // as the pair it belongs to, it is a counter climbing through a rollover.
    const std::vector<ewr::EepromSnapshot> passes = {
        Snap({ {0x30, 0x30}, {0x31, 0x01} }),
        Snap({ {0x30, 0x02}, {0x31, 0x02} }),
        Snap({ {0x30, 0xFE}, {0x31, 0x02} }),
    };

    const std::vector<ewr::ByteTrend> found = ewr::FindTrendingBytes(passes);

    CHECK(found.size() == 1);
    CHECK(found[0].IsPair());
    CHECK(found[0].rising);
    CHECK(found[0].addresses == std::vector<uint16_t>({ 0x30, 0x31 }));
    CHECK(found[0].values == std::vector<uint32_t>({ 0x0130, 0x0202, 0x02FE }));

    // The high byte must not also surface on its own: it is spoken for.
    for (const auto& t : found)
        CHECK(!(t.addresses.size() == 1 && t.addresses[0] == 0x31));
}

void test_discovery_ignores_addresses_that_went_unanswered()
{
    std::cout << "[TEST] test_discovery_ignores_addresses_that_went_unanswered" << std::endl;

    // A -1 is "the printer did not answer", not a low reading. Treating it as
    // one would manufacture a trend out of a failed read.
    const std::vector<ewr::EepromSnapshot> passes = {
        Snap({ {0x30, -1}, {0x44, 1} }),
        Snap({ {0x30, 50}, {0x44, 2} }),
        Snap({ {0x30, 90}, {0x44, 3} }),
    };

    const std::vector<ewr::ByteTrend> found = ewr::FindTrendingBytes(passes);

    CHECK(found.size() == 1);
    CHECK(found[0].addresses == std::vector<uint16_t>{ 0x44 });
}

void test_discovery_json_records_readings_not_a_reset_plan()
{
    std::cout << "[TEST] test_discovery_json_records_readings_not_a_reset_plan" << std::endl;

    const std::vector<ewr::EepromSnapshot> passes = {
        Snap({ {0x30, 1}, {0x31, 0}, {0x70, 9} }),
        Snap({ {0x30, 2}, {0x31, 0}, {0x70, 6} }),
        Snap({ {0x30, 3}, {0x31, 0}, {0x70, 2} }),
    };

    const std::vector<ewr::ByteTrend> found = ewr::FindTrendingBytes(passes);
    CHECK(found.size() == 2);

    const std::string body = "            \"rkey\": 1304,\n";
    const std::string doc = ewr::FormatDiscoveryJson("ET-M1180", body, found);

    CHECK(doc.find("\"schema_version\": 4") != std::string::npos);
    CHECK(doc.find("\"ET-M1180\"") != std::string::npos);
    CHECK(doc.find("\"rkey\": 1304") != std::string::npos);

    // Empty on purpose: nothing observed here is a reset plan, and a populated
    // pad_groups would invite someone to paste it straight into the database.
    CHECK(doc.find("\"pad_groups\": []") != std::string::npos);
    CHECK(doc.find("no reset value") != std::string::npos);

    // Both directions are recorded, with the three readings per address.
    CHECK(doc.find("\"values\": [1, 2, 3]") != std::string::npos);
    CHECK(doc.find("\"direction\": \"rising\"") != std::string::npos);
    CHECK(doc.find("\"values\": [9, 6, 2]") != std::string::npos);
    CHECK(doc.find("\"deltas\": [-3, -4]") != std::string::npos);
    CHECK(doc.find("\"direction\": \"falling\"") != std::string::npos);
}

// Issue #35. The L3110 splits 0x2F between two counters, one per nibble, and
// the database build used to treat that shared byte as "the same counter" and
// keep only the second. These are the reporter's own bytes.
void test_l3110_shows_both_counters_sharing_a_byte()
{
    std::cout << "[TEST] test_l3110_shows_both_counters_sharing_a_byte" << std::endl;

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));

    const ewr::DbPrinterModel* l3110 = nullptr;
    const std::vector<ewr::DbPrinterModel> models = gen.GetAvailableModels();
    for (const auto& m : models)
    {
        if (m.name == "L3110")
            l3110 = &m;
    }
    CHECK(l3110 != nullptr);
    if (!l3110)
        return;

    const std::vector<std::pair<uint16_t, int>> dump = {
        { 0x1C, 0x00 }, { 0x2F, 0x00 }, { 0x30, 0x4D }, { 0x31, 0x02 },
        { 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x82 }, { 0x35, 0x04 },
        { 0x36, 0x5E }, { 0x37, 0x5E },
    };

    const auto specs = l3110->GetAllCounters();
    CHECK(specs.size() == 2);

    bool sawLoaded = false, sawEmpty = false;
    for (const auto& spec : specs)
    {
        const ewr::CounterReading r = ewr::EvaluateCounter(spec, dump);
        CHECK(r.complete);
        if (r.max_value == 6346 && r.value == 589)
            sawLoaded = true;      // 0x30/0x31 + low nibble of 0x2F: 77 + 2*256
        if (r.max_value == 3416 && r.value == 0)
            sawEmpty = true;       // 0x32/0x33 + high nibble of 0x2F
    }
    CHECK(sawLoaded);
    CHECK(sawEmpty);

    // What the reporter actually asked about: two gauges, named apart, main
    // first. The loaded one is the main pad and the empty one the platen pad.
    if (specs.size() == 2)
    {
        CHECK(specs[0].description == "Main Pad Counter");
        CHECK(specs[0].max_value == 6346);
        CHECK(specs[1].description == "Platen Pad Counter");
        CHECK(specs[1].max_value == 3416);
    }
}

// WicReset never names its counters; EWR names them by position. EP-708A,
// EP-808A and XP-630 are the only models where an upstream source labels the
// pads outright, so they are the check that position really means pad.
void test_counter_names_follow_wicreset_pad_order()
{
    std::cout << "[TEST] test_counter_names_follow_wicreset_pad_order" << std::endl;

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));

    const std::vector<ewr::DbPrinterModel> models = gen.GetAvailableModels();
    for (const std::string name : { "EP-708A", "EP-808A", "XP-630" })
    {
        const ewr::DbPrinterModel* model = nullptr;
        for (const auto& m : models)
        {
            if (m.name == name)
                model = &m;
        }
        CHECK(model != nullptr);
        if (!model)
            continue;

        const auto specs = model->GetAllCounters();
        CHECK(specs.size() == 2);
        if (specs.size() != 2)
            continue;

        // reinkpy: 0x10/0x11 is the main pad, 0x12/0x13 the platen pad.
        CHECK(specs[0].description == "Main Pad Counter");
        CHECK(!specs[0].bytes.empty() && specs[0].bytes[0].address == 0x10);
        CHECK(specs[1].description == "Platen Pad Counter");
        CHECK(!specs[1].bytes.empty() && specs[1].bytes[0].address == 0x12);
    }
}

// 489 models used to draw the same label on two to five gauges.
void test_no_model_repeats_a_gauge_label()
{
    std::cout << "[TEST] test_no_model_repeats_a_gauge_label" << std::endl;

    ewr::UniversalGenerator gen;
    CHECK(gen.LoadDatabase("database.json"));

    size_t repeating = 0;
    std::string firstOffender;
    for (const auto& model : gen.GetAvailableModels())
    {
        std::vector<std::string> seen;
        bool repeats = false;
        for (const auto& spec : model.GetAllCounters())
        {
            repeats = repeats || std::find(seen.begin(), seen.end(), spec.description) != seen.end();
            seen.push_back(spec.description);
        }
        if (repeats)
        {
            if (repeating++ == 0)
                firstOffender = model.name;
        }
    }

    if (repeating)
        std::cout << "  first model repeating a label: " << firstOffender << std::endl;
    CHECK(repeating == 0);
}

// A person sees what happens to the printer, not the fallback ladder: an
// interface that failed on the way to one that worked used to print "Handshake
// FAILED" on a reset that succeeded. The rules live in the CLI; --json and
// the C API still get every event.
void test_console_hides_the_machinery()
{
    std::cout << "[TEST] test_console_hides_the_machinery" << std::endl;

    auto event = [](ewr::log::Level level, const char* code, const char* message, int index = -1, int total = -1)
    {
        ewr::log::Event e;
        e.level = level;
        e.code = code;
        e.message = message;
        e.index = index;
        e.total = total;
        return e;
    };

    using L = ewr::log::Level;

    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, false);

        sink(event(L::Info, "usb.sequence_begin", "Executing universal libusb hardware state machine..."));
        sink(event(L::Info, "exec.handshake_failed", "-> Handshake FAILED: no reply"));
        sink(event(L::Info, "usb.interface_fallback", "[!] Interface 1/3 stayed silent to D4, END4 and ESC/P Remote."));
        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 2 | EEPROM write verified (||:42:OK;).", 1, 2));
        sink(event(L::Info, "session.verified", "[SUCCESS] Read-back verification: every counter now holds its reset value."));
        sink(event(L::Trace, "exec.retry", "[RETRY] Command 1"));
        sink(event(L::Warning, "db.entry_skipped", "[!] skipped an entry"));

        const std::string text = out.str();
        CHECK(text.find("FAILED") == std::string::npos);
        CHECK(text.find("state machine") == std::string::npos);
        CHECK(text.find("stayed silent") == std::string::npos);
        CHECK(text.find("trying the next one") != std::string::npos);
        CHECK(text.find("[SUCCESS] Read-back verification") != std::string::npos);
        CHECK(text.find("RETRY") == std::string::npos);

        // Piped: no redraws to pile up in a log.
        CHECK(text.find("Writing to the printer") == std::string::npos);

        // Warnings keep going to stderr, where a redirected stdout cannot lose them.
        CHECK(err.str().find("skipped an entry") != std::string::npos);
        CHECK(text.find("skipped an entry") == std::string::npos);
    }

    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, true);

        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 2", 1, 2));
        sink(event(L::Info, "d4.session_recovering", "[!] The printer stopped answering - waiting for it to come back..."));
        sink(event(L::Info, "exec.write_verified", "-> Command 2 / 2", 2, 2));
        sink(event(L::Info, "session.verifying", "[*] Verifying"));

        const std::string text = out.str();
        CHECK(text.find("-> Command") == std::string::npos);

        // A message in the middle of the counter starts on its own line, and
        // the finished counter ends its own.
        CHECK(text.find("1/2\n[!] The printer stopped answering") != std::string::npos);
        CHECK(text.find("\r[*] Writing to the printer... 2/2\n[*] Verifying") != std::string::npos);
    }

    // The commit is a second device session of one write. Its counter would
    // read as a second reset and its failure is not the reset's, so both stay
    // quiet until the commit says how it went.
    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, true);

        sink(event(L::Info, "session.commit", "[*] Commit step: latching the new counter values..."));
        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 1", 1, 1));
        sink(event(L::Error, "usb.reset_not_confirmed", "[ERROR] no ack\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Error, "snmp.reset_not_confirmed", "[ERROR] no network ack\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Info, "session.commit_failed", "[!] Commit step did not complete (no ack)."));
        sink(event(L::Error, "usb.reset_not_confirmed", "[ERROR] the reset itself\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Error, "snmp.reset_not_confirmed", "[ERROR] the network reset itself\n[!] The waste counter was NOT confirmed as reset."));

        CHECK(out.str().find("Commit step: latching") == std::string::npos);
        CHECK(out.str().find("Writing to the printer") == std::string::npos);
        CHECK(out.str().find("Commit step did not complete") != std::string::npos);
        CHECK(err.str().find("no ack") == std::string::npos);
        CHECK(err.str().find("no network ack") == std::string::npos);
        CHECK(err.str().find("the reset itself") != std::string::npos);
        CHECK(err.str().find("the network reset itself") != std::string::npos);
    }

    // Everything the rules touch is Info except usb.claim_failed, which
    // usb.claim_all_failed repeats when no interface could be claimed. Hiding
    // any other warning or error would lose a real problem.
    const char* const hiddenOnPurpose[] = { "usb.claim_failed" };
    const char* const errorCodes[] = {
        "usb.reset_not_confirmed", "snmp.reset_not_confirmed", "usb.claim_all_failed", "usb.another_run",
        "snmp.another_run", "session.device_not_found",
        "session.preflight_required", "session.db_conflict", "db.parse_error", "usb.busy_status_monitor",
        "usb.access_denied", "usb.open_failed", "usb.soft_reset_settle_timeout",
    };
    for (const char* code : errorCodes)
        CHECK(ewr::cli::ConsoleRuleFor(code).action == ewr::cli::ConsoleAction::Show);
    for (const char* code : hiddenOnPurpose)
        CHECK(ewr::cli::ConsoleRuleFor(code).action == ewr::cli::ConsoleAction::Hide);
}

// "ERROR | ERROR: INK OUT (0x05)" said the state twice.
void test_printer_condition_names_an_error_once()
{
    std::cout << "[TEST] test_printer_condition_names_an_error_once" << std::endl;

    ewr::PrinterStatus st;
    st.valid = true;
    st.stateName = "ERROR";
    st.hasError = true;
    st.errorCode = 0x05;
    st.errorName = "INK OUT";
    CHECK(ewr::DescribePrinterCondition(st) == "ERROR: INK OUT (0x05)");

    // A busy printer that also reports an error keeps both.
    st.stateName = "BUSY";
    CHECK(ewr::DescribePrinterCondition(st) == "BUSY | ERROR: INK OUT (0x05)");

    st.hasError = false;
    st.stateName = "IDLE";
    CHECK(ewr::DescribePrinterCondition(st) == "IDLE");
}

// A run that wrote past an objection has to say so in the result a caller
// reads, not only in events it may not keep. One shape for every gate.
void test_json_reset_data_lists_overrides()
{
    std::cout << "[TEST] test_json_reset_data_lists_overrides" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();

    ewr::ResetOutcome clean;
    clean.phase = ewr::ResetPhase::Done;
    const nlohmann::json none = ewr::JsonResetData(model, false, clean);
    CHECK(none.contains("overrides") && none["overrides"].is_array() && none["overrides"].empty());

    ewr::ResetOutcome forced;
    forced.phase = ewr::ResetPhase::Done;
    forced.overrides.push_back({ "model_mismatch", "R220", -1 });
    forced.overrides.push_back({ "blocker", "INK OUT", 5 });
    forced.overrides.push_back({ "blocker", "PRINTER BUSY", -1 });
    forced.overrides.push_back({ "db_conflict", "", -1 });

    const nlohmann::json out = ewr::JsonResetData(model, false, forced);
    const nlohmann::json& list = out["overrides"];
    CHECK(list.size() == 4);
    if (list.size() != 4)
        return;

    for (const auto& entry : list)
    {
        CHECK(entry.size() == 4);
        CHECK(entry.contains("gate") && entry.contains("detected_model")
              && entry.contains("error") && entry.contains("error_code"));
    }

    CHECK(list[0]["gate"] == "model_mismatch");
    CHECK(list[0]["detected_model"] == "R220");
    CHECK(list[0]["error"].is_null() && list[0]["error_code"].is_null());

    CHECK(list[1]["gate"] == "blocker");
    CHECK(list[1]["error"] == "INK OUT");
    CHECK(list[1]["error_code"] == 5);
    CHECK(list[1]["detected_model"].is_null());

    // Not a printer error: no code, and null rather than -1.
    CHECK(list[2]["error"] == "PRINTER BUSY");
    CHECK(list[2]["error_code"].is_null());

    CHECK(list[3]["gate"] == "db_conflict");
    CHECK(list[3]["detected_model"].is_null() && list[3]["error"].is_null() && list[3]["error_code"].is_null());
}

// Numbers a host may act on travel in `fields`, never only in `message`.
void test_log_carries_fields()
{
    std::cout << "[TEST] test_log_carries_fields" << std::endl;

    ewr::log::Reporter reporter;
    std::map<std::string, std::string> seen;
    reporter.AddSink([&](const ewr::log::Event& e) { seen = e.fields; });

    reporter.Log(ewr::log::Level::Info, ewr::log::Stage::Detect, "usb.soft_reset_settled", "settled",
                 { { "elapsed_ms", "41250" } });
    CHECK(seen.size() == 1 && seen["elapsed_ms"] == "41250");

    reporter.Log(ewr::log::Level::Info, ewr::log::Stage::Detect, "usb.trace_log", "no fields");
    CHECK(seen.empty());
}

// "null means not reported" is a promise, so a struct's -1 never reaches a
// caller as a number it might compare against.
void test_json_never_leaks_minus_one()
{
    std::cout << "[TEST] test_json_never_leaks_minus_one" << std::endl;

    ewr::Blocker ink;
    ink.errorName = "INK OUT";
    ink.errorCode = 5;
    ink.explanation = "Clear it first.";
    const nlohmann::json inkJson = ewr::JsonBlocker(ink);
    CHECK(inkJson["error"] == "INK OUT");
    CHECK(inkJson["error_code"] == 5);
    CHECK(inkJson["explanation"] == "Clear it first.");

    ewr::Blocker busy;
    busy.errorName = "PRINTER BUSY";
    const nlohmann::json busyJson = ewr::JsonBlocker(busy);
    CHECK(busyJson.contains("error_code") && busyJson["error_code"].is_null());

    ewr::InterfaceInfo usbprint;
    usbprint.index = 1;
    usbprint.className = "USBPRINT";
    usbprint.path = "\\\\?\\usb#vid_04b8";
    const nlohmann::json plain = ewr::JsonInterface(usbprint, "");
    CHECK(plain.contains("interface_number") && plain["interface_number"].is_null());
    CHECK(plain["device_id"].is_null());
    CHECK(plain["model_match"].is_null());
    CHECK(plain["index"] == 1 && plain["class"] == "USBPRINT");

    ewr::InterfaceInfo vendor = usbprint;
    vendor.index = 2;
    vendor.interfaceNumber = 2;
    vendor.deviceId = "MFG:EPSON;MDL:ET-2800 Series;";
    const nlohmann::json composite = ewr::JsonInterface(vendor, "ET-2800");
    CHECK(composite["interface_number"] == 2);
    CHECK(composite["device_id"] == "MFG:EPSON;MDL:ET-2800 Series;");
    CHECK(composite["model_match"] == "ET-2800");

    // Valid as soon as any field decodes, so the state may be missing.
    ewr::PrinterStatus inksOnly;
    inksOnly.valid = true;
    const nlohmann::json st = ewr::JsonPrinterStatus(inksOnly);
    CHECK(st["state"].is_null());
    CHECK(st["state_code"].is_null());
    CHECK(st["error_code"].is_null());
}

// ---- SNMP (the --ip network transport) ----------------------------------

namespace snmp_test {

    void AppendTlv(std::vector<unsigned char>& out, unsigned char tag, const std::vector<unsigned char>& content)
    {
        out.push_back(tag);
        if (content.size() < 0x80)
        {
            out.push_back(static_cast<unsigned char>(content.size()));
        }
        else
        {
            out.push_back(0x82);
            out.push_back(static_cast<unsigned char>(content.size() >> 8));
            out.push_back(static_cast<unsigned char>(content.size() & 0xFF));
        }
        out.insert(out.end(), content.begin(), content.end());
    }

    // Shortest BER form of a non-negative INTEGER.
    std::vector<unsigned char> IntegerContent(int value)
    {
        std::vector<unsigned char> bytes;
        do
        {
            bytes.insert(bytes.begin(), static_cast<unsigned char>(value & 0xFF));
            value >>= 8;
        } while (value > 0);

        if (bytes[0] & 0x80)
            bytes.insert(bytes.begin(), 0x00);
        return bytes;
    }

    // A GetRequest read back the way an agent reads it: its ID and its one
    // OID. False for anything else, or for a community other than "public".
    bool ParseRequest(const std::vector<unsigned char>& datagram, int& id, ewr::snmp::Oid& oid)
    {
        std::size_t pos = 0;

        // One TLV of `tag` at pos: stepped into, or over with its content kept.
        auto tlv = [&](unsigned char tag, bool enter, std::vector<unsigned char>* content) -> bool
        {
            if (pos + 2 > datagram.size() || datagram[pos] != tag)
                return false;

            std::size_t length = datagram[pos + 1];
            pos += 2;
            if (length & 0x80)
            {
                const std::size_t count = length & 0x7F;
                length = 0;
                for (std::size_t i = 0; i < count && pos < datagram.size(); ++i)
                    length = (length << 8) | datagram[pos++];
            }

            if (length > datagram.size() - pos)
                return false;
            if (content)
                content->assign(datagram.begin() + static_cast<long>(pos),
                                datagram.begin() + static_cast<long>(pos + length));
            if (!enter)
                pos += length;
            return true;
        };

        std::vector<unsigned char> community, idBytes, oidBytes;
        if (!tlv(0x30, true, nullptr) || !tlv(0x02, false, nullptr) || !tlv(0x04, false, &community)
            || std::string(community.begin(), community.end()) != "public"
            || !tlv(0xA0, true, nullptr) || !tlv(0x02, false, &idBytes)
            || !tlv(0x02, false, nullptr) || !tlv(0x02, false, nullptr)
            || !tlv(0x30, true, nullptr) || !tlv(0x30, true, nullptr) || !tlv(0x06, false, &oidBytes)
            || idBytes.empty() || oidBytes.empty())
            return false;

        id = 0;
        for (unsigned char b : idBytes)
            id = (id << 8) | b;

        oid = { oidBytes[0] / 40u, oidBytes[0] % 40u };
        uint32_t arc = 0;
        for (std::size_t i = 1; i < oidBytes.size(); ++i)
        {
            arc = (arc << 7) | (oidBytes[i] & 0x7Fu);
            if ((oidBytes[i] & 0x80) == 0)
            {
                oid.push_back(arc);
                arc = 0;
            }
        }
        return true;
    }

    // A GetResponse as an agent would send it. The OID is not echoed
    // faithfully - the decoder only steps over it.
    std::vector<unsigned char> MakeResponse(int requestId, int errorStatus, unsigned char valueTag,
                                            const std::vector<unsigned char>& value)
    {
        std::vector<unsigned char> binding;
        AppendTlv(binding, 0x06, { 0x2B, 0x06, 0x01 });
        AppendTlv(binding, valueTag, value);

        std::vector<unsigned char> list;
        AppendTlv(list, 0x30, binding);

        std::vector<unsigned char> pdu;
        AppendTlv(pdu, 0x02, IntegerContent(requestId));
        AppendTlv(pdu, 0x02, IntegerContent(errorStatus));
        AppendTlv(pdu, 0x02, { 0x00 });
        AppendTlv(pdu, 0x30, list);

        std::vector<unsigned char> message;
        AppendTlv(message, 0x02, { 0x00 });
        AppendTlv(message, 0x04, { 'p', 'u', 'b', 'l', 'i', 'c' });
        AppendTlv(message, 0xA2, pdu);

        std::vector<unsigned char> datagram;
        AppendTlv(datagram, 0x30, message);
        return datagram;
    }

    // A printer that serves a fixed set of OIDs. `dropFirst` loses that many
    // requests before it starts answering; `silent` never answers.
    struct ScriptedPrinter final : ewr::snmp::IDatagramChannel
    {
        std::map<std::string, std::vector<unsigned char>> served;
        std::vector<std::vector<unsigned char>> pending;
        std::vector<std::string> requested; // OIDs, one per datagram sent
        int dropFirst = 0;
        bool silent = false;
        bool refuseSend = false; // the local stack will not send at all

        bool Send(const std::vector<unsigned char>& datagram) override
        {
            if (refuseSend)
                return false;

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid) || std::find(known.begin(), known.end(), oid) == known.end())
            {
                requested.push_back("?");
                return true;
            }

            const std::string name = ewr::snmp::FormatOid(oid);
            requested.push_back(name);

            if (silent)
                return true;
            if (dropFirst > 0)
            {
                --dropFirst;
                return true;
            }

            const auto hit = served.find(name);
            if (hit == served.end())
                pending.push_back(MakeResponse(id, 2, 0x05, {}));
            else
                pending.push_back(MakeResponse(id, 0, 0x04, hit->second));
            return true;
        }

        std::vector<unsigned char> Receive(int) override
        {
            if (pending.empty())
                return {};

            std::vector<unsigned char> next = pending.front();
            pending.erase(pending.begin());
            return next;
        }

        void Serve(const ewr::snmp::Oid& oid, const std::vector<unsigned char>& value)
        {
            known.push_back(oid);
            served[ewr::snmp::FormatOid(oid)] = value;
        }

        // Recognised, but answered with noSuchName.
        void Know(const ewr::snmp::Oid& oid) { known.push_back(oid); }

        std::vector<ewr::snmp::Oid> known;
    };

    std::vector<unsigned char> Bytes(const std::string& text)
    {
        return std::vector<unsigned char>(text.begin(), text.end());
    }

    // The EPSON-CTRL command inside a generated query packet.
    std::vector<unsigned char> CommandOf(const std::vector<unsigned char>& packet)
    {
        std::vector<unsigned char> command;
        ewr::ExtractDataPayload(packet, command);
        return command;
    }

    ewr::DbPrinterModel TwoByteModel()
    {
        ewr::DbPrinterModel model;
        model.name = "NetTest";
        model.rkey = 0x0797; // 151, 7
        model.wkey = "Maribaya";

        ewr::PadGroup group;
        group.addresses = { 0x30, 0x1FE };
        group.reset_values = { 0x00, 0x00 };
        model.pad_groups.push_back(group);
        return model;
    }

} // namespace snmp_test

void test_snmp_get_request_is_byte_exact()
{
    std::cout << "[TEST] test_snmp_get_request_is_byte_exact" << std::endl;

    // sysDescr.0, community "public", request ID 1.
    const std::vector<unsigned char> expected = {
        0x30, 0x26, 0x02, 0x01, 0x00, 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',
        0xA0, 0x19, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
        0x30, 0x0E, 0x30, 0x0C, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
        0x05, 0x00
    };
    CHECK(ewr::snmp::EncodeGetRequest("public", 1, { 1, 3, 6, 1, 2, 1, 1, 1, 0 }) == expected);

    // Arcs above 127 take two base-128 bytes: 1248 -> 89 60, 190 -> 81 3E.
    // A command byte such as 0xBE encoded as one byte would be read by the
    // printer as the start of a longer arc and shift everything after it.
    const std::vector<unsigned char> wide = ewr::snmp::EncodeGetRequest("public", 1, { 1, 3, 1248, 190, 127, 128 });
    const std::vector<unsigned char> oidTlv = { 0x06, 0x08, 0x2B, 0x89, 0x60, 0x81, 0x3E, 0x7F, 0x81, 0x00 };
    CHECK(std::search(wide.begin(), wide.end(), oidTlv.begin(), oidTlv.end()) != wide.end());

    // A request ID with the high bit of its low byte set needs a leading 00,
    // or it reads back negative.
    const std::vector<unsigned char> id200 = ewr::snmp::EncodeGetRequest("public", 200, { 1, 3 });
    const std::vector<unsigned char> idTlv = { 0xA0, 0x13, 0x02, 0x02, 0x00, 0xC8, 0x02, 0x01, 0x00 };
    CHECK(std::search(id200.begin(), id200.end(), idTlv.begin(), idTlv.end()) != id200.end());

    CHECK(ewr::snmp::EncodeGetRequest("public", 1, { 1 }).empty());
    CHECK(ewr::snmp::FormatOid({ 1, 3, 6, 1248 }) == "1.3.6.1248");

    // The scripted printers below read requests with their own decoder and
    // answer with their own encoder; both have to agree past one-byte IDs.
    int id = 0;
    ewr::snmp::Oid oid;
    const ewr::snmp::Oid control = ewr::SnmpControlOid({ 0x7C, 0x7C, 0xBE });
    CHECK(snmp_test::ParseRequest(ewr::snmp::EncodeGetRequest("public", 70000, control), id, oid));
    CHECK(id == 70000 && oid == control);
    CHECK(!snmp_test::ParseRequest(ewr::snmp::EncodeGetRequest("private", 1, control), id, oid));

    ewr::snmp::GetResponse response;
    CHECK(ewr::snmp::DecodeGetResponse(snmp_test::MakeResponse(300, 0, 0x04, {}), response));
    CHECK(response.requestId == 300);
}

void test_snmp_response_decoding_is_bounded()
{
    std::cout << "[TEST] test_snmp_response_decoding_is_bounded" << std::endl;

    const std::vector<unsigned char> value = snmp_test::Bytes("@BDC PS\r\nEE:003012;\f");
    const std::vector<unsigned char> datagram = snmp_test::MakeResponse(7, 0, 0x04, value);

    ewr::snmp::GetResponse response;
    CHECK(ewr::snmp::DecodeGetResponse(datagram, response));
    CHECK(response.requestId == 7);
    CHECK(response.errorStatus == 0);
    CHECK(response.valueTag == ewr::snmp::kTagOctetString);
    CHECK(response.value == value);

    // A value past 127 bytes switches every enclosing length to long form.
    const std::vector<unsigned char> big(300, 0x5A);
    CHECK(ewr::snmp::DecodeGetResponse(snmp_test::MakeResponse(9, 0, 0x04, big), response));
    CHECK(response.value == big);

    CHECK(ewr::snmp::DecodeGetResponse(snmp_test::MakeResponse(3, 2, 0x05, {}), response));
    CHECK(response.errorStatus == 2);
    CHECK(response.value.empty());

    // Cut anywhere, a datagram is refused rather than read past its end.
    for (size_t cut = 0; cut < datagram.size(); ++cut)
    {
        const std::vector<unsigned char> part(datagram.begin(), datagram.begin() + static_cast<long>(cut));
        CHECK(!ewr::snmp::DecodeGetResponse(part, response));
    }

    // A request is not a response.
    CHECK(!ewr::snmp::DecodeGetResponse(ewr::snmp::EncodeGetRequest("public", 1, { 1, 3, 6 }), response));
}

void test_snmp_control_oid_carries_the_read_command()
{
    std::cout << "[TEST] test_snmp_control_oid_carries_the_read_command" << std::endl;

    // The form every SNMP tool for these printers sends:
    // <ctrl>.124.124.7.0.<rkey lo>.<rkey hi>.65.190.160.<addr lo>.<addr hi>
    const std::vector<unsigned char> command =
        snmp_test::CommandOf(ewr::UniversalGenerator::GenerateReadPacket(0x0797, 0x01FE));
    const ewr::snmp::Oid expected = { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 44, 1, 1, 2, 1,
                                      124, 124, 7, 0, 151, 7, 65, 190, 160, 254, 1 };
    CHECK(ewr::SnmpControlOid(command) == expected);
}

void test_snmp_gateway_reads_state_through_the_session()
{
    std::cout << "[TEST] test_snmp_gateway_reads_state_through_the_session" << std::endl;

    const ewr::DbPrinterModel model = snmp_test::TwoByteModel();

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();

    script->Serve(ewr::SnmpDeviceIdOid(), snmp_test::Bytes("MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:L3150 Series;CLS:PRINTER;"));
    script->Serve(ewr::SnmpStatusOid(), ewr::ExtractD4Payload(MakeSt2Reply()));
    script->Serve(ewr::SnmpControlOid(snmp_test::CommandOf(
                      ewr::UniversalGenerator::GenerateReadPacket(model.rkey, 0x30))),
                  snmp_test::Bytes("@BDC PS\r\nEE:00302A;\f"));
    // 0x1FE is known to the printer but refused, as an unserved OID is.
    script->Know(ewr::SnmpControlOid(snmp_test::CommandOf(
        ewr::UniversalGenerator::GenerateReadPacket(model.rkey, 0x1FE))));

    std::ostringstream trace;
    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer), &trace);

    const ewr::DeviceIdQueryResult id = gateway.QueryDeviceId();
    CHECK(id.found);
    CHECK(ewr::ParseIeee1284DeviceId(id.deviceId).model == "L3150 Series");
    CHECK(gateway.Answered());

    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);
    const ewr::StateSnapshot state = session.ReadState();

    CHECK(state.available);
    CHECK(state.status.valid);
    CHECK(state.status.errorName == "INK OUT");
    CHECK(state.status.serial == "X7A9000123");
    CHECK(state.values.size() == 2);
    if (state.values.size() == 2)
    {
        CHECK(state.values[0].first == 0x30 && state.values[0].second == 0x2A);
        // Answered, but with no value: unread, not zero.
        CHECK(state.values[1].first == 0x1FE && state.values[1].second == -1);
    }

    // One datagram per question: nothing was retried, nothing else was asked.
    CHECK(script->requested.size() == 4);
    CHECK(trace.str().find("noSuchName") != std::string::npos);
}

void test_snmp_gateway_retries_a_lost_datagram()
{
    std::cout << "[TEST] test_snmp_gateway_retries_a_lost_datagram" << std::endl;

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Serve(ewr::SnmpStatusOid(), ewr::ExtractD4Payload(MakeSt2Reply()));
    script->dropFirst = 2;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    const ewr::StateSnapshot state = ewr::ReadPrinterStatus(gateway);

    CHECK(state.available);
    CHECK(state.status.valid);
    CHECK(script->requested.size() == 3);
}

void test_snmp_gateway_reports_silence_without_asking_on()
{
    std::cout << "[TEST] test_snmp_gateway_reports_silence_without_asking_on" << std::endl;

    const ewr::DbPrinterModel model = snmp_test::TwoByteModel();

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Know(ewr::SnmpStatusOid());
    script->silent = true;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));

    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);
    const ewr::StateSnapshot state = session.ReadState();

    CHECK(!state.available);
    CHECK(!gateway.Answered());
    // The status request and its repeats; the counter reads behind it were
    // never sent into the same silence.
    CHECK(script->requested.size() == 3);

    const ewr::QueryRunResult run = gateway.RunQuery({}, { ewr::UniversalGenerator::GenerateStatusQueryPacket() },
                                                     ewr::DefaultQueryOptions());
    CHECK(!run.deviceFound);
    CHECK(run.query.handshakeFailed);
    CHECK(run.query.replies.size() == 1 && run.query.replies[0].empty());
    CHECK(run.query.error.find("192.0.2.1") != std::string::npos);
}

void test_snmp_gateway_tells_a_blocked_send_from_a_silent_printer()
{
    std::cout << "[TEST] test_snmp_gateway_tells_a_blocked_send_from_a_silent_printer" << std::endl;

    auto blocked = std::make_unique<snmp_test::ScriptedPrinter>();
    blocked->refuseSend = true;
    ewr::SnmpDeviceGateway blockedGateway("192.0.2.1", std::move(blocked));
    CHECK(!blockedGateway.QueryDeviceId().found);
    CHECK(blockedGateway.SendBlocked());

    auto quiet = std::make_unique<snmp_test::ScriptedPrinter>();
    quiet->Know(ewr::SnmpDeviceIdOid());
    quiet->silent = true;
    ewr::SnmpDeviceGateway quietGateway("192.0.2.1", std::move(quiet));
    CHECK(!quietGateway.QueryDeviceId().found);
    CHECK(!quietGateway.SendBlocked());
}

void test_snmp_query_path_never_sends_a_write()
{
    std::cout << "[TEST] test_snmp_query_path_never_sends_a_write" << std::endl;

    const ewr::DbPrinterModel model = snmp_test::TwoByteModel();
    const std::vector<unsigned char> write =
        ewr::UniversalGenerator::GenerateWritePacket(model.rkey, 0x30, 0x00, model.wkey);

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Serve(ewr::SnmpControlOid(snmp_test::CommandOf(write)), snmp_test::Bytes("@BDC PS\r\n||:42:OK;\f"));

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    CHECK(gateway.OverNetwork());

    // Handed to the query path, a write is dropped, not forwarded.
    const ewr::QueryRunResult query = gateway.RunQuery({}, { write }, ewr::DefaultQueryOptions());
    CHECK(script->requested.empty());
    CHECK(query.query.replies.size() == 1 && query.query.replies[0].empty());

    // Nor does a read-back, which shares that path, ever turn into one.
    CHECK(!gateway.Answered());
}

namespace snmp_test {

    // A printer with an EEPROM: reads answer from it, writes with
    // `acceptedKey` change it, writes with any other keyword come back
    // ':42:NG;', and `refuse` makes every write come back ':42:NA;'.
    struct EepromPrinter final : ewr::snmp::IDatagramChannel
    {
        ewr::DbPrinterModel model;
        std::map<uint16_t, uint8_t> eeprom;
        std::string acceptedKey;
        bool refuse = false;
        int writesSeen = 0;
        std::vector<std::vector<unsigned char>> pending;

        bool Send(const std::vector<unsigned char>& datagram) override
        {
            static const char* hex = "0123456789ABCDEF";

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid))
                return true;

            if (oid == ewr::SnmpStatusOid())
            {
                pending.push_back(MakeResponse(id, 0, 0x04,
                    Bytes("@BDC ST2\r\n\x03" + std::string(1, '\0') + "\x01\x01\x04")));
                return true;
            }

            for (auto& cell : eeprom)
            {
                if (oid == ewr::SnmpControlOid(CommandOf(
                               ewr::UniversalGenerator::GenerateReadPacket(model.rkey, cell.first))))
                {
                    std::string read = "@BDC PS\r\nEE:";
                    read += hex[(cell.first >> 12) & 0xF]; read += hex[(cell.first >> 8) & 0xF];
                    read += hex[(cell.first >> 4) & 0xF];  read += hex[cell.first & 0xF];
                    read += hex[(cell.second >> 4) & 0xF]; read += hex[cell.second & 0xF];
                    read += ";\f";
                    pending.push_back(MakeResponse(id, 0, 0x04, Bytes(read)));
                    return true;
                }

                for (const std::string& key : { model.wkey, model.wkey1 })
                {
                    if (key.empty())
                        continue;

                    for (int value : { 0x00, 0x5E })
                    {
                        if (oid != ewr::SnmpControlOid(CommandOf(ewr::UniversalGenerator::GenerateWritePacket(
                                       model.rkey, cell.first, static_cast<uint8_t>(value), key))))
                            continue;

                        ++writesSeen;
                        std::string verdict = "OK";
                        if (refuse)
                            verdict = "NA";
                        else if (key != acceptedKey)
                            verdict = "NG";
                        else
                            cell.second = static_cast<uint8_t>(value);

                        pending.push_back(MakeResponse(id, 0, 0x04, Bytes("@BDC PS\r\n||:42:" + verdict + ";\f")));
                        return true;
                    }
                }
            }

            return true;
        }

        std::vector<unsigned char> Receive(int) override
        {
            if (pending.empty())
                return {};

            std::vector<unsigned char> next = pending.front();
            pending.erase(pending.begin());
            return next;
        }
    };

    std::unique_ptr<EepromPrinter> FullPads()
    {
        auto printer = std::make_unique<EepromPrinter>();
        printer->model = TwoByteModel();
        printer->model.wkey1 = "Nbsjcbzb";
        printer->acceptedKey = printer->model.wkey;
        printer->eeprom[0x30] = 0xCA;
        printer->eeprom[0x1FE] = 0x18;
        return printer;
    }

} // namespace snmp_test

void test_snmp_reset_writes_and_verifies_through_the_session()
{
    std::cout << "[TEST] test_snmp_reset_writes_and_verifies_through_the_session" << std::endl;

    auto printer = snmp_test::FullPads();
    snmp_test::EepromPrinter* script = printer.get();
    const ewr::DbPrinterModel model = script->model;

    std::ostringstream trace;
    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer), &trace);

    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);

    bool asked = false;
    ewr::ResetHandlers handlers;
    handlers.confirmWrite = [&](const ewr::StateSnapshot& before)
    {
        // Nothing has been written by the time the question is put.
        asked = true;
        CHECK(script->writesSeen == 0);
        CHECK(before.values.size() == 2 && before.values[0].second == 0xCA);
        return true;
    };

    const ewr::ResetOutcome outcome = session.Reset(handlers);

    CHECK(asked);
    CHECK(outcome.phase == ewr::ResetPhase::Done);
    CHECK(outcome.success);
    CHECK(outcome.writesTotal == 2 && outcome.writesVerified == 2);
    CHECK(outcome.verificationRan);
    CHECK(outcome.verifyMismatches == 0 && outcome.verifyUnread == 0);
    CHECK(script->eeprom[0x30] == 0x00 && script->eeprom[0x1FE] == 0x00);
    CHECK(script->writesSeen == 2);
    CHECK(!outcome.alternateKeyUsed);
    CHECK(trace.str().find("BEGIN WRITE SESSION") != std::string::npos);

    // Declined at the confirmation, nothing travels.
    auto second = snmp_test::FullPads();
    snmp_test::EepromPrinter* untouched = second.get();
    ewr::SnmpDeviceGateway declinedGateway("192.0.2.1", std::move(second));
    ewr::Session declinedSession(model, declinedGateway, quiet);
    ewr::ResetHandlers decline;
    decline.confirmWrite = [](const ewr::StateSnapshot&) { return false; };
    const ewr::ResetOutcome declined = declinedSession.Reset(decline);
    CHECK(declined.phase == ewr::ResetPhase::Aborted);
    CHECK(untouched->writesSeen == 0);
    CHECK(untouched->eeprom[0x30] == 0xCA);
}

void test_snmp_reset_falls_back_to_the_alternate_key()
{
    std::cout << "[TEST] test_snmp_reset_falls_back_to_the_alternate_key" << std::endl;

    auto printer = snmp_test::FullPads();
    snmp_test::EepromPrinter* script = printer.get();
    script->acceptedKey = script->model.wkey1;
    const ewr::DbPrinterModel model = script->model;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);
    const ewr::ResetOutcome outcome = session.Reset();

    CHECK(outcome.success);
    CHECK(outcome.alternateKeyUsed);
    CHECK(outcome.writesVerified == 2);
    CHECK(script->eeprom[0x30] == 0x00 && script->eeprom[0x1FE] == 0x00);
    // Each write: once rejected, once accepted.
    CHECK(script->writesSeen == 4);
}

void test_snmp_reset_stops_at_a_refused_write()
{
    std::cout << "[TEST] test_snmp_reset_stops_at_a_refused_write" << std::endl;

    auto printer = snmp_test::FullPads();
    snmp_test::EepromPrinter* script = printer.get();
    script->refuse = true;
    const ewr::DbPrinterModel model = script->model;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);

    // What the CLI prints: the per-write verdicts are hidden there, so
    // without the gateway's own event the run ended on a bare RESET FAILED.
    std::ostringstream out, err;
    const int consoleId = ewr::log::Default().AddSink(ewr::cli::ConsoleFor(out, err, false));
    const ewr::ResetOutcome outcome = session.Reset();
    ewr::log::Default().RemoveSink(consoleId);

    CHECK(!outcome.success);
    CHECK(outcome.phase == ewr::ResetPhase::WriteFailed);
    CHECK(outcome.error.find(":42:NA;") != std::string::npos);
    CHECK(err.str().find(":42:NA;") != std::string::npos);
    CHECK(err.str().find("NOT confirmed as reset") != std::string::npos);
    // The first refusal ends the run: the second write is never sent.
    CHECK(script->writesSeen == 1);
    CHECK(script->eeprom[0x30] == 0xCA && script->eeprom[0x1FE] == 0x18);

    // Nothing ever answered: session.device_not_found says so, not this.
    auto silent = std::make_unique<snmp_test::ScriptedPrinter>();
    silent->silent = true;
    ewr::SnmpDeviceGateway silentGateway("192.0.2.1", std::move(silent));

    int notConfirmed = 0;
    const int countId = ewr::log::Default().AddSink([&](const ewr::log::Event& e)
    {
        if (e.code == "snmp.reset_not_confirmed")
            ++notConfirmed;
    });
    ewr::UniversalGenerator generator;
    const ewr::ResetRunResult run = silentGateway.RunReset(generator.GenerateSequence(model), ewr::ExecutorOptions{});
    ewr::log::Default().RemoveSink(countId);

    CHECK(!run.deviceFound);
    CHECK(notConfirmed == 0);
}

void test_snmp_reset_refuses_a_replay_dump()
{
    std::cout << "[TEST] test_snmp_reset_refuses_a_replay_dump" << std::endl;

    auto printer = snmp_test::FullPads();
    snmp_test::EepromPrinter* script = printer.get();
    const ewr::DbPrinterModel model = script->model;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));

    ewr::UniversalGenerator generator;
    ewr::ExecutorOptions replay;
    replay.verifyWrites = false;
    const ewr::ResetRunResult run = gateway.RunReset(generator.GenerateSequence(model), replay);

    CHECK(!run.exec.success);
    CHECK(!run.exec.error.empty());
    CHECK(script->writesSeen == 0);
}

void test_snmp_gateway_waits_for_the_run_lock()
{
    std::cout << "[TEST] test_snmp_gateway_waits_for_the_run_lock" << std::endl;

    ewr::RunLock otherRun;
    {
        ewr::RunLock probe;
        if (probe.Held())
        {
            std::cout << "  (no run lock can be created here - skipped)" << std::endl;
            return;
        }
    }

    auto readTrace = []() -> std::string
    {
        std::ifstream in("ewr_trace.log", std::ios::binary);
        return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string("(absent)");
    };
    const std::string traceBefore = readTrace();

    std::vector<std::string> codes;
    const int sinkId = ewr::log::Default().AddSink([&](const ewr::log::Event& e) { codes.push_back(e.code); });

    ewr::SnmpDeviceGateway gateway("192.0.2.1");
    CHECK(!gateway.ClaimPrinter());
    CHECK(!gateway.QueryDeviceId().found);

    const ewr::QueryRunResult query = gateway.RunQuery({}, { ewr::UniversalGenerator::GenerateStatusQueryPacket() },
                                                       ewr::DefaultQueryOptions());
    CHECK(!query.query.success);
    CHECK(query.query.error.find("Another EWR run") != std::string::npos);

    ewr::UniversalGenerator generator;
    const ewr::ResetRunResult reset = gateway.RunReset(generator.GenerateSequence(snmp_test::TwoByteModel()),
                                                       ewr::ExecutorOptions{});
    CHECK(!reset.exec.success);
    CHECK(reset.exec.packetsSent == 0);
    CHECK(reset.exec.error.find("Another EWR run") != std::string::npos);

    ewr::log::Default().RemoveSink(sinkId);

    // Refused before anything was opened: the other run's trace is intact
    // and no socket was ever pointed at the printer.
    CHECK(readTrace() == traceBefore);
    CHECK(gateway.OpenError().empty());
    CHECK(!gateway.Answered() && !gateway.SendBlocked());
    CHECK(std::find(codes.begin(), codes.end(), "snmp.another_run") != codes.end());
}

void test_snmp_directed_broadcast()
{
    std::cout << "[TEST] test_snmp_directed_broadcast" << std::endl;

    const uint32_t host = 0xC0A80117; // 192.168.1.23
    CHECK(ewr::snmp::DirectedBroadcast(host, 24) == 0xC0A801FF);
    CHECK(ewr::snmp::DirectedBroadcast(host, 16) == 0xC0A8FFFF);
    CHECK(ewr::snmp::DirectedBroadcast(0x0A000005, 8) == 0x0AFFFFFF);

    // A point-to-point link or a lone host has no broadcast of its own; a
    // VPN adapter is often one.
    CHECK(ewr::snmp::DirectedBroadcast(host, 31) == 0);
    CHECK(ewr::snmp::DirectedBroadcast(host, 32) == 0);
    CHECK(ewr::snmp::DirectedBroadcast(host, 0) == 0);
    CHECK(ewr::snmp::DirectedBroadcast(host, 33) == 0);

    CHECK(ewr::snmp::FormatIpv4(0xC0A801FF) == "192.168.1.255");
    CHECK(ewr::snmp::FormatIpv4(0) == "0.0.0.0");

    // Whatever this machine's adapters are, the limited broadcast leads.
    const std::vector<std::string> local = ewr::snmp::LocalBroadcastAddresses();
    CHECK(!local.empty() && local[0] == "255.255.255.255");
}

namespace snmp_test {

    // A network of agents answering a broadcast: each answers every request
    // it hears, so two rounds to two addresses bring four answers apiece.
    struct ScriptedNetwork final : ewr::snmp::IBroadcastChannel
    {
        struct Agent
        {
            std::string address;
            std::string deviceId; // empty: answers noSuchName
            bool wrongId = false;
            bool malformed = false;
        };

        std::vector<Agent> agents;
        std::vector<std::string> sentTo;
        std::vector<std::pair<std::string, std::vector<unsigned char>>> pending;
        bool refuseSend = false; // macOS without the Local Network permission

        bool SendTo(const std::string& address, uint16_t port, const std::vector<unsigned char>& datagram) override
        {
            if (refuseSend)
                return false;

            sentTo.push_back(address + ":" + std::to_string(port));

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid) || oid != ewr::SnmpDeviceIdOid())
                return true;

            for (const Agent& agent : agents)
            {
                if (agent.malformed)
                    pending.push_back({ agent.address, { 0x30, 0x03, 0x02 } });
                else if (agent.deviceId.empty())
                    pending.push_back({ agent.address, MakeResponse(id, 2, 0x05, {}) });
                else
                    pending.push_back({ agent.address, MakeResponse(agent.wrongId ? id + 1 : id, 0, 0x04,
                                                                    Bytes(agent.deviceId)) });
            }
            return true;
        }

        bool ReceiveFrom(int timeoutMs, std::string& from, std::vector<unsigned char>& datagram) override
        {
            if (pending.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
                return false;
            }

            from = pending.front().first;
            datagram = pending.front().second;
            pending.erase(pending.begin());
            return true;
        }
    };

} // namespace snmp_test

void test_snmp_discovery_keeps_only_epson_answers()
{
    std::cout << "[TEST] test_snmp_discovery_keeps_only_epson_answers" << std::endl;

    snmp_test::ScriptedNetwork network;
    network.agents = {
        { "192.168.1.40", "MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:ET-2850 Series;CLS:PRINTER;" },
        { "192.168.1.9", "MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:L3150 Series;CLS:PRINTER;" },
        { "192.168.1.30", "" },                                  // not in Epson's tree
        { "192.168.1.31", "MFG:Hewlett-Packard;MDL:LaserJet;" }, // answers, not an Epson
        { "192.168.1.32", "", false, true },                     // garbage
        { "192.168.1.33", "MFG:EPSON;MDL:L805;", true },         // answers another question
    };

    std::string error;
    const std::vector<ewr::NetworkPrinter> found =
        ewr::DiscoverNetworkPrinters(network, { "255.255.255.255", "192.168.1.255" }, 300, error);
    CHECK(error.empty());

    // Two Epsons, once each, by address as a number: .9 before .40.
    CHECK(found.size() == 2);
    if (found.size() == 2)
    {
        CHECK(found[0].address == "192.168.1.9");
        CHECK(found[0].model == "L3150 Series");
        CHECK(found[1].address == "192.168.1.40");
        CHECK(found[1].model == "ET-2850 Series");
        CHECK(found[1].deviceId.find("MFG:EPSON") != std::string::npos);
    }

    // Two rounds, to the given addresses only, on the SNMP port.
    const std::vector<std::string> expected = {
        "255.255.255.255:161", "192.168.1.255:161", "255.255.255.255:161", "192.168.1.255:161",
    };
    CHECK(network.sentTo == expected);

    // Nothing answering is an empty list, not a failure.
    snmp_test::ScriptedNetwork quiet;
    CHECK(ewr::DiscoverNetworkPrinters(quiet, { "255.255.255.255" }, 30, error).empty());
    CHECK(error.empty());

    // A machine that refuses every send is not an empty network: it says so,
    // and does not sit out the wait for answers that cannot come.
    snmp_test::ScriptedNetwork blocked;
    blocked.refuseSend = true;
    blocked.agents = network.agents;
    const auto before = std::chrono::steady_clock::now();
    CHECK(ewr::DiscoverNetworkPrinters(blocked, { "255.255.255.255" }, 2000, error).empty());
    CHECK(error.find("refused to send") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(1000));
}

void test_run_lock_hands_over_to_the_network()
{
    std::cout << "[TEST] test_run_lock_hands_over_to_the_network" << std::endl;

    {
        ewr::UsbDeviceGateway usb;
        if (!usb.ClaimPrinter())
        {
            std::cout << "  [skip] another EWR run holds the printer" << std::endl;
            return;
        }

        {
            ewr::RunLock probe;
            if (probe.Held())
            {
                std::cout << "  (no run lock can be created here - skipped)" << std::endl;
                return;
            }
        }

        std::unique_ptr<ewr::RunLock> lock = usb.ReleaseRunLock();
        CHECK(lock && lock->Held());
        CHECK(usb.ReleaseRunLock() == nullptr);

        // Never free in between: not in the hand, not after it.
        {
            ewr::RunLock other;
            CHECK(!other.Held());
        }

        ewr::SnmpDeviceGateway net("192.0.2.1");
        net.AdoptRunLock(std::move(lock));
        CHECK(net.ClaimPrinter());
        {
            ewr::RunLock other;
            CHECK(!other.Held());
        }
    }

    // Both gateways gone: the next run gets it.
    ewr::RunLock afterwards;
    CHECK(afterwards.Held());
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
    test_database_carries_no_recovery_channel();
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
    test_status_reply_without_fields_is_invalid();
    test_status_reply_truncation_is_recorded();
    test_d4_payload_extraction();
    test_d4_payload_extraction_on_negotiated_socket();
    test_eeprom_read_reply_parsing();
    test_evaluate_blocker();
    test_evaluate_ink_blocker();
    test_session_reset_lifecycle();
    test_session_ink_reset();
    test_busy_status_blocker();
    test_truncated_status_blocker();
    test_snapshot_requires_a_reply();
    test_confirm_write_gate();
    test_ink_reset_requires_preflight();
    test_session_conflict_gate();
    test_interface_pin_option_threading();
    test_trace_context_description();
    test_session_names_its_model_in_the_trace_context();
    test_classify_eeprom_read_reply();
    test_collect_read_key_candidates();
    test_search_read_key();
    test_json_key_search_data();
    test_query_session_failfast_on_silence();
    test_query_session_happy_path();
    test_d4_framer_length_framing();
    test_d4_framer_resyncs_on_bogus_length();
    test_d4_session_start_negotiates_socket_and_mtu();
    test_d4_session_socketid_fallback();
    test_d4_session_credit_gating_and_chunking();
    test_d4_query_session_layer();
    test_d4_sequence_write_verified();
    test_d4_query_session_survives_a_lost_reply();
    test_d4_query_session_survives_a_lost_credit_reply();
    test_d4_query_session_stops_when_a_fresh_session_stays_silent();
    test_d4_session_discards_replies_held_from_an_earlier_session();
    test_d4_session_restart_waits_for_a_busy_control_service();
    test_d4_session_restart_gives_up_after_its_attempts();
    test_d4_sequence_survives_a_lost_write_ack();
    test_d4_sequence_na_fails_fast();
    test_d4_transport_failure_closes_channel();
    test_d4_reassembles_a_fragmented_reply();
    test_d4_short_reply_costs_no_extra_round_trip();
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
    test_end4_response_rejects_short_declared_length();
    test_end4_factory_command_extraction();
    test_end4_dds_parsing();
    test_esc_remote_sequence_verified();
    test_esc_remote_sequence_silent_fails();
    test_esc_remote_alternate_key();
    test_esc_remote_both_keys_rejected_claims_nothing();
    test_end4_sequence_reports_unframed_bytes();
    test_hex_dump_capping();
    test_executor_caps_inbound_trace_dumps();
    test_end4_sequence_verified();
    test_end4_sequence_silent_fails();
    test_end4_sequence_alternate_key();
    test_json_pads_carry_a_kind_not_just_a_label();
    test_every_counter_kind_matches_its_own_description();
    test_unnamed_counter_reports_no_kind();
    test_reset_coverage_over_the_database();
    test_reset_coverage_shapes();
    test_c_abi_plan_reports_reset_coverage();
    test_console_hides_the_machinery();
    test_printer_condition_names_an_error_once();
    test_json_reset_data_lists_overrides();
    test_log_carries_fields();
    test_json_never_leaks_minus_one();
    test_json_status_separates_unknown_from_zero();
    test_json_state_data_reports_detection_as_unknown();
    test_json_state_data_counts_pads_it_could_not_read();
    test_c_abi_constants_and_status_names();
    test_c_abi_database_calls_need_no_printer();
    test_c_abi_reports_a_missing_database();
    test_c_abi_network_session();
    test_json_contract_envelope_and_order();
    test_json_contract_reports_absent_progress_as_null();
    test_run_lock_admits_one_run_at_a_time();
    test_composite_merges_candidates_in_member_order();
    test_composite_identifies_a_shared_interface_by_number();
    test_composite_routes_every_call_to_the_owning_member();
    test_composite_runs_on_the_transports_that_came_up();
    test_http_personality_detection();
    test_end4_reports_the_http_personality();
    test_esc_remote_reports_the_http_personality();
    test_d4_session_reports_the_http_personality();
    test_discovery_needs_three_passes();
    test_discovery_keeps_only_what_moves_one_way_every_time();
    test_discovery_reads_a_wrapping_low_byte_as_a_pair();
    test_discovery_ignores_addresses_that_went_unanswered();
    test_discovery_json_records_readings_not_a_reset_plan();
    test_l3110_shows_both_counters_sharing_a_byte();
    test_counter_names_follow_wicreset_pad_order();
    test_no_model_repeats_a_gauge_label();
    test_snmp_get_request_is_byte_exact();
    test_snmp_response_decoding_is_bounded();
    test_snmp_control_oid_carries_the_read_command();
    test_snmp_gateway_reads_state_through_the_session();
    test_snmp_gateway_retries_a_lost_datagram();
    test_snmp_gateway_reports_silence_without_asking_on();
    test_snmp_gateway_tells_a_blocked_send_from_a_silent_printer();
    test_snmp_query_path_never_sends_a_write();
    test_snmp_reset_writes_and_verifies_through_the_session();
    test_snmp_reset_falls_back_to_the_alternate_key();
    test_snmp_reset_stops_at_a_refused_write();
    test_snmp_reset_refuses_a_replay_dump();
    test_snmp_gateway_waits_for_the_run_lock();
    test_snmp_directed_broadcast();
    test_snmp_discovery_keeps_only_epson_answers();
    test_run_lock_hands_over_to_the_network();

    std::cout << "\n----------------------------------------" << std::endl;
    if (g_failures == 0)
    {
        std::cout << "[ALL TESTS PASSED] " << g_checks << " checks." << std::endl;
        return 0;
    }

    std::cout << "[TESTS FAILED] " << g_failures << " of " << g_checks << " checks failed." << std::endl;
    return 1;
}
