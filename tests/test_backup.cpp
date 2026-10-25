#include "support.h"

void test_backup_json_round_trip()
{
    std::cout << "[TEST] test_backup_json_round_trip" << std::endl;

    const ewr::Backup original = MakeTestBackup();
    CHECK(original.Answered() == 2);
    CHECK(!original.Complete());

    const std::string text = ewr::backup::ToJson(original);
    const nlohmann::json root = nlohmann::json::parse(text);
    CHECK(root["format"] == "ewr-backup");
    CHECK(root["version"] == 1);
    CHECK(root["complete"] == false);
    CHECK(root["printer"]["address"].is_null()); // USB: no host
    CHECK(root["bytes"][2]["value"].is_null());   // unread stays null, never -1

    ewr::Backup parsed;
    std::string error;
    CHECK(ewr::backup::Parse(text, parsed, error));
    CHECK(error.empty());
    CHECK(parsed.source == "backup");
    CHECK(parsed.model == original.model);
    CHECK(parsed.target == "waste");
    CHECK(parsed.created == original.created);
    CHECK(parsed.serial == original.serial);
    CHECK(parsed.deviceId == original.deviceId);
    CHECK(parsed.transport == "usb");
    CHECK(parsed.host.empty());
    CHECK(parsed.addressLength == 1);
    CHECK(parsed.bytes == original.bytes);
}

// Both dump formats read back as a backup: v1 (1.5 and earlier) names only
// the model, v2 also the printer.
void test_backup_parses_dumps()
{
    std::cout << "[TEST] test_backup_parses_dumps" << std::endl;

    const std::string v1 =
        "# EWR EEPROM dump\n"
        "# model: R220\n"
        "# answered: 2 of 3\n"
        "# address,value,note ('--' = no reply: unknown)\n"
        "0x00,0x4F,\n"
        "0x0C,0x12,Main Pad Counter\n"
        "0x0D,--,Main Pad Counter\n";

    ewr::Backup dump;
    std::string error;
    CHECK(ewr::backup::Parse(v1, dump, error));
    CHECK(dump.source == "dump");
    CHECK(dump.dumpFormat == 1);
    CHECK(dump.model == "R220");
    CHECK(dump.serial.empty());
    CHECK(dump.addressLength == 0);
    CHECK(dump.bytes.size() == 3);
    CHECK((dump.bytes[1] == std::pair<uint16_t, int>(0x0C, 0x12)));
    CHECK(dump.bytes[2].second == -1);

    // Windows line endings and an address above 0xFF, as v2 writes them.
    const std::string v2 =
        "# EWR EEPROM dump\r\n"
        "# format: 2\r\n"
        "# model: ET-2800\r\n"
        "# ewr: 1.6.0\r\n"
        "# serial: X5NA012345\r\n"
        "# address_length: 2\r\n"
        "# address,value,note ('--' = no reply: unknown)\r\n"
        "0x2F,0x00,Main Pad Counter\r\n"
        "0x100,0x01,\r\n";

    CHECK(ewr::backup::Parse(v2, dump, error));
    CHECK(dump.dumpFormat == 2);
    CHECK(dump.model == "ET-2800");
    CHECK(dump.serial == "X5NA012345");
    CHECK(dump.addressLength == 2);
    CHECK(dump.bytes.size() == 2);
    CHECK((dump.bytes[1] == std::pair<uint16_t, int>(0x100, 0x01)));
}

void test_backup_refuses_what_it_cannot_trust()
{
    std::cout << "[TEST] test_backup_refuses_what_it_cannot_trust" << std::endl;

    ewr::Backup out;
    std::string error;

    CHECK(!ewr::backup::Parse("", out, error));
    CHECK(!ewr::backup::Parse("hello", out, error));
    CHECK(!ewr::backup::Parse("{\"format\": \"something-else\", \"version\": 1}", out, error));

    // Written by a newer format: refused rather than half understood.
    nlohmann::json future = nlohmann::json::parse(ewr::backup::ToJson(MakeTestBackup()));
    future["version"] = 2;
    error.clear();
    CHECK(!ewr::backup::Parse(future.dump(), out, error));
    CHECK(!error.empty());

    // A value that is not a byte.
    nlohmann::json wide = nlohmann::json::parse(ewr::backup::ToJson(MakeTestBackup()));
    wide["bytes"][0]["value"] = 300;
    CHECK(!ewr::backup::Parse(wide.dump(), out, error));

    // No model: nothing to check the printer against.
    nlohmann::json anonymous = nlohmann::json::parse(ewr::backup::ToJson(MakeTestBackup()));
    anonymous["model"] = "";
    CHECK(!ewr::backup::Parse(anonymous.dump(), out, error));

    CHECK(!ewr::backup::Parse("# EWR EEPROM dump\n0x00,0x01,\n", out, error)); // no model line
    CHECK(!ewr::backup::Parse("# EWR EEPROM dump\n# model: R220\n0x00,0x1FF,\n", out, error));
    CHECK(!ewr::backup::Parse("# EWR EEPROM dump\n# model: R220\nnot a line\n", out, error));
}

void test_backup_file_is_written_whole_and_never_overwrites()
{
    std::cout << "[TEST] test_backup_file_is_written_whole_and_never_overwrites" << std::endl;

    const fs::path dir = fs::temp_directory_path() / "ewr_backup_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    const ewr::Backup backup = MakeTestBackup();
    CHECK(ewr::backup::FileName(backup) == "TestJet_100_X7A9000123_20261025T093000Z.json");

    std::string first, second, error;
    CHECK(ewr::backup::WriteFile(backup, (dir / "nested").string(), first, error));
    CHECK(ewr::backup::WriteFile(backup, (dir / "nested").string(), second, error));
    CHECK(first != second);
    CHECK(fs::exists(first) && fs::exists(second));

    // No temporary file left beside them.
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir / "nested"))
    {
        files++;
        CHECK(entry.path().extension() == ".json");
    }
    CHECK(files == 2);

    {
        std::ifstream in(first);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ewr::Backup reread;
        CHECK(ewr::backup::Parse(text, reread, error));
        CHECK(reread.bytes == backup.bytes);
    }

    fs::remove_all(dir, ec);
}

// The backup is kept after the last gate and before the first write, from
// the preflight's own reading.
void test_session_backup_comes_before_the_first_write()
{
    std::cout << "[TEST] test_session_backup_comes_before_the_first_write" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.pad_groups[0].addresses = { 0x0C, 0x0D, 0x0C }; // a repeat is backed up once
    model.pad_groups[0].reset_values = { 0x00, 0x00, 0x00 };
    ewr::log::Reporter reporter;

    FakeGateway gw = MakeSessionGateway();
    gw.queryResult.query.replies = {
        MakeSt2Reply(),
        MakeEepromReadReplyEE(0x0C, 0x12),
        MakeEepromReadReplyEE(0x0D, 0x34),
    };
    ewr::Session session(model, gw, reporter);

    int saves = 0;
    bool confirmed = false;
    ewr::ResetHandlers handlers;
    handlers.onBlocker = [](const ewr::Blocker&) { return true; }; // INK OUT
    handlers.confirmWrite = [&](const ewr::StateSnapshot&) { confirmed = true; return true; };
    handlers.saveBackup = [&](const ewr::Backup& backup, std::string& location)
    {
        saves++;
        CHECK(confirmed);           // after the last gate
        CHECK(gw.resetCalls == 0);  // before the first write
        CHECK(backup.model == "TestJet 100");
        CHECK(backup.target == "waste");
        CHECK(backup.serial == "X7A9000123");
        CHECK(backup.addressLength == 1);
        CHECK(backup.transport == "usb");
        CHECK(!backup.created.empty());
        CHECK(backup.bytes.size() == 2);
        CHECK((backup.bytes[0] == std::pair<uint16_t, int>(0x0C, 0x12)));
        CHECK((backup.bytes[1] == std::pair<uint16_t, int>(0x0D, 0x34)));
        location = "backups/test.json";
        return true;
    };

    const ewr::ResetOutcome out = session.Reset(handlers);
    CHECK(saves == 1);
    CHECK(gw.resetCalls == 1);
    CHECK(out.backupTaken);
    CHECK(out.backupLocation == "backups/test.json");

    const nlohmann::json data = ewr::json::ResetData(model, false, out);
    CHECK(data["backup"]["file"] == "backups/test.json");
    CHECK(data["backup"]["bytes"] == 2);
    CHECK(data["backup"]["complete"] == true);
    CHECK(data["backup"]["values"].size() == 2);

    // Declined at the confirmation: no backup file is asked for.
    FakeGateway declinedGw = MakeSessionGateway();
    ewr::Session declinedSession(model, declinedGw, reporter);
    int declinedSaves = 0;
    ewr::ResetHandlers declined;
    declined.onBlocker = [](const ewr::Blocker&) { return true; };
    declined.confirmWrite = [](const ewr::StateSnapshot&) { return false; };
    declined.saveBackup = [&](const ewr::Backup&, std::string&) { declinedSaves++; return true; };
    declinedSession.Reset(declined);
    CHECK(declinedSaves == 0);
}

// A backup that cannot be kept is a gate: no answer means no write, a yes is
// recorded as an override.
void test_session_backup_failure_gates_the_write()
{
    std::cout << "[TEST] test_session_backup_failure_gates_the_write" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter;

    // Not saved, and the host does not go on without it.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        std::vector<std::string> asked;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b) { asked.push_back(b.errorName); return b.errorName == "INK OUT"; };
        handlers.saveBackup = [](const ewr::Backup&, std::string&) { return false; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(gw.resetCalls == 0);
        CHECK(asked.size() == 2 && asked[1] == "BACKUP NOT SAVED");
        CHECK(out.backupTaken);
        CHECK(out.backupLocation.empty());
    }

    // Not saved, and the host goes on: the run says so.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        handlers.saveBackup = [](const ewr::Backup&, std::string&) { return false; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(gw.resetCalls == 1);
        CHECK(out.overrides.size() == 2);
        CHECK(out.overrides.back().gate == "backup");
        CHECK(out.overrides.back().detail == "not_saved");
    }

    // One counter byte unanswered: the backup could not undo it.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.queryResult.query.replies = { MakeSt2Reply(), MakeEepromReadReplyEE(0x0C, 0x00), {} };
        ewr::Session session(model, gw, reporter);

        std::vector<std::string> asked;
        int saves = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b) { asked.push_back(b.errorName); return b.errorName == "INK OUT"; };
        handlers.saveBackup = [&](const ewr::Backup&, std::string&) { saves++; return true; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(gw.resetCalls == 0);
        CHECK(saves == 0);
        CHECK(asked.size() == 2 && asked[1] == "BACKUP INCOMPLETE");
        CHECK(!out.backup.Complete());
    }

    // Without a saveBackup hook (ABI 2 embedders) nothing is gated, and the
    // bytes still come back in the result.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };

        const ewr::ResetOutcome out = session.Reset(handlers);
        CHECK(gw.resetCalls == 1);
        CHECK(out.backupTaken);
        CHECK(out.backup.bytes.size() == 2);
        CHECK(out.overrides.size() == 1); // INK OUT only
    }
}

// No preflight, no values to keep: the reset goes ahead as before, and the
// result says there is no backup rather than an empty one.
void test_session_no_backup_without_a_preflight()
{
    std::cout << "[TEST] test_session_no_backup_without_a_preflight" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter;

    FakeGateway gw = MakeSessionGateway();
    gw.queryResult.query.replies = { {}, {}, {} };
    ewr::Session session(model, gw, reporter);

    int saves = 0;
    ewr::ResetHandlers handlers;
    handlers.saveBackup = [&](const ewr::Backup&, std::string&) { saves++; return true; };

    const ewr::ResetOutcome out = session.Reset(handlers);
    CHECK(saves == 0);
    CHECK(gw.resetCalls == 1);
    CHECK(!out.backupTaken);
    CHECK(ewr::json::ResetData(model, false, out)["backup"].is_null());
}

// Only the bytes the database writes for the model: never more of a write
// than a reset of it.
void test_restore_plan_keeps_only_mapped_bytes()
{
    std::cout << "[TEST] test_restore_plan_keeps_only_mapped_bytes" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel(); // 1-byte, waste 0x0C/0x0D
    ewr::PadGroup platen;
    platen.addresses = { 0x2B, 0x150 };
    platen.reset_values = { 0x00, 0x00 };
    model.pad_groups.push_back(platen);
    ewr::InkGroup black;
    black.color = "black";
    black.addresses = { 0x84 };
    black.reset_values = { 0x00 };
    model.ink_groups.push_back(black);

    ewr::Backup dump;
    dump.source = "dump";
    dump.model = model.name;
    dump.bytes = {
        { 0x00, 0x4F },  // serial, timers, ...: not this model's to write
        { 0x0C, 0x12 },
        { 0x0D, -1 },    // never answered
        { 0x84, 0x30 },  // cartridge ink
        { 0x150, 0x01 }, // mapped, but a 1-byte address field cannot name it
        { 0x0C, 0x99 },  // a repeat: the first one counts
    };

    const ewr::RestorePlan plan = ewr::restore::Plan(model, dump);
    CHECK(plan.model == model.name);
    CHECK(plan.writes.size() == 2);
    CHECK((plan.writes[0] == std::pair<uint16_t, uint8_t>(0x0C, 0x12)));
    CHECK((plan.writes[1] == std::pair<uint16_t, uint8_t>(0x84, 0x30)));
    CHECK(plan.touchesWaste && plan.touchesInk);

    auto reasonFor = [&](uint16_t address) -> std::string {
        for (const auto& skip : plan.skipped)
        {
            if (skip.address == address)
                return skip.reason;
        }
        return "";
    };
    CHECK(reasonFor(0x00) == "unmapped");
    CHECK(reasonFor(0x0D) == "unread");
    CHECK(reasonFor(0x150) == "unencodable");
    CHECK(plan.skipped.size() == 3);

    // The dump never held 0x2B.
    CHECK(plan.missing.size() == 1 && plan.missing[0] == 0x2B);

    // Beyond mem_high is not EEPROM at all.
    model.mem_high = 0xFF;
    model.wlen = 2;
    CHECK(ewr::restore::Plan(model, dump).skipped.size() == 3);
    bool above = false;
    for (const auto& skip : ewr::restore::Plan(model, dump).skipped)
        above = above || (skip.address == 0x150 && skip.reason == "above_limit");
    CHECK(above);

    // A waste backup is not missing the ink bytes it never covered.
    ewr::Backup backup;
    backup.source = "backup";
    backup.target = "waste";
    backup.model = model.name;
    backup.bytes = { { 0x0C, 0x12 }, { 0x0D, 0x34 }, { 0x2B, 0x00 }, { 0x150, 0x00 } };
    const ewr::RestorePlan fromBackup = ewr::restore::Plan(model, backup);
    CHECK(fromBackup.missing.empty());
    CHECK(fromBackup.touchesWaste && !fromBackup.touchesInk);
}

// Bytes that already hold the file's value are left alone, and what is
// written is backed up first under target "restore".
void test_session_restore_writes_only_what_differs()
{
    std::cout << "[TEST] test_session_restore_writes_only_what_differs" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter;

    FakeGateway gw = MakeSessionGateway(); // 0x0C = 0x00, 0x0D = 0x00, INK OUT
    gw.queryResult.query.replies = { MakeSt2Reply(), MakeEepromReadReplyEE(0x0C, 0x00),
                                     MakeEepromReadReplyEE(0x0D, 0x34) };

    ewr::RestorePlan plan;
    plan.model = model.name;
    plan.serial = "X7A9000123"; // the printer's own
    plan.writes = { { 0x0C, 0x12 }, { 0x0D, 0x34 } };
    plan.touchesWaste = true;

    ewr::Session session(model, gw, reporter);

    std::vector<std::string> asked;
    ewr::Backup kept;
    ewr::ResetHandlers handlers;
    handlers.onBlocker = [&](const ewr::Blocker& b) { asked.push_back(b.errorName); return true; };
    handlers.saveBackup = [&](const ewr::Backup& backup, std::string& location)
    {
        kept = backup;
        location = "backups/restore.json";
        return true;
    };

    const ewr::ResetOutcome out = session.Restore(plan, handlers);
    CHECK(gw.resetCalls == 1);
    CHECK(asked.size() == 1 && asked[0] == "INK OUT"); // same serial: no unit gate
    CHECK(out.unchanged.size() == 1 && out.unchanged[0] == 0x0D);
    CHECK(CountWritePackets(gw.lastSequence) == 1);
    CHECK(SequenceHas(gw.lastSequence, ewr::ctrl::WritePacket(
                                           model.rkey, 0x0C, 0x12, model.wkey, model.WriteAddressLength())));
    CHECK(kept.target == "restore");
    CHECK(kept.bytes.size() == 1);
    CHECK((kept.bytes[0] == std::pair<uint16_t, int>(0x0C, 0x00)));
    // The fake read-back still says 0x00: the verdict compares against the file.
    CHECK(out.verificationRan);
    CHECK(out.verifyMismatches == 1);
}

void test_session_restore_gates()
{
    std::cout << "[TEST] test_session_restore_gates" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter;

    ewr::RestorePlan plan;
    plan.model = model.name;
    plan.serial = "Q1Q1000001"; // not the printer's X7A9000123
    plan.writes = { { 0x0C, 0x12 } };
    plan.touchesWaste = true;

    // Another printer's file: declined, nothing written.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);
        std::vector<std::string> asked;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b) { asked.push_back(b.errorName); return b.errorName == "INK OUT"; };

        const ewr::ResetOutcome out = session.Restore(plan, handlers);
        CHECK(out.phase == ewr::ResetPhase::Aborted);
        CHECK(gw.resetCalls == 0);
        CHECK(asked.size() == 2 && asked[1] == "DIFFERENT PRINTER");
    }

    // ... and overruled: on record as unit_mismatch.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };

        const ewr::ResetOutcome out = session.Restore(plan, handlers);
        CHECK(gw.resetCalls == 1);
        bool recorded = false;
        for (const auto& o : out.overrides)
            recorded = recorded || (o.gate == "unit_mismatch" && o.detail == "Q1Q1000001");
        CHECK(recorded);
    }

    // Every byte already holds its value: success, and no write at all.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);
        ewr::RestorePlan same = plan;
        same.serial.clear();
        same.writes = { { 0x0C, 0x00 } };

        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        int confirms = 0;
        handlers.confirmWrite = [&](const ewr::StateSnapshot&) { confirms++; return true; };

        const ewr::ResetOutcome out = session.Restore(same, handlers);
        CHECK(out.success);
        CHECK(out.phase == ewr::ResetPhase::Done);
        CHECK(gw.resetCalls == 0);
        CHECK(confirms == 0);
        CHECK(out.writesTotal == 0);
        CHECK(out.unchanged.size() == 1);
    }

    // A restore never runs blind: it has to know what it would change.
    {
        FakeGateway gw = MakeSessionGateway();
        gw.queryResult.query.replies = { {}, {} };
        ewr::Session session(model, gw, reporter);
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };

        const ewr::ResetOutcome out = session.Restore(plan, handlers);
        CHECK(out.phase == ewr::ResetPhase::NotStarted);
        CHECK(gw.resetCalls == 0);
    }

    // A plan for another entry, or an empty one, is refused before any I/O.
    {
        FakeGateway gw = MakeSessionGateway();
        ewr::Session session(model, gw, reporter);

        ewr::RestorePlan other = plan;
        other.model = "Some Other Model";
        CHECK(!session.Restore(other).success);

        ewr::RestorePlan empty = plan;
        empty.writes.clear();
        CHECK(!session.Restore(empty).success);

        CHECK(gw.queryCalls == 0 && gw.resetCalls == 0);
    }
}

void test_dump_addresses_reach_the_mapped_bytes()
{
    std::cout << "[TEST] test_dump_addresses_reach_the_mapped_bytes" << std::endl;

    ewr::DbPrinterModel wide;
    wide.name = "Wide";
    wide.rlen = 2;
    wide.wlen = 2;
    wide.mem_high = 0x7FF;
    ewr::PadGroup group;
    group.addresses = { 0x2F, 0x1A0 };
    group.reset_values = { 0, 0 };
    wide.pad_groups.push_back(group);
    wide.close_ops.push_back({ 0x100, 0xFE, 0x00 });

    const std::vector<uint16_t> classic = ewr::restore::DumpAddresses(wide);
    CHECK(classic.size() == 256 + 2);
    CHECK(classic[255] == 0xFF && classic[256] == 0x100 && classic[257] == 0x1A0);
    CHECK(ewr::restore::DumpAddresses(wide, true).size() == 0x800);

    // A 1-byte read address cannot name 0x1A0: the read would land on 0xA0.
    ewr::DbPrinterModel narrow = wide;
    narrow.rlen = 1;
    CHECK(ewr::restore::DumpAddresses(narrow).size() == 256);
    CHECK(ewr::restore::DumpAddresses(narrow, true).size() == 256);

    // A small part reads only what it has.
    ewr::DbPrinterModel tiny = wide;
    tiny.mem_high = 0x7F;
    CHECK(ewr::restore::DumpAddresses(tiny).size() == 0x80);
}

// --restore-all: every byte of a dump but the unit's own, on that unit only.
void test_restore_all_plan_never_writes_identity()
{
    std::cout << "[TEST] test_restore_all_plan_never_writes_identity" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel(); // 1-byte, waste 0x0C/0x0D
    model.identity_addresses = { 0x42, 0x43, 0x44, 0x45 };
    model.serial_addresses = { 0x60, 0x61 };

    ewr::Backup dump;
    dump.source = "dump";
    dump.model = model.name;
    dump.bytes = { { 0x07, 0xE0 }, { 0x0C, 0x12 }, { 0x42, 0x01 }, { 0x43, 0x02 }, { 0x44, 0x03 },
                   { 0x45, 0x04 }, { 0x60, 'X' }, { 0x61, -1 }, { 0x70, -1 }, { 0x150, 0x01 } };

    const ewr::RestorePlan plan = ewr::restore::PlanAll(model, dump);
    CHECK(plan.requireSameUnit);
    CHECK(plan.writes.size() == 2); // 0x07 (unmapped) and 0x0C
    CHECK((plan.writes[0] == std::pair<uint16_t, uint8_t>(0x07, 0xE0)));
    CHECK(plan.touchesWaste && !plan.touchesInk);
    CHECK(plan.unitProof.size() == 5); // four head-ID bytes and the one serial byte that answered

    size_t identity = 0;
    for (const auto& skip : plan.skipped)
        identity += skip.reason == "identity" ? 1 : 0;
    CHECK(identity == 6);

    for (const auto& w : plan.writes)
        CHECK(w.first != 0x42 && w.first != 0x60 && w.first != 0x150);
}

void test_session_restore_all_runs_only_on_the_same_unit()
{
    std::cout << "[TEST] test_session_restore_all_runs_only_on_the_same_unit" << std::endl;

    const ewr::DbPrinterModel model = MakeSessionModel();
    ewr::log::Reporter reporter;

    ewr::RestorePlan plan;
    plan.model = model.name;
    plan.requireSameUnit = true;
    plan.writes = { { 0x0C, 0x12 } };
    plan.touchesWaste = true;
    plan.unitProof = { { 0x42, 0x01 }, { 0x43, 0x02 }, { 0x44, 0x03 }, { 0x45, 0x04 } };

    auto gatewayWith = [](uint8_t at45) {
        EepromGateway gw;
        gw.eeprom = { { 0x0C, 0x00 }, { 0x0D, 0x00 }, { 0x42, 0x01 }, { 0x43, 0x02 }, { 0x44, 0x03 }, { 0x45, at45 } };
        return gw;
    };

    // Every proof byte matches: it runs.
    {
        EepromGateway gw = gatewayWith(0x04);
        ewr::Session session(model, gw, reporter);
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        session.Restore(plan, handlers);
        CHECK(gw.sessions >= 1);
    }

    // One proof byte differs: nothing is written, whatever the host answers.
    {
        FakeGateway gw;
        gw.queryResult.deviceFound = true;
        gw.queryResult.query.replies = { MakeSt2State(0x04), MakeEepromReadReplyEE(0x0C, 0x00),
                                         MakeEepromReadReplyEE(0x0D, 0x00), MakeEepromReadReplyEE(0x42, 0x01),
                                         MakeEepromReadReplyEE(0x43, 0x02), MakeEepromReadReplyEE(0x44, 0x03),
                                         MakeEepromReadReplyEE(0x45, 0x09) };
        gw.resetResult.deviceFound = true;
        gw.resetResult.exec.success = true;
        ewr::Session session(model, gw, reporter);

        int asked = 0;
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker&) { asked++; return true; };
        const ewr::ResetOutcome out = session.Restore(plan, handlers);
        CHECK(out.phase == ewr::ResetPhase::NotSupported);
        CHECK(gw.resetCalls == 0);
        CHECK(asked == 0);
    }

    // Too little proof: refused the same way.
    {
        FakeGateway gw;
        gw.queryResult.deviceFound = true;
        gw.queryResult.query.replies = { MakeSt2State(0x04), MakeEepromReadReplyEE(0x0C, 0x00),
                                         MakeEepromReadReplyEE(0x0D, 0x00) };
        gw.resetResult.deviceFound = true;
        gw.resetResult.exec.success = true;
        ewr::Session session(model, gw, reporter);

        ewr::RestorePlan thin = plan;
        thin.unitProof.resize(2);
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        CHECK(session.Restore(thin, handlers).phase == ewr::ResetPhase::NotSupported);
        CHECK(gw.resetCalls == 0);
    }

    // Serials both known and different: refused even with every proof byte.
    {
        FakeGateway gw = MakeSessionGateway(); // the printer reports X7A9000123
        ewr::Session session(model, gw, reporter);
        ewr::RestorePlan other = plan;
        other.serial = "Q1Q1000001";
        ewr::ResetHandlers handlers;
        handlers.onBlocker = [](const ewr::Blocker&) { return true; };
        CHECK(session.Restore(other, handlers).phase == ewr::ResetPhase::NotSupported);
        CHECK(gw.resetCalls == 0);
    }
}
