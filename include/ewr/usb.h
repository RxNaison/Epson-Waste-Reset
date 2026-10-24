#pragma once
#include "ewr/payload.h"
#include "ewr/executor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ewr {

    constexpr uint16_t EPSON_VID = 0x04B8;

    // One spooler queue as Windows lists it. `ports` is the queue's own
    // comma-separated list: a pooled queue names several.
    struct PrintQueue
    {
        std::string name;
        std::string ports;
        std::string driver;
        // Not yet printed. A job stuck in "Deleting" counts: on an R220 a
        // waiting job stalled every EWR write, and cancelling it left the
        // printer blocked until a power cycle.
        int waitingJobs = 0;
    };

    // "Microsoft IPP Class Driver", the queue driver Windows Ready Print
    // installs. Under one, an XP-332 never answered D4 Init (#55) until
    // Epson's own driver replaced it - the queue's jobs went with it.
    bool IsReadyPrintDriver(const std::string& driver);

    // The spooler queues on the port of the first interface whose handshake
    // stayed silent. Empty unless the whole run ended silent; Windows
    // usbprint only.
    struct SilentPortQueues
    {
        // "USB001".
        std::string port;
        std::vector<PrintQueue> queues;
    };

    struct ResetRunResult
    {
        bool deviceFound = false;
        size_t candidatesTried = 0;
        // The attempt that ran to completion, or the last one tried.
        ExecutionResult exec;
        SilentPortQueues silentPort;
    };

    // Drives every Epson interface candidate in turn, best first. With
    // options.validateHandshake on, an interface that stays silent during the
    // handshake falls through to the next one, which makes composite devices
    // with scanner-first layouts self-resolving.
    //
    // appendTraceLog keeps an already-open ewr_trace.log rather than starting
    // a fresh one, so a preflight and its reset share a single trace.
    ResetRunResult ExecutePayloadSequenceWithFallback(
        const std::vector<std::vector<unsigned char>>& sequence,
        const ExecutorOptions& options,
        bool appendTraceLog = false);

    struct QueryRunResult
    {
        bool deviceFound = false;
        size_t candidatesTried = 0;
        // The session that ran to completion, or the last one tried.
        QuerySessionResult query;
        SilentPortQueues silentPort;
    };

    // Read-only counterpart of ExecutePayloadSequenceWithFallback. Silent
    // interfaces are skipped automatically; never sends EEPROM writes.
    QueryRunResult ExecuteQuerySessionWithFallback(
        const std::vector<std::vector<unsigned char>>& handshake,
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
    PrintDataRunResult SendPrintDataWithFallback(const std::vector<unsigned char>& data,
                                                 const ExecutorOptions& options,
                                                 bool appendTraceLog = false);

    // `index` is the 1-based number --list shows and --interface accepts.
    // Stable for fixed hardware: the enumeration sort is deterministic.
    struct InterfaceInfo
    {
        int index = 0;
        // Windows: setup class (USBPRINT, IMAGE, ...); Linux: PRINTER/VENDOR.
        std::string className;
        // mi_XX on Windows; -1 when the device is not composite.
        int interfaceNumber = -1;
        // Windows: OS device path. Linux: a vid/pid/interface summary.
        std::string path;
        // Printer-class interfaces only; empty when nothing answered.
        std::string deviceId;
    };

    // Read-only survey, no D4 traffic. Powers --list, the multi-interface
    // banner and detection.
    std::vector<InterfaceInfo> ListPrinterInterfaces(bool appendTraceLog = false);

    struct DeviceIdQueryResult
    {
        bool found = false;
        // Sanitized raw device ID ("MFG:EPSON;...;MDL:ET-2800 Series;...").
        std::string deviceId;
    };

    // Windows: IOCTL_USBPRINT_GET_1284_ID. Linux: the printer-class
    // GET_DEVICE_ID control request. Read-only, no D4 traffic.
    DeviceIdQueryResult QueryPrinterDeviceId(bool appendTraceLog = false);

#ifdef __ANDROID__
    // The USB devices the app opened through UsbManager, as file descriptors
    // it keeps open. Each device call wraps them afresh; an empty list means
    // no printer is reachable over USB.
    void SetAndroidUsbFds(const std::vector<int>& fds);
#endif

    // Text from outside a device session, such as the network search, into
    // ewr_trace.log under the same start-fresh-or-append rule.
    void AppendTraceNote(const std::string& text, bool appendTraceLog = false);

}
