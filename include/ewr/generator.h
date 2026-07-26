#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

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

    struct PadGroup
    {
        std::string description;
        std::string kind;
        std::vector<uint16_t> addresses;
        std::vector<uint8_t> reset_values;

        std::string EffectiveKind() const
        {
            return kind.empty() ? PadKindFromDescription(description) : kind;
        }
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
    };

    class UniversalGenerator
    {
    public:
        UniversalGenerator() = default;
        ~UniversalGenerator() = default;

        UniversalGenerator(const UniversalGenerator&) = delete;
        UniversalGenerator& operator=(const UniversalGenerator&) = delete;

        bool SyncDatabaseOTA();
        bool LoadDatabase(const std::string& filepath);
        bool IsEmpty() const;
        std::vector<DbPrinterModel> GetAvailableModels() const;
        std::vector<std::vector<unsigned char>> GenerateSequence(const DbPrinterModel& model) const;

    private:
        std::vector<unsigned char> GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value, const std::string& wkey) const;

        std::unordered_map<std::string, DbPrinterModel> database;
    };

} // namespace ewr
