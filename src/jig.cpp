#include "ewr/jig.h"
#include "ewr/generator.h"

#include <algorithm>
#include <string>

namespace ewr {
namespace jig {

    const std::vector<unsigned char> kLeavePacketMode = {
        0x00, 0x00, 0x00, 0x1b, 0x01, '@', 'E', 'J', 'L', ' ',
        '1', '2', '8', '4', '.', '4', '\n', '@', 'E', 'J', 'L',
        ' ', ' ', ' ', ' ', ' ', '\n'
    };

    std::vector<unsigned char> BuildEnterJigMode()
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

        // D4 Init (revision 0x10), the same packet the session core sends.
        const std::vector<unsigned char> kD4Init = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 };

        std::vector<unsigned char> Exchange(ITransport& transport, log::Reporter& reporter,
                                            const std::vector<unsigned char>& packet, const char* what,
                                            int timeoutMs, bool& sent)
        {
            EmitTrace(reporter, "jig.tx", std::string("[OUT] JIG ") + what + " (" + std::to_string(packet.size())
                + " bytes):\n" + HexDump(packet.data(), packet.size()));

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
                    + HexDump(reply.data(), reply.size()));

            return reply;
        }

    } // namespace

    QuerySessionResult ExecuteJigReadSession(ITransport& transport,
                                             const std::vector<std::vector<unsigned char>>& queries,
                                             log::Reporter& reporter,
                                             const ExecutorOptions& options)
    {
        QuerySessionResult result;
        bool sent = false;

        EmitTrace(reporter, "jig.banner", "=== JIG MODE SESSION (read-only) ===\n");

        // Into packet mode first, as WIC does: the leave packet below is a
        // D4 transaction, so it only means "leave" to a printer in D4 mode.
        const std::vector<unsigned char> ejl = UniversalGenerator::GenerateHandshake()[0];
        const std::vector<unsigned char> ejlReply =
            Exchange(transport, reporter, ejl, "EJL enter", options.handshakeDrainTimeoutMs, sent);
        if (!sent)
        {
            result.error = "Transport failure while entering IEEE 1284.4 mode";
            return result;
        }

        const std::vector<unsigned char> initReply =
            Exchange(transport, reporter, kD4Init, "D4 Init", options.handshakeDrainTimeoutMs, sent);
        const std::vector<unsigned char> leaveReply = sent
            ? Exchange(transport, reporter, jig::kLeavePacketMode, "leave packet mode", options.handshakeDrainTimeoutMs, sent)
            : std::vector<unsigned char>{};

        if (!sent)
        {
            result.error = "Transport failure while leaving IEEE 1284.4 mode";
            return result;
        }

        if (ejlReply.empty() && initReply.empty() && leaveReply.empty())
        {
            result.handshakeFailed = true;
            result.error = "Printer is not responding on this interface (no reply to EJL, D4 Init or the"
                           " packet-mode exit).";
            return result;
        }

        result.handshakeConfirmed = true;

        // WIC sends the first read straight behind this; nothing answers it.
        Exchange(transport, reporter, jig::BuildEnterJigMode(), "enter JIGMODE", -1, sent);
        if (!sent)
        {
            result.error = "Transport failure while entering JIG mode";
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

} // namespace ewr
