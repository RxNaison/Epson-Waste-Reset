#pragma once
#include "ewr/database.h"
#include "ewr/gateway.h"
#include "ewr/log.h"
#include "ewr/status.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// EPSON-CTRL: the commands sent to the printer's control service and the
// answers that come back, whatever carries them (D4 or SNMP).
namespace ewr::ctrl {

    constexpr uint8_t CMD_EEPROM_WRITE   = 0x42;
    constexpr uint8_t CMD_EEPROM_READ    = 0x41;
    // A run of bytes in one exchange: address, then a byte count.
    constexpr uint8_t CMD_EEPROM_READ_RANGE = 0x51;
    // Head cleaning / ink charge; the one-byte payload is the model's level.
    constexpr uint8_t CMD_CLEAN          = 0x84;
    constexpr uint8_t CMD_RESTART        = 0x21;
    constexpr uint8_t PREFIX_PIPE        = 0x7C;

    // ---- Packets, each in a D4 data packet for the EPSON-CTRL socket

    // Answered with '@BDC ST2'. Needs no keys from the database.
    std::vector<unsigned char> StatusQueryPacket();

    // Action 0x41, answered with '@BDC PS'. Reads need rkey but no write
    // key. `addressLength` follows the model's 'rlen'.
    std::vector<unsigned char> ReadPacket(uint16_t rkey, uint16_t address, uint8_t addressLength = 2);

    // Action 0x42; `addressLength` follows the model's 'wlen'. Public so
    // a caller can rebuild one write with 'wkey1' after a ':42:NG;'.
    std::vector<unsigned char> WritePacket(uint16_t rkey, uint16_t address, uint8_t value,
                                           const std::string& wkey, uint8_t addressLength = 2);

    // Any '||' factory command: read key, the action code with its two
    // check bytes, then `payload`. The read and write builders above stay
    // as they are - they are what every verified reset sent.
    std::vector<unsigned char> FactoryPacket(uint16_t rkey, uint8_t action, const std::vector<unsigned char>& payload);

    // An EPSON-CTRL command: two letters, a little-endian length, the
    // payload ('vi' + 00 asks for the firmware version).
    std::vector<unsigned char> ControlPacket(char first, char second, const std::vector<unsigned char>& payload);

    // Action 0x51: `count` bytes from `address`, answered in one reply.
    std::vector<unsigned char> RangeReadPacket(uint16_t rkey, uint16_t address, uint8_t count,
                                               uint8_t addressLength = 2);

    // Handshake, then a credit-pair + write triplet per pad address.
    std::vector<std::vector<unsigned char>> ResetSequence(const DbPrinterModel& model);

    // ---- What a packet carries

    bool IsWrite(const std::vector<unsigned char>& packet);

    // What a read-only query session may carry: the status ('st'), version
    // ('vi') and counter ('cd') control commands and the two factory reads
    // (0x41, 0x51). A write, a cleaning or a firmware command has a path of
    // its own, so the read-only one refuses them whoever hands them in.
    bool IsReadOnlyQuery(const std::vector<unsigned char>& packet);

    // Rebuilds a write packet (or a bare write payload) with a different
    // trailing keyword. Returns false, leaving `out` untouched, when the input
    // does not end with `from`, when either keyword is empty, or when the two
    // differ in length - the frame length fields would no longer match.
    bool SubstituteTrailingWriteKey(const std::vector<unsigned char>& packet,
                                    const std::string& from,
                                    const std::string& to,
                                    std::vector<unsigned char>& out);

    // Strips the 6-byte D4 data header d4::DataPacket prepends to every
    // write packet. Handshake and credit packets mean nothing over END4 and
    // are dropped.
    std::vector<std::vector<unsigned char>> ExtractWriteCommands(
        const std::vector<std::vector<unsigned char>>& sequence);

    struct Command
    {
        std::vector<unsigned char> payload;
        bool isWrite = false;
    };

    // The data payloads of a generated sequence, in order.
    std::vector<Command> ExtractCommands(const std::vector<std::vector<unsigned char>>& sequence);

    // ---- Replies

    bool IsWriteOk(const std::vector<unsigned char>& ackData);
    bool IsWriteNg(const std::vector<unsigned char>& ackData);
    // ':42:NA;' - the firmware understood the write but refuses to apply it
    // in its current state (locked by a foreign error). Retrying is pointless.
    bool IsWriteNa(const std::vector<unsigned char>& ackData);

    // valid=false when the stream holds no parseable status. A report whose
    // fields run past the end of the payload parses what arrived and sets
    // `truncated`; callers must not read the absence of an error entry in a
    // truncated report as the absence of an error.
    PrinterStatus ParseStatusReply(const std::vector<unsigned char>& raw);

    // Two firmware reply forms exist:
    //   A) '...:41:HH;'  - action-code echo followed by the value (HH).
    //   B) '...EE:AAVV;' - address echo (AA) followed by the value (VV),
    //      seen on Stylus Photo-era firmware (e.g. R220).
    // When expectedAddress is >= 0 and the reply echoes an address (form B),
    // a mismatching echo is rejected. Returns true and sets value on success.
    bool ParseReadReply(const std::vector<unsigned char>& raw, uint8_t& value, int expectedAddress = -1);

    // Action 0x51's answer: `count` bytes from `address`, in either of the
    // forms above - '...:51:HHHH...;' or '...EE:<address echo><bytes>;' with
    // a 1- or 2-byte echo. False unless exactly `count` bytes parse and an
    // echoed address matches, so a reply of another shape is never misread
    // as values.
    bool ParseRangeReadReply(const std::vector<unsigned char>& raw, uint16_t address, uint8_t count,
                             std::vector<uint8_t>& values);

    // `value` is set only for ReadAnswer::Value.
    ReadAnswer ClassifyReadReply(const std::vector<unsigned char>& raw, uint8_t& value, int expectedAddress = -1);

    // The 'vi' answer's version text: "vi:00:<text>;" -> "<text>". Empty on
    // a refusal or another shape.
    std::string ParseVersionReply(const std::vector<unsigned char>& raw);

    // ---- The write loop

    // One EPSON-CTRL command out, its reply back: the step a write sequence
    // repeats, over a D4 session or an SNMP GET. An empty `reply` is an
    // answer that never came, and a later exchange may still be heard; false
    // means none can be, with the reason in `error`.
    using Exchange = std::function<bool(const std::vector<unsigned char>& command,
                                        std::vector<unsigned char>& reply,
                                        std::string& error)>;

    // The one write loop, whatever carries it. With options.verifyWrites
    // every write must come back ':42:OK;': an unconfirmed one is sent again
    // (a write is idempotent), ':42:NG;' earns one retry with the alternate
    // keyword, and ':42:NA;' ends the run. Fills `result` past the handshake.
    void RunCommands(const Exchange& exchange,
                     const std::vector<Command>& commands,
                     log::Reporter& reporter,
                     const ExecutorOptions& options,
                     ExecutionResult& result);

} // namespace ewr::ctrl
