#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Finding the interesting bytes on a model the database has no addresses for.
//
// The printer is read several times with a head cleaning between each read.
// A cleaning is the one action guaranteed to move the waste counter, so a byte
// that moves the same way at every interval is carrying something, and a byte
// that moves once is noise - a timer, a page count, an ink level settling. Two
// intervals is the minimum that can tell those apart, hence three reads.
//
// This reports what moved and which way. It does not decide what any of it
// means: a waste counter only ever rises, so a falling byte is something else
// entirely, and what a counter must be *set to* is not observable here at all
// (a platen group resets some bytes to 0x5E, not 0). The reader decides.

namespace ewr {

    // One EEPROM read pass: address -> value, -1 where the printer did not answer.
    using EepromSnapshot = std::vector<std::pair<uint16_t, int>>;

    struct ByteTrend
    {
        // One address, or two as a little-endian pair.
        std::vector<uint16_t> addresses;
        // The combined value at each pass, in order.
        std::vector<uint32_t> values;
        // values[i+1] - values[i], signed, one per interval.
        std::vector<int64_t> deltas;
        // Every interval moved the same way. Waste counters are rising ones.
        bool rising = false;

        bool IsPair() const { return addresses.size() == 2; }
    };

    // Bytes that moved the same direction at every interval. A 16-bit pair is
    // reported instead of its two bytes, because a low byte that wraps
    // (0xFF -> 0x03) breaks its own run while the pair it belongs to keeps
    // climbing. Pairs are preferred: an address consumed by one is never also
    // reported alone. Fewer than two intervals yields nothing.
    std::vector<ByteTrend> FindTrendingBytes(const std::vector<EepromSnapshot>& snapshots);

    // The readings as JSON, next to the model's known write path. No pad group
    // is invented: the addresses are observations, not a reset plan.
    std::string FormatDiscoveryJson(const std::string& modelName,
                                    const std::string& modelBodyJson,
                                    const std::vector<ByteTrend>& trends);

} // namespace ewr
