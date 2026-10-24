#pragma once
#include "ewr/generator.h"
#include "ewr/session.h"
#include "ewr/status.h"

#include <string>
#include <vector>

// What a printer says about itself, read-only: firmware version, serial
// number, the counters it keeps (pages, cleanings, ink changes) and the
// codes of its last fatal errors. The counters' and codes' addresses come
// from the database; without an entry, the status and firmware are all
// there is.
namespace ewr {

    struct InfoCounter
    {
        std::string key;  // the database's: "total_print_page_counter"
        long long value = -1; // -1 when a byte did not answer
    };

    struct InfoFatal
    {
        std::string kind; // "printer", "scanner", "general", "battery"
        int slot = 1;     // 1 = the most recent
        int code = -1;    // -1 when unread; 0 means no error was logged
    };

    struct PrinterInfo
    {
        bool available = false; // the printer answered at all
        PrinterStatus status;
        std::string firmware;     // empty when it did not say
        std::string serial;
        std::string serialSource; // "status" or "eeprom"; empty without a serial
        std::vector<InfoCounter> counters;
        long long firstUse = -1;  // raw; the encoding is not known yet
        std::vector<InfoFatal> fatal;
    };

    // The 'vi' answer's version text: "vi:00:<text>;" -> "<text>". Empty on
    // a refusal or another shape.
    std::string ParseVersionReply(const std::vector<unsigned char>& raw);

    // `model` may be null: then only the status and the firmware are read.
    PrinterInfo ReadPrinterInfo(IDeviceGateway& gateway, const DbPrinterModel* model,
                                log::Reporter& reporter = log::Default(),
                                const ExecutorOptions& options = DefaultQueryOptions());

} // namespace ewr
