#include "support.h"

void test_ack_predicates()
{
    std::cout << "[TEST] test_ack_predicates" << std::endl;

    CHECK(ewr::ctrl::IsWriteOk(OkAck()));
    CHECK(!ewr::ctrl::IsWriteOk({}));
    CHECK(!ewr::ctrl::IsWriteOk(HandshakeAck()));
    CHECK(!ewr::ctrl::IsWriteOk(NgAck()));

    CHECK(ewr::ctrl::IsWriteNg(NgAck()));
    CHECK(!ewr::ctrl::IsWriteNg(OkAck()));
    CHECK(!ewr::ctrl::IsWriteNg({}));

    CHECK(ewr::ctrl::IsWriteNa(NaAck()));
    CHECK(!ewr::ctrl::IsWriteNa(OkAck()));
    CHECK(!ewr::ctrl::IsWriteNa(NgAck()));
    CHECK(!ewr::ctrl::IsWriteNa({}));
    CHECK(!ewr::ctrl::IsWriteOk(NaAck()));
    CHECK(!ewr::ctrl::IsWriteNg(NaAck()));

    std::vector<unsigned char> channelOpenAck = { 0x00, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x81, 0x00, 0x02, 0x02, 0x00, 0x01 };
    CHECK(ewr::d4::IsChannelOpenAck(channelOpenAck));
    CHECK(!ewr::d4::IsChannelOpenAck({}));
    CHECK(!ewr::d4::IsChannelOpenAck(OkAck()));

    std::vector<unsigned char> strayByte = { 0x00, 0x81, 0x00, 0x0c, 0x01, 0x00, 0x7F, 0x00 };
    CHECK(!ewr::d4::IsChannelOpenAck(strayByte));
}

void test_write_packet_detection()
{
    std::cout << "[TEST] test_write_packet_detection" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());
    CHECK(seq.size() == 9);

    for (size_t i = 0; i < 5 && i < seq.size(); ++i)
        CHECK(!ewr::ctrl::IsWrite(seq[i]));

    if (seq.size() == 9)
    {
        CHECK(ewr::ctrl::IsWrite(seq[5]));
        CHECK(ewr::ctrl::IsWrite(seq[8]));
    }

    std::vector<unsigned char> adversarial = {
        0x02, 0x02, 0x00, 0x14, 0x00, 0x00, 0x7C, 0x7C,
        0x0A, 0x00, 0x01, 0x00, 0x41, 0xBE, 0xA0, 0x42, 0x42, 0x42, 0x42, 0x42
    };
    CHECK(!ewr::ctrl::IsWrite(adversarial));

    std::vector<unsigned char> truncated = { 0x02, 0x02, 0x00, 0x08, 0x00, 0x00, 0x7C, 0x7C };
    CHECK(!ewr::ctrl::IsWrite(truncated));
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

void test_packet_structure_integrity()
{
    std::cout << "[TEST] test_packet_structure_integrity" << std::endl;
    ewr::Database gen;
    bool loaded = gen.Load("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.Models();
    CHECK(!models.empty());

    size_t inspected = 0;
    for (size_t idx = 0; idx < models.size() && inspected < 20; ++idx)
    {
        const auto& m = models[idx];
        auto addrs = m.GetAllAddresses();
        if (addrs.empty())
            continue;

        inspected++;
        auto seq = ewr::ctrl::ResetSequence(m);

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
            CHECK(writePkt[0] == ewr::d4::SOCKET_EPSON_CTRL && writePkt[1] == ewr::d4::SOCKET_EPSON_CTRL);
            CHECK(((size_t)(writePkt[2] << 8) | writePkt[3]) == writePkt.size());
            CHECK(writePkt[4] == ewr::d4::CREDIT);
            CHECK(writePkt[6] == ewr::ctrl::PREFIX_PIPE && writePkt[7] == ewr::ctrl::PREFIX_PIPE);
            CHECK(((size_t)writePkt[8] | ((size_t)writePkt[9] << 8)) == 6 + addrLen + m.wkey.size());

            CHECK(writePkt[12] == ewr::ctrl::CMD_EEPROM_WRITE);
            CHECK(writePkt[13] == 0xBD);
            CHECK(writePkt[14] == 0x21);

            CHECK(ewr::ctrl::IsWrite(writePkt));
        }
    }

    CHECK(inspected > 0);
}

void test_byte_parity()
{
    std::cout << "[TEST] test_byte_parity" << std::endl;
    ewr::Database gen;
    bool loaded = gen.Load("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.Models();
    CHECK(models.size() > 1400);

    size_t verifiedModels = 0;
    size_t narrowModels = 0;
    size_t totalPackets = 0;
    bool allEqual = true;
    bool narrowFormOk = true;
    bool narrowDiverged = false;

    for (const auto& m : models)
    {
        auto gotSeq = ewr::ctrl::ResetSequence(m);

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
                if (!ewr::ctrl::IsWrite(pkt))
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

void test_generator_respects_mem_high()
{
    std::cout << "[TEST] test_generator_respects_mem_high" << std::endl;

    ewr::DbPrinterModel m = MakeTestModel();
    m.mem_high = 0x0018; // the second address (0x0019) is now out of range

    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(m);

    ewr::log::Default().RemoveSink(sinkId);

    // 3 handshake packets + one triplet for the single in-range address.
    CHECK(seq.size() == 6);

    size_t writes = 0;
    for (const auto& pkt : seq)
        if (ewr::ctrl::IsWrite(pkt))
            writes++;
    CHECK(writes == 1);

    if (seq.size() == 6)
    {
        CHECK(seq[5][15] == 0x18);
        CHECK(seq[5][16] == 0x00);
    }

    CHECK(captured.find("mem_high") != std::string::npos);
}

void test_status_reply_parsing()
{
    std::cout << "[TEST] test_status_reply_parsing" << std::endl;

    const auto st = ewr::ctrl::ParseStatusReply(MakeSt2Reply());

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
    CHECK(ewr::status::Describe(st).find("INK OUT") != std::string::npos);

    CHECK(!st.truncated);

    // Streams that never contain '@BDC ST2' parse to valid == false.
    CHECK(!ewr::ctrl::ParseStatusReply(HandshakeAck()).valid);
    CHECK(!ewr::ctrl::ParseStatusReply({}).valid);
}

// An '@BDC ST2' prefix and a length field are not a status report on their own.
// Reporting one as valid tells the blocker gate the printer is clear when
// nothing about the printer was ever read.
void test_status_reply_without_fields_is_invalid()
{
    std::cout << "[TEST] test_status_reply_without_fields_is_invalid" << std::endl;

    // Declared length 0: the prefix arrived, the body did not.
    const auto empty = ewr::ctrl::ParseStatusReply(WrapSt2Body({}));
    CHECK(!empty.valid);
    CHECK(!empty.hasError);
    CHECK(empty.stateCode == -1);
    CHECK(ewr::status::Describe(empty) == "status unavailable");

    // A body of headers this build does not decode is no evidence either.
    const std::vector<unsigned char> unknownOnly = { 0x71, 0x01, 0x00, 0x72, 0x01, 0x00 };
    CHECK(!ewr::ctrl::ParseStatusReply(WrapSt2Body(unknownOnly)).valid);

    // A recognized header with an empty body decodes nothing.
    const std::vector<unsigned char> emptyField = { 0x01, 0x00 };
    CHECK(!ewr::ctrl::ParseStatusReply(WrapSt2Body(emptyField)).valid);
}

// A report that stops mid-field parses what arrived, but the error entry may be
// part of what did not - so the truncation has to survive into the snapshot.
void test_status_reply_truncation_is_recorded()
{
    std::cout << "[TEST] test_status_reply_truncation_is_recorded" << std::endl;

    // State IDLE, then a serial field announcing 32 bytes that never arrive.
    std::vector<unsigned char> body = { 0x01, 0x01, 0x04, 0x40, 0x20, 'X', '7' };

    // The length field counts the bytes the report claims, not the ones sent.
    const auto st = ewr::ctrl::ParseStatusReply(WrapSt2Body(body, ewr::d4::SOCKET_EPSON_CTRL, 0x25));

    CHECK(st.valid);        // the state field did arrive
    CHECK(st.truncated);    // the serial field did not
    CHECK(st.stateCode == 0x04);
    CHECK(st.stateName == "IDLE");
    CHECK(!st.hasError);

    // A complete report is not flagged.
    CHECK(!ewr::ctrl::ParseStatusReply(WrapSt2Body({ 0x01, 0x01, 0x04 })).truncated);
}

void test_eeprom_read_reply_parsing()
{
    std::cout << "[TEST] test_eeprom_read_reply_parsing" << std::endl;

    uint8_t value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReply(0x5A), value));
    CHECK(value == 0x5A);

    value = 0xFF;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReply(0x00), value));
    CHECK(value == 0x00);

    // A write ack (':42:OK;') is not a read reply.
    CHECK(!ewr::ctrl::ParseReadReply(OkAck(), value));
    CHECK(!ewr::ctrl::ParseReadReply({}, value));

    // '@BDC PS' prefix without a ':41:' echo is rejected too.
    const std::string s = "@BDC PS\r\nrw:42:OK;";
    CHECK(!ewr::ctrl::ParseReadReply(WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())), value));

    // R220-style 'EE:AAVV;' form: address echo plus value (seen on real
    // Stylus Photo R220 hardware, trace 2026-08-03).
    value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0x0C, 0x1C), value));
    CHECK(value == 0x1C);

    // Address echo is validated when the caller provides the expected address.
    value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0x3E, 0x0C), value, 0x3E));
    CHECK(value == 0x0C);

    // A mismatching address echo is rejected.
    CHECK(!ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0x0D, 0x39), value, 0x2B));

    // Expected addresses above one byte skip the echo comparison (form B
    // only echoes a single byte).
    value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0xFC, 0x07), value, 0x2FC));
    CHECK(value == 0x07);

    // Truncated 'EE:' bodies are rejected.
    const std::string t = "@BDC PS\r\nEE:0C";
    CHECK(!ewr::ctrl::ParseReadReply(WrapD4Data(std::vector<unsigned char>(t.begin(), t.end())), value));
}

void test_address_length_framing()
{
    std::cout << "[TEST] test_address_length_framing" << std::endl;

    // 'rlen'/'wlen' select the width of the ADDRESS field, so a 1-byte model's
    // packet is one byte shorter and every field after the address shifts.
    const auto write2 = ewr::ctrl::WritePacket(0x0008, 0x0018, 0x2A, "Arkanoid", 2);
    const auto write1 = ewr::ctrl::WritePacket(0x0008, 0x0018, 0x2A, "Arkanoid", 1);

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

    const auto read2 = ewr::ctrl::ReadPacket(0x0008, 0x0018, 2);
    const auto read1 = ewr::ctrl::ReadPacket(0x0008, 0x0018, 1);

    CHECK(read2.size() == read1.size() + 1);
    CHECK(read1.size() == 16); // D4(6) + '||'(2) + len(2) + rkey(2) + triplet(3) + addr(1)
    CHECK(read2[12] == 0x41 && read2[13] == 0xBE && read2[14] == 0xA0);
    CHECK(read1[15] == 0x18);

    // 2-byte addressing stays the default for callers that do not specify.
    CHECK(ewr::ctrl::ReadPacket(0x0008, 0x0018) == read2);
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

    ewr::Database generator;
    const auto sequence = ewr::ctrl::ResetSequence(m);

    // Handshake (3) + credit pair + one write: 0x120 does not fit a 1-byte
    // address field, so it is skipped rather than silently truncated to 0x20.
    CHECK(sequence.size() == 6);

    size_t writes = 0;
    for (const auto& pkt : sequence)
    {
        if (ewr::ctrl::IsWrite(pkt))
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
    CHECK(ewr::ctrl::ResetSequence(wide).size() == 9);
}

void test_eeprom_read_reply_two_byte_address()
{
    std::cout << "[TEST] test_eeprom_read_reply_two_byte_address" << std::endl;

    uint8_t value = 0;

    // 6 hex digits: 2-byte address echo (big endian) followed by the value.
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE16(0x0130, 0x2A), value, 0x0130));
    CHECK(value == 0x2A);

    // A wide echo can be verified against the requested address in full.
    CHECK(!ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE16(0x0130, 0x2A), value, 0x0131));

    value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE16(0x0002, 0x5E), value));
    CHECK(value == 0x5E);

    // The 1-byte form still parses, including the case where the echo is too
    // narrow to confirm an address above 0xFF.
    value = 0;
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0x0C, 0x1C), value, 0x0C));
    CHECK(value == 0x1C);
    CHECK(ewr::ctrl::ParseReadReply(MakeEepromReadReplyEE(0xFC, 0x07), value, 0x02FC));
}

void test_write_key_substitution()
{
    std::cout << "[TEST] test_write_key_substitution" << std::endl;

    const auto packet = ewr::ctrl::WritePacket(0x0008, 0x0018, 0x00, "Arkanoid", 2);

    std::vector<unsigned char> out;
    CHECK(ewr::ctrl::SubstituteTrailingWriteKey(packet, "Arkanoid", "Bslbopje", out));
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
    CHECK(!ewr::ctrl::SubstituteTrailingWriteKey(packet, "Wrongkey", "Bslbopje", ignored));
    CHECK(!ewr::ctrl::SubstituteTrailingWriteKey(packet, "Arkanoid", "", ignored));
    CHECK(!ewr::ctrl::SubstituteTrailingWriteKey(packet, "Arkanoid", "Short", ignored));
}

// The refusals are different evidence, so they must not collapse into one:
// ':41:NA;' turns a known read down, a bare '||:;' never echoes the action
// code at all (an SX110 answered every read that way).
void test_classify_eeprom_read_reply()
{
    std::cout << "[TEST] test_classify_eeprom_read_reply" << std::endl;

    auto text = [](const std::string& s) { return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())); };

    uint8_t value = 0;
    CHECK(ewr::ctrl::ClassifyReadReply({}, value, 0) == ewr::ReadAnswer::Silent);

    CHECK(ewr::ctrl::ClassifyReadReply(MakeEepromReadReplyEE16(0, 0x5E), value, 0) == ewr::ReadAnswer::Value);
    CHECK(value == 0x5E);
    CHECK(ewr::ctrl::ClassifyReadReply(MakeEepromReadReplyEE(0, 0x07), value, 0) == ewr::ReadAnswer::Value);
    CHECK(value == 0x07);

    CHECK(ewr::ctrl::ClassifyReadReply(text("||:41:NA;\f"), value, 0) == ewr::ReadAnswer::Refused);
    CHECK(ewr::ctrl::ClassifyReadReply(text("||:;\f"), value, 0) == ewr::ReadAnswer::Empty);
    CHECK(ewr::ctrl::ClassifyReadReply(text("||:;\r\n\f"), value, 0) == ewr::ReadAnswer::Empty);

    // Neither a value nor a known refusal: reported, never guessed at.
    CHECK(ewr::ctrl::ClassifyReadReply(text("zz:;\f"), value, 0) == ewr::ReadAnswer::Other);
    // A value for another address is not an answer to this read.
    CHECK(ewr::ctrl::ClassifyReadReply(MakeEepromReadReplyEE16(0x18, 1), value, 0) == ewr::ReadAnswer::Other);
}

// ---------------------------------------------------------------------------
// Factory packets and range reads (0x51)
// ---------------------------------------------------------------------------

void test_factory_packet_matches_the_verified_builders()
{
    std::cout << "[TEST] test_factory_packet_matches_the_verified_builders" << std::endl;

    namespace G = ewr::ctrl;

    // The generic builder must produce exactly what the hardware-verified
    // read builder does, at both address widths.
    CHECK(G::FactoryPacket(0x3B10, 0x41, { 0x0C }) == G::ReadPacket(0x3B10, 0x0C, 1));
    CHECK(G::FactoryPacket(0x0797, 0x41, { 0x2F, 0x01 }) == G::ReadPacket(0x0797, 0x012F, 2));

    const std::vector<unsigned char> range1 = G::RangeReadPacket(0x3B10, 0x0C, 32, 1);
    const std::vector<unsigned char> expected1 = {
        0x02, 0x02, 0x00, 0x11, 0x00, 0x00,       // D4 header, 17 bytes
        '|', '|', 0x07, 0x00,                     // factory command, 7 bytes
        0x10, 0x3B, 0x51, 0xAE, 0xA8,             // key, 0x51 and its check bytes
        0x0C, 0x20,                               // address, count
    };
    CHECK(range1 == expected1);

    const std::vector<unsigned char> range2 = G::RangeReadPacket(0x0797, 0x0120, 16, 2);
    CHECK(range2.size() == expected1.size() + 1);
    CHECK(range2[range2.size() - 3] == 0x20 && range2[range2.size() - 2] == 0x01 && range2.back() == 16);
}

void test_range_reply_parsing()
{
    std::cout << "[TEST] test_range_reply_parsing" << std::endl;

    std::vector<uint8_t> values;

    // Address echo, one byte (R220-era) and two bytes.
    CHECK(ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C1234AB;"), 0x0C, 3, values));
    CHECK((values == std::vector<uint8_t>{ 0x12, 0x34, 0xAB }));
    CHECK(ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:012000ff;"), 0x0120, 2, values));
    CHECK((values == std::vector<uint8_t>{ 0x00, 0xFF }));

    // Action echo.
    CHECK(ewr::ctrl::ParseRangeReadReply(MakeRangeReply("||:51:0102;"), 0x40, 2, values));
    CHECK((values == std::vector<uint8_t>{ 0x01, 0x02 }));

    // Anything else is not values: a refusal, a short or long run, another
    // address, a non-hex digit, a missing terminator.
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("||:51:NA;"), 0x40, 2, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C1234;"), 0x0C, 3, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C12345678;"), 0x0C, 3, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0D1234AB;"), 0x0C, 3, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C12G4AB;"), 0x0C, 3, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C1234AB"), 0x0C, 3, values));
    CHECK(!ewr::ctrl::ParseRangeReadReply(MakeRangeReply("EE:0C;"), 0x0C, 0, values));
    CHECK(values.empty());
}

// The read-only path refuses anything but reads, whoever hands it in.
void test_query_path_refuses_what_is_not_a_read()
{
    std::cout << "[TEST] test_query_path_refuses_what_is_not_a_read" << std::endl;

    namespace G = ewr::ctrl;
    CHECK(ewr::ctrl::IsReadOnlyQuery(G::StatusQueryPacket()));
    CHECK(ewr::ctrl::IsReadOnlyQuery(G::ReadPacket(0x3B10, 0x0C, 1)));
    CHECK(ewr::ctrl::IsReadOnlyQuery(G::RangeReadPacket(0x3B10, 0x0C, 8, 1)));
    CHECK(!ewr::ctrl::IsReadOnlyQuery(G::WritePacket(0x3B10, 0x0C, 0, "McLaren", 1)));
    CHECK(!ewr::ctrl::IsReadOnlyQuery(G::FactoryPacket(0x3B10, 0x84, { 0x02 })));
    CHECK(!ewr::ctrl::IsReadOnlyQuery(G::FactoryPacket(0x3B10, 0x21, {})));
    CHECK(!ewr::ctrl::IsReadOnlyQuery({ 0x02, 0x02, 0x00, 0x0B, 0x00, 0x00, 'f', 'l', 0x01, 0x00, 0x01 }));
    CHECK(!ewr::ctrl::IsReadOnlyQuery({}));
}
