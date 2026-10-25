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

} // namespace ewr
