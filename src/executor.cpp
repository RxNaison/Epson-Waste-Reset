#include "ewr/executor.h"
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

    bool IsChannelOpenAck(const std::vector<unsigned char>& ackData)
    {
        return ackData.size() >= 7 && ackData[6] == EpsonD4::REPLY_OPEN_CHANNEL;
    }

    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    std::ostream& out,
                                    std::ostream& log,
                                    const ExecutorOptions& options)
    {
        ExecutionResult result;

        auto sendAndDrain = [&](const std::vector<unsigned char>& pkt,
                                size_t index,
                                std::vector<unsigned char>& ack) -> bool
        {
            log << "[OUT] Packet " << index + 1 << " (" << pkt.size() << " bytes):\n"
                << HexDump(pkt.data(), pkt.size()) << "\n";

            if (!transport.Send(pkt))
            {
                result.error = "Transport failure while sending packet " + std::to_string(index + 1);
                log << "[!] SEND FAILED on packet " << index + 1 << "\n\n";

                return false;
            }

            result.packetsSent++;

            if (options.interPacketDelayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(options.interPacketDelayMs));

            ack = transport.Drain();

            log << "[IN]  ACK (" << ack.size() << " bytes):\n"
                << HexDump(ack.data(), ack.size()) << "\n";

            if (!ack.empty())
                result.ackCount++;

            return true;
        };

        for (size_t i = 0; i < sequence.size(); ++i)
        {
            const auto& pkt = sequence[i];
            const bool isWrite = IsWritePacket(pkt);

            if (isWrite)
                result.writesTotal++;

            const int maxAttempts = isWrite ? options.maxWriteAttempts : 1;
            bool confirmed = false;

            for (int attempt = 1; attempt <= maxAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    out << "-> Packet " << i + 1 << " / " << sequence.size()
                        << " | Retrying write (attempt " << attempt << "/" << maxAttempts << ")..." << std::endl;
                    log << "[RETRY] Packet " << i + 1 << " attempt " << attempt << "/" << maxAttempts << "\n";

                    if (options.retryDelayMs > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));

                    if (i >= 2 && !IsWritePacket(sequence[i - 2]) && !IsWritePacket(sequence[i - 1]))
                    {
                        std::vector<unsigned char> creditAck;
                        if (!sendAndDrain(sequence[i - 2], i - 2, creditAck))
                            return result;

                        if (!sendAndDrain(sequence[i - 1], i - 1, creditAck))
                            return result;
                    }
                }

                std::vector<unsigned char> ack;
                if (!sendAndDrain(pkt, i, ack))
                    return result;

                if (!isWrite)
                {
                    if (!ack.empty())
                    {
                        out << "-> Packet " << i + 1 << " / " << sequence.size()
                            << " | Triggered ACK: Cleared " << ack.size() << " bytes." << std::endl;
                    }
                    else
                    {
                        out << "-> Packet " << i + 1 << " / " << sequence.size()
                            << " | Sent. (No ACK)" << std::endl;
                    }

                    confirmed = true;
                    break;
                }

                if (IsEepromWriteNgAck(ack))
                {
                    result.writesRejected++;
                    result.error = "Printer REJECTED EEPROM write (packet " + std::to_string(i + 1)
                                 + ", reply ':42:NG;'). The write key may not match this model.";
                    out << "-> Packet " << i + 1 << " / " << sequence.size()
                        << " | EEPROM write REJECTED (||:42:NG;)." << std::endl;
                    log << "[FATAL] Write rejected with ':42:NG;' on packet " << i + 1 << "\n\n";

                    return result;
                }

                if (IsEepromWriteOkAck(ack))
                {
                    result.writesVerified++;
                    confirmed = true;
                    out << "-> Packet " << i + 1 << " / " << sequence.size()
                        << " | EEPROM write verified (||:42:OK;)." << std::endl;

                    break;
                }

                log << "[WARNING] Packet " << i + 1 << " write ACK missing ':42:OK;'\n";
            }

            if (isWrite && !confirmed)
            {
                result.error = "EEPROM write not acknowledged after " + std::to_string(maxAttempts)
                             + " attempts (packet " + std::to_string(i + 1) + ")";
                log << "[FATAL] " << result.error << "\n\n";

                return result;
            }
        }

        if (result.writesTotal == 0)
        {
            result.error = "The sequence contains no EEPROM write packets - nothing was reset.";
            return result;
        }

        if (result.ackCount == 0)
        {
            result.error = "The printer did not acknowledge any packets. The reset sequence was rejected or ignored.";
            return result;
        }

        result.success = (result.writesVerified == result.writesTotal);
        if (!result.success)
        {
            result.error = "Incomplete EEPROM write: verified " + std::to_string(result.writesVerified)
                         + " of " + std::to_string(result.writesTotal) + " write operations.";
        }

        return result;
    }

} // namespace ewr
