#pragma once
#include <functional>
#include <vector>

namespace ewr {

    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        virtual bool Send(const std::vector<unsigned char>& packet) = 0;
        // timeoutMs bounds how long the FIRST read may wait for the reply to
        // start arriving, not the total call. A timeoutMs of zero or less is a
        // poll: it returns whatever is already there. No value of timeoutMs
        // ever means "wait forever".
        virtual std::vector<unsigned char> Drain(int timeoutMs) = 0;

        // Drain, returning as soon as `complete` accepts the bytes read so far.
        virtual std::vector<unsigned char> DrainUntil(
            int timeoutMs, const std::function<bool(const std::vector<unsigned char>&)>& complete)
        {
            (void)complete;
            return Drain(timeoutMs);
        }
    };

} // namespace ewr
