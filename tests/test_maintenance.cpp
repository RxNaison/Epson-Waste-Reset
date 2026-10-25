#include "support.h"

void test_remote_mode_job_bytes()
{
    std::cout << "[TEST] test_remote_mode_job_bytes" << std::endl;

    CHECK((ewr::remote::NozzleCheck() == std::vector<unsigned char>{ 'N', 'C', 0x02, 0x00, 0x00, 0x00 }));
    CHECK((ewr::remote::NozzleCheck(1) == std::vector<unsigned char>{ 'N', 'C', 0x02, 0x00, 0x00, 0x10 }));
    CHECK((ewr::remote::HeadClean() == std::vector<unsigned char>{ 'C', 'H', 0x02, 0x00, 0x00, 0x00 }));
    CHECK((ewr::remote::HeadClean(1, true) == std::vector<unsigned char>{ 'C', 'H', 0x02, 0x00, 0x00, 0x11 }));

    const std::vector<unsigned char> job = ewr::remote::BuildJob({ ewr::remote::NozzleCheck() });
    std::vector<unsigned char> expected = ewr::remote::kLeavePacketMode;
    const std::vector<unsigned char> body = { 0x1B, '@', 0x1B, '(', 'R', 0x08, 0x00, 0x00,
                                              'R', 'E', 'M', 'O', 'T', 'E', '1',
                                              'N', 'C', 0x02, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00 };
    expected.insert(expected.end(), body.begin(), body.end());
    CHECK(job == expected);

    // The packet-mode exit: 27 bytes, five spaces, as the SX110 took it.
    CHECK(ewr::remote::kLeavePacketMode.size() == 27);
    CHECK(ewr::remote::kLeavePacketMode[3] == 0x1B);
}

void test_maintenance_blocker_policy()
{
    std::cout << "[TEST] test_maintenance_blocker_policy" << std::endl;

    using A = ewr::MaintenanceAction;
    ewr::PrinterStatus idle;
    idle.valid = true;
    idle.stateCode = 0x04;
    idle.stateName = "IDLE";
    CHECK(!ewr::gates::EvaluateMaintenance(idle, A::HeadClean).has_value());
    CHECK(!ewr::gates::EvaluateMaintenance(idle, A::NozzleCheck).has_value());

    ewr::PrinterStatus unread;
    CHECK(ewr::gates::EvaluateMaintenance(unread, A::HeadClean).has_value());

    // The very error a reset clears blocks maintenance: the printer refuses it.
    ewr::PrinterStatus full = idle;
    full.hasError = true;
    full.errorCode = 0x10;
    full.errorName = "SERVICE REQUEST";
    const auto pads = ewr::gates::EvaluateMaintenance(full, A::HeadClean);
    CHECK(pads.has_value() && pads->errorCode == 0x10);

    // An empty cartridge: never a cleaning.
    ewr::PrinterStatus empty = full;
    empty.errorCode = 0x05;
    empty.errorName = "INK OUT";
    CHECK(ewr::gates::EvaluateMaintenance(empty, A::HeadClean).has_value());
    CHECK(ewr::gates::EvaluateMaintenance(empty, A::PowerClean).has_value());
    CHECK(ewr::gates::EvaluateMaintenance(empty, A::NozzleCheck).has_value());

    ewr::PrinterStatus busy = idle;
    busy.stateCode = 0x07;
    busy.stateName = "CLEANING";
    const auto working = ewr::gates::EvaluateMaintenance(busy, A::HeadClean);
    CHECK(working.has_value() && working->errorName == "PRINTER BUSY" && working->errorCode == -1);
}

void test_maintenance_routes()
{
    std::cout << "[TEST] test_maintenance_routes" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };
    model.cleaning.charge["normal"] = { 0x07 };

    FakeGateway gw;
    ewr::Maintenance maintenance(gw, &model);
    std::string why;

    ewr::MaintenanceRequest clean;
    CHECK(maintenance.Route(clean, why) == "factory");

    clean.remoteOnly = true;
    CHECK(maintenance.Route(clean, why) == "remote");
    clean.remoteOnly = false;

    // A level the model does not list: the print-data cleaning has none.
    clean.level = "strong";
    CHECK(maintenance.Route(clean, why).empty());
    CHECK(!why.empty());
    clean.level = "normal";

    // Black only: not in this model's table, so the print-data group.
    clean.group = ewr::CleanGroup::Black;
    CHECK(maintenance.Route(clean, why) == "remote");

    ewr::MaintenanceRequest charge;
    charge.action = ewr::MaintenanceAction::PowerClean;
    CHECK(maintenance.Route(charge, why) == "factory");

    // 0xFF is listed where nobody knows what it does: never sent.
    model.cleaning.charge["normal"] = { 0xFF };
    CHECK(maintenance.Route(charge, why) == "remote");

    ewr::MaintenanceRequest nozzle;
    nozzle.action = ewr::MaintenanceAction::NozzleCheck;
    CHECK(maintenance.Route(nozzle, why) == "remote");

    ewr::Maintenance unlisted(gw, nullptr);
    CHECK(unlisted.Route(clean, why) == "remote");
}

void test_maintenance_factory_clean_runs_once_and_is_watched()
{
    std::cout << "[TEST] test_maintenance_factory_clean_runs_once_and_is_watched" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };
    model.pad_groups[0].counters.push_back({ "Main Pad Counter", "main", { { 0x0C, 0xFF, 1 }, { 0x0D, 0xFF, 256 } }, 1000 });

    MaintenanceGateway gw;
    gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x07), MakeSt2State(0x07), MakeSt2State(0x04),
                    MakeSt2State(0x04) };
    gw.eeprom[0x0C] = 100;
    gw.commandReply = MakeCommandReply("||:84:OK;");

    ewr::log::Reporter reporter;
    ewr::Maintenance maintenance(gw, &model, reporter);
    maintenance.pollIntervalMs = 0;

    int progress = 0;
    std::string confirmedRoute;
    ewr::MaintenanceHandlers handlers;
    handlers.confirm = [&](const ewr::StateSnapshot& before, const std::string& route)
    {
        CHECK(before.available && before.values.size() == 2);
        CHECK(gw.commands.empty()); // asked before anything is sent
        confirmedRoute = route;
        return true;
    };
    handlers.onProgress = [&](const ewr::PrinterStatus&, int) { progress++; };

    const ewr::MaintenanceOutcome out = maintenance.Run({}, handlers);
    CHECK(out.phase == ewr::MaintenancePhase::Done);
    CHECK(out.success && out.started && out.sent);
    CHECK(out.route == "factory" && confirmedRoute == "factory");
    CHECK(out.reply == "||:84:OK;");
    CHECK(gw.commands.size() == 1);
    CHECK(gw.commands[0] == ewr::ctrl::FactoryPacket(model.rkey, 0x84, { 0x02 }));
    CHECK(gw.jobs.empty());
    CHECK(progress == 3);
    CHECK(out.after.available); // the pads are read again for the delta
}

void test_maintenance_stops_where_it_should()
{
    std::cout << "[TEST] test_maintenance_stops_where_it_should" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };
    ewr::log::Reporter reporter;

    // Refused: no watching, nothing else sent.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:NA;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::SendFailed);
        CHECK(gw.commands.size() == 1);
        CHECK(gw.nextStatus == 1); // the preflight only
    }

    // No answer at all: sent once, and the status decides.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x02), MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::Done);
        CHECK(gw.commands.size() == 1);
    }

    // The printer objects and nobody overrules it: nothing is sent.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x00, 0x05) };
        ewr::Maintenance maintenance(gw, &model, reporter);
        int asked = 0;
        ewr::MaintenanceHandlers handlers;
        handlers.onBlocker = [&](const ewr::Blocker& b) { asked++; CHECK(b.errorName == "INK OUT"); return false; };

        const ewr::MaintenanceOutcome out = maintenance.Run({}, handlers);
        CHECK(out.phase == ewr::MaintenancePhase::Aborted);
        CHECK(asked == 1);
        CHECK(gw.commands.empty() && gw.jobs.empty());
    }

    // Print data the printer took but never acted on.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;
        maintenance.startWindowSeconds = 0;

        ewr::MaintenanceRequest nozzle;
        nozzle.action = ewr::MaintenanceAction::NozzleCheck;
        const ewr::MaintenanceOutcome out = maintenance.Run(nozzle);
        CHECK(out.phase == ewr::MaintenancePhase::NeverStarted);
        CHECK(out.route == "remote");
        CHECK(gw.jobs.size() == 1 && gw.commands.empty());
        CHECK(gw.jobs[0] == ewr::remote::BuildJob({ ewr::remote::NozzleCheck() }));
    }

    // An error mid-way ends the watch with the printer's own words.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x02), MakeSt2State(0x00, 0x06) };
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;

        ewr::MaintenanceRequest nozzle;
        nozzle.action = ewr::MaintenanceAction::NozzleCheck;
        const ewr::MaintenanceOutcome out = maintenance.Run(nozzle);
        CHECK(out.phase == ewr::MaintenancePhase::Error);
        CHECK(out.error.find("PAPER OUT") != std::string::npos);
    }

    // A connection that cannot carry print data says so.
    {
        FakeGateway gw;
        gw.queryResult.deviceFound = true;
        gw.queryResult.query.replies = { MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, nullptr, reporter);

        ewr::MaintenanceRequest nozzle;
        nozzle.action = ewr::MaintenanceAction::NozzleCheck;
        CHECK(maintenance.Run(nozzle).phase == ewr::MaintenancePhase::NotSupported);
    }
}
