#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace ewr {

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

        // Queries are JIG reads (jig.h), sent outside packet mode in the
        // keyless JIG mode. Takes precedence over useSessionLayer.
        bool jigMode = false;

        // --interface: pin the run to one candidate, identified by its
        // 1-based position in the enumeration order (the number --list
        // shows). Disables the automatic fallback. <= 0 means automatic.
        int interfaceCandidate = 0;

        // A printer can stop answering its EPSON-CTRL service for seconds at a
        // time while its D4 core still answers Init and Exit - an L365 ignored
        // GetSocketID and OpenChannel for over eight seconds mid-dump, then
        // recovered by itself (issue #39). So a session that cannot be rebuilt
        // is tried again this many times, waiting sessionRestartBackoffMs
        // before the second attempt and doubling it after each one.
        int sessionRestartAttempts = 4;
        int sessionRestartBackoffMs = 2000;

        // --usb-soft-reset (Windows: IOCTL_USBPRINT_SOFT_RESET), once per
        // candidate before its first session, followed by a wait until the
        // printer has re-initialized. UsbDeviceGateway forwards it to the
        // run's first device session only. A diagnostic switch, off by default.
        bool usbSoftReset = false;

        // Some models carry a second write keyword ('wkey1'). ':42:NG;' means
        // the keyword did not match, so the executor retries that one write
        // with the alternate. The retry is a tail substitution and is skipped
        // when the keywords differ in length.
        std::string writeKey;
        std::string alternateWriteKey;

        // Which printer and database entry this run is about, printed in the
        // banner of every ewr_trace.log session. A trace sent in by email
        // arrives without the console output, and the raw device ID alone
        // does not say which entry was written. Trace text only: nothing here
        // reaches the printer.
        struct TraceContext
        {
            std::string printerReports; // MDL from the IEEE 1284 device ID
            std::string detectedEntry;  // database entry that name matches
            std::string selectedEntry;  // entry the run actually uses
            std::string selectedBy;     // how it was chosen, e.g. "--model \"L565\""
        };
        TraceContext trace;
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
        // A write was retried with the alternate keyword ('wkey1') after
        // ':42:NG;' AND that retry was confirmed. Worth surfacing: the
        // database's primary keyword is wrong here. False when both keywords
        // were rejected - that is a bad database entry, not a fixed one.
        bool alternateKeyUsed = false;
        std::string error;
    };

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

    // One spooler queue as Windows lists it. `ports` is the queue's own
    // comma-separated list: a pooled queue names several.
    struct PrintQueue
    {
        std::string name;
        std::string ports;
        std::string driver;
        // Not yet printed. A job stuck in "Deleting" counts: on an R220 a
        // waiting job stalled every EWR write, and cancelling it left the
        // printer blocked until a power cycle.
        int waitingJobs = 0;
    };

    // The spooler queues on the port of the first interface whose handshake
    // stayed silent. Empty unless the whole run ended silent; Windows
    // usbprint only.
    struct SilentPortQueues
    {
        // "USB001".
        std::string port;
        std::vector<PrintQueue> queues;
    };

    struct ResetRunResult
    {
        bool deviceFound = false;
        size_t candidatesTried = 0;
        // The attempt that ran to completion, or the last one tried.
        ExecutionResult exec;
        SilentPortQueues silentPort;
    };

    struct QueryRunResult
    {
        bool deviceFound = false;
        size_t candidatesTried = 0;
        // The session that ran to completion, or the last one tried.
        QuerySessionResult query;
        SilentPortQueues silentPort;
    };

    struct DeviceIdQueryResult
    {
        bool found = false;
        // Sanitized raw device ID ("MFG:EPSON;...;MDL:ET-2800 Series;...").
        std::string deviceId;
    };

    // A maintenance command - a head cleaning - sent once: no retry, no
    // second interface, whatever came back. A command that drew no reply may
    // still have run; only the printer's status afterwards can say.
    struct CommandRunResult
    {
        bool supported = false;   // this gateway can send one at all
        bool deviceFound = false;
        bool sent = false;        // it left this machine
        std::vector<unsigned char> reply; // as a query reply; empty when none came
        std::string error;
    };

    // Print data (ESC/P2), the way a nozzle check is printed. Nothing comes
    // back from a print channel.
    struct PrintDataResult
    {
        bool supported = false;
        bool deviceFound = false;
        bool sent = false;        // every byte was accepted
        std::string error;
    };

    // The device I/O a session needs, behind an interface so the lifecycle
    // is unit-testable with a scripted fake and USB-driven in production.
    struct IDeviceGateway
    {
        virtual ~IDeviceGateway() = default;

        // For the lines a session prints about how it reaches the printer.
        virtual bool OverNetwork() const { return false; }

        // Why no EEPROM write sent this way could land, once the gateway has
        // seen enough to say so; empty while it can, or cannot tell. A reset
        // stops on it before the first write, since none could be verified.
        virtual std::string EepromUnreachable() const { return {}; }

        virtual QueryRunResult RunQuery(
            const std::vector<std::vector<unsigned char>>& handshake,
            const std::vector<std::vector<unsigned char>>& queries,
            const ExecutorOptions& options) = 0;

        virtual ResetRunResult RunReset(
            const std::vector<std::vector<unsigned char>>& sequence,
            const ExecutorOptions& options) = 0;

        // Not every way to a printer can send these; the defaults say so.
        virtual CommandRunResult RunCommand(const std::vector<unsigned char>& command,
                                            const ExecutorOptions& options)
        {
            (void)command;
            (void)options;
            return {};
        }

        virtual PrintDataResult SendPrintData(const std::vector<unsigned char>& data,
                                              const ExecutorOptions& options)
        {
            (void)data;
            (void)options;
            return {};
        }
    };

} // namespace ewr
