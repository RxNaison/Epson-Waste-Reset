#include "support.h"

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
    const ewr::snmp::Oid control = ewr::snmp::ControlOid({ 0x7C, 0x7C, 0xBE });
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
        snmp_test::CommandOf(ewr::ctrl::ReadPacket(0x0797, 0x01FE));
    const ewr::snmp::Oid expected = { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 44, 1, 1, 2, 1,
                                      124, 124, 7, 0, 151, 7, 65, 190, 160, 254, 1 };
    CHECK(ewr::snmp::ControlOid(command) == expected);
}

void test_snmp_gateway_reads_state_through_the_session()
{
    std::cout << "[TEST] test_snmp_gateway_reads_state_through_the_session" << std::endl;

    const ewr::DbPrinterModel model = snmp_test::TwoByteModel();

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();

    script->Serve(ewr::snmp::DeviceIdOid(), snmp_test::Bytes("MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:L3150 Series;CLS:PRINTER;"));
    script->Serve(ewr::snmp::StatusOid(), ewr::d4::ExtractPayload(MakeSt2Reply()));
    script->Serve(ewr::snmp::ControlOid(snmp_test::CommandOf(
                      ewr::ctrl::ReadPacket(model.rkey, 0x30))),
                  snmp_test::Bytes("@BDC PS\r\nEE:00302A;\f"));
    // 0x1FE is known to the printer but refused, as an unserved OID is.
    script->Know(ewr::snmp::ControlOid(snmp_test::CommandOf(
        ewr::ctrl::ReadPacket(model.rkey, 0x1FE))));

    std::ostringstream trace;
    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer), &trace);

    const ewr::DeviceIdQueryResult id = gateway.QueryDeviceId();
    CHECK(id.found);
    CHECK(ewr::deviceid::Parse(id.deviceId).model == "L3150 Series");
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

    // One read answered settles it: one refused address is not a lock.
    CHECK(gateway.EepromUnreachable().empty());
}

void test_snmp_gateway_retries_a_lost_datagram()
{
    std::cout << "[TEST] test_snmp_gateway_retries_a_lost_datagram" << std::endl;

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Serve(ewr::snmp::StatusOid(), ewr::d4::ExtractPayload(MakeSt2Reply()));
    script->dropFirst = 2;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    const ewr::StateSnapshot state = ewr::status::Read(gateway);

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
    script->Know(ewr::snmp::StatusOid());
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

    const ewr::QueryRunResult run = gateway.RunQuery({}, { ewr::ctrl::StatusQueryPacket() },
                                                     ewr::ExecutorOptions::ForQueries());
    CHECK(!run.deviceFound);
    CHECK(run.query.handshakeFailed);
    CHECK(run.query.replies.size() == 1 && run.query.replies[0].empty());
    CHECK(run.query.error.find("192.0.2.1") != std::string::npos);

    // Silence is not a refusal: nothing is known about the EEPROM.
    CHECK(gateway.EepromUnreachable().empty());
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
    quiet->Know(ewr::snmp::DeviceIdOid());
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
        ewr::ctrl::WritePacket(model.rkey, 0x30, 0x00, model.wkey);

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Serve(ewr::snmp::ControlOid(snmp_test::CommandOf(write)), snmp_test::Bytes("@BDC PS\r\n||:42:OK;\f"));

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    CHECK(gateway.OverNetwork());

    // Handed to the query path, a write is dropped, not forwarded.
    const ewr::QueryRunResult query = gateway.RunQuery({}, { write }, ewr::ExecutorOptions::ForQueries());
    CHECK(script->requested.empty());
    CHECK(query.query.replies.size() == 1 && query.query.replies[0].empty());

    // Nor does a read-back, which shares that path, ever turn into one.
    CHECK(!gateway.Answered());
}

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
    ewr::Database generator;
    const ewr::ResetRunResult run = silentGateway.RunReset(ewr::ctrl::ResetSequence(model), ewr::ExecutorOptions{});
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

    ewr::Database generator;
    ewr::ExecutorOptions replay;
    replay.verifyWrites = false;
    const ewr::ResetRunResult run = gateway.RunReset(ewr::ctrl::ResetSequence(model), replay);

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

    const ewr::QueryRunResult query = gateway.RunQuery({}, { ewr::ctrl::StatusQueryPacket() },
                                                       ewr::ExecutorOptions::ForQueries());
    CHECK(!query.query.success);
    CHECK(query.query.error.find("Another EWR run") != std::string::npos);

    ewr::Database generator;
    const ewr::ResetRunResult reset = gateway.RunReset(ewr::ctrl::ResetSequence(snmp_test::TwoByteModel()),
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
    std::ostringstream trace;
    const std::vector<ewr::NetworkPrinter> found =
        ewr::snmp::DiscoverPrinters(network, { "255.255.255.255", "192.168.1.255" }, 300, error, &trace);
    CHECK(error.empty());

    // When the printer is not found, this is the only record of why: what
    // was asked, and every answer with the reason it was kept or skipped.
    const std::string log = trace.str();
    CHECK(log.find("Asked:         255.255.255.255, 192.168.1.255") != std::string::npos);
    CHECK(log.find("  192.168.1.9: EPSON, MDL \"L3150 Series\" - kept") != std::string::npos);
    CHECK(log.find("192.168.1.30: SNMP error-status 2") != std::string::npos);
    CHECK(log.find("192.168.1.31: not an Epson (MFG \"Hewlett-Packard\")") != std::string::npos);
    CHECK(log.find("192.168.1.32: not an answer to this search") != std::string::npos);
    CHECK(log.find("192.168.1.33: not an answer to this search") != std::string::npos);
    CHECK(log.find("answered again - already listed") != std::string::npos);
    CHECK(log.find("Result:        2 Epson printer(s): 192.168.1.9, 192.168.1.40") != std::string::npos);

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
    CHECK(ewr::snmp::DiscoverPrinters(quiet, { "255.255.255.255" }, 30, error).empty());
    CHECK(error.empty());

    // A machine that refuses every send is not an empty network: it says so,
    // and does not sit out the wait for answers that cannot come.
    snmp_test::ScriptedNetwork blocked;
    blocked.refuseSend = true;
    blocked.agents = network.agents;
    const auto before = std::chrono::steady_clock::now();
    std::ostringstream blockedTrace;
    CHECK(ewr::snmp::DiscoverPrinters(blocked, { "255.255.255.255" }, 2000, error, &blockedTrace).empty());
    CHECK(error.find("refused to send") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(1000));
    CHECK(blockedTrace.str().find("refused the send to 255.255.255.255") != std::string::npos);
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

// Firmware that locks the EEPROM over the network still answers the status,
// so the run used to reach the writes and blame the refusal on an empty
// cartridge or a paper jam. It now stops before the first write and says to
// use USB, without asking a question that has only one answer.
void test_snmp_locked_eeprom_stops_before_any_write()
{
    std::cout << "[TEST] test_snmp_locked_eeprom_stops_before_any_write" << std::endl;

    auto printer = snmp_test::FullPads();
    snmp_test::EepromPrinter* script = printer.get();
    script->locked = true;
    const ewr::DbPrinterModel model = script->model;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);

    int refusedEvents = 0;
    const int sinkId = ewr::log::Default().AddSink([&](const ewr::log::Event& e)
    {
        if (e.code == "snmp.eeprom_refused")
            ++refusedEvents;
    });

    // --status and --dry-run see it too: the status answers, the counters do not.
    const ewr::StateSnapshot state = session.ReadState();
    CHECK(state.available);
    CHECK(!state.values.empty());
    for (const auto& value : state.values)
        CHECK(value.second == -1);
    CHECK(gateway.EepromUnreachable().find("USB") != std::string::npos);

    bool asked = false;
    bool blockerAsked = false;
    ewr::ResetHandlers handlers;
    handlers.confirmWrite = [&](const ewr::StateSnapshot&) { asked = true; return true; };
    handlers.onBlocker = [&](const ewr::Blocker&) { blockerAsked = true; return true; };
    const ewr::ResetOutcome outcome = session.Reset(handlers);

    ewr::log::Default().RemoveSink(sinkId);

    CHECK(outcome.phase == ewr::ResetPhase::NotSupported);
    CHECK(!outcome.success);
    CHECK(outcome.error.find("Nothing was written") != std::string::npos);
    CHECK(script->writesSeen == 0);
    CHECK(!asked && !blockerAsked);
    CHECK(ewr::json::ResetData(model, false, outcome)["phase"] == "not_supported");

    // Said once per run, not again for every read that follows.
    CHECK(refusedEvents == 1);
}

// Maintenance over the network: a cleaning travels as one SNMP GET, like a
// read, and the read-only path will not carry it.
void test_snmp_runs_a_command_once_and_queries_refuse_it()
{
    std::cout << "[TEST] test_snmp_runs_a_command_once_and_queries_refuse_it" << std::endl;

    const std::vector<unsigned char> clean = ewr::ctrl::FactoryPacket(0x0797, 0x84, { 0x02 });
    const ewr::snmp::Oid oid = ewr::snmp::ControlOid(snmp_test::CommandOf(clean));

    auto printer = std::make_unique<snmp_test::ScriptedPrinter>();
    snmp_test::ScriptedPrinter* script = printer.get();
    script->Serve(oid, snmp_test::Bytes("@BDC PS\r\n||:84:OK;"));

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));

    // The read-only path refuses it before anything is sent.
    const ewr::QueryRunResult query = gateway.RunQuery({}, { clean }, ewr::ExecutorOptions::ForQueries());
    CHECK(script->requested.empty());
    CHECK(query.query.replies.size() == 1 && query.query.replies[0].empty());

    const ewr::CommandRunResult run = gateway.RunCommand(clean, ewr::ExecutorOptions::ForQueries());
    CHECK(run.supported && run.sent && run.deviceFound);
    CHECK(script->requested.size() == 1);
    const std::vector<unsigned char> payload = ewr::d4::ExtractPayload(run.reply);
    CHECK(std::string(payload.begin(), payload.end()).find("||:84:OK;") != std::string::npos);
}

// Print data for a network printer goes to its raw port in one connection.
void test_raw_tcp_delivers_print_data()
{
    std::cout << "[TEST] test_raw_tcp_delivers_print_data" << std::endl;
    using namespace raw_port_test;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != kNone);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    CHECK(listen(listener, 1) == 0);

    socklen_t length = sizeof(local);
    getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length);
    const uint16_t port = ntohs(local.sin_port);

    std::vector<unsigned char> received;
    std::thread server([&]() {
        Socket client = accept(listener, nullptr, nullptr);
        if (client == kNone)
            return;
        unsigned char buffer[512];
        for (;;)
        {
            const int n = static_cast<int>(recv(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
            if (n <= 0)
                break;
            received.insert(received.end(), buffer, buffer + n);
        }
        CloseIt(client);
    });

    const std::vector<unsigned char> job = ewr::remote::BuildJob({ ewr::remote::NozzleCheck() });
    std::string error;
    CHECK(ewr::snmp::SendRawTcp("127.0.0.1", port, job, 3000, error));
    server.join();
    CloseIt(listener);
    CHECK(received == job);

    // Nobody listening: a refusal with a reason, in good time.
    CHECK(!ewr::snmp::SendRawTcp("127.0.0.1", port, job, 1000, error));
    CHECK(!error.empty());

#ifdef _WIN32
    WSACleanup();
#endif
}

// A reset over the network keeps its backup like a USB one, and a restore
// travels the same write path back.
void test_snmp_backup_then_restore_round_trip()
{
    std::cout << "[TEST] test_snmp_backup_then_restore_round_trip" << std::endl;

    auto printer = snmp_test::FullPads(); // 0x30 = 0xCA, 0x1FE = 0x18
    snmp_test::EepromPrinter* script = printer.get();
    const ewr::DbPrinterModel model = script->model;

    ewr::SnmpDeviceGateway gateway("192.0.2.1", std::move(printer));
    ewr::log::Reporter quiet;
    ewr::Session session(model, gateway, quiet);

    ewr::Backup kept;
    ewr::ResetHandlers handlers;
    handlers.saveBackup = [&](const ewr::Backup& backup, std::string& location)
    {
        kept = backup;
        location = "backups/net.json";
        return true;
    };

    const ewr::ResetOutcome reset = session.Reset(handlers);
    CHECK(reset.success);
    CHECK(kept.transport == "network");
    CHECK((kept.bytes == std::vector<std::pair<uint16_t, int>>{ { 0x30, 0xCA }, { 0x1FE, 0x18 } }));
    CHECK(script->eeprom[0x30] == 0x00 && script->eeprom[0x1FE] == 0x00);

    // The backup, through a file and back, undoes the reset.
    ewr::Backup reread;
    std::string error;
    CHECK(ewr::backup::Parse(ewr::backup::ToJson(kept), reread, error));
    const ewr::RestorePlan plan = ewr::restore::Plan(model, reread);
    CHECK(plan.writes.size() == 2 && plan.skipped.empty() && plan.missing.empty());

    ewr::ResetHandlers again;
    const ewr::ResetOutcome restored = session.Restore(plan, again);
    CHECK(restored.success);
    CHECK(restored.verificationRan && restored.verifyMismatches == 0);
    CHECK(script->eeprom[0x30] == 0xCA && script->eeprom[0x1FE] == 0x18);
}
