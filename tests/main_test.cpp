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
    bool failSend = false;

    bool Send(const std::vector<unsigned char>& packet) override
    {
        sent.push_back(packet);
        if (failSend)
            return false;
        pending_ = replyFor ? replyFor(packet) : std::vector<unsigned char>{};
        return true;
    }

    std::vector<unsigned char> Drain() override
    {
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

static std::vector<unsigned char> HandshakeAck()
{
    return { 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 0x83, 0x00, 0x02, 0x02 };
}

static ewr::ExecutorOptions FastOptions()
{
    ewr::ExecutorOptions options;
    options.interPacketDelayMs = 0;
    options.retryDelayMs = 0;
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

            CHECK(writePkt.size() == 18 + m.wkey.size());
            CHECK(writePkt[0] == ewr::EpsonD4::SOCKET_EPSON_CTRL && writePkt[1] == ewr::EpsonD4::SOCKET_EPSON_CTRL);
            CHECK(((size_t)(writePkt[2] << 8) | writePkt[3]) == writePkt.size());
            CHECK(writePkt[4] == ewr::EpsonD4::CREDIT);
            CHECK(writePkt[6] == ewr::EpsonD4::PREFIX_PIPE && writePkt[7] == ewr::EpsonD4::PREFIX_PIPE);
            CHECK(((size_t)writePkt[8] | ((size_t)writePkt[9] << 8)) == 8 + m.wkey.size());

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
    size_t totalPackets = 0;
    bool allEqual = true;

    for (const auto& m : models)
    {
        auto gotSeq = gen.GenerateSequence(m);
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
    CHECK(verifiedModels == models.size());
    std::cout << "  Byte parity verified across " << verifiedModels << " models ("
              << totalPackets << " packets vs. hardware reference)." << std::endl;
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

    std::cout << "\n----------------------------------------" << std::endl;
    if (g_failures == 0)
    {
        std::cout << "[ALL TESTS PASSED] " << g_checks << " checks." << std::endl;
        return 0;
    }

    std::cout << "[TESTS FAILED] " << g_failures << " of " << g_checks << " checks failed." << std::endl;
    return 1;
}
