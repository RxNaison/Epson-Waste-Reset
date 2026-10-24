#pragma once
#include <cstdint>
#include <vector>

// ESC/P2 remote mode: the maintenance commands an Epson inkjet takes as print
// data, the way a nozzle check is printed. Framing from the protocol as Epson
// documents it and Gutenprint describes it (remote commands are two letters,
// a 2-byte little-endian parameter count, then the parameters); no code from
// either. Nothing here touches the EEPROM.
namespace ewr {
namespace remote {

    // Leaves IEEE 1284.4 packet mode, so what follows is plain print data. A
    // printer that is not in packet mode reads past it.
    extern const std::vector<unsigned char> kLeavePacketMode;

    // One remote command: `name` (two letters), the parameter count, the
    // parameters.
    std::vector<unsigned char> Command(char first, char second, const std::vector<unsigned char>& parameters);

    // "NC": print the nozzle check pattern. Variant 0 is the standard
    // pattern; 1 asks for the alternate one some models print.
    std::vector<unsigned char> NozzleCheck(int variant = 0);

    // "CH": a head cleaning. Group 0 is every color, 1 black, 2 color - the
    // last two are not taken by every model. `power` asks for the stronger
    // cycle (the 0x10 flag).
    std::vector<unsigned char> HeadClean(uint8_t group = 0, bool power = false);

    // A whole job: packet mode left, the printer initialized, remote mode
    // entered, the commands, remote mode left.
    std::vector<unsigned char> BuildJob(const std::vector<std::vector<unsigned char>>& commands);

} // namespace remote
} // namespace ewr
