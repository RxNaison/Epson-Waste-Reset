#pragma once
#include <cstdint>
#include <string>
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
        bool valid = false;           // a well-formed ST2 payload was parsed
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

    // Concatenated EPSON-CTRL payload bytes, with socket 0 credit/control
    // traffic skipped. Bytes that do not line up with a plausible D4 header are
    // dropped one at a time, so a partial capture still yields what arrived.
    std::vector<unsigned char> ExtractD4Payload(const std::vector<unsigned char>& raw);

    // valid=false when the stream holds no parseable status.
    PrinterStatus ParseStatusReply(const std::vector<unsigned char>& raw);

    // Two firmware reply forms exist:
    //   A) '...:41:HH;'  - action-code echo followed by the value (HH).
    //   B) '...EE:AAVV;' - address echo (AA) followed by the value (VV),
    //      seen on Stylus Photo-era firmware (e.g. R220).
    // When expectedAddress is >= 0 and the reply echoes an address (form B),
    // a mismatching echo is rejected. Returns true and sets value on success.
    bool ParseEepromReadReply(const std::vector<unsigned char>& raw, uint8_t& value, int expectedAddress = -1);

    std::string DescribePrinterCondition(const PrinterStatus& status);

} // namespace ewr
