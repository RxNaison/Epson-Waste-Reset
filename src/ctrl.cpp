#include "ewr/ctrl.h"
#include "ewr/d4session.h"
#include "ewr/log.h"

#include <sstream>

namespace ewr {

    std::vector<unsigned char> ctrl::WritePacket(uint16_t rkey, uint16_t address, uint8_t value,
                                                                       const std::string& wkey, uint8_t addressLength)
    {
        uint8_t c = ctrl::CMD_EEPROM_WRITE;
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
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        uint16_t len = inner.size();
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return d4::DataPacket(epson_cmd);
    }

    std::vector<unsigned char> ctrl::FactoryPacket(uint16_t rkey, uint8_t action,
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
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        const uint16_t len = static_cast<uint16_t>(inner.size());
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return d4::DataPacket(epson_cmd);
    }

    std::vector<unsigned char> ctrl::ControlPacket(char first, char second,
                                                                         const std::vector<unsigned char>& payload)
    {
        std::vector<unsigned char> command;
        command.push_back(static_cast<unsigned char>(first));
        command.push_back(static_cast<unsigned char>(second));
        command.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
        command.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
        command.insert(command.end(), payload.begin(), payload.end());
        return d4::DataPacket(command);
    }

    std::vector<unsigned char> ctrl::RangeReadPacket(uint16_t rkey, uint16_t address,
                                                                           uint8_t count, uint8_t addressLength)
    {
        std::vector<unsigned char> payload;
        payload.push_back(address & 0xFF);
        if (addressLength != 1)
            payload.push_back((address >> 8) & 0xFF);
        payload.push_back(count);

        return FactoryPacket(rkey, ctrl::CMD_EEPROM_READ_RANGE, payload);
    }

    std::vector<unsigned char> ctrl::StatusQueryPacket()
    {
        // 2-byte command, LE length, payload. 'st' + 0x01 asks for the full
        // '@BDC ST2' status report.
        const std::vector<unsigned char> st_cmd = { 's', 't', 0x01, 0x00, 0x01 };
        return d4::DataPacket(st_cmd);
    }

    std::vector<unsigned char> ctrl::ReadPacket(uint16_t rkey, uint16_t address,
                                                                      uint8_t addressLength)
    {
        uint8_t c = ctrl::CMD_EEPROM_READ;
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
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        epson_cmd.push_back(ctrl::PREFIX_PIPE);
        uint16_t len = static_cast<uint16_t>(inner.size());
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        return d4::DataPacket(epson_cmd);
    }

    std::vector<std::vector<unsigned char>> ctrl::ResetSequence(const DbPrinterModel& model)
    {
        // Handshake, then a credit-pair + write triplet per address.
        std::vector<std::vector<unsigned char>> sequence = d4::Handshake();

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

                sequence.push_back(d4::CreditGrantPacket());
                sequence.push_back(d4::CreditRequestPacket());
                sequence.push_back(WritePacket(model.rkey, group.addresses[i], group.reset_values[i],
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
