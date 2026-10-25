#pragma once
#include "ewr/gateway.h"
#include "ewr/log.h"
#include "ewr/transport.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace ewr::exec {

    // The banner lines for `context`, one per line, each ending in '\n'.
    // Unknown fields are named as unknown rather than left out.
    std::string DescribeTraceContext(const ExecutorOptions::TraceContext& context);

    // User-facing lines are Info events, trace-log lines are Trace events.
    ExecutionResult RunSequence(ITransport& transport,
                                const std::vector<std::vector<unsigned char>>& sequence,
                                log::Reporter& reporter,
                                const ExecutorOptions& options = {});

    ExecutionResult RunSequence(ITransport& transport,
                                const std::vector<std::vector<unsigned char>>& sequence,
                                std::ostream& out,
                                std::ostream& log,
                                const ExecutorOptions& options = {});

    // Handshake, then a credit pair + query packet per query. Never sends
    // EEPROM writes, so it is always safe to run before a reset.
    QuerySessionResult RunQuerySession(ITransport& transport,
                                       const std::vector<std::vector<unsigned char>>& handshake,
                                       const std::vector<std::vector<unsigned char>>& queries,
                                       log::Reporter& reporter,
                                       const ExecutorOptions& options = {});

    QuerySessionResult RunQuerySession(ITransport& transport,
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
        // See d4::LooksLikeHttpReply: the interface is reachable and answering,
        // it just is not the 1284.4 personality.
        bool httpReply = false;
        size_t writesTotal = 0;
        size_t writesVerified = 0;
        size_t writesRejected = 0;
        // A write was retried with the alternate keyword ('wkey1') after ':42:NG;'.
        bool alternateKeyUsed = false;
        std::string error;
    };

    // Takes an ALREADY-OPEN transport whose D4 handshake stayed silent and
    // sends the ExitPacketMode2 preamble, the DDS flush, then each factory
    // write in END4 framing. success requires every write confirmed with
    // ':42:OK;'; a silent printer yields anyReply == false, never a false
    // success. The read wait is bounded so it cannot wedge the run.
    // Third rung, after D4 and END4: ESC/P Remote mode carrying the same '||'
    // factory commands. Stateless where the other two are not - no handshake,
    // no session, no credit, no packet-mode flush - so it survives a transport
    // that reorders, splits or interleaves, which is what usbprint.sys does
    // when the spooler and Epson's status poller share the handle. Shares
    // End4Result: both are non-D4 direct-control paths with the same outcomes.
    End4Result RunEscRemoteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& factoryWriteCommands,
                                    log::Reporter& reporter,
                                    const ExecutorOptions& options = {});

    End4Result RunEnd4Sequence(ITransport& transport,
                               const std::string& deviceId,
                               const std::vector<std::vector<unsigned char>>& factoryWriteCommands,
                               log::Reporter& reporter,
                               const ExecutorOptions& options = {});

} // namespace ewr::exec
