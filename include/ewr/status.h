#pragma once
#include "ewr/gateway.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ewr {

    // Decoded ESC/P-R printer status (the '@BDC ST2' reply). Field meanings
    // and constants follow the values published in Epson's GPLv2
    // epson-inkjet-escpr driver package; this is a clean-room implementation.

    struct InkReading
    {
        int colorCode = -1;
        std::string colorName;   // "Black", "Cyan", ... or "Ink <n>"
        int level = -1;          // 0..100, -1 when not readable
        std::string statusText;  // "OK", "EMPTY", "MISSING", "UNKNOWN", "FAIL"
    };

    struct PrinterStatus
    {
        bool valid = false;           // at least one ST2 field was decoded
        bool truncated = false;       // the field walk hit the end of the payload
        int stateCode = -1;
        std::string stateName;        // "IDLE", "BUSY", ...
        bool hasError = false;        // the printer reported an error entry
        int errorCode = -1;
        std::string errorName;        // "INK OUT", "PAPER JAM", ...
        int maintenanceBoxLevel = -1; // 0..100, -1 when not readable
        std::string maintenanceBoxText;
        std::string serial;
        std::vector<InkReading> inks;
    };

    // How a printer answered one EEPROM read. The refusals are kept apart
    // because they are not the same evidence: ':41:NA;' names the read and
    // turns it down (an R220 with any wrong key), while a bare '||:;' does
    // not even echo the action code (an SX110, with the one key tried on it).
    enum class ReadAnswer
    {
        Value,    // the byte came back: this key works
        Refused,  // '||:41:NA;'
        Empty,    // '||:;'
        Silent,   // nothing arrived
        Other,    // something else entirely
    };

    // Parsed '@BDC ST2' status plus raw counter bytes (address -> value).
    struct StateSnapshot
    {
        bool available = false; // a device answered the query session
        PrinterStatus status;
        std::vector<std::pair<uint16_t, int>> values;
    };

} // namespace ewr

namespace ewr::status {

    // Model-free status read: just the '@BDC ST2' report, no counter reads
    // (those need a model's read key). What the replay path can know about
    // a printer.
    StateSnapshot Read(IDeviceGateway& gateway, const ExecutorOptions& options = ExecutorOptions::ForQueries());

    std::string Describe(const PrinterStatus& status);

} // namespace ewr::status
