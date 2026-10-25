#include "support.h"

void test_scan_models()
{
    std::cout << "[TEST] test_scan_models" << std::endl;
    auto models = ewr::replay::ScanFolder("models");
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

    auto packets = ewr::replay::ParseWiresharkDump(test_filename);
    fs::remove(test_filename);

    CHECK(packets.size() == 1);
    CHECK(packets.size() == 1 && packets[0].size() == 2);
    CHECK(packets.size() == 1 && packets[0].size() == 2 && packets[0][0] == 0xAA && packets[0][1] == 0xBB);
}

void test_executor_success_path()
{
    std::cout << "[TEST] test_executor_success_path" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? OkAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesVerified == 0);
    CHECK(!result.error.empty());
}

void test_executor_fails_on_ng_reply_without_retry()
{
    std::cout << "[TEST] test_executor_fails_on_ng_reply_without_retry" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? NgAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

void test_executor_retries_missing_ack_with_credit_resend()
{
    std::cout << "[TEST] test_executor_retries_missing_ack_with_credit_resend" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    std::map<std::vector<unsigned char>, int> writeAttempts;
    t.replyFor = [&writeAttempts](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::ctrl::IsWrite(pkt))
            return HandshakeAck();
        int attempt = ++writeAttempts[pkt];
        return attempt >= 2 ? OkAck() : std::vector<unsigned char>{};
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? OkAck() : HandshakeAck();
    };

    // The GUI contract: consume a full run as structured events - stable
    // codes, ordered progress, wire detail as Trace - with no streams at all.
    std::vector<ewr::log::Event> events;
    ewr::log::Reporter reporter;
    reporter.AddSink([&events](const ewr::log::Event& e) { events.push_back(e); });

    auto result = ewr::exec::RunSequence(t, seq, reporter, FastOptions());

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(emptyModel);
    CHECK(seq.size() == 3);

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.writesTotal == 0);
    CHECK(!result.error.empty());
}

void test_executor_fails_on_transport_error()
{
    std::cout << "[TEST] test_executor_fails_on_transport_error" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.failSend = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

    CHECK(!result.success);
    CHECK(result.packetsSent == 0);
    CHECK(!result.error.empty());
}

void test_executor_handshake_failfast_on_silence()
{
    std::cout << "[TEST] test_executor_handshake_failfast_on_silence" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t; // total silence: the device never replies

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    // The device talks (credit-grant chatter) but never actually opens the
    // channel - the classic wrong-interface signature. Must still fail fast.
    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

    CHECK(!result.success);
    CHECK(result.handshakeFailed);
    CHECK(!result.handshakeConfirmed);
    CHECK(t.sent.size() == 5);
    CHECK(result.writesTotal == 0);
}

void test_executor_handshake_validation_success()
{
    std::cout << "[TEST] test_executor_handshake_validation_success" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? OkAck() : OpenChannelAck();
    };

    auto options = FastOptions();
    options.validateHandshake = true;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

    CHECK(result.success);
    CHECK(result.handshakeConfirmed);
    CHECK(!result.handshakeFailed);
    CHECK(result.writesVerified == 2);
    CHECK(result.error.empty());
}

void test_executor_drain_timeout_selection()
{
    std::cout << "[TEST] test_executor_drain_timeout_selection" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? OkAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.handshakeDrainTimeoutMs = 1234;
    options.writeAckTimeoutMs = 777;
    options.drainTimeoutMs = 55;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    std::map<std::vector<unsigned char>, int> writeAttempts;
    t.replyFor = [&writeAttempts](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::ctrl::IsWrite(pkt))
            return HandshakeAck();
        int attempt = ++writeAttempts[pkt];
        return attempt >= 2 ? OkAck() : std::vector<unsigned char>{};
    };

    auto options = FastOptions();
    options.resendCreditOnRetry = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(emptyModel); // 3 handshake packets, 0 writes

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>&) { return HandshakeAck(); };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(emptyModel);

    FakeTransport t; // device never says anything at all

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

    // Even in relaxed replay mode, a dead-silent device is never a SUCCESS.
    CHECK(!result.success);
    CHECK(!result.error.empty());
}

void test_executor_replay_ng_still_fatal()
{
    std::cout << "[TEST] test_executor_replay_ng_still_fatal" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? NgAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

    // An explicit :42:NG; rejection aborts immediately even in replay mode.
    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

void test_executor_na_refusal_fails_without_retry()
{
    std::cout << "[TEST] test_executor_na_refusal_fails_without_retry" << std::endl;

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    // The printer answers the handshake normally but refuses every EEPROM
    // write with ':42:NA;' (locked by another error, e.g. empty cartridge).
    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? NaAck() : HandshakeAck();
    };

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, FastOptions());

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

    ewr::Database gen;
    auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? NaAck() : HandshakeAck();
    };

    auto options = FastOptions();
    options.verifyWrites = false;

    std::ofstream log = NullLog();
    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, seq, out, log, options);

    // Even in relaxed replay mode an explicit refusal is never a SUCCESS.
    CHECK(!result.success);
    CHECK(result.writesRejected == 1);
    CHECK(t.sent.size() == 6);
    CHECK(!result.error.empty());
}

void test_alternate_write_key_retry()
{
    std::cout << "[TEST] test_alternate_write_key_retry" << std::endl;

    std::ofstream log = NullLog();

    FakeTransport t;
    t.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        if (!ewr::ctrl::IsWrite(pkt))
            return HandshakeAck();
        return EndsWithKey(pkt, "Bslbopje") ? OkAck() : NgAck();
    };

    ewr::ExecutorOptions options = FastOptions();
    options.writeKey = "Arkanoid";
    options.alternateWriteKey = "Bslbopje";

    std::ostringstream out;
    auto result = ewr::exec::RunSequence(t, legacy::GenerateSequence(MakeTestModel()), out, log, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesTotal == 2);
    CHECK(result.writesVerified == 2);
    CHECK(result.writesRejected == 0);

    // Without an alternate keyword, ':42:NG;' stays fatal on the first write.
    FakeTransport plain;
    plain.replyFor = [](const std::vector<unsigned char>& pkt) -> std::vector<unsigned char> {
        return ewr::ctrl::IsWrite(pkt) ? NgAck() : HandshakeAck();
    };

    std::ostringstream plainOut;
    auto rejected = ewr::exec::RunSequence(plain, legacy::GenerateSequence(MakeTestModel()),
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
        return ewr::ctrl::IsWrite(pkt) ? NgAck() : HandshakeAck();
    };

    std::ostringstream bothOut;
    auto bothRejected = ewr::exec::RunSequence(bothBad, legacy::GenerateSequence(MakeTestModel()),
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

// ---------------------------------------------------------------------------
// --interface pin: ExecutorOptions.interfaceCandidate must survive the
// Session facade's option rebuild on its way to every device call.
// ---------------------------------------------------------------------------

void test_interface_pin_option_threading()
{
    std::cout << "[TEST] test_interface_pin_option_threading" << std::endl;

    // Automatic selection is the default everywhere.
    CHECK(ewr::ExecutorOptions{}.interfaceCandidate == 0);
    CHECK(ewr::ExecutorOptions::ForQueries().interfaceCandidate == 0);

    const ewr::DbPrinterModel model = MakeSessionModel();
    FakeGateway gw = MakeSessionGateway();

    ewr::ExecutorOptions pinnedOptions = ewr::ExecutorOptions::ForQueries();
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

    const std::string same = ewr::exec::DescribeTraceContext(context);
    CHECK(same.find("Printer reports: L565 Series\n") != std::string::npos);
    CHECK(same.find("Detected entry:  L565\n") != std::string::npos);
    CHECK(same.find("Selected entry:  L565 (--yes: the detected model)\n") != std::string::npos);
    CHECK(same.find("[!]") == std::string::npos);

    context.selectedEntry = "L3110";
    context.selectedBy = "--model \"L3110\"";
    CHECK(ewr::exec::DescribeTraceContext(context).find("[!] The selected entry is not the one the printer matches.")
          != std::string::npos);

    // Nothing known: every line still printed, none claiming a mismatch.
    const std::string empty = ewr::exec::DescribeTraceContext({});
    CHECK(empty.find("Printer reports: (unknown)\n") != std::string::npos);
    CHECK(empty.find("Detected entry:  (none)\n") != std::string::npos);
    CHECK(empty.find("Selected entry:  (unknown)\n") != std::string::npos);
    CHECK(empty.find("[!]") == std::string::npos);

    // A printer that matches nothing is not a mismatch either.
    ewr::ExecutorOptions::TraceContext unmatched;
    unmatched.printerReports = "EPSON Mystery 9000";
    unmatched.selectedEntry = "L3110";
    CHECK(ewr::exec::DescribeTraceContext(unmatched).find("[!]") == std::string::npos);

    // A run with no entry on purpose says why, and is no mismatch: --find-key
    // on a printer the database does know selects nothing.
    ewr::ExecutorOptions::TraceContext searching;
    searching.printerReports = "Stylus Photo R220";
    searching.detectedEntry = "R220";
    searching.selectedBy = "--find-key";
    const std::string search = ewr::exec::DescribeTraceContext(searching);
    CHECK(search.find("Selected entry:  (none: --find-key)\n") != std::string::npos);
    CHECK(search.find("[!]") == std::string::npos);
}

void test_esc_remote_sequence_verified()
{
    std::cout << "[TEST] test_esc_remote_sequence_verified" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));
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
    const ewr::exec::End4Result result =
        ewr::exec::RunEscRemoteSequence(transport, commands, reporter, options);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport; // no replyFor -> the printer never answers

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::exec::End4Result result =
        ewr::exec::RunEscRemoteSequence(transport, commands, reporter, options);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

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
    const ewr::exec::End4Result result =
        ewr::exec::RunEscRemoteSequence(transport, commands, reporter, options);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

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
    const ewr::exec::End4Result result =
        ewr::exec::RunEscRemoteSequence(transport, commands, reporter, options);

    CHECK(!result.success);
    CHECK(!result.alternateKeyUsed);
    CHECK(result.writesRejected == 1);
}

void test_end4_framing()
{
    std::cout << "[TEST] test_end4_framing" << std::endl;

    const std::vector<unsigned char> cmd = { 's', 't', 0x01 };
    const std::vector<unsigned char> end4Pkt = ewr::end4::BuildPacket(cmd);

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
    bool parsed = ewr::end4::ParseResponse(rawReply, payload);
    CHECK(parsed);
    CHECK(payload.size() == 4);
    if (payload.size() == 4)
    {
        CHECK(payload[0] == '@');
        CHECK(payload[1] == 'B');
    }
}

// The END4 path hands ParseResponse an arbitrary suffix found by searching
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
        CHECK(!ewr::end4::ParseResponse(malformed, body));
        CHECK(body.empty());
    }

    // Exactly the header size is a well-formed frame with an empty body.
    const std::vector<unsigned char> headerOnly = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x0A,
        '@', 'B', 'D', 'C'
    };

    std::vector<unsigned char> emptyBody = { 0xFF };
    CHECK(ewr::end4::ParseResponse(headerOnly, emptyBody));
    CHECK(emptyBody.empty());

    // A frame that announces more than arrived still hands back the partial
    // body, which is what lets the caller keep draining.
    const std::vector<unsigned char> short_ = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x20,
        '@', 'B', 'D', 'C'
    };

    std::vector<unsigned char> partial;
    CHECK(!ewr::end4::ParseResponse(short_, partial));
    CHECK(partial.size() == 4);
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
        if (ewr::ctrl::IsWrite(pkt))
            expected.emplace_back(pkt.begin() + 6, pkt.end());
    }

    const std::vector<std::vector<unsigned char>> commands = ewr::ctrl::ExtractWriteCommands(sequence);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));
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
    const ewr::exec::End4Result result =
        ewr::exec::RunEnd4Sequence(transport, "MFG:EPSON;DDS:0020;", commands, reporter, options);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport; // no replyFor -> the printer never answers

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100; // keep the bounded give-up quick for the test

    ewr::log::Reporter reporter;
    const ewr::exec::End4Result result =
        ewr::exec::RunEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

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
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

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

    const ewr::exec::End4Result result =
        ewr::exec::RunEnd4Sequence(transport, "DDS:0020;", commands, reporter, options);

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

    ewr::Database gen;
    const auto seq = ewr::ctrl::ResetSequence(MakeTestModel());

    // A channel-open ack trailed by a burst the size of a full drain window.
    std::vector<unsigned char> flood = HandshakeAck();
    flood.resize(16 * 1024, 0xA5);

    FakeTransport t;
    t.replyFor = [&flood](const std::vector<unsigned char>& pkt) {
        return ewr::ctrl::IsWrite(pkt) ? OkAck() : flood;
    };

    std::ostringstream out;
    std::ostringstream log;
    ewr::exec::RunSequence(t, seq, out, log, FastOptions());

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

    CHECK(ewr::format::HexDumpCapped(eight.data(), eight.size(), 8) == ewr::format::HexDump(eight.data(), eight.size()));
    CHECK(ewr::format::HexDumpCapped(eight.data(), eight.size(), 64) == ewr::format::HexDump(eight.data(), eight.size()));

    // Over the cap: head kept, remainder counted rather than silently dropped.
    const std::string capped = ewr::format::HexDumpCapped(eight.data(), eight.size(), 4);
    CHECK(capped.find("... 4 further byte(s) not shown") != std::string::npos);
    CHECK(capped.find(ewr::format::HexDump(eight.data(), 4)) == 0);

    CHECK(ewr::format::HexDumpCapped(eight.data(), 0, 16) == ewr::format::HexDump(eight.data(), 0));
}

void test_end4_sequence_alternate_key()
{
    std::cout << "[TEST] test_end4_sequence_alternate_key" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel(); // writes end with wkey "Arkanoid"
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));
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
    const ewr::exec::End4Result result =
        ewr::exec::RunEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

    CHECK(result.success);
    CHECK(result.alternateKeyUsed);
    CHECK(result.writesVerified == 2);
}

void test_http_personality_detection()
{
    std::cout << "[TEST] test_http_personality_detection" << std::endl;

    const std::vector<unsigned char> http = Http500Reply();
    CHECK(http.size() == 57);
    CHECK(ewr::d4::LooksLikeHttpReply(http.data(), http.size()));

    const std::string ok = "HTTP/1.1 200 OK\r\n";
    CHECK(ewr::d4::LooksLikeHttpReply(reinterpret_cast<const unsigned char*>(ok.data()), ok.size()));

    // Only a leading status line counts. A D4 packet that happens to carry the
    // bytes further in is still a D4 packet.
    const std::vector<unsigned char> d4 = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x80, 0x00 };
    CHECK(!ewr::d4::LooksLikeHttpReply(d4.data(), d4.size()));

    std::vector<unsigned char> buried = { 0x00, 0x00 };
    buried.insert(buried.end(), http.begin(), http.end());
    CHECK(!ewr::d4::LooksLikeHttpReply(buried.data(), buried.size()));

    // Short and empty buffers must not read past the end.
    const std::vector<unsigned char> shortBuf = { 'H', 'T', 'T' };
    CHECK(!ewr::d4::LooksLikeHttpReply(shortBuf.data(), shortBuf.size()));
    CHECK(!ewr::d4::LooksLikeHttpReply(nullptr, 0));
}

// The four-week bug: the run reported "returned data but never an END4 reply",
// which is true and tells nobody which interface to look at instead.
void test_end4_reports_the_http_personality()
{
    std::cout << "[TEST] test_end4_reports_the_http_personality" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) { return Http500Reply(); };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::exec::End4Result result =
        ewr::exec::RunEnd4Sequence(transport, "DDS:0000;", commands, reporter, options);

    CHECK(!result.success);
    CHECK(result.httpReply);
    CHECK(result.anyBytes);
    CHECK(result.error == ewr::d4::kHttpPersonalityError);
}

void test_esc_remote_reports_the_http_personality()
{
    std::cout << "[TEST] test_esc_remote_reports_the_http_personality" << std::endl;

    const ewr::DbPrinterModel model = MakeTestModel();
    const std::vector<std::vector<unsigned char>> commands =
        ewr::ctrl::ExtractWriteCommands(legacy::GenerateSequence(model));

    FakeTransport transport;
    transport.replyFor = [](const std::vector<unsigned char>&) { return Http500Reply(); };

    ewr::ExecutorOptions options;
    options.writeKey = model.wkey;
    options.interPacketDelayMs = 0;
    options.writeAckTimeoutMs = 10;
    options.handshakeDrainTimeoutMs = 100;

    ewr::log::Reporter reporter;
    const ewr::exec::End4Result result =
        ewr::exec::RunEscRemoteSequence(transport, commands, reporter, options);

    CHECK(!result.success);
    CHECK(result.httpReply);
    CHECK(result.error == ewr::d4::kHttpPersonalityError);
}
