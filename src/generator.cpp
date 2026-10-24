#include "ewr/generator.h"
#include "ewr/updater.h"
#include <cstdio>
#include <sstream>

#include "ewr/log.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ewr {

    namespace {

        std::vector<uint16_t> AddressList(const json& value)
        {
            std::vector<uint16_t> out;
            if (!value.is_array())
                return out;
            for (const auto& a : value)
            {
                if (a.is_number_unsigned() && a.get<unsigned long>() <= 0xFFFF)
                    out.push_back(a.get<uint16_t>());
            }
            return out;
        }

        std::map<std::string, std::vector<uint8_t>> LevelMap(const json& value)
        {
            std::map<std::string, std::vector<uint8_t>> out;
            if (!value.is_object())
                return out;
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                if (!it.value().is_array())
                    continue;
                std::vector<uint8_t> payload;
                for (const auto& b : it.value())
                {
                    if (b.is_number_unsigned() && b.get<unsigned long>() <= 0xFF)
                        payload.push_back(b.get<uint8_t>());
                }
                if (!payload.empty())
                    out[it.key()] = std::move(payload);
            }
            return out;
        }

        void ParseMaintenanceFacts(const json& val, DbPrinterModel& model)
        {
            if (val.contains("cleaning") && val["cleaning"].is_object())
            {
                const json& c = val["cleaning"];
                if (c.contains("clean"))
                    model.cleaning.clean = LevelMap(c["clean"]);
                if (c.contains("black"))
                    model.cleaning.black = LevelMap(c["black"]);
                if (c.contains("color"))
                    model.cleaning.color = LevelMap(c["color"]);
                if (c.contains("charge"))
                    model.cleaning.charge = LevelMap(c["charge"]);
            }

            if (val.contains("serial"))
                model.serial_addresses = AddressList(val["serial"]);

            if (val.contains("identity") && val["identity"].is_object())
            {
                for (auto it = val["identity"].begin(); it != val["identity"].end(); ++it)
                {
                    for (uint16_t a : AddressList(it.value()))
                        model.identity_addresses.push_back(a);
                }
            }

            if (val.contains("report") && val["report"].is_array())
            {
                for (const auto& r : val["report"])
                {
                    if (!r.is_object() || !r.contains("key") || !r["key"].is_string())
                        continue;
                    ReportCounter counter;
                    counter.key = r["key"].get<std::string>();
                    if (r.contains("addresses"))
                        counter.addresses = AddressList(r["addresses"]);
                    if (!counter.addresses.empty() && counter.addresses.size() <= 4)
                        model.report.push_back(std::move(counter));
                }
            }

            if (val.contains("first_use"))
                model.first_use = AddressList(val["first_use"]);

            if (val.contains("fatal") && val["fatal"].is_array())
            {
                for (const auto& f : val["fatal"])
                {
                    if (!f.is_object())
                        continue;
                    FatalSlot slot;
                    slot.kind = (f.contains("kind") && f["kind"].is_string()) ? f["kind"].get<std::string>() : "general";
                    slot.slot = (f.contains("slot") && f["slot"].is_number_integer()) ? f["slot"].get<int>() : 1;
                    if (f.contains("addresses"))
                        slot.addresses = AddressList(f["addresses"]);
                    if (!slot.addresses.empty() && slot.addresses.size() <= 2)
                        model.fatal.push_back(std::move(slot));
                }
            }
        }

    } // namespace

    namespace {

        // Schema 4 'spec' indirection: an entry inherits a shared property
        // group and overrides only what differs, so 1400+ models stop repeating
        // identical key/address tables. The merge is shallow on purpose -
        // redefining 'pad_groups' replaces the inherited list, never merges.
        const json& ResolveSpecInheritance(const json& entry, const json* specs,
                                           json& storage, size_t& unresolved)
        {
            if (!specs || !entry.contains("spec") || !entry["spec"].is_string())
                return entry;

            const auto it = specs->find(entry["spec"].get<std::string>());
            if (it == specs->end() || !it->is_object())
            {
                // Better a partially described model than a missing one.
                unresolved++;
                return entry;
            }

            storage = *it;
            storage.update(entry);
            storage.erase("spec");

            return storage;
        }

    } // namespace

    bool WriteKeyFromJson(const std::string& utf8, std::string& bytes)
    {
        std::string out;
        for (std::size_t i = 0; i < utf8.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            if (c < 0x80)
            {
                out += static_cast<char>(c);
                continue;
            }

            // U+0080..U+00FF are exactly the sequences C2 80..C3 BF.
            if ((c != 0xC2 && c != 0xC3) || i + 1 >= utf8.size())
                return false;

            const unsigned char next = static_cast<unsigned char>(utf8[++i]);
            if ((next & 0xC0) != 0x80)
                return false;

            out += static_cast<char>(((c & 0x1F) << 6) | (next & 0x3F));
        }

        bytes = std::move(out);
        return true;
    }

    std::string WriteKeyToJson(const std::string& bytes)
    {
        std::string out = "\"";
        for (const char ch : bytes)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += ch;
            }
            else if (c < 0x20 || c >= 0x7F)
            {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            }
            else
            {
                out += ch;
            }
        }
        out += '"';
        return out;
    }

    bool UniversalGenerator::LoadDatabase(const std::string& filepath)
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);

        std::ifstream file(filepath);
        if (!file.is_open())
            return false;

        try
        {
            json j;
            file >> j;

            if (!j.is_object())
                return false;

            json& modelsRoot = (j.contains("models") && j["models"].is_object())
                ? j["models"]
                : j;

            const int schemaVersion = j.value("schema_version", 0);
            if (schemaVersion > kMaxSupportedDatabaseSchema)
            {
                std::ostringstream oss;
                oss << "[!] Database schema_version " << schemaVersion
                    << " is newer than this build supports ("
                    << kMaxSupportedDatabaseSchema
                    << "). Loading best-effort; please update EWR at:\n"
                    << "    https://github.com/RxNaison/Epson-Waste-Reset/releases";
                ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                              "db.schema_newer", oss.str());
            }

            // Swap only on success: a failed reload must never leave a
            // half-loaded or merged database behind.
            std::unordered_map<std::string, DbPrinterModel> fresh;
            size_t skippedEntries = 0;
            size_t unresolvedSpecs = 0;

            const json* specsRoot = (j.contains("specs") && j["specs"].is_object())
                ? &j["specs"]
                : nullptr;

            auto skipEntry = [&skippedEntries](const std::string& key, const std::string& why)
            {
                skippedEntries++;
                std::ostringstream oss;
                oss << "[!] Skipping malformed database entry '" << key << "': " << why;
                ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                              "db.entry_skipped", oss.str());
            };

            // Models read in a mode that takes no key ("access": "jig"). Kept
            // apart from "models" so a loader from before 1.6, which reads only
            // that, never takes them for keyed models with a key of zero.
            if (&modelsRoot != &j && j.contains("keyless_models") && j["keyless_models"].is_object())
            {
                for (auto& [key, val] : j["keyless_models"].items())
                {
                    if (!modelsRoot.contains(key))
                        modelsRoot[key] = val;
                }
            }

            for (auto& [key, rawVal] : modelsRoot.items())
            {
                // Reserved root keys of the flat form; never printer names.
                if (key == "specs" || key == "schema_version" || key == "keyless_models")
                    continue;

                if (!rawVal.is_object())
                    continue;

                // One malformed entry must not cost every other model.
                try
                {
                    // Resolve inheritance first so the rest of the parser is
                    // identical for the expanded and compact forms.
                    json resolved;
                    const json& val = ResolveSpecInheritance(rawVal, specsRoot, resolved, unresolvedSpecs);

                    DbPrinterModel model;
                    model.name = key;
                    model.rkey = val.value("rkey", 0);
                    if (!WriteKeyFromJson(val.value("wkey", ""), model.wkey)
                        || !WriteKeyFromJson(val.value("wkey1", ""), model.wkey1))
                    {
                        skipEntry(key, "write keyword is not one byte per character");
                        continue;
                    }
                    model.rlen = val.value("rlen", 2);
                    model.wlen = val.value("wlen", 2);
                    model.mem_high = val.value("mem_high", 2047);
                    model.conflict = val.value("conflict", false);
                    model.jig = val.value("access", std::string()) == "jig";

                        if (val.contains("aliases") && val["aliases"].is_array())
                    {
                        for (auto& alias : val["aliases"])
                        {
                            if (alias.is_string())
                                model.aliases.push_back(alias.get<std::string>());
                        }
                    }

                    if (val.contains("pad_groups") && val["pad_groups"].is_array())
                    {
                        for (auto& pg_json : val["pad_groups"])
                        {
                            PadGroup pg;
                            pg.description = pg_json.value("desc", "");
                            pg.kind = pg_json.value("kind", "");

                            if (pg_json.contains("addresses") && pg_json["addresses"].is_array())
                            {
                                for (auto& addr : pg_json["addresses"])
                                    pg.addresses.push_back(addr.get<uint16_t>());
                            }

                            if (pg_json.contains("reset") && pg_json["reset"].is_array())
                            {
                                for (auto& rst : pg_json["reset"])
                                    pg.reset_values.push_back(rst.get<uint8_t>());
                            }

                            while (pg.reset_values.size() < pg.addresses.size())
                                pg.reset_values.push_back(0x00);

                            // Independent from the write list: a shared nibble
                            // byte is read but never reset on its own.
                            if (pg_json.contains("counters") && pg_json["counters"].is_array())
                            {
                                for (auto& c_json : pg_json["counters"])
                                {
                                    if (!c_json.is_object())
                                        continue;

                                    CounterSpec spec;
                                    spec.description = c_json.value("desc", pg.description);
                                    spec.max_value = c_json.value("max", 0u);

                                    if (c_json.contains("bytes") && c_json["bytes"].is_array())
                                    {
                                        size_t plainIndex = 0;
                                        for (auto& b_json : c_json["bytes"])
                                        {
                                            CounterByte cb;

                                            if (b_json.is_number_unsigned())
                                            {
                                                // A bare address list is a plain
                                                // little-endian integer.
                                                if (plainIndex > 3)
                                                    continue;

                                                cb.address = b_json.get<uint16_t>();
                                                cb.weight = static_cast<uint32_t>(1) << (8 * plainIndex);
                                                plainIndex++;
                                            }
                                            else if (b_json.is_object())
                                            {
                                                cb.address = b_json.value("addr", 0);
                                                cb.mask = static_cast<uint8_t>(b_json.value("mask", 255));
                                                cb.weight = b_json.value("weight", 1u);
                                            }
                                            else
                                            {
                                                continue;
                                            }

                                            if (cb.mask != 0)
                                                spec.bytes.push_back(cb);
                                        }
                                    }

                                    if (!spec.bytes.empty())
                                        pg.counters.push_back(std::move(spec));
                                }
                            }

                            model.pad_groups.push_back(std::move(pg));
                        }
                    }
                    else if (val.contains("addresses") && val["addresses"].is_array())
                    {
                        PadGroup pg;
                        pg.description = "Waste counters";
                        pg.kind = "";

                        for (auto& addr : val["addresses"])
                            pg.addresses.push_back(addr.get<uint16_t>());

                        if (val.contains("reset") && val["reset"].is_array())
                        {
                            for (auto& rst : val["reset"])
                                pg.reset_values.push_back(rst.get<uint8_t>());
                        }

                        while (pg.reset_values.size() < pg.addresses.size())
                            pg.reset_values.push_back(0x00);

                        if (!pg.addresses.empty())
                            model.pad_groups.push_back(std::move(pg));
                    }

                    // Read-modify-write of a flag byte (e.g. 0x100 &= 0xFE),
                    // run after the counter writes. Absent on most models.
                    if (val.contains("close") && val["close"].is_array())
                    {
                        for (auto& c_json : val["close"])
                        {
                            if (!c_json.is_object())
                                continue;

                            CloseOp op;
                            op.address = c_json.value("addr", 0);
                            op.and_mask = static_cast<uint8_t>(c_json.value("and", 255));
                            op.or_mask = static_cast<uint8_t>(c_json.value("or", 0));

                            // An entry that changes nothing costs a needless write.
                            if (op.and_mask != 0xFF || op.or_mask != 0x00)
                                model.close_ops.push_back(op);
                        }
                    }

                    // Independent of pad_groups. A group with no addresses is
                    // skipped, never fatal.
                    if (val.contains("ink_groups") && val["ink_groups"].is_array())
                    {
                        for (auto& ig_json : val["ink_groups"])
                        {
                            if (!ig_json.is_object())
                                continue;

                            InkGroup ig;
                            ig.color = ig_json.value("color", "");

                            if (ig_json.contains("addresses") && ig_json["addresses"].is_array())
                            {
                                for (auto& addr : ig_json["addresses"])
                                    ig.addresses.push_back(addr.get<uint16_t>());
                            }

                            if (ig_json.contains("reset") && ig_json["reset"].is_array())
                            {
                                for (auto& rst : ig_json["reset"])
                                    ig.reset_values.push_back(rst.get<uint8_t>());
                            }

                            while (ig.reset_values.size() < ig.addresses.size())
                                ig.reset_values.push_back(0x00);

                            if (!ig.addresses.empty())
                                model.ink_groups.push_back(std::move(ig));
                        }
                    }

                    // Never what decides whether the model loads: a malformed
                    // maintenance key costs the maintenance, not the reset.
                    try
                    {
                        ParseMaintenanceFacts(val, model);
                    }
                    catch (const json::exception& e)
                    {
                        ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database, "db.maintenance_skipped",
                                      "[!] " + key + ": maintenance data in database.json is malformed and was"
                                      " skipped (" + e.what() + ").");
                        model.cleaning = CleaningSpec();
                        model.serial_addresses.clear();
                        model.identity_addresses.clear();
                        model.report.clear();
                        model.first_use.clear();
                        model.fatal.clear();
                    }

                    fresh[key] = std::move(model);
                }
                catch (const json::exception& e)
                {
                    skipEntry(key, e.what());
                }
            }

            if (unresolvedSpecs > 0)
            {
                std::ostringstream oss;
                oss << "[!] " << unresolvedSpecs
                    << " database entr" << (unresolvedSpecs == 1 ? "y references" : "ies reference")
                    << " an unknown spec group; loaded from their own fields only.";
                ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                              "db.unknown_spec", oss.str());
            }

            if (skippedEntries > 0)
            {
                std::ostringstream oss;
                oss << "[!] " << skippedEntries
                    << " malformed database entr" << (skippedEntries == 1 ? "y was" : "ies were")
                    << " skipped; the remaining models loaded normally.";
                ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                              "db.entries_skipped", oss.str());
            }

            database = std::move(fresh);
            return true;
        }
        catch (const json::exception& e)
        {
            ewr::log::Log(ewr::log::Level::Error, ewr::log::Stage::Database,
                          "db.parse_error", std::string("[!] JSON Parse Error: ") + e.what());
            return false;
        }
    }

    bool UniversalGenerator::IsEmpty() const
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        return database.empty();
    }

    std::vector<DbPrinterModel> UniversalGenerator::GetAvailableModels() const
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        std::vector<DbPrinterModel> models;
        models.reserve(database.size());

        for (const auto& pair : database)
            models.push_back(pair.second);

        return models;
    }

    std::vector<unsigned char> UniversalGenerator::GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value,
                                                                       const std::string& wkey, uint8_t addressLength)
    {
        uint8_t c = EpsonD4::CMD_EEPROM_WRITE;
        uint8_t not_c = ~c & 0xFF;
        uint8_t shift_c = ((c >> 1) & 0x7F) | ((c << 7) & 0x80);

        std::vector<unsigned char> inner;
        inner.push_back(rkey & 0xFF);
        inner.push_back((rkey >> 8) & 0xFF);
        inner.push_back(c);
        inner.push_back(not_c);
        inner.push_back(shift_c);
        // Address field: 1 or 2 bytes, little endian, per the model's 'wlen'.
        inner.push_back(address & 0xFF);
        if (addressLength != 1)
            inner.push_back((address >> 8) & 0xFF);
        inner.push_back(value);
        inner.insert(inner.end(), wkey.begin(), wkey.end());

        std::vector<unsigned char> epson_cmd;
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        uint16_t len = inner.size();
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return WrapD4DataPacket(epson_cmd);
    }

    std::vector<unsigned char> UniversalGenerator::GenerateFactoryPacket(uint16_t rkey, uint8_t action,
                                                                         const std::vector<unsigned char>& payload)
    {
        std::vector<unsigned char> inner;
        inner.push_back(rkey & 0xFF);
        inner.push_back((rkey >> 8) & 0xFF);
        inner.push_back(action);
        inner.push_back(static_cast<unsigned char>(~action & 0xFF));
        inner.push_back(static_cast<unsigned char>(((action >> 1) & 0x7F) | ((action << 7) & 0x80)));
        inner.insert(inner.end(), payload.begin(), payload.end());

        std::vector<unsigned char> epson_cmd;
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        const uint16_t len = static_cast<uint16_t>(inner.size());
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return WrapD4DataPacket(epson_cmd);
    }

    std::vector<unsigned char> UniversalGenerator::GenerateControlPacket(char first, char second,
                                                                         const std::vector<unsigned char>& payload)
    {
        std::vector<unsigned char> command;
        command.push_back(static_cast<unsigned char>(first));
        command.push_back(static_cast<unsigned char>(second));
        command.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
        command.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
        command.insert(command.end(), payload.begin(), payload.end());
        return WrapD4DataPacket(command);
    }

    std::vector<unsigned char> UniversalGenerator::GenerateRangeReadPacket(uint16_t rkey, uint16_t address,
                                                                           uint8_t count, uint8_t addressLength)
    {
        std::vector<unsigned char> payload;
        payload.push_back(address & 0xFF);
        if (addressLength != 1)
            payload.push_back((address >> 8) & 0xFF);
        payload.push_back(count);

        return GenerateFactoryPacket(rkey, EpsonD4::CMD_EEPROM_READ_RANGE, payload);
    }

    std::vector<unsigned char> UniversalGenerator::WrapD4DataPacket(const std::vector<unsigned char>& payload)
    {
        // D4 header uses Big-Endian length; inner Epson frames use Little-Endian length
        std::vector<unsigned char> d4;
        d4.push_back(EpsonD4::SOCKET_EPSON_CTRL);
        d4.push_back(EpsonD4::SOCKET_EPSON_CTRL);
        uint16_t d4_len = static_cast<uint16_t>(payload.size() + 6);
        d4.push_back((d4_len >> 8) & 0xFF);
        d4.push_back(d4_len & 0xFF);
        d4.push_back(EpsonD4::CREDIT);
        d4.push_back(0x00);
        d4.insert(d4.end(), payload.begin(), payload.end());

        return d4;
    }

    std::vector<unsigned char> UniversalGenerator::CreditGrantPacket()
    {
        return { 0x00, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x03,
                 EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0x00, 0x01 };
    }

    std::vector<unsigned char> UniversalGenerator::CreditRequestPacket()
    {
        return { 0x00, 0x00, 0x00, 0x0D, 0x01, 0x00, 0x04,
                 EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0xFF, 0xFF, 0x00, 0x01 };
    }

    std::vector<unsigned char> UniversalGenerator::GenerateStatusQueryPacket()
    {
        // 2-byte command, LE length, payload. 'st' + 0x01 asks for the full
        // '@BDC ST2' status report.
        const std::vector<unsigned char> st_cmd = { 's', 't', 0x01, 0x00, 0x01 };
        return WrapD4DataPacket(st_cmd);
    }

    std::vector<unsigned char> UniversalGenerator::GenerateReadPacket(uint16_t rkey, uint16_t address,
                                                                      uint8_t addressLength)
    {
        uint8_t c = EpsonD4::CMD_EEPROM_READ;
        uint8_t not_c = ~c & 0xFF;
        uint8_t shift_c = ((c >> 1) & 0x7F) | ((c << 7) & 0x80);

        std::vector<unsigned char> inner;
        inner.push_back(rkey & 0xFF);
        inner.push_back((rkey >> 8) & 0xFF);
        inner.push_back(c);
        inner.push_back(not_c);
        inner.push_back(shift_c);
        // Address field: 1 or 2 bytes, little endian, per the model's 'rlen'.
        inner.push_back(address & 0xFF);
        if (addressLength != 1)
            inner.push_back((address >> 8) & 0xFF);

        std::vector<unsigned char> epson_cmd;
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        uint16_t len = static_cast<uint16_t>(inner.size());
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return WrapD4DataPacket(epson_cmd);
    }

    std::vector<std::vector<unsigned char>> UniversalGenerator::GenerateHandshake()
    {
        std::vector<std::vector<unsigned char>> handshake;

        // The leading NULs flush the hardware parser state.
        const unsigned char ejl_init[] = {
            0x00, 0x00, 0x00, 0x1B, 0x01, '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4', '.', '4', '\n',
            '@', 'E', 'J', 'L', '\n',
            '@', 'E', 'J', 'L', '\n'
        };
        handshake.push_back(std::vector<unsigned char>(std::begin(ejl_init), std::end(ejl_init)));

        const unsigned char d4_init[] = {
            0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10
        };
        handshake.push_back(std::vector<unsigned char>(std::begin(d4_init), std::end(d4_init)));

        const unsigned char d4_open[] = {
            0x00, 0x00, 0x00, 0x11, 0x01, 0x00, 0x01,
            EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        handshake.push_back(std::vector<unsigned char>(std::begin(d4_open), std::end(d4_open)));

        return handshake;
    }

    std::vector<std::vector<unsigned char>> UniversalGenerator::GenerateSequence(const DbPrinterModel& model) const
    {
        // Handshake, then a credit-pair + write triplet per address.
        std::vector<std::vector<unsigned char>> sequence = GenerateHandshake();

        size_t skippedAddresses = 0;
        size_t unencodableAddresses = 0;

        for (const auto& group : model.pad_groups)
        {
            for (size_t i = 0; i < group.addresses.size(); ++i)
            {
                // Beyond mem_high is undefined mainboard memory.
                if (group.addresses[i] > model.mem_high)
                {
                    skippedAddresses++;
                    continue;
                }

                // A 1-byte-addressing model would silently target the wrong
                // cell for anything above 0xFF.
                if (!model.CanEncodeWriteAddress(group.addresses[i]))
                {
                    unencodableAddresses++;
                    continue;
                }

                sequence.push_back(CreditGrantPacket());
                sequence.push_back(CreditRequestPacket());
                sequence.push_back(GenerateWritePacket(model.rkey, group.addresses[i], group.reset_values[i],
                                                       model.wkey, model.WriteAddressLength()));
            }
        }

        if (skippedAddresses > 0)
        {
            std::ostringstream oss;
            oss << "[!] " << model.name << ": skipped " << skippedAddresses
                << " EEPROM address(es) above this model's memory limit (mem_high="
                << model.mem_high << "). The database entry may be corrupted.";
            ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                          "db.address_above_limit", oss.str());
        }

        if (unencodableAddresses > 0)
        {
            std::ostringstream oss;
            oss << "[!] " << model.name << ": skipped " << unencodableAddresses
                << " EEPROM address(es) above 0xFF that this model's 1-byte address"
                << " field cannot express (wlen=" << model.wlen
                << "). The database entry may be corrupted.";
            ewr::log::Log(ewr::log::Level::Warning, ewr::log::Stage::Database,
                          "db.address_unencodable", oss.str());
        }

        return sequence;
    }

} // namespace ewr
