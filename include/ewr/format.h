#pragma once
#include <cstddef>
#include <string>

namespace ewr::format {

    std::string HexDump(const unsigned char* data, size_t size);

    // A drain may return kMaxDrainBytes, a quarter megabyte of hex per call.
    // The head of a reply is what identifies it.
    inline constexpr size_t kTraceDumpCapBytes = 512;

    std::string HexDumpCapped(const unsigned char* data, size_t size, size_t maxBytes);

    // Now, in UTC, as ISO 8601 (Backup::created).
    std::string UtcTimestamp();

} // namespace ewr::format
