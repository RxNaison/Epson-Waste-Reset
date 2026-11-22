#pragma once

#include "ewr/gateway.h"
#include "ewr/log.h"
#include "transport/transport.h"

#include <cstdint>
#include <vector>

// The keyless "JIG mode" of Epson's 2008-2012 entry-level platform (SX110 and
// its TX/NX twins; ez-reset's JIG class). These printers do not take the keyed
// '||' factory command at all - an SX110 answered every key with a bare
// '||:;' - and are read and written instead in an ESC/P2 remote mode named
// "JIGMODE", outside IEEE 1284.4 packet mode. The reads come from a WIC Reset
// capture on a real SX110, the write from a reset of the same unit by its
// owner's own script (discussion #48).
namespace ewr {
namespace jig {

    // Leaves IEEE 1284.4 packet mode for plain bytes. END4 sends the same
    // packet with tabs; this is the spaces variant the SX110 was seen to take.
    extern const std::vector<unsigned char> kLeavePacketMode;

    // ESC ( R 08 00 00 "JIGMODE".
    std::vector<unsigned char> BuildEnter();

    // "JGPG" 05 00, then page (address high byte), offset (2, LE) and a byte
    // count (2, LE) of 1. The reply is "JGPG" 01 00 and the byte.
    std::vector<unsigned char> BuildReadPacket(uint16_t address);

    // "JGPS" 06 00, the read's five bytes, then the value. The printer
    // answers nothing: only a read shows whether it landed.
    std::vector<unsigned char> BuildWritePacket(uint16_t address, uint8_t value);

    // "JG" ESC 00 00 00.
    std::vector<unsigned char> BuildExit();

    // The address a read packet asks for; -1 when it is not one.
    int ReadPacketAddress(const std::vector<unsigned char>& packet);

    // False when `packet` is not a one-byte write built by BuildWritePacket.
    bool ParseWritePacket(const std::vector<unsigned char>& packet, uint16_t& address, uint8_t& value);

    // False when `raw` holds no complete "JGPG" reply.
    bool ParseReadReply(const std::vector<unsigned char>& raw, uint8_t& value);

    // One JIG read session: enter packet mode, leave it, enter JIG mode, one
    // read per query (each a BuildReadPacket), leave JIG mode. Replies are
    // positional and raw, like exec::RunQuerySession's. handshakeFailed when
    // the printer answered nothing at all before the first read.
    QuerySessionResult RunReadSession(ITransport& transport,
                                      const std::vector<std::vector<unsigned char>>& queries,
                                      log::Reporter& reporter,
                                      const ExecutorOptions& options = {});

    // One JIG write session, entered as the read session is. Every packet must
    // be a BuildWritePacket, or nothing is sent. No write goes out before a
    // read has answered, and each is read back: a write counts as verified
    // when its byte reads back with the value written. A write that does not
    // read back within options.maxWriteAttempts ends the session there.
    ExecutionResult RunWriteSession(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& writes,
                                    log::Reporter& reporter,
                                    const ExecutorOptions& options = {});

} // namespace jig
} // namespace ewr
