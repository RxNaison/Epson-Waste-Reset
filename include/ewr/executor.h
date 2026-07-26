#pragma once
#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace ewr {

    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        virtual bool Send(const std::vector<unsigned char>& packet) = 0;
        virtual std::vector<unsigned char> Drain() = 0;
    };

    struct ExecutorOptions
    {
        int maxWriteAttempts = 3;
        int interPacketDelayMs = 100;
        int retryDelayMs = 200;
    };

    struct ExecutionResult
    {
        bool success = false;
        size_t packetsSent = 0;
        size_t ackCount = 0;
        size_t writesTotal = 0;
        size_t writesVerified = 0;
        size_t writesRejected = 0;
        std::string error;
    };

    bool IsWritePacket(const std::vector<unsigned char>& packet);
    bool IsEepromWriteOkAck(const std::vector<unsigned char>& ackData);
    bool IsEepromWriteNgAck(const std::vector<unsigned char>& ackData);
    bool IsChannelOpenAck(const std::vector<unsigned char>& ackData);

    std::string HexDump(const unsigned char* data, size_t size);

    ExecutionResult ExecuteSequence(ITransport& transport,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    std::ostream& out,
                                    std::ostream& log,
                                    const ExecutorOptions& options = {});

} // namespace ewr
