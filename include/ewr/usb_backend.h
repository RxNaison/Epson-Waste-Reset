#pragma once
#include "ewr/executor.h"
#include "ewr/usb_role.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace ewr {

    // Platform-neutral view of one interface, in fallback order. The backend
    // keeps its own state (device paths, endpoint addresses) keyed by
    // `ordinal`; nothing platform-specific crosses this struct.
    struct UsbCandidate
    {
        std::size_t ordinal = 0;
        // Windows: setup class (USBPRINT, IMAGE, ...); Linux: PRINTER/VENDOR.
        std::string className;
        // mi_XX / bInterfaceNumber; -1 when unknown.
        int interfaceNumber = -1;
        // Windows: OS device path. Linux: a vid/pid/interface summary.
        std::string path;
        // Four lowercase hex digits, or "UNKNOWN" when unparsable.
        std::string pid;
        // Positive function classification. Printer and Maintenance are
        // intentional write-capable candidates; Unknown is compatibility-only,
        // while Scanner is survey/read-only material and is never selected
        // automatically for maintenance writes.
        UsbCandidateRole role = UsbCandidateRole::Unknown;
        // Diagnostic metadata (Windows PnP service/class/instance ID). Empty
        // on backends that do not expose an equivalent concept.
        std::string serviceName;
        std::string pnpClass;
        std::string instanceId;
    };

    // Platform seam for the driver loop in usb_driver.cpp. The driver owns
    // candidate iteration, pinning, retry-and-fallback and the trace
    // choreography; a backend owns enumeration, open/close and raw I/O.
    // usb_windows.cpp and usb_libusb.cpp each implement it; a Windows build
    // runs both behind the composite in usb_composite.cpp.
    class UsbBackend
    {
    public:
        virtual ~UsbBackend() = default;

        // "Windows" or "libusb"; used in trace-log banners and user messages.
        virtual const char* PlatformName() const = 0;

        // Non-empty aborts the run with this as the error.
        virtual const std::string& InitError() const = 0;

        // Best first. Writes discovery detail to the trace. Once per run.
        virtual std::vector<UsbCandidate> Enumerate() = 0;

        virtual std::string Describe(std::size_t ordinal) const = 0;

        // Opens candidate `ordinal` and returns its transport, or nullptr
        // after tracing the failure. The transport stays valid until Close().
        //
        // softReset asks the backend to clear the channel on the fresh handle
        // before any handshake byte (Windows: IOCTL_USBPRINT_SOFT_RESET). It
        // is off by default: on ET-2xxx units the reset leaves the bulk-OUT
        // pipe unable to accept a write, and the stall outlives the handle.
        virtual ITransport* Open(std::size_t ordinal, bool softReset) = 0;

        virtual void Close() = 0;

        // Full sessions (Open -> handshake -> Close) on candidate `ordinal`
        // before the driver falls through to the next. usbprint: 2, libusb: 1,
        // and a Windows run carries both - hence per candidate, not per
        // backend. There is no in-place retry: a failed session is always
        // closed first, so the second attempt inherits nothing from the first.
        virtual int AttemptsPerCandidate(std::size_t ordinal) const = 0;

        // "" when the candidate's class cannot answer.
        virtual std::string QueryDeviceId(std::size_t ordinal) = 0;

        // No candidate opened at all: writes the trace diagnosis, emits the
        // user-facing hints (busy Status Monitor, sudo, ...), returns the error.
        virtual std::string DescribeOpenFailure(bool payloadRun) = 0;
    };

    // `trace` is the run's single open trace stream and must outlive the
    // backend, which must never reopen ewr_trace.log itself.
    std::unique_ptr<UsbBackend> CreateUsbBackend(std::ostream& trace);

    struct UsbBackendMember
    {
        std::unique_ptr<UsbBackend> backend;
        // Prefixed onto this member's class names ("libusb:VENDOR"). Empty
        // leaves them as the member reported them.
        std::string tag;
    };

    // Candidates concatenate in member order, the first member to list an
    // interface keeps it, and every ordinal routes back to its owner. Only
    // Windows composes (usbprint.sys + libusb).
    std::unique_ptr<UsbBackend> CreateCompositeUsbBackend(std::vector<UsbBackendMember> members,
                                                          std::ostream& trace);

} // namespace ewr
