#include "protocol/remote_mode.h"

#include <iterator>

namespace ewr {
namespace remote {

    // The EJL exit packet with five spaces: the variant an SX110 was seen to
    // take (the JIG capture) and the one print tools have sent for years.
    const std::vector<unsigned char> kLeavePacketMode = {
        0x00, 0x00, 0x00, 0x1B, 0x01, '@', 'E', 'J', 'L', ' ',
        '1', '2', '8', '4', '.', '4', '\n', '@', 'E', 'J', 'L',
        ' ', ' ', ' ', ' ', ' ', '\n'
    };

    std::vector<unsigned char> Command(char first, char second, const std::vector<unsigned char>& parameters)
    {
        std::vector<unsigned char> out;
        out.push_back(static_cast<unsigned char>(first));
        out.push_back(static_cast<unsigned char>(second));
        out.push_back(static_cast<unsigned char>(parameters.size() & 0xFF));
        out.push_back(static_cast<unsigned char>((parameters.size() >> 8) & 0xFF));
        out.insert(out.end(), parameters.begin(), parameters.end());
        return out;
    }

    std::vector<unsigned char> NozzleCheck(int variant)
    {
        return Command('N', 'C', { 0x00, static_cast<unsigned char>(variant == 1 ? 0x10 : 0x00) });
    }

    std::vector<unsigned char> HeadClean(uint8_t group, bool power)
    {
        return Command('C', 'H', { 0x00, static_cast<unsigned char>((group & 0x0F) | (power ? 0x10 : 0x00)) });
    }

    std::vector<unsigned char> BuildJob(const std::vector<std::vector<unsigned char>>& commands)
    {
        std::vector<unsigned char> job = kLeavePacketMode;

        // ESC @: a known starting state, whatever the last job left behind.
        job.push_back(0x1B);
        job.push_back('@');

        // ESC ( R, 8 parameter bytes: a NUL, then "REMOTE1".
        const unsigned char enter[] = { 0x1B, '(', 'R', 0x08, 0x00, 0x00, 'R', 'E', 'M', 'O', 'T', 'E', '1' };
        job.insert(job.end(), std::begin(enter), std::end(enter));

        for (const auto& command : commands)
            job.insert(job.end(), command.begin(), command.end());

        // ESC 00 00 00 leaves remote mode.
        const unsigned char leave[] = { 0x1B, 0x00, 0x00, 0x00 };
        job.insert(job.end(), std::begin(leave), std::end(leave));

        return job;
    }

} // namespace remote
} // namespace ewr
