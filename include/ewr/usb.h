#pragma once
#include "ewr/gateway.h"
#include "ewr/run_lock.h"

#include <memory>
#include <string>
#include <vector>

namespace ewr::usb {

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

} // namespace ewr::usb

namespace ewr {

    // The first device call of a run starts ewr_trace.log fresh and every
    // later one appends, so a program run yields one coherent trace file.
    // The --usb-soft-reset switch follows the same lifecycle: only the first
    // session that asks for it gets it.
    class UsbDeviceGateway final : public IDeviceGateway
    {
    public:
        DeviceIdQueryResult QueryDeviceId();

        // Not part of IDeviceGateway on purpose: hosts list interfaces,
        // a Session never needs to.
        std::vector<usb::InterfaceInfo> ListInterfaces();

        QueryRunResult RunQuery(
            const std::vector<std::vector<unsigned char>>& handshake,
            const std::vector<std::vector<unsigned char>>& queries,
            const ExecutorOptions& options) override;

        ResetRunResult RunReset(
            const std::vector<std::vector<unsigned char>>& sequence,
            const ExecutorOptions& options) override;

        CommandRunResult RunCommand(const std::vector<unsigned char>& command,
                                    const ExecutorOptions& options) override;

        // Printer-class interfaces only, and only the first that opens: a
        // print job half on one interface and again on another is two jobs.
        PrintDataResult SendPrintData(const std::vector<unsigned char>& data,
                                      const ExecutorOptions& options) override;

        // Claims the printer for this process and holds it for the life of the
        // gateway. False means another EWR run already has it; see
        // ewr/run_lock.h for why that must not become two runs on one handle.
        //
        // A host calls this before it prints anything, so it can say why it
        // stopped in its own words - the CLI and the C API both claim at
        // startup, which is why a run parked at a prompt still holds the
        // printer. A host that does not bother still claims on its first
        // device call, so the lock cannot be skipped by forgetting it.
        bool ClaimPrinter();

        // Hands a held claim on to SnmpDeviceGateway::AdoptRunLock with no
        // moment in which another run could take it: a second claim in the
        // same process is refused like any other. nullptr when none is held.
        // This gateway is done after it; its next call would claim again.
        std::unique_ptr<RunLock> ReleaseRunLock();

        // The network search's notes into this run's ewr_trace.log: they start
        // it on a run that has not touched USB, and follow the USB scan on one
        // that has. Before ReleaseRunLock, since writing needs the claim.
        void AppendTrace(const std::string& text);

    private:
        // False exactly once: the call that must start the trace file fresh.
        bool NextCallAppends();

        // A soft reset re-initializes the printer. Between the preflight read
        // and the writes, or the writes and the read-back, that would reset
        // the very state the run just read or wrote.
        ExecutorOptions SoftResetOnce(ExecutorOptions options);

        // Fills the trace's "Printer reports" from the last device ID this
        // gateway read, for a host that did not name the printer itself.
        void RememberPrinterName(const std::string& deviceId);
        ExecutorOptions WithPrinterName(ExecutorOptions options) const;

        void ReportSilentPort(const SilentPortQueues& note);

        bool m_traceStarted = false;
        bool m_softResetSent = false;
        bool m_silentPortReported = false;
        std::string m_printerName;
        std::unique_ptr<RunLock> m_runLock;
    };

} // namespace ewr
