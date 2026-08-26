#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <mutex>

namespace ewr {

    namespace EpsonD4
    {
        constexpr uint8_t CMD_EEPROM_WRITE   = 0x42;
        constexpr uint8_t CMD_EEPROM_READ    = 0x41;
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

    // Firmware recovery ('RCMODE') channel. A few families refuse factory
    // EEPROM writes unless the printer is first switched into recovery mode
    // over a separate D4 service ('fwu:ctrl'). Absent for every model that
    // writes fine without it - the common case.
    struct RecoveryChannel
    {
        std::string service;              // D4 service / group name
        std::vector<unsigned char> enter; // command that enters recovery mode
        std::vector<unsigned char> close; // command that leaves recovery mode
        std::vector<unsigned char> reply; // expected acknowledgement token ("OK")

        bool Valid() const { return !service.empty() && !enter.empty(); }
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

    struct DbPrinterModel
    {
        std::string name;
        uint16_t rkey{0};
        std::string wkey;
        std::string wkey1;
        uint16_t rlen{2};
        uint16_t wlen{2};
        uint16_t mem_high{0x7FF};
        std::vector<PadGroup> pad_groups;
        std::vector<CloseOp> close_ops;

        // Independent of pad_groups: a model can reset waste ink, cartridge
        // ink, both, or neither.
        std::vector<InkGroup> ink_groups;

        // Firmware recovery mode: empty for the vast majority of models.
        RecoveryChannel recovery;

        // Marketing and family names ("ET-2800 Series", "L3260") that resolve
        // to this entry. Matching and display only, never the write path.
        std::vector<std::string> aliases;

        // Set by the database build when independent sources disagree about
        // this model's write path (keys, addresses, reset values). The build
        // never silently picks a winner; a host must warn before writing.
        bool conflict{false};

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

        // True when this model must be switched into firmware recovery mode
        // before its EEPROM writes are accepted.
        bool HasRecoveryChannel() const { return recovery.Valid(); }

        std::vector<CounterSpec> GetAllCounters() const
        {
            std::vector<CounterSpec> specs;
            for (const auto& group : pad_groups)
                specs.insert(specs.end(), group.counters.begin(), group.counters.end());

            return specs;
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

    private:
        // Wraps an EPSON-CTRL payload into a D4 data packet (socket 2 -> 2).
        static std::vector<unsigned char> WrapD4DataPacket(const std::vector<unsigned char>& payload);

        mutable std::mutex m_dbMutex;
        std::unordered_map<std::string, DbPrinterModel> database;
    };

} // namespace ewr
