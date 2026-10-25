#include "support.h"

void test_evaluate_blocker()
{
    std::cout << "[TEST] test_evaluate_blocker" << std::endl;

    ewr::PrinterStatus status;

    // Unparseable or error-free statuses never block.
    CHECK(!ewr::gates::Evaluate(status).has_value());
    status.valid = true;
    CHECK(!ewr::gates::Evaluate(status).has_value());

    // The waste-pad errors EWR exists to clear must never block.
    status.hasError = true;
    status.errorCode = 0x10; // SERVICE REQUEST
    status.errorName = "SERVICE REQUEST";
    CHECK(ewr::gates::IsExpectedWastePadError(0x10));
    CHECK(!ewr::gates::Evaluate(status).has_value());

    status.errorCode = 0x2C; // CARTRIDGE OVERFLOW
    CHECK(ewr::gates::IsExpectedWastePadError(0x2C));
    CHECK(!ewr::gates::Evaluate(status).has_value());

    // A foreign lock (ink out, jam, open cover) blocks and carries the error.
    status.errorCode = 0x05;
    status.errorName = "INK OUT";
    CHECK(!ewr::gates::IsExpectedWastePadError(0x05));
    const std::optional<ewr::Blocker> blocker = ewr::gates::Evaluate(status);
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

// Every host gets the entry into the trace, the C API included, without
// having to say it: the Session names the model it runs.
void test_session_names_its_model_in_the_trace_context()
{
    std::cout << "[TEST] test_session_names_its_model_in_the_trace_context" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();

    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; }; // past INK OUT

    FakeGateway quiet = MakeSessionGateway();
    ewr::Session unnamed(model, quiet, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());
    CHECK(unnamed.Reset(handlers).phase == ewr::ResetPhase::Done);
    CHECK(quiet.queryCalls > 0);
    CHECK(quiet.lastQueryOptions.trace.selectedEntry == model.name);
    CHECK(quiet.lastResetOptions.trace.selectedEntry == model.name);

    // What a host says is kept, not overwritten.
    ewr::ExecutorOptions named = ewr::ExecutorOptions::ForQueries();
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

void test_session_reads_a_jig_model()
{
    std::cout << "[TEST] test_session_reads_a_jig_model" << std::endl;

    const ewr::DbPrinterModel model = MakeJigModel();
    JigGateway gw;
    ewr::Session session(model, gw, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());

    const ewr::StateSnapshot state = session.ReadState();
    CHECK(state.available);
    CHECK(state.status.valid);

    // The status over D4 as always, then one JIG session for the counters.
    CHECK(gw.queryOptions.size() == 2);
    if (gw.queryOptions.size() == 2)
    {
        CHECK(!gw.queryOptions[0].jigMode);
        CHECK(gw.queryOptions[1].jigMode);
        // Every byte the reset touches, as for any model: 0x214-0x219.
        CHECK(gw.queries[1].size() == 6);
        for (const auto& packet : gw.queries[1])
            CHECK(ewr::jig::ReadPacketAddress(packet) >= 0x214);
    }

    // The bytes WIC read, as counter values: 480 of 7300 and 37888 of 38900.
    const std::vector<std::pair<uint16_t, int>> expected = {
        { 0x214, 0xe0 }, { 0x215, 0x01 }, { 0x216, 0x00 }, { 0x217, 0x94 }, { 0x218, 0x00 }, { 0x219, 0x00 } };
    CHECK(state.values == expected);

    const nlohmann::json data = ewr::json::StateData(model, state);
    CHECK(data["pads"].size() == 2);
    if (data["pads"].size() == 2)
    {
        CHECK(data["pads"][0]["used"] == 480);
        CHECK(data["pads"][0]["max"] == 7300);
        CHECK(data["pads"][1]["used"] == 37888);
        CHECK(data["pads"][1]["max"] == 38900);
    }

    CHECK(gw.resets == 0);
}

// No JIG write command is known, so a reset must not get as far as reading -
// let alone hand a keyless model to the keyed write path.
void test_session_refuses_to_reset_a_jig_model()
{
    std::cout << "[TEST] test_session_refuses_to_reset_a_jig_model" << std::endl;

    const ewr::DbPrinterModel model = MakeJigModel();
    CHECK(model.HasResettableCounters());
    CHECK(!model.CanReset());

    JigGateway gw;
    ewr::Session session(model, gw, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());

    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; };
    handlers.confirmWrite = [](const ewr::StateSnapshot&) { return true; };

    const ewr::ResetOutcome outcome = session.Reset(handlers);
    CHECK(outcome.phase == ewr::ResetPhase::NotStarted);
    CHECK(!outcome.error.empty());
    CHECK(gw.resets == 0);
    CHECK(gw.queryOptions.empty());

    const ewr::ResetOutcome ink = session.ResetInk(handlers);
    CHECK(ink.phase == ewr::ResetPhase::NotStarted);
    CHECK(gw.resets == 0);

    // An ordinary model is unaffected.
    CHECK(MakeSessionModel().CanReset());
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
    const std::vector<ewr::keysearch::Candidate> got = ewr::keysearch::CollectCandidates(
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

    // The real database: every keyed model lands in exactly one candidate,
    // and no JIG model in any.
    ewr::Database gen;
    CHECK(gen.Load("database.json"));
    const auto models = gen.Models();
    const auto real = ewr::keysearch::CollectCandidates(models);

    const size_t keyed = static_cast<size_t>(std::count_if(models.begin(), models.end(),
        [](const ewr::DbPrinterModel& m) { return !m.jig; }));
    CHECK(keyed < models.size());

    size_t covered = 0;
    std::map<std::pair<uint16_t, uint8_t>, int> seen;
    for (const auto& candidate : real)
    {
        covered += candidate.models.size();
        seen[{ candidate.rkey, candidate.addressLength }]++;
        CHECK(candidate.addressLength == 1 || candidate.addressLength == 2);
    }
    CHECK(covered == keyed);
    CHECK(seen.size() == real.size());
    CHECK(real.size() > 50);
    for (size_t i = 1; i < real.size(); ++i)
        CHECK(real[i - 1].models.size() >= real[i].models.size());
}

void test_search_read_key()
{
    std::cout << "[TEST] test_search_read_key" << std::endl;

    auto text = [](const std::string& s) { return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end())); };
    const std::vector<unsigned char> refused = text("||:41:NA;\f");
    const std::vector<unsigned char> empty = text("||:;\f");

    // 40 keys: three batches when nothing goes wrong.
    std::vector<ewr::keysearch::Candidate> candidates;
    for (uint16_t rkey = 100; rkey < 140; ++rkey)
        candidates.push_back({ rkey, 2, { "Model " + std::to_string(rkey) } });

    // One key fits.
    {
        KeyGateway gw;
        gw.answer = [&](uint16_t rkey, int) { return rkey == 123 ? MakeEepromReadReplyEE16(0, 0x5E) : refused; };

        size_t lastTried = 0, lastTotal = 0;
        ewr::ExecutorOptions options = ewr::ExecutorOptions::ForQueries();
        options.interfaceCandidate = 2;
        options.trace.selectedBy = "--find-key";

        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates, options,
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

        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates);
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

        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates);
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
        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates, ewr::ExecutorOptions::ForQueries(),
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
        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates);
        CHECK(!search.deviceFound);
        CHECK(!search.completed);
        CHECK(search.probes.empty());
        CHECK(!search.error.empty());
    }
    {
        KeyGateway gw;
        gw.handshakeFailed = true;
        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, candidates);
        CHECK(search.deviceFound);
        CHECK(!search.completed);
        CHECK(search.probes.empty());
        CHECK(search.error == "no handshake");
        CHECK(gw.sessions == 1);
    }

    // Nothing to try is a finished search, with no device session opened.
    {
        KeyGateway gw;
        const ewr::keysearch::Result search = ewr::keysearch::Search(gw, {});
        CHECK(search.completed);
        CHECK(gw.sessions == 0);
    }
}

void test_evaluate_ink_blocker()
{
    std::cout << "[TEST] test_evaluate_ink_blocker" << std::endl;

    ewr::PrinterStatus status;

    // No parse / no error: never a blocker, exactly like the waste policy.
    CHECK(!ewr::gates::EvaluateInk(status).has_value());
    status.valid = true;
    CHECK(!ewr::gates::EvaluateInk(status).has_value());

    // INK OUT gates the waste reset but must never gate the ink reset:
    // an empty cartridge is the state the ink reset exists to clear.
    status.hasError = true;
    status.errorCode = 0x05;
    status.errorName = "INK OUT";
    CHECK(ewr::gates::Evaluate(status).has_value());
    CHECK(!ewr::gates::EvaluateInk(status).has_value());

    // The waste-pad errors stay expected for both flavors.
    status.errorCode = 0x10; // SERVICE REQUEST
    CHECK(!ewr::gates::EvaluateInk(status).has_value());
    status.errorCode = 0x2C; // CARTRIDGE OVERFLOW
    CHECK(!ewr::gates::EvaluateInk(status).has_value());

    // A genuinely foreign lock still blocks the ink reset too.
    status.errorCode = 0x06;
    status.errorName = "PAPER JAM";
    const std::optional<ewr::Blocker> blocker = ewr::gates::EvaluateInk(status);
    CHECK(blocker.has_value());
    if (blocker.has_value())
        CHECK(blocker->errorName == "PAPER JAM");
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
            writePackets += ewr::ctrl::IsWrite(packet) ? 1 : 0;
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

void test_session_read_addresses()
{
    std::cout << "[TEST] test_session_read_addresses" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    FakeGateway gw = MakeSessionGateway(); // status + EEPROM reads at 0x0C, 0x0D

    ewr::Session session(model, gw, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());

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

    ewr::Session session(model, silent, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());
    const ewr::StateSnapshot snap = session.ReadState();
    CHECK(!snap.available);
    CHECK(!snap.status.valid);

    // The model-free read is gated the same way.
    FakeGateway silentStatus = silent;
    CHECK(!ewr::status::Read(silentStatus, ewr::ExecutorOptions::ForQueries()).available);

    // One answered query is enough: a printer that answers the counter reads
    // but not the status query is still readable.
    FakeGateway partial = MakeSessionGateway();
    partial.queryResult.query.replies[0].clear();
    ewr::Session partialSession(model, partial, ewr::log::Default(), ewr::ExecutorOptions::ForQueries());
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

    const std::optional<ewr::Blocker> waste = ewr::gates::Evaluate(busy);
    CHECK(waste.has_value());
    if (waste.has_value())
    {
        CHECK(waste->errorCode == -1);
        CHECK(waste->errorName == "PRINTER BUSY");
        CHECK(!waste->explanation.empty());
    }
    CHECK(ewr::gates::EvaluateInk(busy).has_value());

    // Idle with no error stays clean - the gate is BUSY-specific.
    ewr::PrinterStatus idle = busy;
    idle.stateCode = 0x04;
    idle.stateName = "IDLE";
    CHECK(!ewr::gates::Evaluate(idle).has_value());
    CHECK(!ewr::gates::EvaluateInk(idle).has_value());

    // A BUSY report that DOES carry an error keeps the specific policy:
    // expected states pass, and INK OUT still splits the two paths.
    ewr::PrinterStatus busyService = busy;
    busyService.hasError = true;
    busyService.errorCode = 0x10; // SERVICE REQUEST
    busyService.errorName = "SERVICE REQUEST";
    CHECK(!ewr::gates::Evaluate(busyService).has_value());

    ewr::PrinterStatus busyInkOut = busy;
    busyInkOut.hasError = true;
    busyInkOut.errorCode = 0x05; // INK OUT
    busyInkOut.errorName = "INK OUT";
    CHECK(ewr::gates::Evaluate(busyInkOut).has_value());
    CHECK(!ewr::gates::EvaluateInk(busyInkOut).has_value());
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

    const std::optional<ewr::Blocker> waste = ewr::gates::Evaluate(partial);
    CHECK(waste.has_value());
    if (waste.has_value())
    {
        CHECK(waste->errorCode == -1);
        CHECK(waste->errorName == "INCOMPLETE STATUS REPORT");
        CHECK(!waste->explanation.empty());
    }
    CHECK(ewr::gates::EvaluateInk(partial).has_value());

    // A truncated report that still carried its error entry is classified on
    // the error, not on the truncation: the waste-pad errors stay expected.
    ewr::PrinterStatus partialService = partial;
    partialService.hasError = true;
    partialService.errorCode = 0x10; // SERVICE REQUEST
    partialService.errorName = "SERVICE REQUEST";
    CHECK(!ewr::gates::Evaluate(partialService).has_value());

    ewr::PrinterStatus partialJam = partial;
    partialJam.hasError = true;
    partialJam.errorCode = 0x04; // PAPER JAM
    partialJam.errorName = "PAPER JAM";
    const std::optional<ewr::Blocker> jam = ewr::gates::Evaluate(partialJam);
    CHECK(jam.has_value());
    if (jam.has_value())
        CHECK(jam->errorName == "PAPER JAM");

    // A complete report is untouched by the new gate.
    ewr::PrinterStatus complete = partial;
    complete.truncated = false;
    CHECK(!ewr::gates::Evaluate(complete).has_value());
    CHECK(!ewr::gates::EvaluateInk(complete).has_value());
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

void test_session_range_reads()
{
    std::cout << "[TEST] test_session_range_reads" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel(); // 1-byte addresses, mem_high 0x7FF
    ewr::log::Reporter reporter;

    std::vector<uint16_t> page;
    for (uint16_t a = 0; a <= 0xFF; ++a)
        page.push_back(a);

    auto fill = [](EepromGateway& gw) {
        for (uint16_t a = 0; a <= 0xFF; ++a)
            gw.eeprom[a] = static_cast<uint8_t>((a * 7 + 3) & 0xFF);
    };

    auto matches = [](const EepromGateway& gw, const ewr::StateSnapshot& state, size_t count) {
        if (!state.available || state.values.size() != count)
            return false;
        for (const auto& entry : state.values)
        {
            const auto it = gw.eeprom.find(entry.first);
            if (it == gw.eeprom.end() || entry.second != it->second)
                return false;
        }
        return true;
    };

    // Answered: a probe of 8 bytes both ways, then 8 requests of 32.
    {
        EepromGateway gw;
        fill(gw);
        ewr::Session session(model, gw, reporter);

        const ewr::StateSnapshot state = session.ReadAddresses(page);
        CHECK(matches(gw, state, 256));
        CHECK(session.RangeReadSupport() == ewr::Session::RangeReads::Work);
        CHECK(gw.singlePackets == 8);
        CHECK(gw.rangePackets == 1 + 8);
        CHECK(gw.sessions == 3);

        // Settled: the next dump sends no probe.
        gw.sessions = 0;
        CHECK(matches(gw, session.ReadAddresses(page), 256));
        CHECK(gw.sessions == 1);
    }

    // Refused: byte by byte, the probe's single reads included.
    {
        EepromGateway gw;
        fill(gw);
        gw.ranges = EepromGateway::Ranges::Refuse;
        ewr::Session session(model, gw, reporter);

        CHECK(matches(gw, session.ReadAddresses(page), 256));
        CHECK(session.RangeReadSupport() == ewr::Session::RangeReads::Refused);
        CHECK(gw.rangePackets == 1);

        gw.rangePackets = 0;
        session.ReadAddresses(page);
        CHECK(gw.rangePackets == 0);
    }

    // Answered with other values than single reads give: never trusted.
    {
        EepromGateway gw;
        fill(gw);
        gw.ranges = EepromGateway::Ranges::Lie;
        ewr::Session session(model, gw, reporter);

        CHECK(matches(gw, session.ReadAddresses(page), 256));
        CHECK(session.RangeReadSupport() == ewr::Session::RangeReads::Refused);
    }

    // A short list is not worth a probe.
    {
        EepromGateway gw;
        fill(gw);
        ewr::Session session(model, gw, reporter);
        const std::vector<uint16_t> few = { 0x0C, 0x0D, 0x2B };

        CHECK(matches(gw, session.ReadAddresses(few), 3));
        CHECK(gw.rangePackets == 0);
        CHECK(session.RangeReadSupport() == ewr::Session::RangeReads::Unknown);
    }

    // Gaps split the runs; the caller's order is kept.
    {
        EepromGateway gw;
        fill(gw);
        ewr::Session session(model, gw, reporter);

        std::vector<uint16_t> gappy;
        for (uint16_t a = 0x10; a < 0x30; ++a)
            gappy.push_back(a);
        for (uint16_t a = 0x80; a < 0x88; ++a)
            gappy.push_back(a);

        const ewr::StateSnapshot state = session.ReadAddresses(gappy);
        CHECK(matches(gw, state, gappy.size()));
        bool ordered = true;
        for (size_t i = 0; i < gappy.size(); ++i)
            ordered = ordered && state.values[i].first == gappy[i];
        CHECK(ordered);
        CHECK(gw.rangePackets == 1 + 2);
    }
}
