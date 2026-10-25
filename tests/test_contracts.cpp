#include "support.h"

void test_json_key_search_data()
{
    std::cout << "[TEST] test_json_key_search_data" << std::endl;

    ewr::keysearch::Result search;
    search.deviceFound = true;
    search.completed = true;
    search.probes.push_back({ { 15120, 1, { "R200", "R220" } }, ewr::ReadAnswer::Value, 0x07 });
    search.probes.push_back({ { 373, 2, { "SX210" } }, ewr::ReadAnswer::Empty, 0 });
    search.probes.push_back({ { 1609, 2, { "SX200" } }, ewr::ReadAnswer::Refused, 0 });

    const nlohmann::json data = ewr::json::KeySearchData("Stylus Photo R220", search);
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
    const nlohmann::json unnamed = ewr::json::KeySearchData("", ewr::keysearch::Result{});
    CHECK(unnamed["printer"].is_null());
    CHECK(unnamed["keys"].is_array() && unnamed["keys"].empty());
    CHECK(unnamed["completed"] == false);
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
    const nlohmann::json pads = ewr::json::PadUsage(model, values);

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

    const nlohmann::json vaguePads = ewr::json::PadUsage(vague, { { 0x20, 5 } });
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

    const nlohmann::json out = ewr::json::Status(status);

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
    CHECK(ewr::json::Status(silent).is_null());
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

    const nlohmann::json data = ewr::json::StateData(MakeTestModel(), state);

    CHECK(data["model"] == "TestPrinter");
    CHECK(data.contains("detected_model"));
    CHECK(data["detected_model"].is_null());
    CHECK(data["counters"][0]["address"] == 0x18);
    CHECK(data["counters"][0]["value"] == 255);
    // An unread byte is null, not 0 and not absent.
    CHECK(data["counters"][1]["value"].is_null());
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

    const nlohmann::json partial = ewr::json::StateData(model, half);
    CHECK(partial["pads_total"] == 2);
    CHECK(partial["pads"].size() == 1);
    CHECK(partial["pads"][0]["kind"] == "main");

    // Nothing answered at all: still two pads on this model.
    ewr::StateSnapshot silent;
    const nlohmann::json none = ewr::json::StateData(model, silent);
    CHECK(none["pads_total"] == 2);
    CHECK(none["pads"].empty());
    CHECK(none["counters"].empty());
    CHECK(none["printer"].is_null());

    // A model with no pad groups reports zero, which is the other meaning.
    ewr::DbPrinterModel bare;
    bare.name = "NoPads";
    const nlohmann::json empty = ewr::json::StateData(bare, half);
    CHECK(empty["pads_total"] == 0);
    CHECK(empty["pads"].empty());
}

// The C ABI is what another language links against, so the test is the
// promise: codes keep their numbers, an unknown one still answers, and the
// parts that need no printer work without one.
void test_c_abi_constants_and_status_names()
{
    std::cout << "[TEST] test_c_abi_constants_and_status_names" << std::endl;

    // 2 added ewr_session_open_network; 3 the backup callback, ewr_restore
    // and ewr_set_data_dir.
    CHECK(ewr_abi_version() == 3);
    CHECK(ewr_json_contract_version() == ewr::json::Emitter::kContractVersion);
    CHECK(std::string(ewr_version()) == EWR_VERSION);

    // Renumbering these would silently change what a caller's constant means.
    CHECK(EWR_OK == 0);
    CHECK(EWR_ERR_FAILED == 1);
    CHECK(EWR_ERR_INVALID_ARGUMENT == 2);
    CHECK(EWR_ERR_ANOTHER_RUN == 3);
    CHECK(EWR_ERR_DEVICE_NOT_FOUND == 4);
    CHECK(EWR_ERR_BLOCKED == 10);
    CHECK(EWR_ERR_WRITE_UNVERIFIED == 12);
    CHECK(EWR_ERR_MODEL_MISMATCH == 14);
    CHECK(EWR_ERR_BAD_BACKUP == 15);
    CHECK(EWR_ERR_NOT_STARTED == 16);
    CHECK(EWR_ERR_PRINTER_ERROR == 17);
    CHECK(EWR_CLEAN_ALL == 0 && EWR_CLEAN_BLACK == 1 && EWR_CLEAN_COLOR == 2);

    CHECK(std::string(ewr_status_name(EWR_OK)) == "ok");
    CHECK(std::string(ewr_status_name(EWR_ERR_BLOCKED)) == "blocked");
    CHECK(std::string(ewr_status_name(EWR_ERR_INCOMPLETE_DUMP)) == "incomplete_dump");
    CHECK(std::string(ewr_status_name(EWR_ERR_MODEL_MISMATCH)) == "model_mismatch");
    CHECK(std::string(ewr_status_name(EWR_ERR_BAD_BACKUP)) == "bad_backup");
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
        ewr::json::Emitter emitter(out);
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
        CHECK(lines[i]["v"] == ewr::json::Emitter::kContractVersion);
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
    ewr::json::Emitter emitter(out);

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

// A run that wrote past an objection has to say so in the result a caller
// reads, not only in events it may not keep. One shape for every gate.
void test_json_reset_data_lists_overrides()
{
    std::cout << "[TEST] test_json_reset_data_lists_overrides" << std::endl;

    ewr::DbPrinterModel model = MakeSessionModel();

    ewr::ResetOutcome clean;
    clean.phase = ewr::ResetPhase::Done;
    const nlohmann::json none = ewr::json::ResetData(model, false, clean);
    CHECK(none.contains("overrides") && none["overrides"].is_array() && none["overrides"].empty());

    ewr::ResetOutcome forced;
    forced.phase = ewr::ResetPhase::Done;
    forced.overrides.push_back({ "model_mismatch", "R220", -1 });
    forced.overrides.push_back({ "blocker", "INK OUT", 5 });
    forced.overrides.push_back({ "blocker", "PRINTER BUSY", -1 });
    forced.overrides.push_back({ "db_conflict", "", -1 });
    forced.overrides.push_back({ "backup", "not_saved", -1 });

    const nlohmann::json out = ewr::json::ResetData(model, false, forced);
    const nlohmann::json& list = out["overrides"];
    CHECK(list.size() == 5);
    if (list.size() != 5)
        return;

    for (const auto& entry : list)
    {
        CHECK(entry.size() == 5);
        CHECK(entry.contains("gate") && entry.contains("detected_model")
              && entry.contains("error") && entry.contains("error_code") && entry.contains("detail"));
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
    CHECK(list[3]["detail"].is_null());

    // A gate with no key of its own says why in `detail`.
    CHECK(list[4]["gate"] == "backup");
    CHECK(list[4]["detail"] == "not_saved");
    CHECK(list[4]["error"].is_null() && list[4]["detected_model"].is_null());
    CHECK(list[0]["detail"].is_null() && list[1]["detail"].is_null());
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
    const nlohmann::json inkJson = ewr::json::BlockerData(ink);
    CHECK(inkJson["error"] == "INK OUT");
    CHECK(inkJson["error_code"] == 5);
    CHECK(inkJson["explanation"] == "Clear it first.");

    ewr::Blocker busy;
    busy.errorName = "PRINTER BUSY";
    const nlohmann::json busyJson = ewr::json::BlockerData(busy);
    CHECK(busyJson.contains("error_code") && busyJson["error_code"].is_null());

    ewr::usb::InterfaceInfo usbprint;
    usbprint.index = 1;
    usbprint.className = "USBPRINT";
    usbprint.path = "\\\\?\\usb#vid_04b8";
    const nlohmann::json plain = ewr::json::InterfaceEntry(usbprint, "");
    CHECK(plain.contains("interface_number") && plain["interface_number"].is_null());
    CHECK(plain["device_id"].is_null());
    CHECK(plain["model_match"].is_null());
    CHECK(plain["index"] == 1 && plain["class"] == "USBPRINT");

    ewr::usb::InterfaceInfo vendor = usbprint;
    vendor.index = 2;
    vendor.interfaceNumber = 2;
    vendor.deviceId = "MFG:EPSON;MDL:ET-2800 Series;";
    const nlohmann::json composite = ewr::json::InterfaceEntry(vendor, "ET-2800");
    CHECK(composite["interface_number"] == 2);
    CHECK(composite["device_id"] == "MFG:EPSON;MDL:ET-2800 Series;");
    CHECK(composite["model_match"] == "ET-2800");

    // Valid as soon as any field decodes, so the state may be missing.
    ewr::PrinterStatus inksOnly;
    inksOnly.valid = true;
    const nlohmann::json st = ewr::json::Status(inksOnly);
    CHECK(st["state"].is_null());
    CHECK(st["state_code"].is_null());
    CHECK(st["error_code"].is_null());
}

// ewr_restore refuses what it cannot use before it asks the printer anything.
void test_c_abi_restore_refuses_bad_files()
{
    std::cout << "[TEST] test_c_abi_restore_refuses_bad_files" << std::endl;

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
    CHECK(ewr_restore(session, "R220", nullptr, &json) == EWR_ERR_INVALID_ARGUMENT);
    CHECK(ewr_restore(session, "R220", "not a backup", &json) == EWR_ERR_BAD_BACKUP);
    CHECK(!std::string(ewr_session_last_error(session)).empty());
    CHECK(json == nullptr);

    // An R220 backup handed to another model is refused, not written.
    ewr::Backup backup = MakeTestBackup();
    backup.model = "R220";
    const std::string text = ewr::backup::ToJson(backup);
    CHECK(ewr_restore(session, "L3150", text.c_str(), &json) == EWR_ERR_BAD_BACKUP);

    // A file whose bytes are none of the model's.
    backup.bytes = { { 0x00, 0x01 }, { 0x01, 0x02 } };
    const std::string foreign = ewr::backup::ToJson(backup);
    CHECK(ewr_restore(session, nullptr, foreign.c_str(), &json) == EWR_ERR_BAD_BACKUP);

    // A model the database does not know.
    backup.model = "No Such Printer 9000";
    const std::string unknown = ewr::backup::ToJson(backup);
    CHECK(ewr_restore(session, nullptr, unknown.c_str(), &json) == EWR_ERR_MODEL_UNKNOWN);

    ewr_session_close(session);
}

void test_c_abi_restore_all_needs_its_consent()
{
    std::cout << "[TEST] test_c_abi_restore_all_needs_its_consent" << std::endl;

    ewr_session* session = nullptr;
    if (ewr_session_open("database.json", &session) == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }

    const std::string dump = "# EWR EEPROM dump\n# model: R220\n0x0C,0x00,\n";
    char* json = nullptr;
    const int refused = ewr_restore_all(session, nullptr, dump.c_str(), 0, &json);
    CHECK(refused == EWR_ERR_INVALID_ARGUMENT || refused == EWR_ERR_NOT_SUPPORTED);
    CHECK(json == nullptr);

    // A backup is not a dump.
    ewr::Backup backup = MakeTestBackup();
    backup.model = "R220";
    const std::string text = ewr::backup::ToJson(backup);
    const int notDump = ewr_restore_all(session, nullptr, text.c_str(), 1, &json);
    CHECK(notDump == EWR_ERR_BAD_BACKUP || notDump == EWR_ERR_NOT_SUPPORTED);

    ewr_session_close(session);
}

// What an app that downloads database.json itself checks before swapping.
void test_c_abi_database_check()
{
    std::cout << "[TEST] test_c_abi_database_check" << std::endl;

    char* json = nullptr;
    CHECK(ewr_database_check("database.json", &json) == EWR_OK);
    if (json)
    {
        const nlohmann::json out = nlohmann::json::parse(json);
        CHECK(out["models"].get<int>() >= 1450);
        CHECK(out["schema_version"] == 4);
        ewr_string_free(json);
    }

    const fs::path bad = fs::temp_directory_path() / "ewr_not_a_database.json";
    {
        std::ofstream out(bad);
        out << "{\"models\": {}}";
    }
    json = nullptr;
    CHECK(ewr_database_check(bad.string().c_str(), &json) == EWR_ERR_DATABASE);
    CHECK(json == nullptr);
    CHECK(ewr_database_check("no/such/file.json", &json) == EWR_ERR_DATABASE);

    std::error_code ec;
    fs::remove(bad, ec);
}

// An event's level as ewr_c.h numbers it. log::Level has a Debug between
// Trace and Info, so a plain cast reported every Info event as WARNING.
void test_c_abi_event_levels_use_the_header_numbers()
{
    std::cout << "[TEST] test_c_abi_event_levels_use_the_header_numbers" << std::endl;

    ewr_session* session = nullptr;
    if (ewr_session_open("database.json", &session) == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }

    std::vector<int> levels;
    ewr_session_set_event_callback(session, [](const ewr_event* event, void* user) {
        if (std::string(event->code).rfind("test.level", 0) == 0)
            static_cast<std::vector<int>*>(user)->push_back(event->level);
    }, &levels);

    ewr::log::Log(ewr::log::Level::Info, ewr::log::Stage::General, "test.level.info", "info");
    ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::General, "test.level.warning", "warning");
    ewr::log::Log(ewr::log::Level::Error, ewr::log::Stage::General, "test.level.error", "error");

    CHECK((levels == std::vector<int>{ EWR_LEVEL_INFO, EWR_LEVEL_WARNING, EWR_LEVEL_ERROR }));
    ewr_session_close(session);
}

// What the CLI refuses for a JIG model, the C ABI refuses too: no plan for a
// reset ewr_reset will not run, no dump walking memory JIG mode was never seen
// to serve, and no "resettable" in the model list. None of it touches USB.
void test_c_abi_refuses_what_a_jig_model_cannot_do()
{
    std::cout << "[TEST] test_c_abi_refuses_what_a_jig_model_cannot_do" << std::endl;

    ewr_session* session = nullptr;
    if (ewr_session_open("database.json", &session) == EWR_ERR_ANOTHER_RUN)
    {
        std::cout << "  [skip] another EWR run holds the printer" << std::endl;
        ewr_session_close(session);
        return;
    }

    char* json = nullptr;
    CHECK(ewr_list_models(session, &json) == EWR_OK);
    const nlohmann::json models = nlohmann::json::parse(json);
    ewr_string_free(json);

    int seen = 0;
    for (const auto& entry : models["models"])
    {
        if (entry["name"] == "SX110")
        {
            ++seen;
            CHECK(entry["resettable"] == false);
            CHECK(entry["has_ink_reset"] == false);
        }
        if (entry["name"] == "R220")
            CHECK(entry["resettable"] == true);
    }
    CHECK(seen == 1);

    json = nullptr;
    CHECK(ewr_plan(session, "SX110", 0, &json) == EWR_ERR_NOT_SUPPORTED);
    CHECK(json == nullptr);
    CHECK(ewr_dump(session, "SX110", &json) == EWR_ERR_NOT_SUPPORTED);
    CHECK(json == nullptr);
    CHECK(!std::string(ewr_session_last_error(session)).empty());

    ewr_session_close(session);
}
