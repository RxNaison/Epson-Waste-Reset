#include "support.h"

void test_remote_mode_job_bytes()
{
    std::cout << "[TEST] test_remote_mode_job_bytes" << std::endl;

    CHECK((ewr::remote::NozzleCheck() == std::vector<unsigned char>{ 'N', 'C', 0x02, 0x00, 0x00, 0x00 }));
    CHECK((ewr::remote::NozzleCheck(1) == std::vector<unsigned char>{ 'N', 'C', 0x02, 0x00, 0x00, 0x10 }));
    CHECK((ewr::remote::HeadClean() == std::vector<unsigned char>{ 'C', 'H', 0x02, 0x00, 0x00, 0x00 }));
    CHECK((ewr::remote::HeadClean(1, true) == std::vector<unsigned char>{ 'C', 'H', 0x02, 0x00, 0x00, 0x11 }));

    CHECK((ewr::remote::JobEnd() == std::vector<unsigned char>{ 'J', 'E', 0x01, 0x00, 0x00 }));

    // The command in remote mode, then JE in remote mode of its own: the
    // ending Epson's driver sends after a cleaning.
    const std::vector<unsigned char> job = ewr::remote::BuildJob({ ewr::remote::NozzleCheck() });
    std::vector<unsigned char> expected = ewr::remote::kLeavePacketMode;
    const std::vector<unsigned char> body = { 0x1B, '@', 0x1B, '(', 'R', 0x08, 0x00, 0x00,
                                              'R', 'E', 'M', 'O', 'T', 'E', '1',
                                              'N', 'C', 0x02, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00,
                                              0x1B, '@', 0x1B, '(', 'R', 0x08, 0x00, 0x00,
                                              'R', 'E', 'M', 'O', 'T', 'E', '1',
                                              'J', 'E', 0x01, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00 };
    expected.insert(expected.end(), body.begin(), body.end());
    CHECK(job == expected);

    // A job that prints a sheet ejects it with a form feed once remote mode
    // is left, before the JE block.
    std::vector<unsigned char> page = expected;
    const std::vector<unsigned char> firstExit = { 'N', 'C', 0x02, 0x00, 0x00, 0x00, 0x1B, 0x00, 0x00, 0x00 };
    page.insert(std::search(page.begin(), page.end(), firstExit.begin(), firstExit.end()) + firstExit.size(),
                static_cast<unsigned char>(0x0C));
    CHECK(ewr::remote::BuildJob({ ewr::remote::NozzleCheck() }, true) == page);
    CHECK(std::count(job.begin(), job.end(), 0x0C) == 0);

    // The packet-mode exit: 27 bytes, five spaces, as the SX110 took it.
    CHECK(ewr::remote::kLeavePacketMode.size() == 27);
    CHECK(ewr::remote::kLeavePacketMode[3] == 0x1B);

    // The factory body is the '||' packet's, unchanged.
    const std::vector<unsigned char> factoryBody = ewr::ctrl::FactoryBody(0x0719, 0x84, { 0x02 });
    CHECK((factoryBody == std::vector<unsigned char>{ 0x19, 0x07, 0x84, 0x7B, 0x42, 0x02 }));
    const std::vector<unsigned char> packet = ewr::ctrl::FactoryPacket(0x0719, 0x84, { 0x02 });
    const std::vector<unsigned char> pipe = { '|', '|', 0x06, 0x00 };
    CHECK(std::search(packet.begin(), packet.end(), pipe.begin(), pipe.end()) != packet.end());
    CHECK(std::equal(factoryBody.begin(), factoryBody.end(), packet.end() - factoryBody.size()));

    // The same body as print data: ESC @, ESC | 00 and a length. The bytes
    // one tool sends an XP-205 for its cleaning.
    std::vector<unsigned char> factoryJob = ewr::remote::kLeavePacketMode;
    const std::vector<unsigned char> wrapped = { 0x1B, '@', 0x1B, '|', 0x00, 0x06, 0x00,
                                                 0x19, 0x07, 0x84, 0x7B, 0x42, 0x02 };
    factoryJob.insert(factoryJob.end(), wrapped.begin(), wrapped.end());
    CHECK(ewr::remote::BuildFactoryJob(factoryBody) == factoryJob);

    // State codes only epson_print_conf names.
    CHECK(ewr::ctrl::ParseStatusReply(MakeSt2State(0x0F)).stateName == "NOZZLE CHECK");
    CHECK(ewr::ctrl::ParseStatusReply(MakeSt2State(0x11)).stateName == "CHARGING");
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

    // A level listed for all nozzles but not for black alone: the message
    // names the group, not the level (#64: L1800, L3060).
    model.cleaning.clean["gentle"] = { 0x00 };
    clean.level = "gentle";
    why.clear();
    CHECK(maintenance.Route(clean, why).empty());
    CHECK(why.find("black nozzles alone") != std::string::npos);
    clean.group = ewr::CleanGroup::All;
    CHECK(maintenance.Route(clean, why) == "factory");

    // --print-data with a factory-only level: the message says why.
    clean.remoteOnly = true;
    why.clear();
    CHECK(maintenance.Route(clean, why).empty());
    CHECK(why.find("factory command") != std::string::npos);
    clean.remoteOnly = false;
    clean.level = "normal";

    // The experiment needs a listed byte, and never takes a nozzle check.
    clean.factoryAsPrintData = true;
    CHECK(maintenance.Route(clean, why) == "factory_print");
    clean.level = "strong";
    CHECK(maintenance.Route(clean, why).empty());
    clean.level = "normal";
    clean.factoryAsPrintData = false;

    ewr::MaintenanceRequest nozzleExperiment;
    nozzleExperiment.action = ewr::MaintenanceAction::NozzleCheck;
    nozzleExperiment.factoryAsPrintData = true;
    CHECK(maintenance.Route(nozzleExperiment, why).empty());

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

// PR #64: an L350, L1800 and L3060 each did the job, and EWR called it lost
// or never started, because the status query waits behind print data.
void test_maintenance_print_data_watch_and_fallback()
{
    std::cout << "[TEST] test_maintenance_print_data_watch_and_fallback" << std::endl;

    const std::vector<unsigned char> silent;
    ewr::log::Reporter reporter;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };
    model.cleaning.charge["normal"] = { 0x0A };
    model.pad_groups[0].counters.push_back({ "Main Pad Counter", "main", { { 0x0C, 0xFF, 1 }, { 0x0D, 0xFF, 256 } }, 1000 });

    // Silent for longer than a lost printer would be, then idle: done.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent, silent, silent, silent, MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        ewr::MaintenanceRequest request;
        request.remoteOnly = true;
        const ewr::MaintenanceOutcome out = maintenance.Run(request);
        CHECK(out.phase == ewr::MaintenancePhase::Done && out.success);
        CHECK(out.started && out.silentWhileWorking);
        CHECK(out.route == "remote" && out.fallbackFrom.empty());
        CHECK(gw.jobs.size() == 1 && gw.commands.empty());
        CHECK(out.after.available); // the pads are read again: the proof a cleaning ran
    }

    // A nozzle check over before any poll is answered: done, not never_started.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent, MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;
        maintenance.startWindowSeconds = 0;

        ewr::MaintenanceRequest nozzle;
        nozzle.action = ewr::MaintenanceAction::NozzleCheck;
        const ewr::MaintenanceOutcome out = maintenance.Run(nozzle);
        CHECK(out.phase == ewr::MaintenancePhase::Done);
        CHECK(out.silentWhileWorking);
    }

    // Silent until the action's own timeout.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent };
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;
        maintenance.cleanTimeoutSeconds = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::TimedOut);
        CHECK(out.silentWhileWorking);
    }

    // Unplugged is not busy: lost after three polls, print data or not.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent };
        gw.goneWhenSilent = true;
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::Error);
        CHECK(!out.silentWhileWorking && !out.started);
        CHECK(gw.nextStatus == 4);
    }

    // Over EPSON-CTRL the status has its own channel: silence is still lost.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent };
        gw.commandReply = MakeCommandReply("||:84:OK;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.route == "factory");
        CHECK(out.phase == ewr::MaintenancePhase::Error);
        CHECK(!out.silentWhileWorking);
    }

    // NA at the normal level: nothing ran, so the print-data cleaning goes
    // instead - once, after the one refused command.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent, MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:NA;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::Done);
        CHECK(out.route == "remote" && out.fallbackFrom == "factory");
        CHECK(out.reply == "||:84:NA;");
        CHECK(gw.commands.size() == 1 && gw.jobs.size() == 1);
        CHECK(gw.jobs[0] == ewr::remote::BuildJob({ ewr::remote::HeadClean(0, false) }));
    }

    // The same for an ink charge: the print-data one carries the power flag.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x07), MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:NA;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        ewr::MaintenanceRequest charge;
        charge.action = ewr::MaintenanceAction::PowerClean;
        const ewr::MaintenanceOutcome out = maintenance.Run(charge);
        CHECK(out.phase == ewr::MaintenancePhase::Done && !out.silentWhileWorking);
        CHECK(gw.commands.size() == 1 && gw.commands[0] == ewr::ctrl::FactoryPacket(model.rkey, 0x84, { 0x0A }));
        CHECK(gw.jobs.size() == 1 && gw.jobs[0] == ewr::remote::BuildJob({ ewr::remote::HeadClean(0, true) }));
    }

    // The experiment: the model's byte as print data, nothing over EPSON-CTRL.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent, MakeSt2State(0x04) };
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        ewr::MaintenanceRequest request;
        request.factoryAsPrintData = true;
        const ewr::MaintenanceOutcome out = maintenance.Run(request);
        CHECK(out.route == "factory_print" && out.phase == ewr::MaintenancePhase::Done);
        CHECK(gw.commands.empty() && gw.jobs.size() == 1);
        CHECK(gw.jobs[0] == ewr::remote::BuildFactoryJob(ewr::ctrl::FactoryBody(model.rkey, 0x84, { 0x02 })));
    }

    // NA at a level the printer's own cleaning does not have: the same
    // command goes inside a print job, once, as it ran on an L3060.
    {
        ewr::DbPrinterModel strong = model;
        strong.cleaning.clean["strong"] = { 0x11 };
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), silent, MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:NA;");
        ewr::Maintenance maintenance(gw, &strong, reporter);
        maintenance.pollIntervalMs = 0;

        std::vector<ewr::log::Event> events;
        const int sink = reporter.AddSink([&](const ewr::log::Event& e) { events.push_back(e); });

        ewr::MaintenanceRequest request;
        request.level = "strong";
        const ewr::MaintenanceOutcome out = maintenance.Run(request);
        reporter.RemoveSink(sink);
        CHECK(out.phase == ewr::MaintenancePhase::Done);
        CHECK(out.route == "factory_print" && out.fallbackFrom == "factory");
        CHECK(out.reply == "||:84:NA;");
        CHECK(gw.commands.size() == 1 && gw.commands[0] == ewr::ctrl::FactoryPacket(strong.rkey, 0x84, { 0x11 }));
        CHECK(gw.jobs.size() == 1);
        CHECK(gw.jobs[0] == ewr::remote::BuildFactoryJob(ewr::ctrl::FactoryBody(strong.rkey, 0x84, { 0x11 })));

        const auto fallback = std::find_if(events.begin(), events.end(), [](const ewr::log::Event& e) {
            return e.code == "maintenance.fallback";
        });
        CHECK(fallback != events.end() && fallback->fields.at("to") == "factory_print");
    }
}

// An L3060 at work on print data does not answer in D4, but sends its status
// outside packet mode; an L350 sends nothing (PR #64). The print-data watch
// takes the report, so the printer is watched at work.
void test_maintenance_watches_a_printer_out_of_packet_mode()
{
    std::cout << "[TEST] test_maintenance_watches_a_printer_out_of_packet_mode" << std::endl;

    ewr::log::Reporter reporter;
    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };

    // Print data: CLEANING twice from outside packet mode, then idle.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x07), MakeSt2State(0x07), MakeSt2State(0x04) };
        gw.outOfPacketMode = { 1, 2 };
        ewr::Maintenance maintenance(gw, nullptr, reporter);
        maintenance.pollIntervalMs = 0;

        std::vector<ewr::log::Event> events;
        const int sink = reporter.AddSink([&](const ewr::log::Event& e) { events.push_back(e); });

        ewr::MaintenanceRequest request;
        request.remoteOnly = true;
        const ewr::MaintenanceOutcome out = maintenance.Run(request);
        reporter.RemoveSink(sink);
        CHECK(out.phase == ewr::MaintenancePhase::Done && out.started);
        CHECK(!out.silentWhileWorking);
        // The preflight reads as always; only the watch takes a plain report.
        CHECK((gw.pollsAcceptedPlain == std::vector<bool>{ false, true, true, true }));

        const auto started = std::find_if(events.begin(), events.end(), [](const ewr::log::Event& e) {
            return e.code == "maintenance.started";
        });
        CHECK(started != events.end() && started->fields.at("state") == "CLEANING");
    }

    // Over EPSON-CTRL the printer stays in packet mode: no poll takes one.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04), MakeSt2State(0x07), MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:OK;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.route == "factory" && out.phase == ewr::MaintenancePhase::Done);
        CHECK(std::none_of(gw.pollsAcceptedPlain.begin(), gw.pollsAcceptedPlain.end(), [](bool b) { return b; }));
    }

    // status::Read takes the plain report only when asked to.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x07) };
        gw.outOfPacketMode = { 0 };

        ewr::ExecutorOptions options;
        CHECK(!ewr::status::Read(gw, options).available);
        options.acceptPlainStatus = true;
        const ewr::StateSnapshot plain = ewr::status::Read(gw, options);
        CHECK(plain.available && plain.deviceFound && plain.status.stateCode == 0x07);
    }
}

void test_maintenance_stops_where_it_should()
{
    std::cout << "[TEST] test_maintenance_stops_where_it_should" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();
    model.cleaning.clean["normal"] = { 0x02 };
    ewr::log::Reporter reporter;

    // Refused with something other than NA: no watching, nothing else sent.
    {
        MaintenanceGateway gw;
        gw.statuses = { MakeSt2State(0x04) };
        gw.commandReply = MakeCommandReply("||:84:NG;");
        ewr::Maintenance maintenance(gw, &model, reporter);
        maintenance.pollIntervalMs = 0;

        const ewr::MaintenanceOutcome out = maintenance.Run({});
        CHECK(out.phase == ewr::MaintenancePhase::SendFailed);
        CHECK(out.fallbackFrom.empty());
        CHECK(gw.commands.size() == 1 && gw.jobs.empty());
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
        CHECK(gw.jobs[0] == ewr::remote::BuildJob({ ewr::remote::NozzleCheck() }, true));
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
