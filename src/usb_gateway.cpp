#include "ewr/usb.h"
#include "ewr/ctrl.h"
#include "ewr/d4session.h"
#include "ewr/deviceid.h"
#include "ewr/jig.h"
#include "ewr/log.h"
#include "ewr/usb_driver.h"

#include <memory>
#include <string>
#include <vector>

namespace ewr {

    namespace {

        const char* const kAnotherRunError =
            "Another EWR run is already driving a printer on this machine.";

    } // namespace

    bool UsbDeviceGateway::NextCallAppends()
    {
        const bool append = m_traceStarted;
        m_traceStarted = true;
        return append;
    }

    // Two runs share one usbprint handle and take each other's replies, and
    // the loser may be the one mid-write, so the second run does not start.
    bool UsbDeviceGateway::ClaimPrinter()
    {
        if (!m_runLock)
            m_runLock = std::make_unique<RunLock>();

        if (m_runLock->Held())
            return true;

        log::Log(log::Level::Error, log::Stage::Detect, "usb.another_run",
                 "[!] Another EWR run is already driving a printer on this machine.\n"
                 "    Two runs share one USB handle and take each other's replies, so this one\n"
                 "    stops here. Close the other run - or look for a leftover ewr process\n"
                 "    waiting at a prompt - and try again.");
        return false;
    }

    void UsbDeviceGateway::AppendTrace(const std::string& text)
    {
        if (!ClaimPrinter())
            return;

        usb::AppendTraceNote(text, NextCallAppends());
    }

    std::unique_ptr<RunLock> UsbDeviceGateway::ReleaseRunLock()
    {
        if (!m_runLock || !m_runLock->Held())
            return nullptr;

        return std::move(m_runLock);
    }

    DeviceIdQueryResult UsbDeviceGateway::QueryDeviceId()
    {
        if (!ClaimPrinter())
            return {};

        DeviceIdQueryResult result = usb::QueryDeviceId(NextCallAppends());
        RememberPrinterName(result.deviceId);
        return result;
    }

    std::vector<usb::InterfaceInfo> UsbDeviceGateway::ListInterfaces()
    {
        if (!ClaimPrinter())
            return {};

        std::vector<usb::InterfaceInfo> interfaces = usb::ListInterfaces(NextCallAppends());
        for (const usb::InterfaceInfo& info : interfaces)
        {
            if (!info.deviceId.empty())
            {
                RememberPrinterName(info.deviceId);
                break;
            }
        }
        return interfaces;
    }

    void UsbDeviceGateway::RememberPrinterName(const std::string& deviceId)
    {
        const std::string name = ParseIeee1284DeviceId(deviceId).model;
        if (!name.empty())
            m_printerName = name;
    }

    ExecutorOptions UsbDeviceGateway::WithPrinterName(ExecutorOptions options) const
    {
        if (options.trace.printerReports.empty())
            options.trace.printerReports = m_printerName;
        return options;
    }

    // Once per run: a failed read is often followed by another that fails the
    // same way.
    void UsbDeviceGateway::ReportSilentPort(const SilentPortQueues& note)
    {
        if (!m_silentPortReported)
            m_silentPortReported = usb::ReportSilentPortQueues(note);
    }

    ExecutorOptions UsbDeviceGateway::SoftResetOnce(ExecutorOptions options)
    {
        if (m_softResetSent)
            options.usbSoftReset = false;

        m_softResetSent = m_softResetSent || options.usbSoftReset;
        return options;
    }

    QueryRunResult UsbDeviceGateway::RunQuery(
        const std::vector<std::vector<unsigned char>>& handshake,
        const std::vector<std::vector<unsigned char>>& queries,
        const ExecutorOptions& options)
    {
        if (!ClaimPrinter())
        {
            QueryRunResult busy;
            busy.query.error = kAnotherRunError;
            return busy;
        }

        // JIG mode has reads of its own shape, and nothing else.
        for (const auto& query : queries)
        {
            const bool read = options.jigMode ? jig::ReadPacketAddress(query) >= 0 : ctrl::IsReadOnlyQuery(query);
            if (!read)
            {
                QueryRunResult refused;
                refused.query.error = "A query session was handed a packet that is not a read; nothing was sent.";
                log::Log(log::Level::Error, log::Stage::Read, "usb.query_not_read_only",
                         "[ERROR] " + refused.query.error);
                return refused;
            }
        }

        QueryRunResult run = usb::RunQuerySession(handshake, queries, WithPrinterName(SoftResetOnce(options)),
                                                             NextCallAppends());
        ReportSilentPort(run.silentPort);
        return run;
    }

    CommandRunResult UsbDeviceGateway::RunCommand(const std::vector<unsigned char>& command,
                                                  const ExecutorOptions& options)
    {
        CommandRunResult out;
        out.supported = true;

        if (!ClaimPrinter())
        {
            out.error = kAnotherRunError;
            return out;
        }

        // One attempt, one session: the fallback moves to another interface
        // only when a handshake fails, which is before the command is sent.
        ExecutorOptions once = WithPrinterName(SoftResetOnce(options));
        once.maxWriteAttempts = 1;

        const QueryRunResult run = usb::RunQuerySession(d4::Handshake(),
                                                                   { command }, once, NextCallAppends());
        ReportSilentPort(run.silentPort);

        out.deviceFound = run.deviceFound;
        out.sent = run.deviceFound && !run.query.handshakeFailed && run.query.packetsSent > 0;
        if (!run.query.replies.empty())
            out.reply = run.query.replies[0];
        out.error = run.query.error;
        return out;
    }

    PrintDataResult UsbDeviceGateway::SendPrintData(const std::vector<unsigned char>& data,
                                                    const ExecutorOptions& options)
    {
        PrintDataResult out;
        out.supported = true;

        if (!ClaimPrinter())
        {
            out.error = kAnotherRunError;
            return out;
        }

        const usb::PrintDataRunResult run = usb::SendPrintData(data, WithPrinterName(SoftResetOnce(options)),
                                                                 NextCallAppends());
        out.deviceFound = run.deviceFound;
        out.sent = run.sent;
        out.error = run.error;
        return out;
    }

    ResetRunResult UsbDeviceGateway::RunReset(
        const std::vector<std::vector<unsigned char>>& sequence,
        const ExecutorOptions& options)
    {
        if (!ClaimPrinter())
        {
            ResetRunResult busy;
            busy.exec.error = kAnotherRunError;
            return busy;
        }

        ResetRunResult run = usb::RunSequence(sequence, WithPrinterName(SoftResetOnce(options)),
                                                                NextCallAppends());
        ReportSilentPort(run.silentPort);
        return run;
    }

} // namespace ewr
