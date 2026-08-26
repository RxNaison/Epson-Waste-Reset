#include "ewr/d4session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace ewr {

    namespace {

        using Clock = std::chrono::steady_clock;

        int RemainingMs(Clock::time_point deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return left > 0 ? static_cast<int>(left) : 0;
        }

        void EmitProgress(log::Reporter& reporter, log::Stage stage, const char* code,
                          size_t index, size_t total, const std::string& message)
        {
            log::Event event;
            event.level = log::Level::Info;
            event.stage = stage;
            event.code = code;
            event.message = message;
            event.index = static_cast<int>(index);
            event.total = static_cast<int>(total);
            reporter.Emit(event);
        }

        void EmitTrace(log::Reporter& reporter, const char* code, const std::string& message)
        {
            reporter.Log(log::Level::Trace, log::Stage::General, code, message);
        }

    } // namespace

    namespace D4 {

        std::string DescribeResult(uint8_t code)
        {
            const char* name = nullptr;
            switch (code)
            {
                case 0x01: name = "malformed or unsupported packet"; break;
                case 0x02: name = "channel not open"; break;
                case 0x03: name = "no credit available"; break;
                case 0x04: name = "resources unavailable"; break;
                default: break;
            }

            char buf[16];
            snprintf(buf, sizeof(buf), "0x%02X", code);

            std::string text(buf);
            if (name)
            {
                text += " (";
                text += name;
                text += ")";
            }

            return text;
        }

    } // namespace D4

    bool D4Framer::ReadPacket(D4Packet& pkt, int timeoutMs, log::Reporter& reporter)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

        for (;;)
        {
            // Bytes [2..3] are the big-endian total length, header included.
            // The timeout bounds only how long bytes may take to arrive; the
            // length decides where the packet ends.
            if (m_buffer.size() >= 4)
            {
                const size_t total = (static_cast<size_t>(m_buffer[2]) << 8) | m_buffer[3];

                if (total < 6)
                {
                    EmitTrace(reporter, "d4.framing_error", "[!] D4 framing error: header announces "
                        + std::to_string(total) + " bytes (minimum is 6). Discarding buffered bytes.");
                    m_buffer.clear();
                    return false;
                }

                if (m_buffer.size() >= total)
                {
                    pkt.psid = m_buffer[0];
                    pkt.ssid = m_buffer[1];
                    pkt.credit = m_buffer[4];
                    pkt.control = m_buffer[5];
                    pkt.payload.assign(m_buffer.begin() + 6, m_buffer.begin() + static_cast<long>(total));
                    m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<long>(total));
                    return true;
                }
            }

            const int remaining = RemainingMs(deadline);
            if (remaining == 0)
                return false;

            const std::vector<unsigned char> chunk = m_transport.Drain(remaining);
            if (!chunk.empty())
            {
                m_buffer.insert(m_buffer.end(), chunk.begin(), chunk.end());
            }
            else if (Clock::now() >= deadline)
            {
                return false;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    D4Session::D4Session(ITransport& transport, log::Reporter& reporter, const D4SessionOptions& options)
        : m_transport(transport)
        , m_framer(transport)
        , m_reporter(reporter)
        , m_options(options)
    {
    }

    D4Session::D4Session(ITransport& transport, std::ostream& logStream, const D4SessionOptions& options)
        : m_transport(transport)
        , m_framer(transport)
        , m_reporter(m_ownedReporter)
        , m_options(options)
    {
        std::ostream* stream = &logStream;
        m_ownedReporter.AddSink([stream](const log::Event& event)
        {
            if (event.level == log::Level::Trace)
                (*stream) << event.message << "\n";
        });
    }

    bool D4Session::Fail(const std::string& error)
    {
        m_lastError = error;
        EmitTrace(m_reporter, "d4.fatal", "[FATAL] " + error);
        return false;
    }

    bool D4Session::SendPacket(uint8_t psid, uint8_t ssid, const std::vector<unsigned char>& payload, uint8_t credit)
    {
        std::vector<unsigned char> raw;
        const uint16_t total = static_cast<uint16_t>(payload.size() + 6);
        raw.push_back(psid);
        raw.push_back(ssid);
        raw.push_back(static_cast<unsigned char>((total >> 8) & 0xFF));
        raw.push_back(static_cast<unsigned char>(total & 0xFF));
        raw.push_back(credit);
        raw.push_back(0x00);
        raw.insert(raw.end(), payload.begin(), payload.end());

        EmitTrace(m_reporter, "d4.tx", "[OUT] D4 packet (" + std::to_string(raw.size()) + " bytes):\n"
              + HexDump(raw.data(), raw.size()));

        if (!m_transport.Send(raw))
            return Fail("Transport failure while sending a D4 packet");

        if (m_options.interPacketDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(m_options.interPacketDelayMs));

        return true;
    }

    void D4Session::Absorb(const D4Packet& pkt)
    {
        if (!pkt.IsTransaction())
        {
            // Consumes credit we granted, and may piggy-back fresh send credit
            // in the header's credit field.
            if (m_printerCredit > 0)
                m_printerCredit--;

            m_sendCredit += pkt.credit;
            return;
        }

        if (pkt.payload.empty())
            return;

        const uint8_t cmd = pkt.payload[0];

        if (cmd == (D4::CMD_CREDIT_REQUEST | D4::REPLY_BIT) && pkt.payload.size() >= 6)
        {
            const int granted = (pkt.payload[4] << 8) | pkt.payload[5];
            m_sendCredit += granted;
            EmitTrace(m_reporter, "d4.credit", "[i] Credit granted: " + std::to_string(granted)
                + " (send credit now " + std::to_string(m_sendCredit) + ")");
        }
        else if (cmd == D4::CMD_CREDIT && pkt.payload.size() >= 5)
        {
            // Printer-initiated credit grant.
            const int granted = (pkt.payload[3] << 8) | pkt.payload[4];
            m_sendCredit += granted;
            EmitTrace(m_reporter, "d4.credit", "[i] Printer granted unsolicited credit: " + std::to_string(granted));
        }
        else if (cmd == D4::CMD_ERROR)
        {
            const uint8_t code = pkt.payload.size() >= 4 ? pkt.payload[3] : 0xFF;
            m_lastError = "Printer sent a D4 error packet: " + D4::DescribeResult(code);
            EmitTrace(m_reporter, "d4.error", "[!] " + m_lastError);
        }
    }

    bool D4Session::WaitFor(uint8_t replyCmd, D4Packet& pkt, int timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

        for (;;)
        {
            D4Packet incoming;
            if (!m_framer.ReadPacket(incoming, RemainingMs(deadline), m_reporter))
                return false;

            EmitTrace(m_reporter, "d4.rx", "[IN]  D4 " + std::string(incoming.IsTransaction() ? "transaction" : "data")
                  + " payload (" + std::to_string(incoming.payload.size()) + " bytes):\n"
                  + HexDump(incoming.payload.data(), incoming.payload.size()));

            Absorb(incoming);

            if (incoming.IsTransaction() && !incoming.payload.empty())
            {
                if (incoming.payload[0] == replyCmd)
                {
                    pkt = incoming;
                    return true;
                }

                if (incoming.payload[0] == D4::CMD_ERROR)
                    return false;
            }

            if (RemainingMs(deadline) == 0)
                return false;
        }
    }

    bool D4Session::WaitForData(D4Packet& pkt, int timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

        for (;;)
        {
            D4Packet incoming;
            if (!m_framer.ReadPacket(incoming, RemainingMs(deadline), m_reporter))
                return false;

            EmitTrace(m_reporter, "d4.rx", "[IN]  D4 " + std::string(incoming.IsTransaction() ? "transaction" : "data")
                  + " payload (" + std::to_string(incoming.payload.size()) + " bytes):\n"
                  + HexDump(incoming.payload.data(), incoming.payload.size()));

            Absorb(incoming);

            if (!incoming.IsTransaction() && incoming.psid == m_socket)
            {
                pkt = incoming;
                return true;
            }

            if (RemainingMs(deadline) == 0)
                return false;
        }
    }

    bool D4Session::Start()
    {
        // 1. EJL escape out of the printing PDL into IEEE 1284.4 mode. Some
        //    firmware answers with one short transaction packet; absorb it.
        {
            const std::vector<std::vector<unsigned char>> handshake = UniversalGenerator::GenerateHandshake();
            const std::vector<unsigned char>& ejl = handshake[0];

            EmitTrace(m_reporter, "d4.tx", "[OUT] EJL enter (" + std::to_string(ejl.size()) + " bytes):\n"
                  + HexDump(ejl.data(), ejl.size()));

            if (!m_transport.Send(ejl))
                return Fail("Transport failure while sending the EJL enter packet");

            D4Packet ignored;
            if (m_framer.ReadPacket(ignored, m_options.replyTimeoutMs, m_reporter))
            {
                EmitTrace(m_reporter, "d4.rx", "[IN]  D4 pre-session reply (" + std::to_string(ignored.payload.size()) + " payload bytes)");
                Absorb(ignored);
            }
        }

        if (!SendPacket(0x00, 0x00, { D4::CMD_INIT, D4::INIT_REVISION }, 0x01))
            return false;

        D4Packet reply;
        if (!WaitFor(D4::CMD_INIT | D4::REPLY_BIT, reply, m_options.replyTimeoutMs))
            return Fail(m_lastError.empty()
                ? "Printer did not answer the D4 Init transaction"
                : m_lastError);

        if (reply.payload.size() >= 2 && reply.payload[1] != 0x00)
            return Fail("D4 Init rejected: result " + D4::DescribeResult(reply.payload[1]));

        // 3. GetSocketID("EPSON-CTRL"). Firmware without it falls back to the
        //    well-known socket 2.
        {
            std::vector<unsigned char> payload;
            payload.push_back(D4::CMD_GET_SOCKET_ID);
            payload.insert(payload.end(), m_options.serviceName.begin(), m_options.serviceName.end());

            if (!SendPacket(0x00, 0x00, payload, 0x01))
                return false;

            D4Packet sock;
            if (WaitFor(D4::CMD_GET_SOCKET_ID | D4::REPLY_BIT, sock, m_options.replyTimeoutMs)
                && sock.payload.size() >= 3
                && sock.payload[1] == 0x00
                && sock.payload[2] != 0x00)
            {
                m_socket = sock.payload[2];
                EmitTrace(m_reporter, "d4.socket", "[i] GetSocketID(\"" + m_options.serviceName + "\") -> socket "
                      + std::to_string(static_cast<int>(m_socket)));
            }
            else
            {
                m_socket = EpsonD4::SOCKET_EPSON_CTRL;
                EmitTrace(m_reporter, "d4.socket", "[i] GetSocketID not answered; using well-known socket "
                      + std::to_string(static_cast<int>(m_socket)));
            }
        }

        // 4. OpenChannel (proposed MTU 256/256, zero initial credit).
        {
            const std::vector<unsigned char> open = {
                D4::CMD_OPEN_CHANNEL, m_socket, m_socket,
                0x01, 0x00, // proposed max packet size, primary -> secondary
                0x01, 0x00, // proposed max packet size, secondary -> primary
                0x00, 0x00, // initial credit
                0x00, 0x00  // maximum outstanding credit (0 = unlimited)
            };

            if (!SendPacket(0x00, 0x00, open, 0x01))
                return false;

            D4Packet openReply;
            if (!WaitFor(D4::CMD_OPEN_CHANNEL | D4::REPLY_BIT, openReply, m_options.replyTimeoutMs))
                return Fail(m_lastError.empty()
                    ? "Printer did not answer OpenChannel (no channel-open reply)"
                    : m_lastError);

            if (openReply.payload.size() >= 2 && openReply.payload[1] != 0x00)
                return Fail("OpenChannel rejected: result " + D4::DescribeResult(openReply.payload[1]));

            // Reply layout: result, psid, ssid, MTU P->S (2, BE),
            // MTU S->P (2, BE), initial credit (2, BE).
            if (openReply.payload.size() >= 8)
            {
                const uint16_t toPrinter = static_cast<uint16_t>((openReply.payload[4] << 8) | openReply.payload[5]);
                const uint16_t toHost = static_cast<uint16_t>((openReply.payload[6] << 8) | openReply.payload[7]);

                if (toPrinter >= 7)
                    m_mtuToPrinter = toPrinter;
                if (toHost >= 7)
                    m_mtuToHost = toHost;
            }

            if (openReply.payload.size() >= 10)
                m_sendCredit += (openReply.payload[8] << 8) | openReply.payload[9];

            m_channelOpen = true;
            EmitTrace(m_reporter, "d4.channel", "[i] Channel open on socket " + std::to_string(static_cast<int>(m_socket))
                  + ": MTU " + std::to_string(m_mtuToPrinter) + " to printer / " + std::to_string(m_mtuToHost)
                  + " to host, initial send credit " + std::to_string(m_sendCredit));
        }

        return true;
    }

    bool D4Session::EnsurePrinterCredit()
    {
        if (m_printerCredit > 0)
            return true;

        if (!SendPacket(0x00, 0x00, { D4::CMD_CREDIT, m_socket, m_socket, 0x00, 0x01 }, 0x01))
            return false;

        D4Packet reply;
        if (!WaitFor(D4::CMD_CREDIT | D4::REPLY_BIT, reply, m_options.replyTimeoutMs))
            return Fail(m_lastError.empty()
                ? "Printer did not acknowledge a credit grant"
                : m_lastError);

        if (reply.payload.size() >= 2 && reply.payload[1] != 0x00)
            return Fail("Credit grant rejected: result " + D4::DescribeResult(reply.payload[1]));

        m_printerCredit += 1;
        return true;
    }

    bool D4Session::EnsureSendCredit(int timeoutMs)
    {
        if (m_sendCredit > 0)
            return true;

        // Block rather than fire data into a zero-credit channel.
        if (!SendPacket(0x00, 0x00, { D4::CMD_CREDIT_REQUEST, m_socket, m_socket, 0xFF, 0xFF, 0x00, 0x01 }, 0x01))
            return false;

        D4Packet reply;
        if (!WaitFor(D4::CMD_CREDIT_REQUEST | D4::REPLY_BIT, reply, timeoutMs))
            return Fail(m_lastError.empty()
                ? "Printer never granted send credit (flow control stalled)"
                : m_lastError);

        // WaitFor -> Absorb already booked the granted amount.
        if (m_sendCredit <= 0)
            return Fail("Printer answered the credit request but granted zero credit");

        return true;
    }

    bool D4Session::SendData(const std::vector<unsigned char>& payload)
    {
        if (!m_channelOpen)
            return Fail("SendData called before the channel was opened");

        const size_t mtuPayload = (m_mtuToPrinter > 6) ? static_cast<size_t>(m_mtuToPrinter - 6) : 1;

        size_t offset = 0;
        do
        {
            const size_t chunkLen = std::min(mtuPayload, payload.size() - offset);

            if (!EnsureSendCredit(m_options.replyTimeoutMs))
                return false;

            const std::vector<unsigned char> chunk(payload.begin() + static_cast<long>(offset),
                                                   payload.begin() + static_cast<long>(offset + chunkLen));
            if (!SendPacket(m_socket, m_socket, chunk, 0x00))
                return false;

            m_sendCredit -= 1;
            offset += chunkLen;
        } while (offset < payload.size());

        return true;
    }

    bool D4Session::Exchange(const std::vector<unsigned char>& payload, std::vector<unsigned char>& reply)
    {
        // Grant credit to answer, send, then wait for the framed reply.
        if (!EnsurePrinterCredit())
            return false;

        if (!SendData(payload))
            return false;

        D4Packet pkt;
        if (!WaitForData(pkt, m_options.dataTimeoutMs))
        {
            if (m_lastError.empty())
                m_lastError = "The printer did not answer on the EPSON-CTRL data channel";
            return false;
        }

        reply = pkt.payload;
        return true;
    }

    void D4Session::Close()
    {
        const int timeout = std::min(m_options.replyTimeoutMs, 500);

        if (m_channelOpen)
        {
            if (SendPacket(0x00, 0x00, { D4::CMD_CLOSE_CHANNEL, m_socket, m_socket }, 0x01))
            {
                D4Packet reply;
                WaitFor(D4::CMD_CLOSE_CHANNEL | D4::REPLY_BIT, reply, timeout);
            }

            m_channelOpen = false;
        }

        if (SendPacket(0x00, 0x00, { D4::CMD_EXIT }, 0x01))
        {
            D4Packet reply;
            WaitFor(D4::CMD_EXIT | D4::REPLY_BIT, reply, timeout);
        }
    }

    bool ExtractDataPayload(const std::vector<unsigned char>& packet, std::vector<unsigned char>& payload)
    {
        if (packet.size() < 7 || packet[0] == 0x00 || packet[0] != packet[1])
            return false;

        const size_t total = (static_cast<size_t>(packet[2]) << 8) | packet[3];
        if (total < 7 || total > packet.size())
            return false;

        payload.assign(packet.begin() + 6, packet.begin() + static_cast<long>(total));
        return true;
    }

    namespace {

        bool ContainsBytes(const std::vector<unsigned char>& haystack,
                           const std::vector<unsigned char>& needle)
        {
            if (needle.empty())
                return true;
            if (haystack.size() < needle.size())
                return false;
            return std::search(haystack.begin(), haystack.end(),
                               needle.begin(), needle.end()) != haystack.end();
        }

        // One RCMODE command on its own short-lived D4 session over the
        // recovery service, sharing the write transport. Best-effort: failures
        // are logged and swallowed so the caller still attempts the writes.
        bool RunRecoveryCommand(ITransport& transport, log::Reporter& reporter,
                                const D4SessionOptions& baseOptions,
                                const ExecutorOptions& options, bool entering)
        {
            const std::vector<unsigned char>& command =
                entering ? options.recoveryEnter : options.recoveryClose;

            if (command.empty() || options.recoveryService.empty())
                return false;

            const char* phase = entering ? "enter" : "leave";

            D4SessionOptions recoveryOptions = baseOptions;
            recoveryOptions.serviceName = options.recoveryService;

            reporter.Log(entering ? log::Level::Info : log::Level::Trace,
                         log::Stage::Handshake, "exec.recovery_begin",
                         entering
                             ? "\n[*] This model needs firmware recovery mode for its EEPROM writes - switching the printer into it..."
                             : "[i] Leaving firmware recovery mode...");

            D4Session recovery(transport, reporter, recoveryOptions);

            if (!recovery.Start())
            {
                reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.recovery_unavailable",
                             std::string("[!] Could not open the '") + options.recoveryService
                                 + "' recovery service (" + recovery.LastError()
                                 + "). Continuing with the reset anyway.");
                return false;
            }

            std::vector<unsigned char> reply;
            const bool answered = recovery.Exchange(command, reply);
            recovery.Close();

            const bool acknowledged = answered && ContainsBytes(reply, options.recoveryReply);

            if (acknowledged)
                reporter.Log(log::Level::Trace, log::Stage::Handshake, "exec.recovery_ok",
                             std::string("[RCMODE] Printer acknowledged the ") + phase + " command.");
            else
                reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.recovery_no_ack",
                             std::string("[!] Printer did not acknowledge the recovery ") + phase
                                 + " command; continuing anyway.");

            return acknowledged;
        }

        // Leaves RCMODE however the write session ends. Armed only when an
        // enter was actually attempted.
        struct RecoveryLeaveGuard
        {
            ITransport& transport;
            log::Reporter& reporter;
            const D4SessionOptions& baseOptions;
            const ExecutorOptions& options;
            bool armed;

            ~RecoveryLeaveGuard()
            {
                if (armed && !options.recoveryClose.empty())
                    RunRecoveryCommand(transport, reporter, baseOptions, options, /*entering=*/false);
            }
        };

    } // namespace

    ExecutionResult ExecuteSequenceD4(ITransport& transport,
                                      const std::vector<std::vector<unsigned char>>& sequence,
                                      log::Reporter& reporter,
                                      const ExecutorOptions& options)
    {
        ExecutionResult result;

        D4SessionOptions sessionOptions;
        sessionOptions.replyTimeoutMs = options.handshakeDrainTimeoutMs;
        sessionOptions.dataTimeoutMs = options.writeAckTimeoutMs;
        sessionOptions.interPacketDelayMs = options.interPacketDelayMs;

        // Recovery-mode models must be in RCMODE around the writes. The leave
        // is a scope guard so every early return still restores the printer.
        // Both steps are best-effort on the write transport.
        const bool useRecovery =
            !options.recoveryEnter.empty() && !options.recoveryService.empty();
        if (useRecovery)
            RunRecoveryCommand(transport, reporter, sessionOptions, options, /*entering=*/true);
        RecoveryLeaveGuard recoveryLeaveGuard{ transport, reporter, sessionOptions, options, useRecovery };

        D4Session session(transport, reporter, sessionOptions);

        EmitTrace(reporter, "d4.banner", "=== IEEE 1284.4 SESSION CORE ===\n");

        if (!session.Start())
        {
            result.handshakeFailed = true;
            result.error = "Printer is not responding to the IEEE 1284.4 handshake ("
                         + session.LastError() + ") - wrong USB interface or unsupported model.";
            reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.handshake_failed",
                         "-> Handshake FAILED: " + session.LastError());
            return result;
        }

        result.handshakeConfirmed = true;

        // The session owns handshake and credit choreography, so only data
        // payloads survive.
        struct Item
        {
            std::vector<unsigned char> payload;
            bool isWrite;
        };

        std::vector<Item> items;
        for (const auto& pkt : sequence)
        {
            std::vector<unsigned char> payload;
            if (ExtractDataPayload(pkt, payload))
                items.push_back({ std::move(payload), IsWritePacket(pkt) });
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            const bool isWrite = items[i].isWrite;
            if (isWrite)
                result.writesTotal++;

            const bool verifyThisWrite = isWrite && options.verifyWrites;
            const int maxAttempts = verifyThisWrite ? options.maxWriteAttempts : 1;
            bool confirmed = false;

            // Copied, not referenced: a ':42:NG;' may rebuild this payload with
            // the model's alternate keyword before the next attempt.
            std::vector<unsigned char> payload = items[i].payload;
            bool triedAlternateKey = false;

            for (int attempt = 1; attempt <= maxAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    EmitProgress(reporter, log::Stage::Write, "exec.write_retry", i + 1, items.size(),
                                 "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                     + " | Retrying write (attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts) + ")...");
                    EmitTrace(reporter, "exec.retry", "[RETRY] Command " + std::to_string(i + 1)
                        + " attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts));

                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
                }

                std::vector<unsigned char> reply;
                const bool answered = session.Exchange(payload, reply);

                result.packetsSent++;

                if (!answered
                    && session.LastError().find("Transport failure") != std::string::npos)
                {
                    result.error = session.LastError();
                    return result;
                }

                if (!reply.empty())
                    result.ackCount++;

                if (isWrite && IsEepromWriteNgAck(reply))
                {
                    std::vector<unsigned char> alternate;
                    if (!triedAlternateKey
                        && SubstituteTrailingWriteKey(payload, options.writeKey, options.alternateWriteKey, alternate))
                    {
                        triedAlternateKey = true;
                        result.alternateKeyUsed = true;
                        payload = std::move(alternate);

                        EmitProgress(reporter, log::Stage::Write, "exec.write_key_retry", i + 1, items.size(),
                                     "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                         + " | Key rejected (||:42:NG;) - retrying with the alternate keyword.");
                        EmitTrace(reporter, "exec.retry", "[RETRY] Write rejected with ':42:NG;' on command "
                            + std::to_string(i + 1) + "; retrying with the alternate keyword ('wkey1')");

                        continue;
                    }

                    result.writesRejected++;
                    result.error = "Printer REJECTED EEPROM write (command " + std::to_string(i + 1)
                                 + ", reply ':42:NG;'). The write key may not match this model.";
                    EmitProgress(reporter, log::Stage::Write, "exec.write_rejected", i + 1, items.size(),
                                 "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                     + " | EEPROM write REJECTED (||:42:NG;).");
                    EmitTrace(reporter, "exec.trace_fatal", "[FATAL] Write rejected with ':42:NG;' on command " + std::to_string(i + 1) + "\n");
                    session.Close();

                    return result;
                }

                if (isWrite && IsEepromWriteNaAck(reply))
                {
                    result.writesRejected++;
                    result.error = "Printer REFUSED EEPROM write (command " + std::to_string(i + 1)
                                 + ", reply ':42:NA;'). The printer is likely locked by another error"
                                   " state (empty cartridge, paper jam, open cover). Clear that error"
                                   " first, then run EWR again.";
                    EmitProgress(reporter, log::Stage::Write, "exec.write_refused", i + 1, items.size(),
                                 "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                     + " | EEPROM write REFUSED (||:42:NA;) - printer locked by another error.");
                    EmitTrace(reporter, "exec.trace_fatal", "[FATAL] Write refused with ':42:NA;' on command " + std::to_string(i + 1) + "\n");
                    session.Close();

                    return result;
                }

                if (!verifyThisWrite)
                {
                    EmitProgress(reporter, log::Stage::Write, reply.empty() ? "exec.packet_sent" : "exec.packet_acked",
                                 i + 1, items.size(),
                                 "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                     + (reply.empty() ? " | Sent. (No reply)" : " | Answered."));
                    confirmed = true;
                    break;
                }

                if (IsEepromWriteOkAck(reply))
                {
                    result.writesVerified++;
                    confirmed = true;
                    EmitProgress(reporter, log::Stage::Write, "exec.write_verified", i + 1, items.size(),
                                 "-> Command " + std::to_string(i + 1) + " / " + std::to_string(items.size())
                                     + " | EEPROM write verified (||:42:OK;).");
                    break;
                }

                EmitTrace(reporter, "exec.write_unconfirmed", "[WARNING] Command " + std::to_string(i + 1) + " write reply missing ':42:OK;'");
            }

            if (verifyThisWrite && !confirmed)
            {
                result.writesUnverified = true;

                if (result.ackCount > 0)
                {
                    result.error = "EEPROM write not confirmed after " + std::to_string(maxAttempts)
                                 + " attempts (command " + std::to_string(i + 1)
                                 + "): the printer replies, but never with ':42:OK;'. Some writes may have"
                                   " been applied without confirmation - power-cycle the printer and check"
                                   " whether the error cleared before retrying.";
                }
                else
                {
                    result.error = "EEPROM write not acknowledged after " + std::to_string(maxAttempts)
                                 + " attempts (command " + std::to_string(i + 1) + ")";
                }

                EmitTrace(reporter, "exec.trace_fatal", "[FATAL] " + result.error + "\n");
                session.Close();

                return result;
            }
        }

        session.Close();

        if (options.verifyWrites && result.writesTotal == 0)
        {
            result.error = "The sequence contains no EEPROM write packets - nothing was reset.";
            return result;
        }

        if (result.ackCount == 0)
        {
            result.error = "The printer did not acknowledge any packets. The reset sequence was rejected or ignored.";
            return result;
        }

        if (options.verifyWrites)
        {
            result.success = (result.writesVerified == result.writesTotal);
            if (!result.success)
            {
                result.error = "Incomplete EEPROM write: verified " + std::to_string(result.writesVerified)
                             + " of " + std::to_string(result.writesTotal) + " write operations.";
            }
        }
        else
        {
            result.success = true;
        }

        return result;
    }

    QuerySessionResult ExecuteQuerySessionD4(ITransport& transport,
                                             const std::vector<std::vector<unsigned char>>& queries,
                                             log::Reporter& reporter,
                                             const ExecutorOptions& options)
    {
        QuerySessionResult result;

        D4SessionOptions sessionOptions;
        sessionOptions.replyTimeoutMs = options.handshakeDrainTimeoutMs;
        sessionOptions.dataTimeoutMs = options.writeAckTimeoutMs;
        sessionOptions.interPacketDelayMs = options.interPacketDelayMs;

        D4Session session(transport, reporter, sessionOptions);

        EmitTrace(reporter, "d4.banner", "=== IEEE 1284.4 SESSION CORE (read-only) ===\n");

        if (!session.Start())
        {
            result.handshakeFailed = true;
            result.error = "Printer is not responding to the IEEE 1284.4 handshake ("
                         + session.LastError() + ") - wrong USB interface or unsupported model.";
            reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.handshake_failed",
                         "-> Handshake FAILED: " + session.LastError());
            return result;
        }

        result.handshakeConfirmed = true;

        bool allAnswered = true;

        for (const auto& queryPacket : queries)
        {
            std::vector<unsigned char> payload;
            if (!ExtractDataPayload(queryPacket, payload))
            {
                EmitTrace(reporter, "exec.query_skipped", "[WARNING] Skipping a query packet that is not a D4 data packet");
                result.replies.push_back({});
                allAnswered = false;
                continue;
            }

            std::vector<unsigned char> reply;
            const int maxAttempts = (options.maxWriteAttempts > 1) ? options.maxWriteAttempts : 1;

            for (int attempt = 1; attempt <= maxAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    EmitTrace(reporter, "exec.retry", "[RETRY] Query attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts));

                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
                }

                result.packetsSent++;

                reply.clear();
                if (session.Exchange(payload, reply))
                    break;

                if (session.LastError().find("Transport failure") != std::string::npos)
                {
                    result.error = session.LastError();
                    return result;
                }

                EmitTrace(reporter, "exec.query_unanswered", "[WARNING] Query drew no reply");
            }

            if (reply.empty())
            {
                allAnswered = false;
                result.replies.push_back({});
            }
            else
            {
                // Downstream parsers expect raw drained bytes, not a payload.
                std::vector<unsigned char> framed;
                const uint16_t total = static_cast<uint16_t>(reply.size() + 6);
                framed.push_back(session.Socket());
                framed.push_back(session.Socket());
                framed.push_back(static_cast<unsigned char>((total >> 8) & 0xFF));
                framed.push_back(static_cast<unsigned char>(total & 0xFF));
                framed.push_back(0x00);
                framed.push_back(0x01);
                framed.insert(framed.end(), reply.begin(), reply.end());
                result.replies.push_back(std::move(framed));
            }
        }

        session.Close();

        result.success = allAnswered;

        if (!result.success && result.error.empty())
            result.error = "The printer did not answer one or more status/read queries.";

        return result;
    }

} // namespace ewr
