#pragma once
#include "ewr/gateway.h"
#include "ewr/log.h"
#include "transport/transport.h"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

// IEEE 1284.4 (D4) session layer. Replies are length-framed from the
// 6-byte D4 header (timeouts are a safety net, not the framing), the
// control socket comes from GetSocketID("EPSON-CTRL") with the well-known
// socket 2 as fallback, the MTU is negotiated at OpenChannel, and data
// packets block on bidirectional credit accounting.
namespace ewr::d4 {

    constexpr uint8_t CMD_INIT           = 0x00;
    constexpr uint8_t CMD_OPEN_CHANNEL   = 0x01;
    constexpr uint8_t CMD_CLOSE_CHANNEL  = 0x02;
    constexpr uint8_t CMD_CREDIT         = 0x03;
    constexpr uint8_t CMD_CREDIT_REQUEST = 0x04;
    constexpr uint8_t CMD_EXIT           = 0x08;
    constexpr uint8_t CMD_GET_SOCKET_ID  = 0x09;
    constexpr uint8_t CMD_ERROR          = 0x7F;
    constexpr uint8_t REPLY_BIT          = 0x80;
    constexpr uint8_t INIT_REVISION      = 0x10;
    constexpr uint8_t SOCKET_EPSON_CTRL  = 0x02;
    constexpr uint8_t CREDIT             = 0x00; // must stay 0, nonzero locks up the hardware buffer

    std::string DescribeResult(uint8_t code);

    // ---- The packets a generated sequence carries

    // An EPSON-CTRL payload in a data packet for socket 2. The D4 header's
    // length is big-endian; the Epson frames inside are little-endian.
    std::vector<unsigned char> DataPacket(const std::vector<unsigned char>& payload);
    std::vector<std::vector<unsigned char>> Handshake();
    std::vector<unsigned char> CreditGrantPacket();
    std::vector<unsigned char> CreditRequestPacket();

    // ---- Reading raw replies

    // Concatenated data-channel payload bytes, with socket 0 credit/control
    // traffic skipped. Any packet whose psid and ssid match and are non-zero is
    // a data packet: GetSocketID can hand back a socket other than the
    // well-known 2, and this parser sees the negotiated one. Bytes that do not
    // line up with a plausible D4 header are dropped one at a time, so a partial
    // capture still yields what arrived.
    std::vector<unsigned char> ExtractPayload(const std::vector<unsigned char>& raw);

    // False for transaction/handshake packets, which carry no ctrl payload.
    bool ExtractDataPayload(const std::vector<unsigned char>& packet, std::vector<unsigned char>& payload);

    bool IsChannelOpenAck(const std::vector<unsigned char>& ackData);

    // ET-2xxx interface 1 carries two alternate settings on one endpoint pair:
    // alt 0 is 1284.4, alt 1 is IPP-over-USB. Only SET_INTERFACE picks between
    // them and usbprint.sys cannot issue it, so a Windows run can write D4 into
    // an HTTP server and get a 500 back (issue #16).
    bool LooksLikeHttpReply(const unsigned char* data, size_t size);

    inline constexpr const char* kHttpPersonalityError =
        "This interface answered with HTTP (IPP-over-USB), not IEEE 1284.4 - it is the"
        " printer's IPP personality, so the maintenance channel is on another interface.";

    // ---- The session

    struct Packet
    {
        uint8_t psid = 0;
        uint8_t ssid = 0;
        uint8_t credit = 0;
        uint8_t control = 0;
        std::vector<unsigned char> payload;

        bool IsTransaction() const { return psid == 0x00 && ssid == 0x00; }

        // Control bit 0 marks the last packet of a message. Every complete
        // reply in the hardware traces carries it; a printer that splits a
        // reply across packets clears it on all but the last.
        bool IsEndOfMessage() const { return (control & 0x01) != 0; }
    };

    // Leftover bytes stay buffered between calls, so replies split across USB
    // reads reassemble and back-to-back packets are delivered one at a time.
    class Framer
    {
    public:
        explicit Framer(ITransport& transport) : m_transport(transport) {}

        bool ReadPacket(Packet& pkt, int timeoutMs, log::Reporter& reporter);

        bool ReadPacket(Packet& pkt, int timeoutMs, std::ostream& logStream)
        {
            log::Reporter reporter;
            std::ostream* stream = &logStream;
            reporter.AddSink([stream](const log::Event& event)
            {
                if (event.level == log::Level::Trace)
                    (*stream) << event.message << "\n";
            });
            return ReadPacket(pkt, timeoutMs, reporter);
        }

        bool HasBufferedData() const { return !m_buffer.empty(); }

        void DiscardBuffered() { m_buffer.clear(); }

        // Sticky for the life of the framer: the resync below would otherwise
        // discard the status line as unframed junk before anyone saw it.
        bool SawHttpReply() const { return m_sawHttpReply; }

    private:
        ITransport& m_transport;
        std::vector<unsigned char> m_buffer;
        bool m_sawHttpReply = false;
    };

    struct SessionOptions
    {
        // The EJL -> D4 mode switch can take well over a second on some models.
        int replyTimeoutMs = 2000;
        int dataTimeoutMs = 2000;
        int interPacketDelayMs = 0;
        // Window for a continuation packet once a reply has already started.
        // Short on purpose: it is only ever waited out by a fragment that
        // filled the MTU on firmware that does not set end-of-message.
        int fragmentTimeoutMs = 250;
        // Bounds a firmware that never sets end-of-message.
        int maxReplyFragments = 16;
        // How long a fresh session waits, after granting its first credit,
        // for a reply held over from an earlier one. Such a reply comes in the
        // same burst as the credit acknowledgement.
        int heldReplyWindowMs = 250;
        std::string serviceName = "EPSON-CTRL";
    };

    class Session
    {
    public:
        Session(ITransport& transport, log::Reporter& reporter, const SessionOptions& options = {});

        Session(ITransport& transport, std::ostream& logStream, const SessionOptions& options = {});

        // EJL enter -> Init -> GetSocketID -> OpenChannel (+ MTU parse).
        bool Start();
        // Chunked to the negotiated MTU and credit-gated. A reply the printer
        // splits across packets is reassembled: one credit buys one packet, so
        // a fragment that filled the MTU without the end-of-message bit buys
        // another and appends.
        bool Exchange(const std::vector<unsigned char>& payload, std::vector<unsigned char>& reply);
        // Same, without waiting for a reply.
        bool SendData(const std::vector<unsigned char>& payload);
        // Best-effort CloseChannel + Exit. Idempotent, and inert on a session
        // that never started, so it is safe to call from a scope guard.
        void Close();
        // Close, forget every credit count, Start again. The only repair for
        // an exchange that drew no reply: the printer may have spent its
        // credit on a reply that never arrived (another process reading the
        // shared usbprint handle takes it), and then neither side will grant
        // the other more. Only a fresh Init resets both books.
        bool Restart();

        bool ChannelOpen() const { return m_channelOpen; }
        uint8_t Socket() const { return m_socket; }
        uint16_t MtuToPrinter() const { return m_mtuToPrinter; }
        uint16_t MtuToHost() const { return m_mtuToHost; }
        int SendCredit() const { return m_sendCredit; }
        const std::string& LastError() const { return m_lastError; }

        // See LooksLikeHttpReply: separates "wrong personality of the right
        // interface" from a printer that simply never answered.
        bool SawHttpReply() const { return m_framer.SawHttpReply(); }

    private:
        bool SendPacket(uint8_t psid, uint8_t ssid, const std::vector<unsigned char>& payload, uint8_t credit);
        // Absorbs credit/error traffic until the wanted reply code arrives.
        bool WaitFor(uint8_t replyCmd, Packet& pkt, int timeoutMs);
        bool WaitForData(Packet& pkt, int timeoutMs);
        // Credit bookkeeping for every incoming packet.
        void Absorb(const Packet& pkt);
        bool EnsureSendCredit(int timeoutMs);
        bool EnsurePrinterCredit();
        void DiscardHeldReplies();
        bool Fail(const std::string& error);

        ITransport& m_transport;
        Framer m_framer;
        // Data packets that turned up while waiting for a transaction reply -
        // a reply fragment can arrive alongside the credit ack that bought it.
        // Dropping them would hand the caller a truncated reply.
        std::vector<Packet> m_pendingData;
        // Owns the reporter only when constructed over an ostream; otherwise
        // events go to the caller's reporter.
        log::Reporter m_ownedReporter;
        log::Reporter& m_reporter;
        SessionOptions m_options;

        bool m_started = false;
        bool m_closed = false;
        bool m_channelOpen = false;
        uint8_t m_socket = SOCKET_EPSON_CTRL;
        uint16_t m_mtuToPrinter = 0x0040;
        uint16_t m_mtuToHost = 0x0100;
        int m_sendCredit = 0;    // credit we hold for sending data
        int m_printerCredit = 0; // credit we granted the printer
        std::string m_lastError;
    };

    // Session-layer twins of the executor entry points. Generated
    // handshake/credit packets are skipped: the session owns that choreography.
    ExecutionResult RunSequence(ITransport& transport,
                                const std::vector<std::vector<unsigned char>>& sequence,
                                log::Reporter& reporter,
                                const ExecutorOptions& options);

    QuerySessionResult RunQuerySession(ITransport& transport,
                                       const std::vector<std::vector<unsigned char>>& queries,
                                       log::Reporter& reporter,
                                       const ExecutorOptions& options);

} // namespace ewr::d4
