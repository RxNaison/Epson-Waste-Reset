#pragma once
#include "ewr/executor.h"
#include "ewr/generator.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace ewr {

    // IEEE 1284.4 (D4) session layer. Replies are length-framed from the
    // 6-byte D4 header (timeouts are a safety net, not the framing), the
    // control socket comes from GetSocketID("EPSON-CTRL") with the well-known
    // socket 2 as fallback, the MTU is negotiated at OpenChannel, and data
    // packets block on bidirectional credit accounting.

    namespace D4 {
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

        std::string DescribeResult(uint8_t code);
    }

    struct D4Packet
    {
        uint8_t psid = 0;
        uint8_t ssid = 0;
        uint8_t credit = 0;
        uint8_t control = 0;
        std::vector<unsigned char> payload;

        bool IsTransaction() const { return psid == 0x00 && ssid == 0x00; }
    };

    // Leftover bytes stay buffered between calls, so replies split across USB
    // reads reassemble and back-to-back packets are delivered one at a time.
    class D4Framer
    {
    public:
        explicit D4Framer(ITransport& transport) : m_transport(transport) {}

        bool ReadPacket(D4Packet& pkt, int timeoutMs, log::Reporter& reporter);

        bool ReadPacket(D4Packet& pkt, int timeoutMs, std::ostream& logStream)
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

    private:
        ITransport& m_transport;
        std::vector<unsigned char> m_buffer;
    };

    struct D4SessionOptions
    {
        // The EJL -> D4 mode switch can take well over a second on some models.
        int replyTimeoutMs = 2000;
        int dataTimeoutMs = 2000;
        int interPacketDelayMs = 0;
        std::string serviceName = "EPSON-CTRL";
    };

    class D4Session
    {
    public:
        D4Session(ITransport& transport, log::Reporter& reporter, const D4SessionOptions& options = {});

        D4Session(ITransport& transport, std::ostream& logStream, const D4SessionOptions& options = {});

        // EJL enter -> Init -> GetSocketID -> OpenChannel (+ MTU parse).
        bool Start();
        // Chunked to the negotiated MTU and credit-gated.
        bool Exchange(const std::vector<unsigned char>& payload, std::vector<unsigned char>& reply);
        // Same, without waiting for a reply.
        bool SendData(const std::vector<unsigned char>& payload);
        // Best-effort CloseChannel + Exit. Idempotent, and inert on a session
        // that never started, so it is safe to call from a scope guard.
        void Close();

        bool ChannelOpen() const { return m_channelOpen; }
        uint8_t Socket() const { return m_socket; }
        uint16_t MtuToPrinter() const { return m_mtuToPrinter; }
        uint16_t MtuToHost() const { return m_mtuToHost; }
        int SendCredit() const { return m_sendCredit; }
        const std::string& LastError() const { return m_lastError; }

    private:
        bool SendPacket(uint8_t psid, uint8_t ssid, const std::vector<unsigned char>& payload, uint8_t credit);
        // Absorbs credit/error traffic until the wanted reply code arrives.
        bool WaitFor(uint8_t replyCmd, D4Packet& pkt, int timeoutMs);
        bool WaitForData(D4Packet& pkt, int timeoutMs);
        // Credit bookkeeping for every incoming packet.
        void Absorb(const D4Packet& pkt);
        bool EnsureSendCredit(int timeoutMs);
        bool EnsurePrinterCredit();
        bool Fail(const std::string& error);

        ITransport& m_transport;
        D4Framer m_framer;
        // Owns the reporter only when constructed over an ostream; otherwise
        // events go to the caller's reporter.
        log::Reporter m_ownedReporter;
        log::Reporter& m_reporter;
        D4SessionOptions m_options;

        bool m_started = false;
        bool m_closed = false;
        bool m_channelOpen = false;
        uint8_t m_socket = EpsonD4::SOCKET_EPSON_CTRL;
        uint16_t m_mtuToPrinter = 0x0040;
        uint16_t m_mtuToHost = 0x0100;
        int m_sendCredit = 0;    // credit we hold for sending data
        int m_printerCredit = 0; // credit we granted the printer
        std::string m_lastError;
    };

    // Session-layer twins of the executor entry points. Generated
    // handshake/credit packets are skipped: the session owns that choreography.
    ExecutionResult ExecuteSequenceD4(ITransport& transport,
                                      const std::vector<std::vector<unsigned char>>& sequence,
                                      log::Reporter& reporter,
                                      const ExecutorOptions& options);

    QuerySessionResult ExecuteQuerySessionD4(ITransport& transport,
                                             const std::vector<std::vector<unsigned char>>& queries,
                                             log::Reporter& reporter,
                                             const ExecutorOptions& options);

    // False for transaction/handshake packets, which carry no ctrl payload.
    bool ExtractDataPayload(const std::vector<unsigned char>& packet, std::vector<unsigned char>& payload);

} // namespace ewr
