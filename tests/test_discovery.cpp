#include "support.h"

void test_device_id_extraction_and_parsing()
{
    std::cout << "[TEST] test_device_id_extraction_and_parsing" << std::endl;

    const std::string id = "MFG:EPSON;CMD:ESCPL2,BDC,D4;MDL:ET-2800 Series;CLS:PRINTER;DES:EPSON ET-2800 Series;";

    // With the IEEE 1284 two-byte big-endian length prefix.
    std::vector<unsigned char> prefixed;
    prefixed.push_back(0x00);
    prefixed.push_back(static_cast<unsigned char>(id.size() + 2));
    prefixed.insert(prefixed.end(), id.begin(), id.end());
    CHECK(ewr::deviceid::ExtractString(prefixed.data(), prefixed.size()) == id);

    // Without the prefix (driver already stripped it) and with a trailing NUL;
    // garbage after the NUL must be ignored.
    std::vector<unsigned char> bare(id.begin(), id.end());
    bare.push_back('\0');
    bare.push_back('X');
    CHECK(ewr::deviceid::ExtractString(bare.data(), bare.size()) == id);

    CHECK(ewr::deviceid::ExtractString(nullptr, 0).empty());

    const ewr::deviceid::Info info = ewr::deviceid::Parse(id);
    CHECK(info.manufacturer == "EPSON");
    CHECK(info.model == "ET-2800 Series");
    CHECK(info.commandSet == "ESCPL2,BDC,D4");

    // Long-form keys and stray whitespace.
    const ewr::deviceid::Info longForm = ewr::deviceid::Parse(
        "MANUFACTURER: Seiko Epson ; MODEL: Stylus Photo R220 ;");
    CHECK(longForm.manufacturer == "Seiko Epson");
    CHECK(longForm.model == "Stylus Photo R220");
}

void test_device_id_model_matching()
{
    std::cout << "[TEST] test_device_id_model_matching" << std::endl;

    const std::vector<std::string> known = { "ET-2800", "ET-2850", "L3150", "R220", "XP-2200" };

    // Exact family match: the "Series" suffix and punctuation are ignored.
    auto m = ewr::deviceid::MatchNames("ET-2800 Series", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "ET-2800");

    // An "EPSON" prefix is ignored too.
    m = ewr::deviceid::MatchNames("EPSON ET-2850 Series", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "ET-2850");

    // Whole-word partial match inside a marketing name.
    m = ewr::deviceid::MatchNames("Stylus Photo R220", known);
    CHECK(!m.empty());
    if (!m.empty())
        CHECK(m[0] == "R220");

    // No cross-family bleed: ET-2850 must never suggest ET-2800.
    m = ewr::deviceid::MatchNames("ET-2850 Series", known);
    for (const auto& name : m)
        CHECK(name != "ET-2800");

    // No match at all.
    CHECK(ewr::deviceid::MatchNames("WF-7840 Series", known).empty());
    CHECK(ewr::deviceid::MatchNames("", known).empty());

    // ---- Alias-aware matching: the database owns the name table now.
    std::vector<ewr::deviceid::ModelNameEntry> entries;
    entries.push_back({ "ET-2800", { "ET-2800 Series", "ET-2803", "L3260 Series", "L3260" } });
    entries.push_back({ "ET-2850", { "ET-2850 Series" } });
    entries.push_back({ "R220", { "Stylus Photo R220" } });

    // The issue-#16 device: an alias hit resolves to the owning entry.
    auto e = ewr::deviceid::MatchEntries("ET-2803", entries);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "ET-2800");

    // Marketing decoration still normalizes away on aliases.
    e = ewr::deviceid::MatchEntries("EPSON L3260 Series", entries);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "ET-2800");

    // Several aliases of one entry hitting at once yield the entry once.
    e = ewr::deviceid::MatchEntries("ET-2800 Series", entries);
    size_t owners = 0;
    for (const auto& name : e)
    {
        if (name == "ET-2800")
            owners++;
    }
    CHECK(owners == 1);

    // An entry whose own name matches outranks an alias-only match, so an
    // alias never shadows a real model.
    std::vector<ewr::deviceid::ModelNameEntry> shadow;
    shadow.push_back({ "ET-2800", { "L3260" } });
    shadow.push_back({ "L3260", {} });
    e = ewr::deviceid::MatchEntries("L3260", shadow);
    CHECK(!e.empty());
    if (!e.empty())
        CHECK(e[0] == "L3260");

    // Aliases join the no-match rule, not replace it.
    CHECK(ewr::deviceid::MatchEntries("WF-7840 Series", entries).empty());
}

void test_discovery_needs_three_passes()
{
    std::cout << "[TEST] test_discovery_needs_three_passes" << std::endl;

    // Two passes give one interval, and one interval cannot tell a counter
    // from a byte that happened to move once.
    const std::vector<ewr::discovery::EepromSnapshot> two = {
        Snap({ {0x30, 10} }),
        Snap({ {0x30, 11} }),
    };
    CHECK(ewr::discovery::FindTrendingBytes(two).empty());
    CHECK(ewr::discovery::FindTrendingBytes({}).empty());
}

void test_discovery_keeps_only_what_moves_one_way_every_time()
{
    std::cout << "[TEST] test_discovery_keeps_only_what_moves_one_way_every_time" << std::endl;

    const std::vector<ewr::discovery::EepromSnapshot> passes = {
        Snap({ {0x30, 10}, {0x40, 5}, {0x50, 7}, {0x60, 9}, {0x70, 90} }),
        Snap({ {0x30, 14}, {0x40, 9}, {0x50, 7}, {0x60, 3}, {0x70, 80} }),
        Snap({ {0x30, 21}, {0x40, 9}, {0x50, 7}, {0x60, 8}, {0x70, 55} }),
    };

    const std::vector<ewr::discovery::ByteTrend> found = ewr::discovery::FindTrendingBytes(passes);

    // 0x30 rises twice and 0x70 falls twice: both are trends. 0x40 rises then
    // stalls, 0x50 never moves, 0x60 falls then rises - the noise to reject.
    CHECK(found.size() == 2);

    CHECK(found[0].addresses == std::vector<uint16_t>{ 0x30 });
    CHECK(found[0].rising);
    CHECK(found[0].values == std::vector<uint32_t>({ 10, 14, 21 }));
    CHECK(found[0].deltas == std::vector<int64_t>({ 4, 7 }));

    // A falling byte is reported too - it is not a waste counter, and saying
    // so is the reader's job, not this function's.
    CHECK(found[1].addresses == std::vector<uint16_t>{ 0x70 });
    CHECK(!found[1].rising);
    CHECK(found[1].values == std::vector<uint32_t>({ 90, 80, 55 }));
    CHECK(found[1].deltas == std::vector<int64_t>({ -10, -25 }));
}

void test_discovery_reads_a_wrapping_low_byte_as_a_pair()
{
    std::cout << "[TEST] test_discovery_reads_a_wrapping_low_byte_as_a_pair" << std::endl;

    // 0x0130 -> 0x0202 -> 0x02FE. The low byte goes 0x30, 0x02, 0xFE: it breaks
    // its own run at the first interval. Judged alone it is discarded; judged
    // as the pair it belongs to, it is a counter climbing through a rollover.
    const std::vector<ewr::discovery::EepromSnapshot> passes = {
        Snap({ {0x30, 0x30}, {0x31, 0x01} }),
        Snap({ {0x30, 0x02}, {0x31, 0x02} }),
        Snap({ {0x30, 0xFE}, {0x31, 0x02} }),
    };

    const std::vector<ewr::discovery::ByteTrend> found = ewr::discovery::FindTrendingBytes(passes);

    CHECK(found.size() == 1);
    CHECK(found[0].IsPair());
    CHECK(found[0].rising);
    CHECK(found[0].addresses == std::vector<uint16_t>({ 0x30, 0x31 }));
    CHECK(found[0].values == std::vector<uint32_t>({ 0x0130, 0x0202, 0x02FE }));

    // The high byte must not also surface on its own: it is spoken for.
    for (const auto& t : found)
        CHECK(!(t.addresses.size() == 1 && t.addresses[0] == 0x31));
}

void test_discovery_ignores_addresses_that_went_unanswered()
{
    std::cout << "[TEST] test_discovery_ignores_addresses_that_went_unanswered" << std::endl;

    // A -1 is "the printer did not answer", not a low reading. Treating it as
    // one would manufacture a trend out of a failed read.
    const std::vector<ewr::discovery::EepromSnapshot> passes = {
        Snap({ {0x30, -1}, {0x44, 1} }),
        Snap({ {0x30, 50}, {0x44, 2} }),
        Snap({ {0x30, 90}, {0x44, 3} }),
    };

    const std::vector<ewr::discovery::ByteTrend> found = ewr::discovery::FindTrendingBytes(passes);

    CHECK(found.size() == 1);
    CHECK(found[0].addresses == std::vector<uint16_t>{ 0x44 });
}

void test_discovery_json_records_readings_not_a_reset_plan()
{
    std::cout << "[TEST] test_discovery_json_records_readings_not_a_reset_plan" << std::endl;

    const std::vector<ewr::discovery::EepromSnapshot> passes = {
        Snap({ {0x30, 1}, {0x31, 0}, {0x70, 9} }),
        Snap({ {0x30, 2}, {0x31, 0}, {0x70, 6} }),
        Snap({ {0x30, 3}, {0x31, 0}, {0x70, 2} }),
    };

    const std::vector<ewr::discovery::ByteTrend> found = ewr::discovery::FindTrendingBytes(passes);
    CHECK(found.size() == 2);

    const std::string body = "            \"rkey\": 1304,\n";
    const std::string doc = ewr::discovery::ToJson("ET-M1180", body, found);

    CHECK(doc.find("\"schema_version\": 4") != std::string::npos);
    CHECK(doc.find("\"ET-M1180\"") != std::string::npos);
    CHECK(doc.find("\"rkey\": 1304") != std::string::npos);

    // Empty on purpose: nothing observed here is a reset plan, and a populated
    // pad_groups would invite someone to paste it straight into the database.
    CHECK(doc.find("\"pad_groups\": []") != std::string::npos);
    CHECK(doc.find("no reset value") != std::string::npos);

    // Both directions are recorded, with the three readings per address.
    CHECK(doc.find("\"values\": [1, 2, 3]") != std::string::npos);
    CHECK(doc.find("\"direction\": \"rising\"") != std::string::npos);
    CHECK(doc.find("\"values\": [9, 6, 2]") != std::string::npos);
    CHECK(doc.find("\"deltas\": [-3, -4]") != std::string::npos);
    CHECK(doc.find("\"direction\": \"falling\"") != std::string::npos);
}
