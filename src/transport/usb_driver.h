#pragma once
#include "ewr/gateway.h"
#include "ewr/usb.h"

#include <cstdint>
#include <string>
#include <vector>

// The platform-neutral driver loop over every Epson interface: candidate
// iteration, the --interface pin, retry and fallback, and the trace.
namespace ewr::usb {

    constexpr uint16_t EPSON_VID = 0x04B8;

    // "Microsoft IPP Class Driver", the queue driver Windows Ready Print
    // installs. Under one, an XP-332 never answered D4 Init (#55) until
    // Epson's own driver replaced it - the queue's jobs went with it.
    bool IsReadyPrintDriver(const std::string& driver);

    // The warnings for a USB run that ended silent: jobs waiting in a print
    // queue on the printer's port, a Ready Print queue driver. False when
    // neither applies.
    bool ReportSilentPortQueues(const SilentPortQueues& note);

    // Drives every Epson interface candidate in turn, best first. With
    // options.validateHandshake on, an interface that stays silent during the
    // handshake falls through to the next one, which makes composite devices
    // with scanner-first layouts self-resolving.
    //
    // appendTraceLog keeps an already-open ewr_trace.log rather than starting
    // a fresh one, so a preflight and its reset share a single trace.
    ResetRunResult RunSequence(const std::vector<std::vector<unsigned char>>& sequence,
                               const ExecutorOptions& options,
                               bool appendTraceLog = false);

    // Read-only counterpart of RunSequence. Silent interfaces are skipped
    // automatically; never sends EEPROM writes.
    QueryRunResult RunQuerySession(const std::vector<std::vector<unsigned char>>& handshake,
                                   const std::vector<std::vector<unsigned char>>& queries,
                                   const ExecutorOptions& options,
                                   bool appendTraceLog = false);

    struct PrintDataRunResult
    {
        bool deviceFound = false;
        bool sent = false;
        std::string error;
    };

    // Raw print data to the first printer-class interface that opens (the
    // --interface pin, when set, if it is one). Never a vendor-specific
    // interface - on an ET-2xxx that is the maintenance engine, not the
    // print engine - and never a second interface after the first took bytes.
    PrintDataRunResult SendPrintData(const std::vector<unsigned char>& data,
                                     const ExecutorOptions& options,
                                     bool appendTraceLog = false);

    // Read-only survey, no D4 traffic. Powers --list, the multi-interface
    // banner and detection.
    std::vector<InterfaceInfo> ListInterfaces(bool appendTraceLog = false);

    // Windows: IOCTL_USBPRINT_GET_1284_ID. Linux: the printer-class
    // GET_DEVICE_ID control request. Read-only, no D4 traffic.
    DeviceIdQueryResult QueryDeviceId(bool appendTraceLog = false);

#ifdef __ANDROID__
    // The USB devices the app opened through UsbManager, as file descriptors
    // it keeps open. Each device call wraps them afresh; an empty list means
    // no printer is reachable over USB.
    void SetAndroidFds(const std::vector<int>& fds);
#endif

    // Text from outside a device session, such as the network search, into
    // ewr_trace.log under the same start-fresh-or-append rule.
    void AppendTraceNote(const std::string& text, bool appendTraceLog = false);

} // namespace ewr::usb
