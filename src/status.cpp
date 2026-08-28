#include "ewr/status.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace ewr {

    namespace {

        // Names for the ESC/P-R status constants (values published in Epson's
        // GPLv2 epson-inkjet-escpr driver package).
        std::string PrinterStateName(int code)
        {
            switch (code)
            {
                case 0x00: return "ERROR";
                case 0x01: return "SELF PRINTING";
                case 0x02: return "BUSY";
                case 0x03: return "WAITING";
                case 0x04: return "IDLE";
                case 0x05: return "PAUSE";
                case 0x06: return "INK DRYING";
                case 0x07: return "CLEANING";
                case 0x08: return "FACTORY SHIPMENT";
                case 0x09: return "MOTOR DRIVE OFF";
                case 0x0A: return "SHUTDOWN";
                case 0x0B: return "WAIT PAPER INIT";
                case 0x0C: return "INIT PAPER";
                default:   return "UNKNOWN STATE";
            }
        }

        std::string PrinterErrorName(int code)
        {
            switch (code)
            {
                case 0x00: return "FATAL";
                case 0x01: return "INTERFACE";
                case 0x04: return "PAPER JAM";
                case 0x05: return "INK OUT";
                case 0x06: return "PAPER OUT";
                case 0x0A: return "PAPER SIZE";
                case 0x0C: return "PAPER PATH";
                case 0x10: return "SERVICE REQUEST";
                case 0x12: return "DOUBLE FEED";
                case 0x1A: return "INK COVER OPEN";
                case 0x22: return "NO MAINTENANCE BOX";
                case 0x25: return "COVER OPEN";
                case 0x29: return "NO TRAY";
                case 0x2A: return "CARD LOADING";
                case 0x2B: return "CD/DVD CONFIG";
                case 0x2C: return "CARTRIDGE OVERFLOW";
                case 0x36: return "MAINTENANCE BOX COVER OPEN";
                case 0x37: return "SCANNER OPEN";
                default:   return "UNKNOWN ERROR";
            }
        }

        std::string InkColorName(int code)
        {
            switch (code)
            {
                case 0:  return "Black";
                case 1:  return "Cyan";
                case 2:  return "Magenta";
                case 3:  return "Yellow";
                case 4:  return "Light Cyan";
                case 5:  return "Light Magenta";
                case 6:  return "Dark Yellow";
                case 7:  return "Gray";
                case 8:  return "Light Black";
                case 9:  return "Red";
                case 10: return "Blue";
                case 11: return "Gloss Optimizer";
                case 12: return "Light Gray";
                case 13: return "Orange";
                default: return "Ink " + std::to_string(code);
            }
        }

        // Consumable level encoding: 110 = missing, 105 = unknown, 0..100 = fill.
        void DecodeConsumableLevel(int rawLevel, int& level, std::string& text)
        {
            if (rawLevel == 110) { level = -1; text = "MISSING"; return; }
            if (rawLevel == 105) { level = -1; text = "UNKNOWN"; return; }
            if (rawLevel < 0 || rawLevel > 100) { level = -1; text = "FAIL"; return; }
            if (rawLevel == 0)   { level = 0;  text = "EMPTY"; return; }

            level = rawLevel;
            text = "OK";
        }

    } // namespace

    std::vector<unsigned char> ExtractD4Payload(const std::vector<unsigned char>& raw)
    {
        std::vector<unsigned char> payload;

        size_t i = 0;
        while (i + 6 <= raw.size())
        {
            const unsigned char psid = raw[i];
            const unsigned char ssid = raw[i + 1];
            const size_t len = (static_cast<size_t>(raw[i + 2]) << 8) | raw[i + 3];

            const bool plausible = len >= 6 && i + len <= raw.size();

            if (plausible && psid == ssid && psid != 0x00)
            {
                // Data packet on the negotiated socket - GetSocketID may answer
                // with something other than the well-known 2, and the session
                // frames its replies with whatever it got. Same rule the D4
                // layer itself applies in ExtractDataPayload.
                payload.insert(payload.end(), raw.begin() + i + 6, raw.begin() + i + len);
                i += len;
            }
            else if (plausible && psid == 0x00 && ssid == 0x00)
            {
                // Transaction-channel packet (credit / control traffic): skip.
                i += len;
            }
            else
            {
                // Not aligned with a D4 header: resync one byte at a time.
                ++i;
            }
        }

        return payload;
    }

    PrinterStatus ParseStatusReply(const std::vector<unsigned char>& raw)
    {
        PrinterStatus status;

        const std::vector<unsigned char> payload = ExtractD4Payload(raw);
        const std::string haystack(payload.begin(), payload.end());
        static const std::string kPrefix = "@BDC ST2\r\n";

        const size_t pos = haystack.find(kPrefix);
        if (pos == std::string::npos)
            return status;

        const size_t bodyStart = pos + kPrefix.size();
        if (bodyStart + 2 > payload.size())
            return status;

        // 2-byte little-endian struct length, then [header, size, data] fields.
        const size_t declared = payload[bodyStart] | (static_cast<size_t>(payload[bodyStart + 1]) << 8);
        const size_t end = std::min(bodyStart + 2 + declared, payload.size());

        size_t fieldsDecoded = 0;

        size_t i = bodyStart + 2;
        while (i + 2 <= end)
        {
            const unsigned char header = payload[i];
            const unsigned char fieldLen = payload[i + 1];
            i += 2;

            // The report announced more than it delivered. Keep what parsed,
            // but record that the rest never arrived.
            if (i + fieldLen > end)
            {
                status.truncated = true;
                break;
            }

            const unsigned char* field = payload.data() + i;

            bool recognized = true;

            switch (header)
            {
                case 0x01: // printer state
                    if (fieldLen >= 1)
                    {
                        status.stateCode = field[0];
                        status.stateName = PrinterStateName(field[0]);
                    }
                    break;

                case 0x02: // error state
                    if (fieldLen >= 1)
                    {
                        status.hasError = true;
                        status.errorCode = field[0];
                        status.errorName = PrinterErrorName(field[0]);
                    }
                    break;

                case 0x0D: // maintenance box level
                    if (fieldLen >= 1)
                        DecodeConsumableLevel(field[0], status.maintenanceBoxLevel, status.maintenanceBoxText);
                    break;

                case 0x0F: // ink levels: [entry_size, entry0..., entry1...]
                    if (fieldLen >= 1)
                    {
                        const size_t entrySize = field[0];
                        if (entrySize >= 3)
                        {
                            for (size_t off = 1; off + entrySize <= fieldLen; off += entrySize)
                            {
                                InkReading ink;
                                ink.colorCode = field[off + 1];
                                ink.colorName = InkColorName(field[off + 1]);
                                DecodeConsumableLevel(field[off + 2], ink.level, ink.statusText);
                                status.inks.push_back(ink);
                            }
                        }
                    }
                    break;

                case 0x40: // serial number (printable characters only)
                    for (size_t off = 0; off < fieldLen; ++off)
                    {
                        if (std::isprint(static_cast<int>(field[off])))
                            status.serial += static_cast<char>(field[off]);
                    }
                    break;

                default:
                    recognized = false;
                    break;
            }

            // A recognized header with an empty body decodes nothing, so it is
            // no evidence about the printer either.
            if (recognized && fieldLen >= 1)
                ++fieldsDecoded;

            i += fieldLen;
        }

        // An '@BDC ST2' prefix with a length field and nothing readable behind
        // it is not a status report. Reporting it as valid would tell the
        // blocker gate the printer is clear when nothing was ever read.
        status.valid = fieldsDecoded > 0;
        return status;
    }

    bool ParseEepromReadReply(const std::vector<unsigned char>& raw, uint8_t& value, int expectedAddress)
    {
        const std::vector<unsigned char> payload = ExtractD4Payload(raw);
        const std::string haystack(payload.begin(), payload.end());

        const size_t prefix = haystack.find("@BDC PS\r\n");
        if (prefix == std::string::npos)
            return false;

        auto hexNibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        auto readHexByte = [&](size_t pos, uint8_t& out) -> bool
        {
            if (pos + 1 >= haystack.size())
                return false;
            const int hi = hexNibble(haystack[pos]);
            const int lo = hexNibble(haystack[pos + 1]);
            if (hi < 0 || lo < 0)
                return false;
            out = static_cast<uint8_t>((hi << 4) | lo);
            return true;
        };

        // Form A: '...:41:HH;' - the reply echoes the action code followed by
        // the EEPROM byte value in ASCII hex. Token search keeps this robust
        // to firmware-specific bytes between the prefix and the echo.
        const size_t marker = haystack.find(":41:", prefix);
        if (marker != std::string::npos)
            return readHexByte(marker + 4, value);

        // Form B: '...EE:AAVV;' or '...EE:AAAAVV;' - the reply echoes the
        // EEPROM address followed by the value in ASCII hex. The address echo
        // is as wide as the model's address field: one byte on Stylus
        // Photo-era firmware (e.g. R220), two bytes (big endian) on models
        // with rlen == 2. It doubles as an integrity check when the caller
        // knows which address it asked for.
        const size_t ee = haystack.find("EE:", prefix);
        if (ee != std::string::npos)
        {
            const size_t body = ee + 3;
            const size_t term = haystack.find(';', body);
            if (term == std::string::npos)
                return false;

            // 4 hex digits: 1-byte address + value. 6: 2-byte address + value.
            const size_t digits = term - body;
            if (digits != 4 && digits != 6)
                return false;

            const size_t addrBytes = (digits - 2) / 2;
            uint32_t addr = 0;
            for (size_t i = 0; i < addrBytes; ++i)
            {
                uint8_t b = 0;
                if (!readHexByte(body + i * 2, b))
                    return false;
                addr = (addr << 8) | b;
            }

            uint8_t val = 0;
            if (!readHexByte(body + addrBytes * 2, val))
                return false;

            // A 1-byte echo cannot confirm an address above 0xFF, so only
            // compare what the firmware actually sent back.
            if (expectedAddress >= 0 && (addrBytes == 2 || expectedAddress <= 0xFF) &&
                static_cast<uint32_t>(expectedAddress) != addr)
                return false;

            value = val;
            return true;
        }

        return false;
    }

    std::string DescribePrinterCondition(const PrinterStatus& status)
    {
        if (!status.valid)
            return "status unavailable";

        std::ostringstream oss;
        oss << (status.stateName.empty() ? "UNKNOWN STATE" : status.stateName);

        if (status.hasError)
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%02X", status.errorCode);
            oss << " | ERROR: " << status.errorName << " (" << buf << ")";
        }

        return oss.str();
    }

} // namespace ewr
