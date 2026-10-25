#include "support.h"

void test_legacy_schema_loading()
{
    std::cout << "[TEST] test_legacy_schema_loading" << std::endl;

    std::string test_filename = "test_legacy_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"LegacyModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24, 25], "reset": [0, 0]}})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];
        CHECK(m.pad_groups.size() == 1);
        CHECK(m.GetAllAddresses().size() == 2);
        CHECK(m.HasResettableCounters());
        CHECK(!m.IsPlatenOnly());

        auto seq = ewr::ctrl::ResetSequence(m);
        CHECK(seq.size() == 9);

        size_t writes = 0;
        for (const auto& pkt : seq)
            if (ewr::ctrl::IsWrite(pkt))
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

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];
        CHECK(m.pad_groups.size() == 1);
        CHECK(m.GetAllAddresses().size() == 2);
        CHECK(m.pad_groups[0].kind == "main");

        size_t writes = 0;
        for (const auto& pkt : ewr::ctrl::ResetSequence(m))
            if (ewr::ctrl::IsWrite(pkt))
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

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        CHECK(models[0].name == "EnvelopeModel");
        CHECK(models[0].GetAllAddresses().size() == 2);
        CHECK(models[0].HasResettableCounters());
    }
}

void test_alias_conflict_schema_loading()
{
    std::cout << "[TEST] test_alias_conflict_schema_loading" << std::endl;

    std::string test_filename = "test_alias_conflict_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {)"
            << R"("AliasModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "aliases": ["AliasModel Series", "AM-100/101/103"], "conflict": true,)"
            << R"( "future_unknown_key": {"nested": [1, 2, 3]},)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]},)"
            << R"("PlainModel": {"rkey": 9, "wkey": "Pong",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [30], "reset": [0]}]}}})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 2);

    const ewr::DbPrinterModel* aliasModel = nullptr;
    const ewr::DbPrinterModel* plainModel = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "AliasModel")
            aliasModel = &m;
        else if (m.name == "PlainModel")
            plainModel = &m;
    }

    // The additive keys parse, and an unknown future key never kills the entry.
    CHECK(aliasModel != nullptr);
    if (aliasModel)
    {
        CHECK(aliasModel->aliases.size() == 2);
        if (aliasModel->aliases.size() == 2)
        {
            CHECK(aliasModel->aliases[0] == "AliasModel Series");
            CHECK(aliasModel->aliases[1] == "AM-100/101/103");
        }
        CHECK(aliasModel->conflict);
        CHECK(aliasModel->HasResettableCounters());
    }

    // Entries without the new keys keep the safe defaults.
    CHECK(plainModel != nullptr);
    if (plainModel)
    {
        CHECK(plainModel->aliases.empty());
        CHECK(!plainModel->conflict);
    }
}

void test_future_schema_version_warning()
{
    std::cout << "[TEST] test_future_schema_version_warning" << std::endl;

    std::string test_filename = "test_future_schema_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 99, "models": {"FutureModel": {"rkey": 8, "wkey": "FutureKey",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [24, 25], "reset": [0, 0]}]}}})";
    }

    // The loader reports through the default reporter now, not stderr: the
    // library must stay silent on consoles it does not own.
    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);

    ewr::log::Default().RemoveSink(sinkId);
    fs::remove(test_filename);

    CHECK(loaded);
    CHECK(captured.find("schema_version 99") != std::string::npos);
    CHECK(captured.find("https://github.com/RxNaison/Epson-Waste-Reset/releases") != std::string::npos);
}

// v1.3.0-v1.4.0 read database.json from main and, for any model carrying a
// "recovery" key, send its enter command before writing: ez-reset's firmware-
// updater data, which left an L565 on "Preparing to update". Current builds
// ignore the key, so only this check protects the installs that still read it.
void test_database_carries_no_recovery_channel()
{
    std::cout << "[TEST] test_database_carries_no_recovery_channel" << std::endl;

    std::ifstream in("database.json", std::ios::binary);
    CHECK(in.good());

    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty());
    CHECK(text.find("\"recovery\"") == std::string::npos);
}

void test_generator_local_db()
{
    std::cout << "[TEST] test_generator_local_db" << std::endl;
    ewr::Database gen;
    bool loaded = gen.Load("database.json");
    if (!loaded)
    {
        std::cout << "  [SKIP] no database file found in current working directory." << std::endl;
        return;
    }

    CHECK(!gen.IsEmpty());
    auto available = gen.Models();
    CHECK(!available.empty());
    std::cout << "  Loaded " << available.size() << " models." << std::endl;
}

void test_database_integrity()
{
    std::cout << "[TEST] test_database_integrity" << std::endl;
    ewr::Database gen;
    bool loaded = gen.Load("database.json");
    CHECK(loaded);
    if (!loaded)
        return;

    auto models = gen.Models();
    CHECK(models.size() > 1400);

    size_t withCounters = 0;
    size_t withoutCounters = 0;
    bool namesOk = true;
    bool keysOk = true;
    bool keyBytesOk = true;
    bool arraysOk = true;
    bool jigResettable = false;

    for (const auto& m : models)
    {
        if (m.name.empty())
            namesOk = false;

        // A keyword is 8 bytes on the wire; UTF-8 made three families' 12 (#59).
        // A JIG model has none to send.
        if (!m.jig && (m.wkey.size() != 8 || (!m.wkey1.empty() && m.wkey1.size() != 8)))
            keyBytesOk = false;

        if (!m.HasResettableCounters())
        {
            withoutCounters++;
            continue;
        }

        withCounters++;

        // A keyed model needs both keys; a JIG model must carry none, since
        // nothing it is read or written with uses them.
        if (m.jig ? (m.rkey != 0 || !m.wkey.empty() || !m.wkey1.empty())
                  : (m.rkey == 0 || m.wkey.empty()))
            keysOk = false;
        if (m.GetAllAddresses().size() != m.GetAllResetValues().size())
            arraysOk = false;
        if (m.jig && m.CanReset())
            jigResettable = true;
    }

    CHECK(namesOk);
    CHECK(keysOk);
    CHECK(keyBytesOk);
    CHECK(arraysOk);
    CHECK(!jigResettable);
    CHECK(withCounters > 1000);

    std::cout << "  " << withCounters << " models with resettable counters, "
              << withoutCounters << " without (hidden in CLI)." << std::endl;
}

void test_database_load_skips_malformed_entries()
{
    std::cout << "[TEST] test_database_load_skips_malformed_entries" << std::endl;

    const std::string path = "test_malformed_db.json";
    {
        std::ofstream out(path);
        out << R"({"GoodModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24], "reset": [0]},)"
            << R"( "BadModel": {"rkey": 8, "wkey": "Arkanoid", "addresses": ["not-a-number"], "reset": [0]},)"
            << R"( "GoodModel2": {"rkey": 8, "wkey": "Arkanoid", "addresses": [25], "reset": [0]}})";
    }

    std::string captured;
    const int sinkId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { captured += e.message + "\n"; });

    ewr::Database gen;
    bool loaded = gen.Load(path);

    ewr::log::Default().RemoveSink(sinkId);
    fs::remove(path);

    // One rotten entry must not poison the other 1400+ models.
    CHECK(loaded);
    CHECK(gen.Models().size() == 2);
    CHECK(captured.find("BadModel") != std::string::npos);
}

void test_write_key_bytes_above_0x7f_are_sent_raw()
{
    std::cout << "[TEST] test_write_key_bytes_above_0x7f_are_sent_raw" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    std::map<std::string, ewr::DbPrinterModel> byName;
    for (auto& m : gen.Models())
        byName[m.name] = m;

    // The write #59's SC-P600 should have received: its trace had length
    // 0x14 and the keyword as c2 97 26 c2 86 ...
    const auto p600 = byName.find("SC-P600");
    CHECK(p600 != byName.end());
    if (p600 != byName.end())
    {
        const std::vector<unsigned char> expected = {
            0x02, 0x02, 0x00, 0x1A, 0x00, 0x00, 0x7C, 0x7C, 0x10, 0x00, 0x02, 0x52, 0x42, 0xBD, 0x21,
            0x29, 0x01, 0x00, 0x97, 0x26, 0x86, 0x97, 0x36, 0x16, 0x16, 0xB6 };
        CHECK(ewr::ctrl::WritePacket(p600->second.rkey, 0x0129, 0x00, p600->second.wkey,
                                                           p600->second.WriteAddressLength()) == expected);
    }

    // As ez-reset's <keyword> lists them. R220's NUL-padded key is
    // hardware-verified and must not move.
    const struct { const char* model; std::vector<unsigned char> key; } cases[] = {
        { "SC-P400", { 0x37, 0x57, 0x36, 0x86, 0x96, 0xB6, 0x17, 0x47 } },
        { "SC-P700", { 0x37, 0x57, 0xA6, 0x16, 0xB7, 0x56, 0x76, 0xF7 } },
        { "R220",    { 'M', 'c', 'L', 'a', 'r', 'e', 'n', 0x00 } },
    };

    for (const auto& c : cases)
    {
        const auto it = byName.find(c.model);
        CHECK(it != byName.end());
        if (it == byName.end())
            continue;

        const ewr::DbPrinterModel& m = it->second;
        const auto pkt = ewr::ctrl::WritePacket(m.rkey, 0x30, 0x00, m.wkey,
                                                                      m.WriteAddressLength());
        CHECK(((size_t)pkt[8] | ((size_t)pkt[9] << 8)) == 6 + m.WriteAddressLength() + 8u);
        CHECK(std::equal(c.key.begin(), c.key.end(), pkt.end() - c.key.size()));
    }
}

void test_write_key_json_round_trip()
{
    std::cout << "[TEST] test_write_key_json_round_trip" << std::endl;

    std::string bytes;
    CHECK(ewr::db::WriteKeyFromJson("\xC2\x97&", bytes) && bytes == "\x97&");
    CHECK(!ewr::db::WriteKeyFromJson("\xC4\x80", bytes)); // U+0100 has no single byte
    CHECK(!ewr::db::WriteKeyFromJson("ab\xC2", bytes));   // truncated sequence
    CHECK(!ewr::db::WriteKeyFromJson("\x97", bytes));     // a raw byte is not UTF-8

    const std::string p600("\x97\x26\x86\x97\x36\x16\x16\xB6", 8);
    const std::string r220("McLaren\0", 8);

    // database.json's own spelling, so a --find-addresses file pastes back in.
    CHECK(ewr::db::WriteKeyToJson(p600) == "\"\\u0097&\\u0086\\u00976\\u0016\\u0016\\u00b6\"");
    CHECK(ewr::db::WriteKeyToJson(r220) == "\"McLaren\\u0000\"");

    for (const std::string& key : { p600, r220, std::string("Ar\"ka\\noid") })
    {
        std::string back;
        CHECK(ewr::db::WriteKeyFromJson(nlohmann::json::parse(ewr::db::WriteKeyToJson(key)).get<std::string>(), back));
        CHECK(back == key);
    }

    const std::string path = "test_write_key_db.json";
    {
        std::ofstream out(path);
        out << R"({"schema_version": 4, "models": {)"
            << " \"HighKey\": {\"rkey\": 8, \"wkey\": \"\\u0097&\\u0086\\u00976\\u0016\\u0016\\u00b6\", \"addresses\": [24], \"reset\": [0]},"
            << " \"WideKey\": {\"rkey\": 8, \"wkey\": \"Arkan\\u0100id\", \"addresses\": [25], \"reset\": [0]},"
            << " \"WideKey1\": {\"rkey\": 8, \"wkey\": \"Arkanoid\", \"wkey1\": \"Bslbop\\u0100e\","
            << R"( "addresses": [26], "reset": [0]}}})";
    }

    std::string skipped;
    const int sinkId = ewr::log::Default().AddSink([&](const ewr::log::Event& e) {
        if (e.code == "db.entry_skipped")
            skipped += e.message + "\n";
    });

    ewr::Database gen;
    const bool loaded = gen.Load(path);

    ewr::log::Default().RemoveSink(sinkId);
    fs::remove(path);

    // A key with no byte spelling is skipped, never sent as something else.
    CHECK(loaded);
    const auto models = gen.Models();
    CHECK(models.size() == 1);
    CHECK(models.size() == 1 && models[0].name == "HighKey" && models[0].wkey == p600);
    CHECK(skipped.find("'WideKey'") != std::string::npos);
    CHECK(skipped.find("'WideKey1'") != std::string::npos);
}

void test_database_reload_replaces_previous_contents()
{
    std::cout << "[TEST] test_database_reload_replaces_previous_contents" << std::endl;

    const std::string pathA = "test_reload_a.json";
    const std::string pathB = "test_reload_b.json";
    {
        std::ofstream out(pathA);
        out << R"({"ModelA": {"rkey": 8, "wkey": "Arkanoid", "addresses": [24], "reset": [0]}})";
    }
    {
        std::ofstream out(pathB);
        out << R"({"ModelB": {"rkey": 8, "wkey": "Arkanoid", "addresses": [25], "reset": [0]}})";
    }

    ewr::Database gen;
    CHECK(gen.Load(pathA));
    auto models = gen.Models();
    CHECK(models.size() == 1 && models[0].name == "ModelA");

    // A reload (e.g. after an OTA sync) fully replaces the old contents.
    CHECK(gen.Load(pathB));
    models = gen.Models();
    CHECK(models.size() == 1 && models[0].name == "ModelB");

    // A failed reload leaves the previous database untouched.
    CHECK(!gen.Load("no_such_db_file.json"));
    models = gen.Models();
    CHECK(models.size() == 1 && models[0].name == "ModelB");

    fs::remove(pathA);
    fs::remove(pathB);
}

void test_schema4_counter_specs_loading()
{
    std::cout << "[TEST] test_schema4_counter_specs_loading" << std::endl;

    // Shaped after a real L3260 entry: two pads whose overflow nibbles share
    // one byte (0x2F), each with its own service limit.
    std::string test_filename = "test_schema4_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {"SchemaFourModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main",)"
            << R"( "addresses": [48, 49, 50, 51], "reset": [0, 0, 0, 0],)"
            << R"( "counters": [)"
            << R"( {"desc": "Main pad", "max": 6346, "bytes": [48, 49, {"addr": 47, "mask": 15, "weight": 254}]},)"
            << R"( {"desc": "Second pad", "max": 3416, "bytes": [50, 51, {"addr": 47, "mask": 240, "weight": 379}]})"
            << R"( ]}],)"
            << R"( "close": [{"addr": 256, "and": 254}, {"addr": 300, "and": 255, "or": 0}]}}})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 1);
    if (models.size() != 1)
        return;

    const auto& m = models[0];
    const auto counters = m.GetAllCounters();
    CHECK(counters.size() == 2);
    if (counters.size() != 2)
        return;

    // A bare address list is a plain little-endian integer: weights 1, 256.
    CHECK(counters[0].bytes.size() == 3);
    CHECK(counters[0].bytes[0].address == 48 && counters[0].bytes[0].weight == 1);
    CHECK(counters[0].bytes[1].address == 49 && counters[0].bytes[1].weight == 256);
    CHECK(counters[0].bytes[2].address == 47 && counters[0].bytes[2].weight == 254);
    CHECK(counters[0].bytes[2].mask == 0x0F);
    CHECK(counters[0].bytes[2].Shift() == 0);
    CHECK(counters[1].bytes[2].mask == 0xF0);
    CHECK(counters[1].bytes[2].Shift() == 4);
    CHECK(counters[0].HasLimit() && counters[0].max_value == 6346);

    // 0x2F is read but never written, so it only shows up in the read list.
    const auto writeAddrs = m.GetAllAddresses();
    const auto readAddrs = m.GetReadAddresses();
    CHECK(writeAddrs.size() == 4);
    CHECK(readAddrs.size() == 5);
    CHECK(readAddrs[4] == 47);

    // 0x30 = 0x10, 0x31 = 0x01, low nibble of 0x2F = 5.
    const std::vector<std::pair<uint16_t, int>> values = {
        { 48, 0x10 }, { 49, 0x01 }, { 50, 0x00 }, { 51, 0x00 }, { 47, 0x35 }
    };

    const auto main = counters[0].Evaluate(values);
    CHECK(main.complete);
    CHECK(main.value == 16 + 256 + (5 * 254)); // 1542
    CHECK(main.Percent() == 24);

    // The high nibble of the shared byte belongs to the second pad.
    const auto second = counters[1].Evaluate(values);
    CHECK(second.complete);
    CHECK(second.value == 3 * 379); // 1137
    CHECK(second.Percent() == 33);

    // A byte that did not answer makes the whole reading incomplete rather
    // than silently reporting a too-low value.
    const std::vector<std::pair<uint16_t, int>> partial = {
        { 48, 0x10 }, { 49, 0x01 }, { 47, -1 }
    };
    const auto incomplete = counters[0].Evaluate(partial);
    CHECK(!incomplete.complete);
    CHECK(incomplete.Percent() == -1);

    // Past the service limit the percentage is reported as-is, not clamped.
    ewr::CounterSpec small;
    small.max_value = 100;
    ewr::CounterByte only;
    only.address = 48;
    small.bytes.push_back(only);
    const auto over = small.Evaluate({ { 48, 200 } });
    CHECK(over.Percent() == 200);

    // Without a limit there is nothing to turn into a percentage.
    ewr::CounterSpec unlimited;
    unlimited.bytes.push_back(only);
    CHECK(!unlimited.HasLimit());
    CHECK(unlimited.Evaluate({ { 48, 200 } }).Percent() == -1);
}

void test_schema4_close_ops()
{
    std::cout << "[TEST] test_schema4_close_ops" << std::endl;

    std::string test_filename = "test_schema4_close_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {)"
            << R"( "CommitModel": {"rkey": 8, "wkey": "Arkanoid", "mem_high": 4095,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}],)"
            << R"( "close": [{"addr": 256, "and": 254}, {"addr": 300, "and": 255, "or": 0}]},)"
            << R"( "PlainModel": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}]})"
            << R"( }})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 2);

    const ewr::DbPrinterModel* commit = nullptr;
    const ewr::DbPrinterModel* plain = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "CommitModel")
            commit = &m;
        else if (m.name == "PlainModel")
            plain = &m;
    }

    CHECK(commit != nullptr);
    CHECK(plain != nullptr);
    if (!commit || !plain)
        return;

    // The second entry changes nothing, so it must not cost an EEPROM write.
    CHECK(commit->HasCloseOps());
    CHECK(commit->close_ops.size() == 1);
    CHECK(commit->close_ops[0].address == 0x100);
    CHECK(commit->close_ops[0].and_mask == 0xFE);
    CHECK(commit->close_ops[0].or_mask == 0x00);

    // Read-modify-write: clear bit 0, leave everything else alone.
    CHECK(commit->close_ops[0].Apply(0x01) == 0x00);
    CHECK(commit->close_ops[0].Apply(0xFF) == 0xFE);
    CHECK(commit->close_ops[0].Apply(0x2A) == 0x2A); // already committed

    // Models without a commit step keep the plain reset flow.
    CHECK(!plain->HasCloseOps());
    CHECK(plain->GetAllCounters().empty());

    // The commit write is a normal factory write, so a 1-byte model could not
    // even address 0x100 - that guard is shared with the counter writes.
    ewr::DbPrinterModel narrow = *commit;
    narrow.wlen = 1;
    CHECK(!narrow.CanEncodeWriteAddress(narrow.close_ops[0].address));

    // Schema 4 is what this build advertises to the OTA endpoint.
    CHECK(ewr::kMaxSupportedDatabaseSchema == 4);
}

void test_schema4_spec_groups()
{
    std::cout << "[TEST] test_schema4_spec_groups" << std::endl;

    std::string test_filename = "test_schema4_specs_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4,)"
            << R"( "specs": {"L-family": {"rkey": 8, "wkey": "Arkanoid", "wkey1": "Bslbopje", "mem_high": 4095,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48, 49], "reset": [0, 0],)"
            << R"( "counters": [{"desc": "Main pad", "max": 6346, "bytes": [48, 49]}]}],)"
            << R"( "close": [{"addr": 256, "and": 254}]}},)"
            << R"( "models": {)"
            << R"( "Inherited": {"spec": "L-family"},)"
            << R"( "Overridden": {"spec": "L-family", "rkey": 9, "mem_high": 2047},)"
            << R"( "Orphan": {"spec": "Missing-family", "rkey": 10, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Platen Pad Counter", "kind": "platen", "addresses": [24], "reset": [0]}]})"
            << R"( }})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);

    auto models = gen.Models();
    CHECK(models.size() == 3);

    const ewr::DbPrinterModel* inherited = nullptr;
    const ewr::DbPrinterModel* overridden = nullptr;
    const ewr::DbPrinterModel* orphan = nullptr;
    for (const auto& m : models)
    {
        if (m.name == "Inherited")
            inherited = &m;
        else if (m.name == "Overridden")
            overridden = &m;
        else if (m.name == "Orphan")
            orphan = &m;
    }

    CHECK(inherited != nullptr);
    CHECK(overridden != nullptr);
    CHECK(orphan != nullptr);
    if (!inherited || !overridden || !orphan)
        return;

    // Everything comes from the shared group, including schema 4 extras.
    CHECK(inherited->rkey == 8);
    CHECK(inherited->wkey == "Arkanoid");
    CHECK(inherited->wkey1 == "Bslbopje");
    CHECK(inherited->mem_high == 4095);
    CHECK(inherited->GetAllAddresses().size() == 2);
    CHECK(inherited->GetAllCounters().size() == 1);
    CHECK(inherited->HasCloseOps());
    CHECK(inherited->close_ops[0].address == 0x100);

    size_t writes = 0;
    for (const auto& pkt : ewr::ctrl::ResetSequence(*inherited))
        if (ewr::ctrl::IsWrite(pkt))
            writes++;
    CHECK(writes == 2);

    // Own keys win over the inherited ones; the rest is still inherited.
    CHECK(overridden->rkey == 9);
    CHECK(overridden->mem_high == 2047);
    CHECK(overridden->wkey == "Arkanoid");
    CHECK(overridden->GetAllAddresses().size() == 2);
    CHECK(overridden->HasCloseOps());

    // An unknown group must not lose the model: it loads from its own fields.
    CHECK(orphan->rkey == 10);
    CHECK(orphan->IsPlatenOnly());
    CHECK(orphan->GetAllAddresses().size() == 1);
    CHECK(!orphan->HasCloseOps());

    // In the flat form 'specs' is a reserved root key, not a printer name.
    std::string flat_filename = "test_flat_specs_db.json";
    {
        std::ofstream out(flat_filename);
        out << R"({"schema_version": 4,)"
            << R"( "specs": {"L-family": {"rkey": 8, "wkey": "Arkanoid",)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [48], "reset": [0]}]}},)"
            << R"( "FlatModel": {"spec": "L-family"}})";
    }

    ewr::Database flatGen;
    bool flatLoaded = flatGen.Load(flat_filename);
    fs::remove(flat_filename);

    CHECK(flatLoaded);

    auto flatModels = flatGen.Models();
    CHECK(flatModels.size() == 1);
    if (flatModels.size() == 1)
    {
        CHECK(flatModels[0].name == "FlatModel");
        CHECK(flatModels[0].wkey == "Arkanoid");
        CHECK(flatModels[0].GetAllAddresses().size() == 1);
    }
}

void test_log_reporter_and_database_events()
{
    std::cout << "[TEST] test_log_reporter_and_database_events" << std::endl;

    // Reporter mechanics: fan-out, removal, silence without sinks.
    ewr::log::Reporter reporter;
    CHECK(!reporter.HasSinks());
    CHECK(reporter.AddSink(nullptr) == 0);

    std::vector<ewr::log::Event> seen;
    const int sinkId = reporter.AddSink([&](const ewr::log::Event& e) { seen.push_back(e); });
    CHECK(sinkId != 0);
    CHECK(reporter.HasSinks());

    reporter.Log(ewr::log::Level::Warning, ewr::log::Stage::Database, "test.code", "message one");
    CHECK(seen.size() == 1);
    CHECK(seen[0].code == "test.code");
    CHECK(seen[0].level == ewr::log::Level::Warning);
    CHECK(seen[0].stage == ewr::log::Stage::Database);
    CHECK(seen[0].message == "message one");
    CHECK(!seen[0].HasProgress());

    reporter.RemoveSink(sinkId);
    CHECK(!reporter.HasSinks());
    reporter.Log(ewr::log::Level::Error, ewr::log::Stage::General, "test.gone", "not seen");
    CHECK(seen.size() == 1);

    // The database loader reports through the default reporter now: an entry
    // referencing an unknown spec group must produce db.unknown_spec, and the
    // model must still load from its own fields.
    std::vector<std::string> codes;
    const int defaultId = ewr::log::Default().AddSink(
        [&](const ewr::log::Event& e) { codes.push_back(e.code); });

    const std::string test_filename = "test_log_events_db.json";
    fs::remove(test_filename);
    {
        std::ofstream f(test_filename);
        f << R"({
            "schema_version": 4,
            "specs": {
                "known-family": { "rlen": 2, "wlen": 2 }
            },
            "models": {
                "GhostModel": {
                    "spec": "NoSuchSpec",
                    "rkey": 8, "wkey": "Arkanoid",
                    "addresses": [24, 25], "reset": [0, 0]
                }
            }
        })";
    }

    ewr::Database gen;
    CHECK(gen.Load(test_filename));
    fs::remove(test_filename);

    ewr::log::Default().RemoveSink(defaultId);

    bool sawUnknownSpec = false;
    for (const auto& code : codes)
    {
        if (code == "db.unknown_spec")
            sawUnknownSpec = true;
    }
    CHECK(sawUnknownSpec);
}

void test_jig_models_load_and_stay_out_of_the_key_search()
{
    std::cout << "[TEST] test_jig_models_load_and_stay_out_of_the_key_search" << std::endl;

    const std::string path = "test_jig_model.json";
    {
        std::ofstream out(path);
        out << R"({"models": {
            "SX110": {"access": "jig", "rlen": 2, "wlen": 2, "mem_high": 767,
                      "addresses": [532, 533, 534, 535, 536, 537], "reset": [0, 0, 0, 0, 0, 0]},
            "R220": {"rkey": 15120, "wkey": "McLaren", "rlen": 1, "wlen": 1, "addresses": [12, 13], "reset": [0, 0]}
        }})";
    }

    ewr::Database gen;
    CHECK(gen.Load(path));
    fs::remove(path);

    const auto models = gen.Models();
    bool sawJig = false, sawKeyed = false;
    for (const auto& model : models)
    {
        if (model.name == "SX110")
        {
            sawJig = true;
            CHECK(model.jig);
            CHECK(!model.CanReset());
        }
        if (model.name == "R220")
        {
            sawKeyed = true;
            CHECK(!model.jig);
            CHECK(model.CanReset());
        }
    }
    CHECK(sawJig && sawKeyed);

    const auto candidates = ewr::keysearch::CollectCandidates(models);
    CHECK(candidates.size() == 1);
    for (const auto& candidate : candidates)
        for (const auto& name : candidate.models)
            CHECK(name != "SX110");

    // The real database: the SX110 names itself "Epson Stylus SX110" in its
    // device ID, and that has to land on the JIG entry, read-only.
    ewr::Database real;
    CHECK(real.Load("database.json"));
    std::vector<ewr::deviceid::ModelNameEntry> entries;
    for (const auto& model : real.Models())
    {
        entries.push_back({ model.name, model.aliases });
        if (model.name == "SX110")
        {
            CHECK(model.jig);
            CHECK(!model.CanReset());
            CHECK(model.GetReadAddresses().size() == 6);
        }
    }
    const std::vector<std::string> matches = ewr::deviceid::MatchEntries("Epson Stylus SX110", entries);
    CHECK(!matches.empty() && matches[0] == "SX110");
}

void test_ink_groups_loading()
{
    std::cout << "[TEST] test_ink_groups_loading" << std::endl;

    const std::string test_filename = "test_ink_groups_db.json";
    {
        std::ofstream out(test_filename);
        out << R"({"schema_version": 4, "models": {"InkModel": {"rkey": 8, "wkey": "Arkanoid", "rlen": 1, "wlen": 1,)"
            << R"( "pad_groups": [{"desc": "Main Pad Counter", "kind": "main", "addresses": [26, 27], "reset": [0, 0]}],)"
            << R"( "ink_groups": [)"
            << R"({"color": "black", "addresses": [2, 3, 4, 5], "reset": [0, 0, 0, 0]},)"
            << R"({"color": "cyan", "addresses": [14, 15, 16, 17], "reset": [0, 0, 0, 0]})"
            << R"(]}}})";
    }

    ewr::Database gen;
    bool loaded = gen.Load(test_filename);
    fs::remove(test_filename);

    CHECK(loaded);
    auto models = gen.Models();
    CHECK(models.size() == 1);

    if (models.size() == 1)
    {
        const auto& m = models[0];

        // Waste pads and cartridge ink are independent maps: both parse, and
        // the ink map never bleeds into the waste addresses (a waste reset
        // must never touch ink counters, and the reverse).
        CHECK(m.HasResettableCounters());
        CHECK(m.HasInkReset());
        CHECK(m.ink_groups.size() == 2);
        CHECK(m.GetAllAddresses().size() == 2); // waste pads only: 26, 27

        const std::vector<uint16_t> inkAddrs = m.GetInkAddresses();
        CHECK(inkAddrs.size() == 8);
        if (inkAddrs.size() == 8)
        {
            CHECK(inkAddrs[0] == 2);
            CHECK(inkAddrs[4] == 14);
        }

        const std::vector<uint8_t> inkResets = m.GetInkResetValues();
        CHECK(inkResets.size() == 8);
        bool allZero = true;
        for (uint8_t v : inkResets)
            allZero = allZero && (v == 0x00);
        CHECK(allZero);

        CHECK(m.ink_groups[0].color == "black");
        CHECK(m.ink_groups[1].color == "cyan");
    }

    // The common case: a model with no ink_groups reports no ink reset and
    // yields no ink addresses.
    ewr::DbPrinterModel plain;
    CHECK(!plain.HasInkReset());
    CHECK(plain.GetInkAddresses().empty());
}

// A counter's kind is what the counter says about itself, and nothing else.
// A pad group is a set of bytes reset together, not one pad: 489 entries file
// a "Platen Pad Counter" under a group marked main, and 87 counters named only
// by position ("Waste Counter 3") sit in groups marked main, 6 of them alone in
// one. Taking the group's kind labelled the first set wrongly and the second
// set with a guess the database build explicitly refuses to make (#35).
// Asserted over the real database rather than a few models, so the rule holds
// whatever build_db.py regenerates.
void test_every_counter_kind_matches_its_own_description()
{
    std::cout << "[TEST] test_every_counter_kind_matches_its_own_description" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    size_t named = 0;
    size_t platen = 0;
    size_t unnamed = 0;

    for (const auto& model : gen.Models())
    {
        for (const auto& counter : model.GetAllCounters())
        {
            const std::string own = ewr::db::PadKindFromDescription(counter.description);

            if (counter.kind != own)
            {
                std::cout << "  [detail] " << model.name << ": \"" << counter.description
                          << "\" carries kind \"" << counter.kind << "\"" << std::endl;
            }
            CHECK(counter.kind == own);

            if (own.empty())
                ++unnamed;
            else
                ++named;

            if (own == "platen")
                ++platen;
        }
    }

    // Each branch has to have been exercised, or the test passes while
    // checking nothing: named counters, platen ones among them, and counters
    // whose pad nobody knows.
    CHECK(named > 100);
    CHECK(platen > 0);
    CHECK(unnamed > 0);
}

// The shapes from the database, as a caller sees them: a counter named only by
// position reports no kind, whether it shares its group (E-300) or sits in it
// alone (PM-A920) - and whatever the group is marked.
void test_unnamed_counter_reports_no_kind()
{
    std::cout << "[TEST] test_unnamed_counter_reports_no_kind" << std::endl;

    auto counter = [](const char* description, uint16_t address, uint32_t max)
    {
        ewr::CounterSpec spec;
        spec.description = description;
        spec.max_value = max;
        spec.bytes = { ewr::CounterByte{ address, 0xFF, 1 } };
        return spec;
    };

    // E-300: main, platen and a third counter in one group marked main.
    ewr::DbPrinterModel shared;
    shared.name = "SharedGroup";
    ewr::PadGroup group;
    group.description = "Main Pad Counter";
    group.kind = "main";
    group.addresses = { 0x10, 0x28, 0x26 };
    group.counters = { counter("Main Pad Counter", 0x10, 60000),
                       counter("Platen Pad Counter", 0x28, 4177),
                       counter("Waste Counter 3", 0x26, 600) };
    shared.pad_groups.push_back(group);

    const nlohmann::json pads = ewr::json::PadUsage(shared, { { 0x10, 1 }, { 0x28, 2 }, { 0x26, 3 } });
    CHECK(pads.size() == 3);
    CHECK(pads[0]["kind"] == "main");
    CHECK(pads[1]["kind"] == "platen");
    CHECK(pads[2]["kind"].is_null());
    // Unknown pad, but still a counter with a limit: it stays in `pads`.
    CHECK(pads[2]["max"] == 600);

    // PM-A920: the unnamed counter alone in a group marked main.
    ewr::DbPrinterModel lone;
    lone.name = "LoneCounter";
    ewr::PadGroup single;
    single.description = "Main Pad Counter";
    single.kind = "main";
    single.addresses = { 0x30 };
    single.counters = { counter("Waste Counter 3", 0x30, 3260) };
    lone.pad_groups.push_back(single);

    const nlohmann::json lonePads = ewr::json::PadUsage(lone, { { 0x30, 7 } });
    CHECK(lonePads.size() == 1);
    CHECK(lonePads[0]["kind"].is_null());
}

// `pads` says how full the readable pads are; `reset_covers` says which pads a
// reset would clear, readable or not. Without it, 809 of the 1337 resettable
// models hid a pad the reset clears: 669 have no counter at all (an L6490's
// only pad), and 140 reset a pad they cannot read beside one they can (an
// R220's platen pad). The invariants, over every model in the real database:
//   - a model the reset writes to never reports an empty list;
//   - a known pad appears once, however many groups it turns up in;
//   - the readable entries are exactly the pads `pads` can report;
//   - bytes the reset writes that no counter reads show up as their group's pad.
void test_reset_coverage_over_the_database()
{
    std::cout << "[TEST] test_reset_coverage_over_the_database" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    size_t resettable = 0;
    size_t withUnreadable = 0;

    for (const auto& model : gen.Models())
    {
        std::vector<uint16_t> written;
        std::vector<uint16_t> read;
        for (const auto& group : model.pad_groups)
        {
            written.insert(written.end(), group.addresses.begin(), group.addresses.end());
            for (const auto& counter : group.counters)
                for (const auto& part : counter.bytes)
                    read.push_back(part.address);
        }
        if (written.empty())
            continue;

        ++resettable;
        const std::vector<ewr::PadCoverage> covers = model.GetResetCoverage();

        if (covers.empty())
            std::cout << "  [detail] " << model.name << ": resettable, but covers nothing" << std::endl;
        CHECK(!covers.empty());

        size_t mains = 0, platens = 0, unnamedReadable = 0;
        bool anyUnreadable = false;
        for (const auto& pad : covers)
        {
            mains += (pad.kind == "main") ? 1 : 0;
            platens += (pad.kind == "platen") ? 1 : 0;
            unnamedReadable += (pad.kind.empty() && pad.readable) ? 1 : 0;
            anyUnreadable = anyUnreadable || !pad.readable;
        }
        CHECK(mains <= 1);
        CHECK(platens <= 1);
        withUnreadable += anyUnreadable ? 1 : 0;

        // Readable entries against the counters themselves.
        bool mainCounter = false, platenCounter = false;
        size_t unnamedCounters = 0;
        for (const auto& counter : model.GetAllCounters())
        {
            mainCounter = mainCounter || counter.kind == "main";
            platenCounter = platenCounter || counter.kind == "platen";
            unnamedCounters += counter.kind.empty() ? 1 : 0;
        }

        auto readableKind = [&](const char* kind)
        {
            for (const auto& pad : covers)
                if (pad.kind == kind && pad.readable)
                    return true;
            return false;
        };
        CHECK(readableKind("main") == mainCounter);
        CHECK(readableKind("platen") == platenCounter);
        CHECK(unnamedReadable == unnamedCounters);

        // A group whose written bytes no counter reads is still covered.
        for (const auto& group : model.pad_groups)
        {
            bool unread = false;
            for (uint16_t addr : group.addresses)
                unread = unread || std::find(read.begin(), read.end(), addr) == read.end();

            const std::string kind = group.EffectiveKind();
            if (!unread || kind.empty())
                continue;

            bool present = false;
            for (const auto& pad : covers)
                present = present || pad.kind == kind;
            if (!present)
                std::cout << "  [detail] " << model.name << ": group \"" << group.description
                          << "\" has unread bytes but no " << kind << " entry" << std::endl;
            CHECK(present);
        }
    }

    // Both halves of the reason this exists have to be in the data, or the
    // test checks nothing that matters.
    CHECK(resettable > 1000);
    CHECK(withUnreadable > 500);
}

// The shapes each rule came from, as a caller sees them.
void test_reset_coverage_shapes()
{
    std::cout << "[TEST] test_reset_coverage_shapes" << std::endl;

    // A missing entry has to fail its check, not read past the end of the
    // vector and take the rest of the suite down with it.
    auto at = [](const std::vector<ewr::PadCoverage>& covers, size_t i)
    {
        return (i < covers.size()) ? covers[i] : ewr::PadCoverage{ "<missing>", "", false };
    };

    auto counter = [](const char* description, uint16_t address)
    {
        ewr::CounterSpec spec;
        spec.description = description;
        spec.max_value = 100;
        spec.bytes = { ewr::CounterByte{ address, 0xFF, 1 } };
        return spec;
    };
    auto group = [](const char* description, const char* kind, std::vector<uint16_t> addresses)
    {
        ewr::PadGroup g;
        g.description = description;
        g.kind = kind;
        g.addresses = std::move(addresses);
        return g;
    };

    // L6490: its only pad has bytes to reset and no counter to read them.
    {
        ewr::DbPrinterModel m;
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28, 47, 50 }));
        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 1);
        CHECK(at(covers, 0).kind == "platen");
        CHECK(!at(covers, 0).readable);
        CHECK(m.GetAllCounters().empty()); // so `pads_total` is 0
    }

    // R220: a readable main pad, and a platen pad the reset clears blind.
    {
        ewr::DbPrinterModel m;
        auto main = group("Main Pad Counter", "main", { 12, 13 });
        main.counters = { counter("Main Pad Counter", 12) };
        main.counters[0].bytes.push_back(ewr::CounterByte{ 13, 0xFF, 256 });
        m.pad_groups.push_back(main);
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 43, 62, 63 }));

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 2);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && !at(covers, 1).readable);
    }

    // E-300: one group marked main holds all three counters. Its label alone
    // would say the reset covers only main.
    {
        ewr::DbPrinterModel m;
        auto g = group("Main Pad Counter", "main", { 16, 38, 40 });
        g.counters = { counter("Main Pad Counter", 16),
                       counter("Platen Pad Counter", 40),
                       counter("Waste Counter 3", 38) };
        m.pad_groups.push_back(g);

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 3);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && at(covers, 1).readable);
        CHECK(at(covers, 2).kind.empty() && at(covers, 2).readable);
        CHECK(at(covers, 2).name == "Waste Counter 3");
    }

    // BX305FW: the platen counter lives in the main group and the platen group
    // holds more platen bytes nobody reads. Still one platen pad, and readable.
    {
        ewr::DbPrinterModel m;
        auto first = group("Main Pad Counter", "main", { 24, 26 });
        first.counters = { counter("Main Pad Counter", 24), counter("Platen Pad Counter", 26) };
        m.pad_groups.push_back(first);
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28, 46 }));

        const auto covers = m.GetResetCoverage();
        CHECK(covers.size() == 2);
        CHECK(at(covers, 0).kind == "main" && at(covers, 0).readable);
        CHECK(at(covers, 1).kind == "platen" && at(covers, 1).readable);
    }

    // What a caller gets: the key is there whether or not the printer answered,
    // because it comes from the database, not from the reading.
    {
        ewr::DbPrinterModel m;
        m.name = "BlindPlaten";
        m.pad_groups.push_back(group("Platen Pad Counter", "platen", { 28 }));

        ewr::StateSnapshot silent;
        const nlohmann::json data = ewr::json::StateData(m, silent);
        CHECK(data["pads_total"] == 0);
        CHECK(data["pads"].empty());
        CHECK(data["reset_covers"].size() == 1);
        if (data["reset_covers"].size() == 1)
        {
            CHECK(data["reset_covers"][0]["kind"] == "platen");
            CHECK(data["reset_covers"][0]["readable"] == false);
        }
    }
}

// Issue #35. The L3110 splits 0x2F between two counters, one per nibble, and
// the database build used to treat that shared byte as "the same counter" and
// keep only the second. These are the reporter's own bytes.
void test_l3110_shows_both_counters_sharing_a_byte()
{
    std::cout << "[TEST] test_l3110_shows_both_counters_sharing_a_byte" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    const ewr::DbPrinterModel* l3110 = nullptr;
    const std::vector<ewr::DbPrinterModel> models = gen.Models();
    for (const auto& m : models)
    {
        if (m.name == "L3110")
            l3110 = &m;
    }
    CHECK(l3110 != nullptr);
    if (!l3110)
        return;

    const std::vector<std::pair<uint16_t, int>> dump = {
        { 0x1C, 0x00 }, { 0x2F, 0x00 }, { 0x30, 0x4D }, { 0x31, 0x02 },
        { 0x32, 0x00 }, { 0x33, 0x00 }, { 0x34, 0x82 }, { 0x35, 0x04 },
        { 0x36, 0x5E }, { 0x37, 0x5E },
    };

    const auto specs = l3110->GetAllCounters();
    CHECK(specs.size() == 2);

    bool sawLoaded = false, sawEmpty = false;
    for (const auto& spec : specs)
    {
        const ewr::CounterReading r = spec.Evaluate(dump);
        CHECK(r.complete);
        if (r.max_value == 6346 && r.value == 589)
            sawLoaded = true;      // 0x30/0x31 + low nibble of 0x2F: 77 + 2*256
        if (r.max_value == 3416 && r.value == 0)
            sawEmpty = true;       // 0x32/0x33 + high nibble of 0x2F
    }
    CHECK(sawLoaded);
    CHECK(sawEmpty);

    // What the reporter actually asked about: two gauges, named apart, main
    // first. The loaded one is the main pad and the empty one the platen pad.
    if (specs.size() == 2)
    {
        CHECK(specs[0].description == "Main Pad Counter");
        CHECK(specs[0].max_value == 6346);
        CHECK(specs[1].description == "Platen Pad Counter");
        CHECK(specs[1].max_value == 3416);
    }
}

// WicReset never names its counters; EWR names them by position. EP-708A,
// EP-808A and XP-630 are the only models where an upstream source labels the
// pads outright, so they are the check that position really means pad.
void test_counter_names_follow_wicreset_pad_order()
{
    std::cout << "[TEST] test_counter_names_follow_wicreset_pad_order" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    const std::vector<ewr::DbPrinterModel> models = gen.Models();
    for (const std::string name : { "EP-708A", "EP-808A", "XP-630" })
    {
        const ewr::DbPrinterModel* model = nullptr;
        for (const auto& m : models)
        {
            if (m.name == name)
                model = &m;
        }
        CHECK(model != nullptr);
        if (!model)
            continue;

        const auto specs = model->GetAllCounters();
        CHECK(specs.size() == 2);
        if (specs.size() != 2)
            continue;

        // reinkpy: 0x10/0x11 is the main pad, 0x12/0x13 the platen pad.
        CHECK(specs[0].description == "Main Pad Counter");
        CHECK(!specs[0].bytes.empty() && specs[0].bytes[0].address == 0x10);
        CHECK(specs[1].description == "Platen Pad Counter");
        CHECK(!specs[1].bytes.empty() && specs[1].bytes[0].address == 0x12);
    }
}

// 489 models used to draw the same label on two to five gauges.
void test_no_model_repeats_a_gauge_label()
{
    std::cout << "[TEST] test_no_model_repeats_a_gauge_label" << std::endl;

    ewr::Database gen;
    CHECK(gen.Load("database.json"));

    size_t repeating = 0;
    std::string firstOffender;
    for (const auto& model : gen.Models())
    {
        std::vector<std::string> seen;
        bool repeats = false;
        for (const auto& spec : model.GetAllCounters())
        {
            repeats = repeats || std::find(seen.begin(), seen.end(), spec.description) != seen.end();
            seen.push_back(spec.description);
        }
        if (repeats)
        {
            if (repeating++ == 0)
                firstOffender = model.name;
        }
    }

    if (repeating)
        std::cout << "  first model repeating a label: " << firstOffender << std::endl;
    CHECK(repeating == 0);
}

// The database's maintenance facts: present where they should be, and shaped
// so nothing downstream has to guess.
void test_database_maintenance_facts()
{
    std::cout << "[TEST] test_database_maintenance_facts" << std::endl;

    ewr::Database generator;
    CHECK(generator.Load("database.json"));
    const std::vector<ewr::DbPrinterModel> models = generator.Models();
    CHECK(models.size() >= 1450); // a malformed new key must never drop a model

    size_t withCleaning = 0;
    size_t withSerial = 0;
    size_t withReport = 0;
    bool shapes = true;

    for (const auto& model : models)
    {
        if (!model.cleaning.Empty())
            withCleaning++;
        if (!model.serial_addresses.empty())
            withSerial++;
        if (!model.report.empty())
            withReport++;

        for (const auto* levels : { &model.cleaning.clean, &model.cleaning.black,
                                    &model.cleaning.color, &model.cleaning.charge })
        {
            for (const auto& level : *levels)
            {
                shapes = shapes && !level.second.empty() && level.second.size() <= 2
                         && (level.first == "gentle" || level.first == "normal"
                             || level.first == "medium" || level.first == "strong");
            }
        }

        shapes = shapes && model.serial_addresses.size() <= 32;
        for (const auto& counter : model.report)
            shapes = shapes && !counter.key.empty() && !counter.addresses.empty() && counter.addresses.size() <= 4;
        for (const auto& slot : model.fatal)
            shapes = shapes && slot.slot >= 1 && !slot.addresses.empty() && slot.addresses.size() <= 2;
    }

    CHECK(shapes);
    CHECK(withCleaning >= 800);
    CHECK(withSerial >= 700);
    CHECK(withReport >= 800);

    // The R220 this project is verified on: cleaning 0x02, ink charge 0x07,
    // and its head ID bytes are kept out of any full restore.
    for (const auto& model : models)
    {
        if (model.name != "R220")
            continue;
        CHECK(model.cleaning.clean.count("normal") && model.cleaning.clean.at("normal") == std::vector<uint8_t>{ 0x02 });
        CHECK(model.cleaning.charge.count("normal") && model.cleaning.charge.at("normal") == std::vector<uint8_t>{ 0x07 });
        CHECK(std::find(model.identity_addresses.begin(), model.identity_addresses.end(), 0x42)
              != model.identity_addresses.end());
    }
}

// JIG models live under "keyless_models": a pre-1.6 loader, which reads
// database.json from main and only its "models", must never see one there
// and take it for a keyed model with key 0. This build sees all of them.
void test_database_keeps_keyless_models_from_old_loaders()
{
    std::cout << "[TEST] test_database_keeps_keyless_models_from_old_loaders" << std::endl;

    std::ifstream in("database.json");
    const nlohmann::json root = nlohmann::json::parse(in);
    CHECK(root.contains("keyless_models") && root["keyless_models"].is_object());

    bool jigInModels = false;
    for (const auto& item : root["models"].items())
    {
        const nlohmann::json& entry = item.value();
        if (entry.value("access", std::string()) == "jig")
            jigInModels = true;
        if (entry.contains("spec") && root["specs"][entry["spec"].get<std::string>()].value("access", std::string()) == "jig")
            jigInModels = true;
    }
    CHECK(!jigInModels);

    size_t keyless = 0;
    for (const auto& item : root["keyless_models"].items())
    {
        keyless++;
        CHECK(item.value().value("access", std::string()) == "jig");
        CHECK(!root["models"].contains(item.key()));
    }
    CHECK(keyless == 14);

    ewr::Database generator;
    CHECK(generator.Load("database.json"));
    size_t jig = 0;
    for (const auto& model : generator.Models())
        jig += model.jig ? 1 : 0;
    CHECK(jig == keyless);
}
