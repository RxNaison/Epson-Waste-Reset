#include "protocol/jig.h"
#include "protocol/d4session.h"
#include "ewr/format.h"
#include "ewr/database.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace ewr {
namespace jig {

    const std::vector<unsigned char> kLeavePacketMode = {
        0x00, 0x00, 0x00, 0x1b, 0x01, '@', 'E', 'J', 'L', ' ',
        '1', '2', '8', '4', '.', '4', '\n', '@', 'E', 'J', 'L',
        ' ', ' ', ' ', ' ', ' ', '\n'
    };

    std::vector<unsigned char> BuildEnter()
    {
        return { 0x1b, '(', 'R', 0x08, 0x00, 0x00, 'J', 'I', 'G', 'M', 'O', 'D', 'E' };
    }

    std::vector<unsigned char> BuildReadPacket(uint16_t address)
    {
        return { 'J', 'G', 'P', 'G', 0x05, 0x00,
                 static_cast<unsigned char>(address >> 8),
                 static_cast<unsigned char>(address & 0xFF), 0x00,
                 0x01, 0x00 };
    }

    std::vector<unsigned char> BuildWritePacket(uint16_t address, uint8_t value)
    {
        return { 'J', 'G', 'P', 'S', 0x06, 0x00,
                 static_cast<unsigned char>(address >> 8),
                 static_cast<unsigned char>(address & 0xFF), 0x00,
                 0x01, 0x00, value };
    }

    std::vector<unsigned char> BuildExit()
    {
        return { 'J', 'G', 0x1b, 0x00, 0x00, 0x00 };
    }

    int ReadPacketAddress(const std::vector<unsigned char>& packet)
    {
        if (packet.size() != 11 || !std::equal(packet.begin(), packet.begin() + 4, "JGPG"))
            return -1;

        return (packet[6] << 8) + (packet[7] | (packet[8] << 8));
    }

    bool ParseWritePacket(const std::vector<unsigned char>& packet, uint16_t& address, uint8_t& value)
    {
        static const unsigned char kHead[] = { 'J', 'G', 'P', 'S', 0x06, 0x00 };
        if (packet.size() != 12 || !std::equal(kHead, kHead + 6, packet.begin())
            || packet[8] != 0x00 || packet[9] != 0x01 || packet[10] != 0x00)
            return false;

        address = static_cast<uint16_t>((packet[6] << 8) | packet[7]);
        value = packet[11];
        return true;
    }

    bool ParseReadReply(const std::vector<unsigned char>& raw, uint8_t& value)
    {
        static const unsigned char kTag[] = { 'J', 'G', 'P', 'G' };
        const auto at = std::search(raw.begin(), raw.end(), kTag, kTag + 4);
        if (at == raw.end())
            return false;

        const size_t start = static_cast<size_t>(at - raw.begin());
        if (start + 7 > raw.size())
            return false;

        const size_t length = raw[start + 4] | (raw[start + 5] << 8);
        if (length < 1 || start + 6 + length > raw.size())
            return false;

        value = raw[start + 6];
        return true;
    }

} // namespace jig

    namespace {

        void EmitTrace(log::Reporter& reporter, const char* code, const std::string& message)
        {
            reporter.Log(log::Level::Trace, log::Stage::General, code, message);
        }

        void EmitProgress(log::Reporter& reporter, const char* code, size_t index, size_t total,
                          const std::string& message)
        {
            log::Event event;
            event.level = log::Level::Info;
            event.stage = log::Stage::Write;
            event.code = code;
            event.message = message;
            event.index = static_cast<int>(index);
            event.total = static_cast<int>(total);
            reporter.Emit(event);
        }

        // D4 Init (revision 0x10), the same packet the session core sends.
        const std::vector<unsigned char> kD4Init = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 };

        std::vector<unsigned char> Exchange(ITransport& transport, log::Reporter& reporter,
                                            const std::vector<unsigned char>& packet, const char* what,
                                            int timeoutMs, bool& sent)
        {
            EmitTrace(reporter, "jig.tx", std::string("[OUT] JIG ") + what + " (" + std::to_string(packet.size())
                + " bytes):\n" + format::HexDump(packet.data(), packet.size()));

            sent = transport.Send(packet);
            if (!sent || timeoutMs < 0)
                return {};

            std::vector<unsigned char> reply = transport.Drain(timeoutMs);

            // A reply split across reads: take what follows right behind it.
            while (!reply.empty())
            {
                const std::vector<unsigned char> more = transport.Drain(0);
                if (more.empty())
                    break;
                reply.insert(reply.end(), more.begin(), more.end());
            }

            if (!reply.empty())
                EmitTrace(reporter, "jig.rx", "[IN]  JIG reply (" + std::to_string(reply.size()) + " bytes):\n"
                    + format::HexDump(reply.data(), reply.size()));

            return reply;
        }

        struct Entry
        {
            bool entered = false;   // JIGMODE went out behind an answered handshake
            bool answered = false;  // something came back before it
            bool silent = false;    // nothing did: the driver moves to the next interface
            std::string error;
        };

        Entry Enter(ITransport& transport, log::Reporter& reporter, const ExecutorOptions& options)
        {
            Entry entry;
            bool sent = false;

            // Into packet mode first, as WIC does: the leave packet below is a
            // D4 transaction, so it only means "leave" to a printer in D4 mode.
            const std::vector<unsigned char> ejlReply =
                Exchange(transport, reporter, d4::Handshake()[0], "EJL enter", options.handshakeDrainTimeoutMs, sent);
            if (!sent)
            {
                entry.error = "Transport failure while entering IEEE 1284.4 mode";
                return entry;
            }

            const std::vector<unsigned char> initReply =
                Exchange(transport, reporter, kD4Init, "D4 Init", options.handshakeDrainTimeoutMs, sent);
            const std::vector<unsigned char> leaveReply = sent
                ? Exchange(transport, reporter, jig::kLeavePacketMode, "leave packet mode", options.handshakeDrainTimeoutMs, sent)
                : std::vector<unsigned char>{};

            if (!sent)
            {
                entry.error = "Transport failure while leaving IEEE 1284.4 mode";
                return entry;
            }

            if (ejlReply.empty() && initReply.empty() && leaveReply.empty())
            {
                entry.silent = true;
                entry.error = "Printer is not responding on this interface (no reply to EJL, D4 Init or the"
                              " packet-mode exit).";
                return entry;
            }

            entry.answered = true;

            // WIC sends the first read straight behind this; nothing answers it.
            Exchange(transport, reporter, jig::BuildEnter(), "enter JIGMODE", -1, sent);
            if (!sent)
            {
                entry.error = "Transport failure while entering JIG mode";
                return entry;
            }

            entry.entered = true;
            return entry;
        }

    } // namespace

    QuerySessionResult jig::RunReadSession(ITransport& transport,
                                             const std::vector<std::vector<unsigned char>>& queries,
                                             log::Reporter& reporter,
                                             const ExecutorOptions& options)
    {
        QuerySessionResult result;
        bool sent = false;

        EmitTrace(reporter, "jig.banner", "=== JIG MODE SESSION (read-only) ===\n");

        const Entry entry = Enter(transport, reporter, options);
        result.handshakeConfirmed = entry.answered;
        result.handshakeFailed = entry.silent;
        if (!entry.entered)
        {
            result.error = entry.error;
            return result;
        }

        bool allAnswered = true;
        for (size_t q = 0; q < queries.size(); ++q)
        {
            if (jig::ReadPacketAddress(queries[q]) < 0)
            {
                EmitTrace(reporter, "jig.query_skipped", "[WARNING] Skipping a query that is not a JIG read");
                result.replies.push_back({});
                allAnswered = false;
                continue;
            }

            result.packetsSent++;
            std::vector<unsigned char> reply =
                Exchange(transport, reporter, queries[q], "read", options.writeAckTimeoutMs, sent);

            if (!sent)
            {
                result.error = "Transport failure during JIG read " + std::to_string(q + 1);
                result.replies.resize(queries.size());
                return result;
            }

            uint8_t value = 0;
            if (!jig::ParseReadReply(reply, value))
            {
                allAnswered = false;

                // The very first read going unanswered means JIG mode never
                // came up - not a printer of this platform. Asking the rest
                // would only add a timeout each.
                if (q == 0)
                {
                    result.error = "The printer did not answer a JIG read: it does not seem to take JIG mode.";
                    result.replies.resize(queries.size());
                    Exchange(transport, reporter, jig::BuildExit(), "exit", -1, sent);
                    return result;
                }
            }

            result.replies.push_back(std::move(reply));
        }

        Exchange(transport, reporter, jig::BuildExit(), "exit", -1, sent);

        result.success = allAnswered;
        if (!allAnswered && result.error.empty())
            result.error = "Some JIG reads drew no reply.";

        return result;
    }

    ExecutionResult jig::RunWriteSession(ITransport& transport,
                                         const std::vector<std::vector<unsigned char>>& writes,
                                         log::Reporter& reporter,
                                         const ExecutorOptions& options)
    {
        ExecutionResult result;

        std::vector<std::pair<uint16_t, uint8_t>> planned;
        for (const auto& packet : writes)
        {
            uint16_t address = 0;
            uint8_t value = 0;
            if (!jig::ParseWritePacket(packet, address, value))
            {
                result.error = "A JIG write session was handed a packet that is not a JIG write; nothing was sent.";
                reporter.Log(log::Level::Error, log::Stage::Write, "jig.write_not_jig", "[ERROR] " + result.error);
                return result;
            }
            planned.push_back({ address, value });
        }

        if (planned.empty())
        {
            result.error = "A JIG write session was handed no writes; nothing was sent.";
            return result;
        }

        EmitTrace(reporter, "jig.banner", "=== JIG MODE SESSION (writes) ===\n");

        const Entry entry = Enter(transport, reporter, options);
        result.handshakeConfirmed = entry.answered;
        result.handshakeFailed = entry.silent;
        if (!entry.entered)
        {
            result.error = entry.error;
            return result;
        }

        bool sent = false;
        const auto leave = [&]() { Exchange(transport, reporter, jig::BuildExit(), "exit", -1, sent); };

        // A printer that did not enter JIG mode takes these bytes as print
        // data, so a read has to answer before the first write goes out.
        uint8_t current = 0;
        const std::vector<unsigned char> probe =
            Exchange(transport, reporter, jig::BuildReadPacket(planned[0].first), "read", options.writeAckTimeoutMs, sent);
        if (sent)
            result.packetsSent++;
        if (!sent || !jig::ParseReadReply(probe, current))
        {
            result.error = sent ? "The printer did not answer a JIG read: it does not seem to take JIG mode."
                                  " Nothing was written."
                                : "Transport failure during the JIG read before the writes. Nothing was written.";
            leave();
            return result;
        }

        result.writesTotal = planned.size();
        const int maxAttempts = std::max(options.maxWriteAttempts, 1);

        for (size_t i = 0; i < planned.size(); ++i)
        {
            const uint16_t address = planned[i].first;
            const uint8_t value = planned[i].second;
            const std::string step = "-> Write " + std::to_string(i + 1) + " / " + std::to_string(planned.size());

            char target[32];
            snprintf(target, sizeof(target), "0x%04X <- 0x%02X", address, value);

            bool verified = false;
            int lastRead = -1;
            for (int attempt = 1; attempt <= maxAttempts && !verified; ++attempt)
            {
                if (attempt > 1)
                {
                    EmitProgress(reporter, "jig.write_retry", i + 1, planned.size(),
                                 step + " | Retrying (attempt " + std::to_string(attempt) + "/" + std::to_string(maxAttempts) + ")...");
                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
                }

                // Nothing answers a write; anything that does arrive is in the trace.
                Exchange(transport, reporter, jig::BuildWritePacket(address, value), "write", options.drainTimeoutMs, sent);
                if (sent)
                {
                    result.packetsSent++;
                    const std::vector<unsigned char> reply =
                        Exchange(transport, reporter, jig::BuildReadPacket(address), "read back", options.writeAckTimeoutMs, sent);
                    if (sent)
                        result.packetsSent++;

                    uint8_t readBack = 0;
                    if (sent && jig::ParseReadReply(reply, readBack))
                    {
                        result.ackCount++;
                        lastRead = readBack;
                        verified = readBack == value;
                    }
                }

                if (!sent)
                {
                    result.error = "Transport failure during JIG write " + std::to_string(i + 1);
                    leave();
                    return result;
                }

                if (!verified)
                    EmitTrace(reporter, "jig.write_unconfirmed", std::string("[WARNING] JIG write ") + target
                        + (lastRead < 0 ? ": the read-back went unanswered" : ": it reads back another value"));
            }

            if (!verified)
            {
                result.writesUnverified = true;

                char found[64];
                if (lastRead < 0)
                    snprintf(found, sizeof(found), "its read-back went unanswered");
                else
                    snprintf(found, sizeof(found), "it reads back 0x%02X", lastRead);

                result.error = "EEPROM write not confirmed after " + std::to_string(maxAttempts) + " attempts (write "
                             + std::to_string(i + 1) + ", " + target + "): " + found + ".";
                EmitTrace(reporter, "jig.trace_fatal", "[FATAL] " + result.error + "\n");
                leave();
                return result;
            }

            result.writesVerified++;
            EmitProgress(reporter, "jig.write_verified", i + 1, planned.size(),
                         step + " | EEPROM " + target + " verified by read-back.");
        }

        leave();
        result.success = true;
        return result;
    }

} // namespace ewr
