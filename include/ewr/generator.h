#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <mutex>

namespace ewr {

    namespace EpsonD4
    {
        constexpr uint8_t CMD_EEPROM_WRITE   = 0x42;
        constexpr uint8_t CMD_EEPROM_READ    = 0x41;
        // A run of bytes in one exchange: address, then a byte count.
        constexpr uint8_t CMD_EEPROM_READ_RANGE = 0x51;
        // Head cleaning / ink charge; the one-byte payload is the model's level.
        constexpr uint8_t CMD_CLEAN          = 0x84;
        constexpr uint8_t CMD_RESTART        = 0x21;
        constexpr uint8_t PREFIX_PIPE        = 0x7C;
        constexpr uint8_t SOCKET_EPSON_CTRL  = 0x02;
        constexpr uint8_t CREDIT             = 0x00; // must stay 0, nonzero locks up the hardware buffer
        constexpr uint8_t REPLY_OPEN_CHANNEL = 0x81;
        constexpr uint8_t REPLY_CREDIT_GRANT = 0x83;
        constexpr uint8_t REPLY_CREDIT_REQ   = 0x84;
    }

    inline std::string PadKindFromDescription(const std::string& d)
    {
        auto has = [&](const char* s) { return d.find(s) != std::string::npos; };

        if (has("Platen") || has("platen"))
            return "platen";

        if (has("Main") || has("main") || d == "Waste counter")
            return "main";

        return "";
    }

    // One EEPROM byte that contributes to a counter value. Several models pack
    // two counters into a single byte (recent L-series keep one nibble per pad
    // at 0x2F), so every part carries its own mask and weight:
    //     value = SUM over parts of (((raw & mask) >> shift) * weight)
    struct CounterByte
    {
        uint16_t address{0};
        uint8_t mask{0xFF};
        uint32_t weight{1};

        // Low zero bits of the mask: 0x0F -> 0, 0xF0 -> 4.
        uint8_t Shift() const
        {
            if (mask == 0)
                return 0;

            uint8_t shift = 0;
            uint8_t m = mask;
            while ((m & 0x01) == 0)
            {
                m >>= 1;
                shift++;
            }

            return shift;
        }

        uint32_t Contribution(uint8_t raw) const
        {
            return static_cast<uint32_t>((raw & mask) >> Shift()) * weight;
        }
    };

    struct CounterSpec
    {
        std::string description;
        // Which pad this counts: "main", "platen", or empty when the counter
        // does not say. Filled by GetAllCounters from the counter's own label,
        // never from its group - see there for why.
        std::string kind;
        std::vector<CounterByte> bytes;
        uint32_t max_value{0}; // 0 = unknown, no percentage can be shown

        bool HasLimit() const { return max_value > 0; }

        std::vector<uint16_t> ReadAddresses() const
        {
            std::vector<uint16_t> addrs;
            for (const auto& b : bytes)
            {
                bool seen = false;
                for (uint16_t known : addrs)
                    seen = seen || (known == b.address);

                if (!seen)
                    addrs.push_back(b.address);
            }

            return addrs;
        }
    };

    // Post-reset commit step. Some firmwares only latch the new counter values
    // once a flag byte is rewritten afterwards (read-modify-write, typically
    // 0x100 &= 0xFE); skipping it lets the printer restore the old values on
    // the next power cycle.
    struct CloseOp
    {
        uint16_t address{0};
        uint8_t and_mask{0xFF};
        uint8_t or_mask{0x00};

        uint8_t Apply(uint8_t current) const
        {
            return static_cast<uint8_t>((current & and_mask) | or_mask);
        }
    };

    struct PadGroup
    {
        std::string description;
        std::string kind;
        std::vector<uint16_t> addresses;
        std::vector<uint8_t> reset_values;
        std::vector<CounterSpec> counters;

        std::string EffectiveKind() const
        {
            return kind.empty() ? PadKindFromDescription(description) : kind;
        }
    };

    // One pad a waste-pad reset covers. `readable` is whether EWR has a
    // counter to report how full it is - not whether this read worked. 809 of
    // the 1337 resettable models reset a pad they cannot read (an L6490's only
    // pad, an R220's platen pad), and a caller that sees only the readable ones
    // would think a reset does nothing there.
    struct PadCoverage
    {
        std::string kind;   // "main", "platen", or empty when nobody knows
        std::string name;   // human label - never parse it
        bool readable{false};
    };

    // One cartridge color's ink-consumption counter: a short run of EEPROM
    // bytes storing USED ink (0x00 = full). On chipped-cartridge models the
    // EEPROM copy is a firmware-managed mirror of the cartridge chip, so a
    // reset only holds where the printer itself accounts the ink. Independent
    // of PadGroup: pad_groups track the printer's own waste pads.
    struct InkGroup
    {
        std::string color;                 // "black", "cyan", "lightmagenta", ...
        std::vector<uint16_t> addresses;   // this color's consumption-counter bytes
        std::vector<uint8_t> reset_values; // parallel to addresses; 0x00 == full
    };

    // What the database knows of a model's own maintenance commands. Each
    // value is the payload of the keyed factory command 0x84, as listed; only
    // a one-byte payload is ever sent, because that is the only form verified
    // on hardware.
    struct CleaningSpec
    {
        // "gentle", "normal", "medium", "strong" -> payload.
        std::map<std::string, std::vector<uint8_t>> clean;  // every color
        std::map<std::string, std::vector<uint8_t>> black;
        std::map<std::string, std::vector<uint8_t>> color;
        std::map<std::string, std::vector<uint8_t>> charge; // ink charge: the power clean

        bool Empty() const { return clean.empty() && black.empty() && color.empty() && charge.empty(); }
    };

    // A counter the printer keeps about itself: pages, cleanings, ink swaps.
    // `key` is the database's ("total_print_page_counter"), never shown as
    // is; the bytes are least significant first, like the waste counters.
    struct ReportCounter
    {
        std::string key;
        std::vector<uint16_t> addresses;
    };

    // Where the printer keeps the code of one of its last fatal errors.
    struct FatalSlot
    {
        std::string kind; // "printer", "scanner", "general", "battery"
        int slot = 1;     // 1 = the most recent
        std::vector<uint16_t> addresses;
    };

    // database.json spells a write keyword as one code point per byte
    // ("\u0097&\u0086..."), but the JSON parser hands back UTF-8, which sent
    // 0x97 as c2 97 and made the SC-P600's 8-byte keyword 12 (#59). False on a
    // code point above U+00FF or malformed UTF-8.
    bool WriteKeyFromJson(const std::string& utf8, std::string& bytes);

    // The inverse: a quoted JSON string literal, in database.json's spelling.
    std::string WriteKeyToJson(const std::string& bytes);

    struct DbPrinterModel
    {
        std::string name;
        uint16_t rkey{0};
        std::string wkey;  // raw keyword bytes, not text
        std::string wkey1;
        uint16_t rlen{2};
        uint16_t wlen{2};
        uint16_t mem_high{0x7FF};
        std::vector<PadGroup> pad_groups;
        std::vector<CloseOp> close_ops;

        // Independent of pad_groups: a model can reset waste ink, cartridge
        // ink, both, or neither.
        std::vector<InkGroup> ink_groups;

        // Maintenance and identity facts. None of them is ever part of
        // a reset: --info reads them, --head-clean sends them, and a full
        // restore refuses to write the identity bytes.
        CleaningSpec cleaning;
        std::vector<uint16_t> serial_addresses;   // ASCII, one character per byte
        std::vector<uint16_t> identity_addresses; // head ID, USB ID, MAC
        std::vector<ReportCounter> report;
        std::vector<uint16_t> first_use;
        std::vector<FatalSlot> fatal;

        // Marketing and family names ("ET-2800 Series", "L3260") that resolve
        // to this entry. Matching and display only, never the write path.
        std::vector<std::string> aliases;

        // Set by the database build when independent sources disagree about
        // this model's write path (keys, addresses, reset values). The build
        // never silently picks a winner; a host must warn before writing.
        bool conflict{false};

        // "access": "jig" - read in the keyless JIG mode (jig.h), not with
        // the keyed '||' commands. rkey/wkey mean nothing here, and no write
        // command is known yet, so these models are read-only.
        bool jig{false};

        // 'rlen'/'wlen' are the byte length of the ADDRESS field in factory
        // read/write commands - not the length of the value. Older models
        // (e.g. Stylus Photo R220) use 1-byte addressing; sending one of them
        // a 2-byte address shifts every field after it, so the value and the
        // keyword are parsed from the wrong offsets. Anything but 1 means 2.
        uint8_t ReadAddressLength() const { return rlen == 1 ? 1 : 2; }
        uint8_t WriteAddressLength() const { return wlen == 1 ? 1 : 2; }

        // A 1-byte-addressing model can only reach 0x00-0xFF; truncating a
        // larger address would write to the wrong cell.
        bool CanEncodeWriteAddress(uint16_t address) const
        {
            return WriteAddressLength() == 2 || address <= 0xFF;
        }

        bool IsPlatenOnly() const
        {
            if (pad_groups.empty())
                return false;

            bool hasPlaten = false;
            for (const auto& group : pad_groups)
            {
                const std::string k = group.EffectiveKind();
                if (k == "platen")
                    hasPlaten = true;
                else
                    return false;
            }

            return hasPlaten;
        }

        bool HasResettableCounters() const
        {
            for (const auto& group : pad_groups)
            {
                if (!group.addresses.empty())
                    return true;
            }

            return false;
        }

        std::vector<uint16_t> GetAllAddresses() const
        {
            std::vector<uint16_t> addrs;
            for (const auto& group : pad_groups)
                addrs.insert(addrs.end(), group.addresses.begin(), group.addresses.end());

            return addrs;
        }

        std::vector<uint8_t> GetAllResetValues() const
        {
            std::vector<uint8_t> resets;
            for (const auto& group : pad_groups)
                resets.insert(resets.end(), group.reset_values.begin(), group.reset_values.end());

            return resets;
        }

        // Per-color equivalents of the waste helpers above.
        bool HasInkReset() const
        {
            for (const auto& group : ink_groups)
            {
                if (!group.addresses.empty())
                    return true;
            }

            return false;
        }

        std::vector<uint16_t> GetInkAddresses() const
        {
            std::vector<uint16_t> addrs;
            for (const auto& group : ink_groups)
                addrs.insert(addrs.end(), group.addresses.begin(), group.addresses.end());

            return addrs;
        }

        std::vector<uint8_t> GetInkResetValues() const
        {
            std::vector<uint8_t> resets;
            for (const auto& group : ink_groups)
                resets.insert(resets.end(), group.reset_values.begin(), group.reset_values.end());

            return resets;
        }

        bool HasCloseOps() const { return !close_ops.empty(); }

        // What a reset can actually be run for. A JIG model keeps its pad
        // groups, so its counters can be read and its coverage shown, but
        // EWR has no JIG write command to run a reset with.
        bool CanReset() const { return !jig && (HasResettableCounters() || HasInkReset()); }

        std::vector<CounterSpec> GetAllCounters() const
        {
            std::vector<CounterSpec> specs;
            for (const auto& group : pad_groups)
            {
                for (CounterSpec spec : group.counters)
                {
                    // A counter is labelled by what it says about itself and by
                    // nothing else. A pad group is a set of bytes reset
                    // together, not one pad: 489 entries file a "Platen Pad
                    // Counter" under a group marked main, and 87 counters named
                    // only by position ("Waste Counter 3") sit in groups marked
                    // main - 6 of them alone in one. Nobody knows their pad,
                    // and the database build refuses to guess it (#35), so the
                    // group's kind would be a guess here too.
                    if (spec.kind.empty())
                        spec.kind = PadKindFromDescription(spec.description);

                    specs.push_back(std::move(spec));
                }
            }

            return specs;
        }

        // The pads a waste-pad reset covers, readable or not.
        //
        // Not the groups' labels: a group is a set of bytes reset together
        // and can mix pads - an E-300's one group marked main also holds the
        // platen counter's bytes. So a pad is covered when the reset writes a
        // byte its counter reads, labelled by the counter's own kind; and bytes
        // the reset writes that no counter reads are covered too, unreadable,
        // labelled by their group. A known kind is one entry however many
        // places it turns up; a counter that names no pad is its own.
        //
        // Ordered main, platen, then the unnamed ones in database order.
        std::vector<PadCoverage> GetResetCoverage() const
        {
            std::vector<uint16_t> written;
            std::vector<uint16_t> read;
            for (const auto& group : pad_groups)
            {
                written.insert(written.end(), group.addresses.begin(), group.addresses.end());
                for (const auto& counter : group.counters)
                {
                    for (const auto& part : counter.bytes)
                        read.push_back(part.address);
                }
            }

            auto contains = [](const std::vector<uint16_t>& list, uint16_t addr)
            {
                for (uint16_t known : list)
                {
                    if (known == addr)
                        return true;
                }
                return false;
            };

            PadCoverage main{ "main", "", false };
            PadCoverage platen{ "platen", "", false };
            bool hasMain = false;
            bool hasPlaten = false;
            std::vector<PadCoverage> unnamed;

            auto add = [&](const std::string& kind, const std::string& name, bool readable)
            {
                PadCoverage* slot = (kind == "main") ? &main : (kind == "platen") ? &platen : nullptr;
                bool* seen = (kind == "main") ? &hasMain : (kind == "platen") ? &hasPlaten : nullptr;

                if (!slot)
                {
                    unnamed.push_back({ "", name, readable });
                    return;
                }

                if (!*seen)
                    slot->name = name;
                slot->readable = slot->readable || readable;
                *seen = true;
            };

            for (const auto& group : pad_groups)
            {
                for (const auto& counter : group.counters)
                {
                    bool touched = false;
                    for (const auto& part : counter.bytes)
                        touched = touched || contains(written, part.address);

                    // A counter the reset writes nothing of is not something it
                    // covers, however readable.
                    if (!touched)
                        continue;

                    const std::string kind = counter.kind.empty()
                        ? PadKindFromDescription(counter.description) : counter.kind;
                    add(kind, counter.description, true);
                }

                bool unread = false;
                for (uint16_t addr : group.addresses)
                    unread = unread || !contains(read, addr);

                if (unread)
                    add(group.EffectiveKind(), group.description, false);
            }

            std::vector<PadCoverage> out;
            if (hasMain)
                out.push_back(main);
            if (hasPlaten)
                out.push_back(platen);
            out.insert(out.end(), unnamed.begin(), unnamed.end());
            return out;
        }

        // Everything worth reading before a reset: the bytes that get written,
        // plus any extra byte a counter needs to be interpreted (shared nibble
        // bytes are usually not in the write list).
        std::vector<uint16_t> GetReadAddresses() const
        {
            std::vector<uint16_t> addrs;
            auto add = [&addrs](uint16_t addr) {
                for (uint16_t known : addrs)
                {
                    if (known == addr)
                        return;
                }
                addrs.push_back(addr);
            };

            for (const auto& group : pad_groups)
            {
                for (uint16_t addr : group.addresses)
                    add(addr);

                for (const auto& spec : group.counters)
                {
                    for (uint16_t addr : spec.ReadAddresses())
                        add(addr);
                }
            }

            return addrs;
        }
    };

    struct CounterReading
    {
        std::string description;
        uint32_t value{0};
        uint32_t max_value{0};
        bool complete{false}; // every byte the counter needs answered

        // -1 when the limit is unknown or a byte is missing. Not clamped: a pad
        // past its service life legitimately reads above 100%.
        int Percent() const
        {
            if (!complete || max_value == 0)
                return -1;

            return static_cast<int>((static_cast<uint64_t>(value) * 100) / max_value);
        }
    };

    // `values` maps EEPROM address -> byte value, with -1 for "no reply".
    inline CounterReading EvaluateCounter(const CounterSpec& spec,
                                          const std::vector<std::pair<uint16_t, int>>& values)
    {
        CounterReading reading;
        reading.description = spec.description;
        reading.max_value = spec.max_value;
        reading.complete = !spec.bytes.empty();

        for (const auto& part : spec.bytes)
        {
            int raw = -1;
            for (const auto& entry : values)
            {
                if (entry.first == part.address)
                {
                    raw = entry.second;
                    break;
                }
            }

            if (raw < 0)
            {
                reading.complete = false;
                continue;
            }

            reading.value += part.Contribution(static_cast<uint8_t>(raw));
        }

        return reading;
    }

    class UniversalGenerator
    {
    public:
        UniversalGenerator() = default;
        ~UniversalGenerator() = default;

        UniversalGenerator(const UniversalGenerator&) = delete;
        UniversalGenerator& operator=(const UniversalGenerator&) = delete;

        bool LoadDatabase(const std::string& filepath);
        bool IsEmpty() const;
        std::vector<DbPrinterModel> GetAvailableModels() const;
        std::vector<std::vector<unsigned char>> GenerateSequence(const DbPrinterModel& model) const;

        static std::vector<std::vector<unsigned char>> GenerateHandshake();
        static std::vector<unsigned char> CreditGrantPacket();
        static std::vector<unsigned char> CreditRequestPacket();
        // Answered with '@BDC ST2'. Needs no keys from the database.
        static std::vector<unsigned char> GenerateStatusQueryPacket();
        // Action 0x41, answered with '@BDC PS'. Reads need rkey but no write
        // key. `addressLength` follows the model's 'rlen'.
        static std::vector<unsigned char> GenerateReadPacket(uint16_t rkey, uint16_t address,
                                                             uint8_t addressLength = 2);
        // Action 0x42; `addressLength` follows the model's 'wlen'. Public so
        // a caller can rebuild one write with 'wkey1' after a ':42:NG;'.
        static std::vector<unsigned char> GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value,
                                                              const std::string& wkey, uint8_t addressLength = 2);

        // Any '||' factory command: read key, the action code with its two
        // check bytes, then `payload`. The read and write builders above stay
        // as they are - they are what every verified reset sent.
        static std::vector<unsigned char> GenerateFactoryPacket(uint16_t rkey, uint8_t action,
                                                                const std::vector<unsigned char>& payload);

        // An EPSON-CTRL command: two letters, a little-endian length, the
        // payload ('vi' + 00 asks for the firmware version).
        static std::vector<unsigned char> GenerateControlPacket(char first, char second,
                                                                const std::vector<unsigned char>& payload);

        // Action 0x51: `count` bytes from `address`, answered in one reply.
        static std::vector<unsigned char> GenerateRangeReadPacket(uint16_t rkey, uint16_t address, uint8_t count,
                                                                  uint8_t addressLength = 2);

    private:
        // Wraps an EPSON-CTRL payload into a D4 data packet (socket 2 -> 2).
        static std::vector<unsigned char> WrapD4DataPacket(const std::vector<unsigned char>& payload);

        mutable std::mutex m_dbMutex;
        std::unordered_map<std::string, DbPrinterModel> database;
    };

} // namespace ewr
