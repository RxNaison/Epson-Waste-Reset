#include "ewr/executor.h"
#include "ewr/d4session.h"
#include "ewr/end4.h"
#include "ewr/generator.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace ewr {

    namespace {

        bool ContainsToken(const std::vector<unsigned char>& data,
                           const unsigned char* token, size_t tokenLen)
        {
            if (data.size() < tokenLen)
                return false;

            return std::search(data.begin(), data.end(), token, token + tokenLen) != data.end();
        }

    } // namespace

    std::string HexDump(const unsigned char* data, size_t size)
    {
        if (size == 0)
            return "    (Empty)\n";

        std::ostringstream oss;
        for (size_t i = 0; i < size; ++i)
        {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";

            if ((i + 1) % 16 == 0 || i == size - 1)
            {
                if (i == size - 1 && (i + 1) % 16 != 0)
                {
                    for (size_t p = 0; p < 16 - ((i + 1) % 16); ++p)
                        oss << "   ";
                }

                oss << " | ";
                size_t start = (i / 16) * 16;

                for (size_t j = start; j <= i; ++j)
                    oss << (char)((data[j] >= 32 && data[j] <= 126) ? data[j] : '.');

                oss << "\n";
            }
        }

        return oss.str();
    }

    bool IsWritePacket(const std::vector<unsigned char>& p)
    {
        // [0..5] D4 header, [6..9] 7C 7C + LE inner length,
        // [10..11] rkey, [12] cmd, [13] ~cmd, [14] ror1(cmd).
        if (p.size() < 15)
            return false;

        const unsigned char c = EpsonD4::CMD_EEPROM_WRITE;
        const unsigned char not_c = static_cast<unsigned char>(~c);
        const unsigned char ror_c = static_cast<unsigned char>(((c >> 1) & 0x7F) | ((c << 7) & 0x80));

        bool result = p[0] == EpsonD4::SOCKET_EPSON_CTRL
                   && p[1] == EpsonD4::SOCKET_EPSON_CTRL
                   && p[6] == EpsonD4::PREFIX_PIPE
                   && p[7] == EpsonD4::PREFIX_PIPE
                   && p[12] == c
                   && p[13] == not_c
                   && p[14] == ror_c;

        return result;
    }

    bool IsEepromWriteOkAck(const std::vector<unsigned char>& ackData)
    {
        static const unsigned char token[] = { ':', '4', '2', ':', 'O', 'K', ';' };
        return ContainsToken(ackData, token, sizeof(token));
    }

    bool IsEepromWriteNgAck(const std::vector<unsigned char>& ackData)
    {
        static const unsigned char token[] = { ':', '4', '2', ':', 'N', 'G', ';' };
        return ContainsToken(ackData, token, sizeof(token));
    }

    bool IsEepromWriteNaAck(const std::vector<unsigned char>& ackData)
    {
        static const unsigned char token[] = { ':', '4', '2', ':', 'N', 'A', ';' };
        return ContainsToken(ackData, token, sizeof(token));
    }

    bool IsChannelOpenAck(const std::vector<unsigned char>& ackData)
    {
        return ackData.size() >= 7 && ackData[6] == EpsonD4::REPLY_OPEN_CHANNEL;
    }

    bool SubstituteTrailingWriteKey(const std::vector<unsigned char>& packet,
                                    const std::string& from,
                                    const std::string& to,
                                    std::vector<unsigned char>& out)
    {
        if (from.empty() || to.empty() || from.size() != to.size() || packet.size() < from.size())
            return false;

        const size_t offset = packet.size() - from.size();
        for (size_t i = 0; i < from.size(); ++i)
        {
            if (packet[offset + i] != static_cast<unsigned char>(from[i]))
                return false;
        }

        out.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(offset));
        for (char c : to)
            out.push_back(static_cast<unsigned char>(c));

        return true;
    }

    namespace {

        // index/total ride as machine fields so a UI can drive a progress bar
        // without parsing the message.
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

    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    log::Reporter& reporter,
                                    const ExecutorOptions& options)
    {
        if (options.useSessionLayer)
            return ExecuteSequenceD4(transport, sequence, reporter, options);

        ExecutionResult result;

        // Everything before the first EEPROM write is handshake: EJL init,
        // D4 init, OpenChannel, first credit exchange.
        size_t firstWriteIndex = sequence.size();
        for (size_t i = 0; i < sequence.size(); ++i)
        {
            if (IsWritePacket(sequence[i]))
            {
                firstWriteIndex = i;
                break;
            }
        }

        auto sendAndDrain = [&](const std::vector<unsigned char>& pkt,
                                size_t index,
                                int drainTimeoutMs,
                                std::vector<unsigned char>& ack) -> bool
        {
            EmitTrace(reporter, "exec.tx", "[OUT] Packet " + std::to_string(index + 1)
                + " (" + std::to_string(pkt.size()) + " bytes):\n"
                + HexDump(pkt.data(), pkt.size()));

            if (!transport.Send(pkt))
            {
                result.error = "Transport failure while sending packet " + std::to_string(index + 1);
                EmitTrace(reporter, "exec.tx_failed", "[!] SEND FAILED on packet " + std::to_string(index + 1) + "\n");

                return false;
            }

            result.packetsSent++;

            if (options.interPacketDelayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(options.interPacketDelayMs));

            ack = transport.Drain(drainTimeoutMs);

            EmitTrace(reporter, "exec.rx", "[IN]  ACK (" + std::to_string(ack.size()) + " bytes):\n"
                + HexDump(ack.data(), ack.size()));

            if (!ack.empty())
                result.ackCount++;

            if (IsChannelOpenAck(ack))
                result.handshakeConfirmed = true;

            return true;
        };

        for (size_t i = 0; i < sequence.size(); ++i)
        {
            // The handshake is over and no channel-open reply ever came.
            // Writing anyway only moves the failure to the end of the run.
            if (options.validateHandshake && i == firstWriteIndex && !result.handshakeConfirmed)
            {
                result.handshakeFailed = true;
                result.error = "Printer is not responding to the IEEE 1284.4 handshake "
                               "(no channel-open reply) - wrong USB interface or unsupported model.";
                reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.handshake_failed",
                             "-> Handshake FAILED: no channel-open reply from the printer.");
                EmitTrace(reporter, "exec.trace_fatal", "[FATAL] " + result.error + "\n");

                return result;
            }

            // Copied, not referenced: a ':42:NG;' may rebuild this packet with
            // the model's alternate keyword before the next attempt.
            std::vector<unsigned char> pkt = sequence[i];
            const bool isWrite = IsWritePacket(pkt);
            bool triedAlternateKey = false;

            if (isWrite)
                result.writesTotal++;

            // The EJL -> D4 mode switch can take well over a second.
            const int drainTimeout = (i < firstWriteIndex)
                ? options.handshakeDrainTimeoutMs
                : (isWrite ? options.writeAckTimeoutMs : options.drainTimeoutMs);

            const bool verifyThisWrite = isWrite && options.verifyWrites;
            const int maxAttempts = verifyThisWrite ? options.maxWriteAttempts : 1;
            bool confirmed = false;

            for (int attempt = 1; attempt <= maxAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    EmitProgress(reporter, log::Stage::Write, "exec.write_retry", i + 1, sequence.size(),
                                 "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                     + " | Retrying write (attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts) + ")...");
                    EmitTrace(reporter, "exec.retry", "[RETRY] Packet " + std::to_string(i + 1)
                        + " attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts));

                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));

                    // Only generated sequences guarantee the two packets before
                    // a write are its credit pair; replay dumps have no shape.
                    if (options.resendCreditOnRetry
                        && i >= 2 && !IsWritePacket(sequence[i - 2]) && !IsWritePacket(sequence[i - 1]))
                    {
                        std::vector<unsigned char> creditAck;
                        if (!sendAndDrain(sequence[i - 2], i - 2, options.drainTimeoutMs, creditAck))
                            return result;

                        if (!sendAndDrain(sequence[i - 1], i - 1, options.drainTimeoutMs, creditAck))
                            return result;
                    }
                }

                std::vector<unsigned char> ack;
                if (!sendAndDrain(pkt, i, drainTimeout, ack))
                    return result;

                // The keyword did not match. Models carrying a second,
                // Caesar-shifted keyword ('wkey1') get one retry with it;
                // otherwise fatal on every path, replay included.
                if (isWrite && IsEepromWriteNgAck(ack))
                {
                    std::vector<unsigned char> alternate;
                    if (!triedAlternateKey
                        && SubstituteTrailingWriteKey(pkt, options.writeKey, options.alternateWriteKey, alternate))
                    {
                        triedAlternateKey = true;
                        result.alternateKeyUsed = true;
                        pkt = std::move(alternate);

                        EmitProgress(reporter, log::Stage::Write, "exec.write_key_retry", i + 1, sequence.size(),
                                     "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                         + " | Key rejected (||:42:NG;) - retrying with the alternate keyword.");
                        EmitTrace(reporter, "exec.retry", "[RETRY] Write rejected with ':42:NG;' on packet "
                            + std::to_string(i + 1) + "; retrying with the alternate keyword ('wkey1')");

                        continue;
                    }

                    result.writesRejected++;
                    result.error = "Printer REJECTED EEPROM write (packet " + std::to_string(i + 1)
                                 + ", reply ':42:NG;'). The write key may not match this model.";
                    EmitProgress(reporter, log::Stage::Write, "exec.write_rejected", i + 1, sequence.size(),
                                 "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                     + " | EEPROM write REJECTED (||:42:NG;).");
                    EmitTrace(reporter, "exec.trace_fatal", "[FATAL] Write rejected with ':42:NG;' on packet " + std::to_string(i + 1) + "\n");

                    return result;
                }

                // Keys accepted, but the firmware will not service the write in
                // its current state - typically locked by ink-out, paper-jam or
                // cover-open. Retrying cannot help.
                if (isWrite && IsEepromWriteNaAck(ack))
                {
                    result.writesRejected++;
                    result.error = "Printer REFUSED EEPROM write (packet " + std::to_string(i + 1)
                                 + ", reply ':42:NA;'). The printer is likely locked by another error"
                                   " state (empty cartridge, paper jam, open cover). Clear that error"
                                   " first, then run EWR again.";
                    EmitProgress(reporter, log::Stage::Write, "exec.write_refused", i + 1, sequence.size(),
                                 "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                     + " | EEPROM write REFUSED (||:42:NA;) - printer locked by another error.");
                    EmitTrace(reporter, "exec.trace_fatal", "[FATAL] Write refused with ':42:NA;' on packet " + std::to_string(i + 1) + "\n");

                    return result;
                }

                if (!verifyThisWrite)
                {
                    if (!ack.empty())
                    {
                        EmitProgress(reporter, log::Stage::Write, "exec.packet_acked", i + 1, sequence.size(),
                                     "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                         + " | Triggered ACK: Cleared " + std::to_string(ack.size()) + " bytes.");
                    }
                    else
                    {
                        EmitProgress(reporter, log::Stage::Write, "exec.packet_sent", i + 1, sequence.size(),
                                     "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                         + " | Sent. (No ACK)");
                    }

                    confirmed = true;
                    break;
                }

                if (IsEepromWriteOkAck(ack))
                {
                    result.writesVerified++;
                    confirmed = true;
                    EmitProgress(reporter, log::Stage::Write, "exec.write_verified", i + 1, sequence.size(),
                                 "-> Packet " + std::to_string(i + 1) + " / " + std::to_string(sequence.size())
                                     + " | EEPROM write verified (||:42:OK;).");

                    break;
                }

                EmitTrace(reporter, "exec.write_unconfirmed", "[WARNING] Packet " + std::to_string(i + 1) + " write ACK missing ':42:OK;'");
            }

            if (verifyThisWrite && !confirmed)
            {
                result.writesUnverified = true;

                if (result.ackCount > 0)
                {
                    // The device is talking, just never confirming. Worth its
                    // own message: silence and refusal need different advice.
                    result.error = "EEPROM write not confirmed after " + std::to_string(maxAttempts)
                                 + " attempts (packet " + std::to_string(i + 1)
                                 + "): the printer replies, but never with ':42:OK;'. Some writes may have"
                                   " been applied without confirmation - power-cycle the printer and check"
                                   " whether the error cleared before retrying.";
                }
                else
                {
                    result.error = "EEPROM write not acknowledged after " + std::to_string(maxAttempts)
                                 + " attempts (packet " + std::to_string(i + 1) + ")";
                }

                EmitTrace(reporter, "exec.trace_fatal", "[FATAL] " + result.error + "\n");

                return result;
            }
        }

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
            // The dump's shape is unknown, so success means only: delivered in
            // full, device talking back, nothing rejected.
            result.success = true;
        }

        return result;
    }

    QuerySessionResult ExecuteQuerySession(ITransport& transport,
                                           const std::vector<std::vector<unsigned char>>& handshake,
                                           const std::vector<std::vector<unsigned char>>& queries,
                                           log::Reporter& reporter,
                                           const ExecutorOptions& options)
    {
        // The session core builds its own handshake, so the caller's are
        // deliberately dropped rather than sent twice.
        if (options.useSessionLayer)
            return ExecuteQuerySessionD4(transport, queries, reporter, options);

        QuerySessionResult result;

        size_t packetIndex = 0;

        auto sendAndDrain = [&](const std::vector<unsigned char>& pkt,
                                int drainTimeoutMs,
                                std::vector<unsigned char>& ack) -> bool
        {
            EmitTrace(reporter, "exec.tx", "[OUT] Packet " + std::to_string(packetIndex + 1)
                + " (" + std::to_string(pkt.size()) + " bytes):\n"
                + HexDump(pkt.data(), pkt.size()));

            if (!transport.Send(pkt))
            {
                result.error = "Transport failure while sending packet " + std::to_string(packetIndex + 1);
                EmitTrace(reporter, "exec.tx_failed", "[!] SEND FAILED on packet " + std::to_string(packetIndex + 1) + "\n");

                return false;
            }

            packetIndex++;
            result.packetsSent++;

            if (options.interPacketDelayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(options.interPacketDelayMs));

            ack = transport.Drain(drainTimeoutMs);

            EmitTrace(reporter, "exec.rx", "[IN]  ACK (" + std::to_string(ack.size()) + " bytes):\n"
                + HexDump(ack.data(), ack.size()));

            if (IsChannelOpenAck(ack))
                result.handshakeConfirmed = true;

            return true;
        };

        // The EJL -> D4 mode switch can take well over a second.
        for (const auto& pkt : handshake)
        {
            std::vector<unsigned char> ack;
            if (!sendAndDrain(pkt, options.handshakeDrainTimeoutMs, ack))
                return result;
        }

        // Queries mean nothing on a closed channel; same fail-fast as writes.
        if (!result.handshakeConfirmed)
        {
            result.handshakeFailed = true;
            result.error = "Printer is not responding to the IEEE 1284.4 handshake "
                           "(no channel-open reply) - wrong USB interface or unsupported model.";
            reporter.Log(log::Level::Info, log::Stage::Handshake, "exec.handshake_failed",
                         "-> Handshake FAILED: no channel-open reply from the printer.");
            EmitTrace(reporter, "exec.trace_fatal", "[FATAL] " + result.error + "\n");

            return result;
        }

        const std::vector<unsigned char> creditGrant = UniversalGenerator::CreditGrantPacket();
        const std::vector<unsigned char> creditRequest = UniversalGenerator::CreditRequestPacket();

        bool allAnswered = true;

        for (size_t q = 0; q < queries.size(); ++q)
        {
            std::vector<unsigned char> reply;

            const int maxAttempts = (options.maxWriteAttempts > 1) ? options.maxWriteAttempts : 1;
            for (int attempt = 1; attempt <= maxAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    EmitTrace(reporter, "exec.retry", "[RETRY] Query " + std::to_string(q + 1)
                        + " attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts));

                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
                }

                // Grant the printer credit to answer, request credit to send.
                std::vector<unsigned char> ack;
                if (!sendAndDrain(creditGrant, options.drainTimeoutMs, ack))
                    return result;

                if (!sendAndDrain(creditRequest, options.drainTimeoutMs, ack))
                    return result;

                reply.clear();
                if (!sendAndDrain(queries[q], options.writeAckTimeoutMs, reply))
                    return result;

                if (!reply.empty())
                    break;

                EmitTrace(reporter, "exec.query_unanswered", "[WARNING] Query " + std::to_string(q + 1) + " drew no reply");
            }

            if (reply.empty())
                allAnswered = false;

            result.replies.push_back(std::move(reply));
        }

        result.success = allAnswered;

        if (!result.success && result.error.empty())
            result.error = "The printer did not answer one or more status/read queries.";

        return result;
    }

    namespace {

        int AddOutSink(log::Reporter& reporter, std::ostream& outStream)
        {
            std::ostream* stream = &outStream;
            return reporter.AddSink([stream](const log::Event& event)
            {
                if (event.level == log::Level::Info)
                    (*stream) << event.message << std::endl;
            });
        }

        int AddTraceSink(log::Reporter& reporter, std::ostream& logStream)
        {
            std::ostream* stream = &logStream;
            return reporter.AddSink([stream](const log::Event& event)
            {
                if (event.level == log::Level::Trace)
                    (*stream) << event.message << "\n";
            });
        }

    } // namespace

    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    std::ostream& out,
                                    std::ostream& log,
                                    const ExecutorOptions& options)
    {
        log::Reporter reporter;
        AddOutSink(reporter, out);
        AddTraceSink(reporter, log);
        return ExecuteSequence(transport, sequence, reporter, options);
    }

    QuerySessionResult ExecuteQuerySession(ITransport& transport,
                                           const std::vector<std::vector<unsigned char>>& handshake,
                                           const std::vector<std::vector<unsigned char>>& queries,
                                           std::ostream& out,
                                           std::ostream& log,
                                           const ExecutorOptions& options)
    {
        log::Reporter reporter;
        AddOutSink(reporter, out);
        AddTraceSink(reporter, log);
        return ExecuteQuerySession(transport, handshake, queries, reporter, options);
    }

    // ---- END4 direct-control fallback --------------------------------------

    std::vector<std::vector<unsigned char>> ExtractFactoryWriteCommands(
        const std::vector<std::vector<unsigned char>>& sequence)
    {
        // WrapD4DataPacket prepends 02 02, big-endian length, 00 00 - so the
        // raw '||' command starts at offset 6.
        constexpr size_t kD4DataHeaderSize = 6;

        std::vector<std::vector<unsigned char>> commands;
        for (const auto& packet : sequence)
        {
            if (IsWritePacket(packet) && packet.size() > kD4DataHeaderSize)
                commands.emplace_back(packet.begin() + kD4DataHeaderSize, packet.end());
        }

        return commands;
    }

    namespace {

        bool HasEnd4Marker(const std::vector<unsigned char>& buffer)
        {
            static const unsigned char marker[] = { 'E', 'N', 'D', '4' };
            return buffer.size() >= sizeof(marker)
                && std::search(buffer.begin(), buffer.end(), marker, marker + sizeof(marker)) != buffer.end();
        }

        // Accumulates an END4 reply, bounded by a hard deadline so a silent
        // printer can never wedge the run. Stops as soon as a decisive
        // ':42:...' token has arrived behind an END4 marker.
        std::vector<unsigned char> DrainForEnd4Reply(ITransport& transport,
                                                     int firstReadTimeoutMs,
                                                     int totalDeadlineMs)
        {
            const auto start = std::chrono::steady_clock::now();
            std::vector<unsigned char> buffer;

            for (;;)
            {
                std::vector<unsigned char> chunk = transport.Drain(firstReadTimeoutMs);
                if (!chunk.empty())
                    buffer.insert(buffer.end(), chunk.begin(), chunk.end());

                if (HasEnd4Marker(buffer)
                    && (IsEepromWriteOkAck(buffer) || IsEepromWriteNgAck(buffer) || IsEepromWriteNaAck(buffer)))
                    break;

                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsedMs >= totalDeadlineMs)
                    break;

                if (chunk.empty())
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            return buffer;
        }

    } // namespace

    End4Result ExecuteEnd4Sequence(ITransport& transport,
                                   const std::string& deviceId,
                                   const std::vector<std::vector<unsigned char>>& factoryWriteCommands,
                                   log::Reporter& reporter,
                                   const ExecutorOptions& options)
    {
        End4Result result;

        EmitTrace(reporter, "end4.begin",
            "[END4] D4 stayed silent; entering END4 direct-control mode (raw data line, no IEEE 1284.4 framing).");

        if (factoryWriteCommands.empty())
        {
            result.error = "END4: no EEPROM write commands to send.";
            EmitTrace(reporter, "end4.empty", "[END4] " + result.error);
            return result;
        }

        // 1) Preamble: take the printer out of packet (D4) mode.
        if (!transport.Send(end4::kExitPacketMode2))
        {
            result.error = "END4: transport failed sending the ExitPacketMode2 preamble.";
            EmitTrace(reporter, "end4.tx_failed", "[END4] " + result.error);
            return result;
        }
        EmitTrace(reporter, "end4.preamble", "[END4] Sent ExitPacketMode2 preamble ("
            + std::to_string(end4::kExitPacketMode2.size()) + " bytes).");

        // 2) Packet-mode flush: DDS bytes of 0x11, chunked and bounded.
        const size_t ddsLen = end4::ParseDdsFlushLength(deviceId);
        if (ddsLen == 0)
        {
            EmitTrace(reporter, "end4.dds_absent",
                "[END4] No usable DDS field in the device ID; skipping the packet-mode flush.");
        }
        else
        {
            constexpr size_t kMaxFlush = 1024 * 1024; // never trust an ID into flushing > 1 MiB
            const size_t flushLen = (ddsLen > kMaxFlush) ? kMaxFlush : ddsLen;
            if (flushLen != ddsLen)
                EmitTrace(reporter, "end4.dds_capped", "[END4] DDS flush length " + std::to_string(ddsLen)
                    + " capped to " + std::to_string(flushLen) + " bytes.");

            EmitTrace(reporter, "end4.dds", "[END4] Flushing " + std::to_string(flushLen)
                + " packet-mode bytes (DDS) in 0x8000-byte chunks...");

            constexpr size_t kChunk = 0x8000;
            const std::vector<unsigned char> filler(kChunk, 0x11);
            size_t remaining = flushLen;
            while (remaining > 0)
            {
                const size_t n = (remaining < kChunk) ? remaining : kChunk;
                const std::vector<unsigned char> piece(filler.begin(),
                    filler.begin() + static_cast<std::vector<unsigned char>::difference_type>(n));
                if (!transport.Send(piece))
                {
                    result.error = "END4: transport failed during the DDS packet-mode flush.";
                    EmitTrace(reporter, "end4.tx_failed", "[END4] " + result.error);
                    return result;
                }
                remaining -= n;
            }
        }

        // Clear anything the preamble / flush stirred up before the writes.
        transport.Drain(options.drainTimeoutMs);

        // 3) Each factory write, framed in END4 and confirmed with ':42:OK;'.
        const int totalDeadlineMs = (options.handshakeDrainTimeoutMs > 0)
            ? options.handshakeDrainTimeoutMs * 2
            : 4000;

        for (size_t i = 0; i < factoryWriteCommands.size(); ++i)
        {
            result.writesTotal++;
            std::vector<unsigned char> command = factoryWriteCommands[i];
            bool confirmed = false;
            bool triedAlternateKey = false;

            for (int attempt = 1; attempt <= 2; ++attempt) // original + one alternate-key retry
            {
                const std::vector<unsigned char> packet = end4::BuildEnd4Packet(command);

                EmitTrace(reporter, "end4.tx", "[END4 OUT] write " + std::to_string(i + 1) + "/"
                    + std::to_string(factoryWriteCommands.size()) + " (" + std::to_string(packet.size())
                    + " bytes):\n" + HexDump(packet.data(), packet.size()));

                if (!transport.Send(packet))
                {
                    result.error = "END4: transport failed sending write " + std::to_string(i + 1) + ".";
                    EmitTrace(reporter, "end4.tx_failed", "[END4] " + result.error);
                    return result;
                }

                if (options.interPacketDelayMs > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(options.interPacketDelayMs));

                const std::vector<unsigned char> reply =
                    DrainForEnd4Reply(transport, options.writeAckTimeoutMs, totalDeadlineMs);

                EmitTrace(reporter, "end4.rx", "[END4 IN] reply (" + std::to_string(reply.size())
                    + " bytes):\n" + HexDump(reply.data(), reply.size()));

                // Verification below still runs on the whole buffer, which is
                // robust to usbprint.sys prefix junk.
                {
                    static const unsigned char marker[] = { 'E', 'N', 'D', '4' };
                    const auto markerIt = std::search(reply.begin(), reply.end(), marker, marker + sizeof(marker));
                    if (markerIt != reply.end())
                    {
                        result.anyReply = true;
                        const std::vector<unsigned char> framed(markerIt, reply.end());
                        std::vector<unsigned char> payload;
                        if (end4::ParseEnd4Response(framed, payload))
                            EmitTrace(reporter, "end4.payload", "[END4] control payload ("
                                + std::to_string(payload.size()) + " bytes):\n"
                                + HexDump(payload.data(), payload.size()));
                    }
                }

                if (IsEepromWriteOkAck(reply))
                {
                    result.writesVerified++;
                    confirmed = true;
                    EmitProgress(reporter, log::Stage::Write, "end4.write_verified", i + 1, factoryWriteCommands.size(),
                        "-> END4 write " + std::to_string(i + 1) + " / " + std::to_string(factoryWriteCommands.size())
                            + " verified (||:42:OK;).");
                    break;
                }

                if (IsEepromWriteNgAck(reply))
                {
                    std::vector<unsigned char> alternate;
                    if (!triedAlternateKey
                        && SubstituteTrailingWriteKey(command, options.writeKey, options.alternateWriteKey, alternate))
                    {
                        triedAlternateKey = true;
                        result.alternateKeyUsed = true;
                        command = std::move(alternate);
                        EmitTrace(reporter, "end4.retry", "[END4] write " + std::to_string(i + 1)
                            + " rejected (||:42:NG;); retrying with the alternate keyword ('wkey1').");
                        continue;
                    }

                    result.writesRejected++;
                    result.error = "END4: printer rejected EEPROM write " + std::to_string(i + 1)
                        + " (||:42:NG;). The write key does not match this model.";
                    EmitTrace(reporter, "end4.fatal", "[END4] " + result.error);
                    return result;
                }

                if (IsEepromWriteNaAck(reply))
                {
                    result.writesRejected++;
                    result.error = "END4: printer refused EEPROM write " + std::to_string(i + 1)
                        + " (||:42:NA;). It is locked by another error, or needs firmware-recovery mode"
                          " that only the (currently silent) D4 channel can enter.";
                    EmitTrace(reporter, "end4.fatal", "[END4] " + result.error);
                    return result;
                }

                // Do not spend the alternate-key retry on silence.
                EmitTrace(reporter, "end4.unconfirmed", "[END4] write " + std::to_string(i + 1)
                    + (HasEnd4Marker(reply) ? " drew no '||:42:OK;' confirmation." : " drew no END4 reply."));
                break;
            }

            if (!confirmed)
            {
                if (!result.anyReply)
                {
                    result.error = "END4: the printer never answered on the END4 channel (no 'END4' reply)"
                                   " - usbprint.sys is most likely filtering this transport as well.";
                }
                else if (result.error.empty())
                {
                    result.error = "END4: write " + std::to_string(i + 1)
                        + " was not confirmed with '||:42:OK;'.";
                }
                EmitTrace(reporter, "end4.fatal", "[END4] " + result.error);
                return result;
            }
        }

        result.success = (result.writesVerified == result.writesTotal);
        if (!result.success && result.error.empty())
            result.error = "END4: verified " + std::to_string(result.writesVerified) + " of "
                + std::to_string(result.writesTotal) + " writes.";

        if (result.success)
        {
            reporter.Log(log::Level::Info, log::Stage::Write, "end4.success",
                "-> END4 fallback verified all " + std::to_string(result.writesVerified)
                    + " EEPROM write(s) (||:42:OK;).");
        }

        return result;
    }

} // namespace ewr
