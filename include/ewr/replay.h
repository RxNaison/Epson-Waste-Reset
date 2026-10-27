#pragma once
#include <cstddef>
#include <string>
#include <vector>

// Replaying a Wireshark capture (models/*.c): opaque bytes of unknown shape,
// sent as they are on the legacy executor.
namespace ewr::replay {

    struct Capture
    {
        std::string name;
        std::string filepath;
    };

    std::vector<Capture> ScanFolder(const std::string& folderPath);
    std::vector<std::vector<unsigned char>> ParseWiresharkDump(const std::string& filepath);

    // How many of `packets` are EEPROM writes.
    size_t CountWrites(const std::vector<std::vector<unsigned char>>& packets);

} // namespace ewr::replay
