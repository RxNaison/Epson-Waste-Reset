#pragma once
#include "ewr/payload.h"
#include "ewr/executor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ewr {

    constexpr uint16_t EPSON_VID = 0x04B8;

    struct ResetRunResult
    {
        bool deviceFound = false;
        size_t candidatesTried = 0;
        // The attempt that ran to completion, or the last one tried.
        ExecutionResult exec;
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
    };

    // Read-only counterpart of ExecutePayloadSequenceWithFallback. Silent
    // interfaces are skipped automatically; never sends EEPROM writes.
    QueryRunResult ExecuteQuerySessionWithFallback(
        const std::vector<std::vector<unsigned char>>& handshake,
        const std::vector<std::vector<unsigned char>>& queries,
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

}
