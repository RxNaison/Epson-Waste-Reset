#include "support.h"

void test_d4_payload_extraction()
{
    std::cout << "[TEST] test_d4_payload_extraction" << std::endl;

    const std::vector<unsigned char> inner = { 'H', 'i', '!' };
    const auto pkt = WrapD4Data(inner);

    // Clean stream.
    CHECK(ewr::d4::ExtractPayload(pkt) == inner);

    // Stray NAK byte before the packet (the ET-2803 mi_00 chatter): resync.
    std::vector<unsigned char> noisy = { 0x15 };
    noisy.insert(noisy.end(), pkt.begin(), pkt.end());
    CHECK(ewr::d4::ExtractPayload(noisy) == inner);

    // A transaction-channel packet is skipped; the data packet after it is kept.
    std::vector<unsigned char> mixed = HandshakeAck();
    mixed.insert(mixed.end(), pkt.begin(), pkt.end());
    CHECK(ewr::d4::ExtractPayload(mixed) == inner);
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
        CHECK(ewr::d4::ExtractPayload(pkt) == inner);

        // Transaction traffic ahead of it is still skipped, not merged in.
        std::vector<unsigned char> mixed = HandshakeAck();
        mixed.insert(mixed.end(), pkt.begin(), pkt.end());
        CHECK(ewr::d4::ExtractPayload(mixed) == inner);
    }

    // Mismatched psid/ssid is not a data packet: resync rather than accept it.
    std::vector<unsigned char> mismatched = WrapD4DataOnSocket(inner, 0x04);
    mismatched[1] = 0x05;
    CHECK(ewr::d4::ExtractPayload(mismatched) != inner);

    // The whole status and read-back path has to survive the other socket, not
    // just the extractor.
    const auto st = ewr::ctrl::ParseStatusReply(WrapSt2Body({ 0x01, 0x01, 0x02 }, 0x04));
    CHECK(st.valid);
    CHECK(st.stateCode == 0x02);

    const std::string ps = "@BDC PS\r\nrw:41:5A;";
    uint8_t value = 0;
    CHECK(ewr::ctrl::ParseReadReply(
        WrapD4DataOnSocket(std::vector<unsigned char>(ps.begin(), ps.end()), 0x04), value));
    CHECK(value == 0x5A);
}

void test_query_session_failfast_on_silence()
{
    std::cout << "[TEST] test_query_session_failfast_on_silence" << std::endl;

    FakeTransport t; // never replies (the ET-2803 mi_01 behavior)

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunQuerySession(
        t,
        ewr::d4::Handshake(),
        { ewr::ctrl::StatusQueryPacket() },
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
    queries.push_back(ewr::ctrl::StatusQueryPacket());
    queries.push_back(ewr::ctrl::ReadPacket(model.rkey, 0x0018));

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunQuerySession(
        t, ewr::d4::Handshake(), queries, out, log, FastOptions());

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(!result.handshakeFailed);
    CHECK(result.replies.size() == 2);
    // 3 handshake + (credit grant + credit request + query) x 2 = 9 packets.
    CHECK(t.sent.size() == 9);

    if (result.replies.size() == 2)
    {
        const auto st = ewr::ctrl::ParseStatusReply(result.replies[0]);
        CHECK(st.valid);
        CHECK(st.hasError && st.errorCode == 0x05);

        uint8_t value = 0xFF;
        CHECK(ewr::ctrl::ParseReadReply(result.replies[1], value));
        CHECK(value == 0x00);
    }
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet first, next;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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

        ewr::d4::Framer framer(t);
        ewr::d4::Packet out;
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
    ewr::d4::SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;

    ewr::d4::Session session(t, log, options);
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
    ewr::d4::SessionOptions options;
    options.replyTimeoutMs = 30; // GetSocketID times out fast in tests
    options.dataTimeoutMs = 30;

    ewr::d4::Session session(t, log, options);
    CHECK(session.Start());
    CHECK(session.ChannelOpen());
    CHECK(session.Socket() == ewr::d4::SOCKET_EPSON_CTRL);
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
    ewr::d4::SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;

    ewr::d4::Session session(t, log, options);
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
    auto result = ewr::exec::RunQuerySession(
        t,
        ewr::d4::Handshake(),
        { ewr::ctrl::StatusQueryPacket(),
          ewr::ctrl::ReadPacket(0x0008, 0x18) },
        out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.replies.size() == 2);
    CHECK(ewr::d4::ExtractPayload(result.replies[0]) == ewr::d4::ExtractPayload(st2));

    uint8_t value = 0;
    CHECK(ewr::ctrl::ParseReadReply(result.replies[1], value, 0x18));
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
    auto result = ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(result.writesRejected == 0);
    CHECK(result.error.empty());
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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, LostReplyOptions());

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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, LostReplyOptions());

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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, LostReplyOptions());

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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, LostReplyOptions());

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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, LostReplyOptions());

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
        ewr::d4::RunQuerySession(t, LostReplyQueries(), reporter, options);

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
        ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, LostReplyOptions());

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
    auto result = ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

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
    const auto result = ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

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
// Every byte below is from a WIC Reset capture on a real SX110 (discussion
// #48): the packets EWR builds must be the ones that printer was seen to take.
void test_jig_packets_match_the_wic_capture()
{
    std::cout << "[TEST] test_jig_packets_match_the_wic_capture" << std::endl;

    using bytes = std::vector<unsigned char>;

    CHECK(ewr::jig::kLeavePacketMode == bytes({ 0x00, 0x00, 0x00, 0x1b, 0x01, '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4',
                                                '.', '4', '\n', '@', 'E', 'J', 'L', ' ', ' ', ' ', ' ', ' ', '\n' }));
    CHECK(ewr::jig::BuildEnter() == bytes({ 0x1b, 0x28, 0x52, 0x08, 0x00, 0x00, 'J', 'I', 'G', 'M', 'O', 'D', 'E' }));
    CHECK(ewr::jig::BuildReadPacket(0x214) == bytes({ 'J', 'G', 'P', 'G', 0x05, 0x00, 0x02, 0x14, 0x00, 0x01, 0x00 }));
    CHECK(ewr::jig::BuildReadPacket(0x138) == bytes({ 'J', 'G', 'P', 'G', 0x05, 0x00, 0x01, 0x38, 0x00, 0x01, 0x00 }));
    CHECK(ewr::jig::BuildExit() == bytes({ 'J', 'G', 0x1b, 0x00, 0x00, 0x00 }));

    CHECK(ewr::jig::ReadPacketAddress(ewr::jig::BuildReadPacket(0x214)) == 0x214);
    CHECK(ewr::jig::ReadPacketAddress(ewr::jig::BuildReadPacket(0x138)) == 0x138);
    CHECK(ewr::jig::ReadPacketAddress(ewr::jig::BuildReadPacket(0x2FF)) == 0x2FF);
    CHECK(ewr::jig::ReadPacketAddress(ewr::jig::BuildExit()) == -1);
    CHECK(ewr::jig::ReadPacketAddress(OkAck()) == -1);

    uint8_t value = 0;
    CHECK(ewr::jig::ParseReadReply({ 'J', 'G', 'P', 'G', 0x01, 0x00, 0xe0 }, value));
    CHECK(value == 0xe0);
    CHECK(!ewr::jig::ParseReadReply({ 'J', 'G', 'P', 'G', 0x01, 0x00 }, value));       // cut short
    CHECK(!ewr::jig::ParseReadReply({ 'J', 'G', 'P', 'G', 0x00, 0x00, 0x05 }, value)); // no byte declared
    CHECK(!ewr::jig::ParseReadReply({ '|', '|', ':', ';', 0x0c }, value));
    CHECK(!ewr::jig::ParseReadReply({}, value));
}

void test_jig_read_session_replays_the_capture()
{
    std::cout << "[TEST] test_jig_read_session_replays_the_capture" << std::endl;

    FakeTransport t = MakeJigPrinter({ { 0x214, 0xe0 }, { 0x215, 0x01 }, { 0x216, 0x00 }, { 0x217, 0x94 } });

    std::vector<std::vector<unsigned char>> queries;
    for (uint16_t address = 0x214; address <= 0x217; ++address)
        queries.push_back(ewr::jig::BuildReadPacket(address));

    // Through the same entry point a query session uses.
    ewr::ExecutorOptions options = FastOptions();
    options.jigMode = true;
    options.useSessionLayer = true; // jigMode wins
    std::ofstream log = NullLog();
    std::ostringstream out;
    const ewr::QuerySessionResult result = ewr::exec::RunQuerySession(t, {}, queries, out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(result.packetsSent == 4);
    CHECK(result.replies.size() == 4);

    const uint8_t expected[] = { 0xe0, 0x01, 0x00, 0x94 };
    for (size_t i = 0; i < result.replies.size() && i < 4; ++i)
    {
        uint8_t value = 0;
        CHECK(ewr::jig::ParseReadReply(result.replies[i], value));
        CHECK(value == expected[i]);
    }

    // WIC's order, packet for packet: EJL, Init, leave, JIGMODE, reads, exit.
    CHECK(t.sent.size() == 9);
    if (t.sent.size() == 9)
    {
        CHECK(t.sent[0] == ewr::d4::Handshake()[0]);
        CHECK(t.sent[1] == std::vector<unsigned char>({ 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 }));
        CHECK(t.sent[2] == ewr::jig::kLeavePacketMode);
        CHECK(t.sent[3] == ewr::jig::BuildEnter());
        for (size_t i = 0; i < 4; ++i)
            CHECK(t.sent[4 + i] == queries[i]);
        CHECK(t.sent[8] == ewr::jig::BuildExit());
    }

    // Nothing keyed, nothing that writes: no '||' command anywhere.
    const unsigned char bars[] = { '|', '|' };
    for (const auto& packet : t.sent)
        CHECK(std::search(packet.begin(), packet.end(), bars, bars + 2) == packet.end());
}

void test_jig_read_session_refuses_cleanly()
{
    std::cout << "[TEST] test_jig_read_session_refuses_cleanly" << std::endl;

    std::vector<std::vector<unsigned char>> queries;
    for (uint16_t address = 0x214; address <= 0x217; ++address)
        queries.push_back(ewr::jig::BuildReadPacket(address));

    const ewr::ExecutorOptions options = FastOptions();

    // A D4 printer that is not of this platform: the first read goes
    // unanswered, so the rest are not asked, and JIG mode is still left.
    {
        FakeTransport t = MakeJigPrinter({}, /*takesJigMode=*/false);
        ewr::log::Reporter reporter;
        const ewr::QuerySessionResult result = ewr::jig::RunReadSession(t, queries, reporter, options);
        CHECK(!result.success);
        CHECK(result.handshakeConfirmed);
        CHECK(!result.handshakeFailed);
        CHECK(!result.error.empty());
        CHECK(result.packetsSent == 1);
        CHECK(result.replies.size() == queries.size());
        CHECK(!t.sent.empty() && t.sent.back() == ewr::jig::BuildExit());
    }

    // An interface that answers nothing at all is a handshake failure, so the
    // driver moves on to the next one - and no read is sent into it.
    {
        FakeTransport t;
        ewr::log::Reporter reporter;
        const ewr::QuerySessionResult result = ewr::jig::RunReadSession(t, queries, reporter, options);
        CHECK(result.handshakeFailed);
        CHECK(!result.success);
        CHECK(result.packetsSent == 0);
        for (const auto& packet : t.sent)
            CHECK(ewr::jig::ReadPacketAddress(packet) < 0);
    }
}

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

    ewr::d4::SessionOptions options;
    options.replyTimeoutMs = 200;
    options.dataTimeoutMs = 200;
    options.fragmentTimeoutMs = 100;

    std::ofstream log = NullLog();
    ewr::d4::Session session(t, log, options);
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

        ewr::d4::SessionOptions options;
        options.replyTimeoutMs = 200;
        options.dataTimeoutMs = 200;
        options.fragmentTimeoutMs = 100;

        std::ofstream log = NullLog();
        ewr::d4::Session session(t, log, options);
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
    auto result = ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

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
    auto bothRejected = ewr::exec::RunSequence(bothBad, legacy::GenerateSequence(MakeTestModel()),
                                             bothOut, log, options);

    CHECK(!bothRejected.success);
    CHECK(!bothRejected.alternateKeyUsed);
    CHECK(bothRejected.writesRejected == 1);
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
        ewr::d4::RunQuerySession(transport, {}, reporter, options);

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(result.error == ewr::d4::kHttpPersonalityError);
}
