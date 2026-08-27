#pragma once
#include "ewr/log.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace ewr {

    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        virtual bool Send(const std::vector<unsigned char>& packet) = 0;
        // timeoutMs bounds how long the FIRST read may wait for the reply to
        // start arriving, not the total call.
        virtual std::vector<unsigned char> Drain(int timeoutMs) = 0;
    };

    struct ExecutorOptions
    {
        int maxWriteAttempts = 3;
        int interPacketDelayMs = 100;
        int retryDelayMs = 200;

        // Everything before the first EEPROM write. The EJL -> D4 mode switch
        // can take well over a second on some models.
        int handshakeDrainTimeoutMs = 2000;
        int writeAckTimeoutMs = 500;
        int drainTimeoutMs = 250;

        // The switches below assume the shape of a generated Smart Protocol
        // sequence (EJL init, D4 init, OpenChannel, then credit-grant /
        // credit-request / write triplets), so they must stay OFF for replay
        // dumps, whose shape is unknown.
        //
        // Fails fast before the first EEPROM write when no 0x81 arrives.
        bool validateHandshake = false;
        // Re-sends the credit pair preceding the write (sequence[i-2], [i-1]).
        bool resendCreditOnRetry = false;
        // When off (replay), success means the dump was delivered, the device
        // replied at least once, and nothing came back ':42:NG;'.
        bool verifyWrites = true;

        // Length-framed reads, negotiated MTU and credit accounting instead
        // of the raw drain-window loop. Replay dumps must keep it off.
        bool useSessionLayer = false;

        // --interface: pin the run to one candidate, identified by its
        // 1-based position in the enumeration order (the number --list
        // shows). Disables the automatic fallback. <= 0 means automatic.
        int interfaceCandidate = 0;

        // --usb-soft-reset (Windows: IOCTL_USBPRINT_SOFT_RESET). Off by
        // default: on ET-2xxx units the reset stalls the next bulk-OUT write
        // and the stall survives close/reopen. A switch so testers can A/B it.
        bool usbSoftResetOnOpen = false;

        // Some models carry a second write keyword ('wkey1'). ':42:NG;' means
        // the keyword did not match, so the executor retries that one write
        // with the alternate. The retry is a tail substitution and is skipped
        // when the keywords differ in length.
        std::string writeKey;
        std::string alternateWriteKey;

        // The ET-28xx, L3xxx and several WF/XP families silently ignore
        // factory EEPROM writes unless first switched into firmware recovery
        // mode ('RCMODE') over a separate D4 service. Data-driven from the
        // database entry; empty means the model needs no such step.
        std::string recoveryService;              // D4 service, e.g. "fwu:ctrl"
        std::vector<unsigned char> recoveryEnter; // enter command, e.g. 67 6D 01 00 01
        std::vector<unsigned char> recoveryClose; // leave command, e.g. 67 6D 01 00 03
        std::vector<unsigned char> recoveryReply; // expected token, e.g. 4F 4B ("OK")
    };

    struct ExecutionResult
    {
        bool success = false;
        size_t packetsSent = 0;
        size_t ackCount = 0;
        size_t writesTotal = 0;
        size_t writesVerified = 0;
        size_t writesRejected = 0;
        bool handshakeConfirmed = false;
        // Nothing was written; callers may retry another interface.
        bool handshakeFailed = false;
        // At least one write drew replies, but ':42:OK;' never arrived.
        bool writesUnverified = false;
        // Worth surfacing: the database's primary keyword is wrong here.
        bool alternateKeyUsed = false;
        std::string error;
    };

    bool IsWritePacket(const std::vector<unsigned char>& packet);
    bool IsEepromWriteOkAck(const std::vector<unsigned char>& ackData);
    bool IsEepromWriteNgAck(const std::vector<unsigned char>& ackData);
    // ':42:NA;' - the firmware understood the write but refuses to apply it
    // in its current state (locked by a foreign error). Retrying is pointless.
    bool IsEepromWriteNaAck(const std::vector<unsigned char>& ackData);
    bool IsChannelOpenAck(const std::vector<unsigned char>& ackData);

    // Rebuilds a write packet (or a bare write payload) with a different
    // trailing keyword. Returns false, leaving `out` untouched, when the input
    // does not end with `from`, when either keyword is empty, or when the two
    // differ in length - the frame length fields would no longer match.
    bool SubstituteTrailingWriteKey(const std::vector<unsigned char>& packet,
                                    const std::string& from,
                                    const std::string& to,
                                    std::vector<unsigned char>& out);

    std::string HexDump(const unsigned char* data, size_t size);

    // A drain may return kMaxDrainBytes, a quarter megabyte of hex per call.
    // The head of a reply is what identifies it.
    inline constexpr size_t kTraceDumpCapBytes = 512;

    std::string HexDumpCapped(const unsigned char* data, size_t size, size_t maxBytes);

    // User-facing lines are Info events, trace-log lines are Trace events.
    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    log::Reporter& reporter,
                                    const ExecutorOptions& options = {});

    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    std::ostream& out,
                                    std::ostream& log,
                                    const ExecutorOptions& options = {});

    struct QuerySessionResult
    {
        bool success = false;
        bool handshakeConfirmed = false;
        // No queries were sent; callers may retry another interface.
        bool handshakeFailed = false;
        size_t packetsSent = 0;
        std::vector<std::vector<unsigned char>> replies;
        std::string error;
    };

    // Handshake, then a credit pair + query packet per query. Never sends
    // EEPROM writes, so it is always safe to run before a reset.
    QuerySessionResult ExecuteQuerySession(ITransport& transport,
                                           const std::vector<std::vector<unsigned char>>& handshake,
                                           const std::vector<std::vector<unsigned char>>& queries,
                                           log::Reporter& reporter,
                                           const ExecutorOptions& options = {});

    QuerySessionResult ExecuteQuerySession(ITransport& transport,
                                           const std::vector<std::vector<unsigned char>>& handshake,
                                           const std::vector<std::vector<unsigned char>>& queries,
                                           std::ostream& out,
                                           std::ostream& log,
                                           const ExecutorOptions& options = {});

    // ---- END4 direct-control fallback --------------------------------------
    //
    // On some composite ET-2xxx units the IEEE 1284.4 handshake stays silent
    // under Windows' usbprint.sys stack - the channel-open reply never
    // returns, so a D4 reset cannot begin. END4 is Epson's proprietary way to
    // carry the same factory EEPROM commands over the raw data line without
    // D4 framing, which those machines still answer. Only attempted after D4
    // has already gone silent; never runs on a printer that speaks D4.

    struct End4Result
    {
        bool success = false;
        // Separates a silent transport from "answered, but not OK".
        bool anyReply = false;
        // Inbound bytes of any framing. Only silence here indicts the transport:
        // `anyReply` is false whenever the framing merely fails to match.
        bool anyBytes = false;
        size_t writesTotal = 0;
        size_t writesVerified = 0;
        size_t writesRejected = 0;
        // A write was retried with the alternate keyword ('wkey1') after ':42:NG;'.
        bool alternateKeyUsed = false;
        std::string error;
    };

    // Strips the 6-byte D4 data header WrapD4DataPacket prepends to every
    // write packet. Handshake and credit packets mean nothing over END4 and
    // are dropped.
    std::vector<std::vector<unsigned char>> ExtractFactoryWriteCommands(
        const std::vector<std::vector<unsigned char>>& sequence);

    // Takes an ALREADY-OPEN transport whose D4 handshake stayed silent and
    // sends the ExitPacketMode2 preamble, the DDS flush, then each factory
    // write in END4 framing. success requires every write confirmed with
    // ':42:OK;'; a silent printer yields anyReply == false, never a false
    // success. The read wait is bounded so it cannot wedge the run.
    End4Result ExecuteEnd4Sequence(ITransport& transport,
                                   const std::string& deviceId,
                                   const std::vector<std::vector<unsigned char>>& factoryWriteCommands,
                                   log::Reporter& reporter,
                                   const ExecutorOptions& options = {});

} // namespace ewr
