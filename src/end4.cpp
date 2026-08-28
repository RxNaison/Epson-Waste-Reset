#include "ewr/end4.h"
#include <cstdint>
#include <string>

namespace ewr {
namespace end4 {

    // "\x00\x00\x00\x1b\x01@EJL 1284.4\n@EJL\t\t\t\t\t\n"
    const std::vector<unsigned char> kExitPacketMode2 = {
        0x00, 0x00, 0x00, 0x1b, 0x01, '@', 'E', 'J', 'L', ' ',
        '1', '2', '8', '4', '.', '4', '\n', '@', 'E', 'J', 'L',
        '\t', '\t', '\t', '\t', '\t', '\n'
    };

    // END4 request frame:
    //   0..3   'END4'
    //   4..8   02 01 00 00 00   fixed control block
    //   9      total frame length (header + command), one byte
    //   10..13 00 00 02 00      fixed sub-header
    //   14..   command
    std::vector<unsigned char> BuildEnd4Packet(const std::vector<unsigned char>& command)
    {
        constexpr std::size_t kHeaderSize = 14;

        std::vector<unsigned char> packet = {
            'E', 'N', 'D', '4',
            0x02, 0x01, 0x00, 0x00, 0x00,
            static_cast<unsigned char>((command.size() + kHeaderSize) & 0xFF),
            0x00, 0x00, 0x02, 0x00
        };
        packet.reserve(kHeaderSize + command.size());
        packet.insert(packet.end(), command.begin(), command.end());

        return packet;
    }

    std::vector<unsigned char> BuildEscRemotePacket(const std::vector<unsigned char>& factoryCmd)
    {
        std::vector<unsigned char> packet;

        // INIT: \x1b@ \x1b@
        packet.push_back(0x1b); packet.push_back('@');
        packet.push_back(0x1b); packet.push_back('@');

        // REMOTE_MODE: \x1b(R\x08\x00REMOTE1
        packet.push_back(0x1b); packet.push_back('('); packet.push_back('R');
        packet.push_back(0x08); packet.push_back(0x00);
        packet.push_back('R'); packet.push_back('E'); packet.push_back('M');
        packet.push_back('O'); packet.push_back('T'); packet.push_back('E');
        packet.push_back('1');

        packet.insert(packet.end(), factoryCmd.begin(), factoryCmd.end());

        // EXIT_REMOTE: \x1b\x00\x00\x00 \x1b@
        packet.push_back(0x1b); packet.push_back(0x00); packet.push_back(0x00); packet.push_back(0x00);
        packet.push_back(0x1b); packet.push_back('@');

        return packet;
    }

    bool ParseEnd4Response(const std::vector<unsigned char>& rawBytes, std::vector<unsigned char>& outPayload)
    {
        constexpr std::size_t kReplyHeaderSize = 10;

        outPayload.clear();
        if (rawBytes.size() < kReplyHeaderSize)
            return false;

        if (rawBytes[0] != 'E' || rawBytes[1] != 'N' || rawBytes[2] != 'D' || rawBytes[3] != '4')
            return false;

        const std::size_t expectedLen = rawBytes[9];

        // A frame whose declared total is shorter than its own header is
        // malformed, not truncated: there is no body to point at, and treating
        // the length as an end offset would run it backwards past the start.
        if (expectedLen < kReplyHeaderSize)
            return false;

        if (rawBytes.size() < expectedLen)
        {
            // Truncated: hand back the partial body so the caller can keep
            // draining instead of discarding what already arrived.
            outPayload.assign(rawBytes.begin() + kReplyHeaderSize, rawBytes.end());
            return false;
        }

        outPayload.assign(rawBytes.begin() + kReplyHeaderSize,
                          rawBytes.begin() + static_cast<std::ptrdiff_t>(expectedLen));
        return true;
    }

    std::size_t ParseDdsFlushLength(const std::string& deviceId)
    {
        const std::string key = "DDS:";
        const std::size_t keyPos = deviceId.find(key);
        if (keyPos == std::string::npos)
            return 0;

        const std::size_t valueStart = keyPos + key.size();
        const std::size_t semicolon = deviceId.find(';', valueStart);
        std::string value = (semicolon == std::string::npos)
            ? deviceId.substr(valueStart)
            : deviceId.substr(valueStart, semicolon - valueStart);

        // The field is plain hex digits, but tolerate stray whitespace.
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return 0;
        const std::size_t last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);

        try
        {
            return static_cast<std::size_t>(std::stoul(value, nullptr, 16));
        }
        catch (...)
        {
            return 0;
        }
    }

} // namespace end4
} // namespace ewr
