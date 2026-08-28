#pragma once

#include <cstddef>
#include <vector>
#include <string>

namespace ewr {
namespace end4 {

    // Takes the printer out of IEEE 1284.4 packet mode; the first thing an
    // END4 session sends.
    extern const std::vector<unsigned char> kExitPacketMode2;

    // Wraps one control command ("st...", "||...") in END4 packet framing.
    // The length field is a single byte, so `command` must not exceed 241.
    std::vector<unsigned char> BuildEnd4Packet(const std::vector<unsigned char>& command);

    // Wraps a factory command in ESC/P-Remote framing (ESC ( R ... REMOTE1).
    // No transport drives this yet: it is the framing the raw non-D4 path
    // needs, kept alongside END4 because both target the same data line.
    std::vector<unsigned char> BuildEscRemotePacket(const std::vector<unsigned char>& factoryCmd);

    // Strips the 10-byte END4 header. False when the buffer is not a complete
    // END4 packet; outPayload still carries whatever body arrived, and is left
    // empty when the frame declares a length shorter than its own header.
    bool ParseEnd4Response(const std::vector<unsigned char>& rawBytes, std::vector<unsigned char>& outPayload);

    // Number of packet-mode flush bytes (0x11) an END4 session sends after the
    // ExitPacketMode2 preamble. The count is the 'DDS' field of the IEEE 1284
    // device ID, read as hexadecimal (e.g. "DDS:022500;" -> 0x022500). Returns
    // 0 when the field is absent or unparseable, so the caller skips the flush.
    std::size_t ParseDdsFlushLength(const std::string& deviceId);

} // namespace end4
} // namespace ewr
