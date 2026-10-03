#pragma once
#include "ewr/run_lock.h"
#include "ewr/session.h"
#include "ewr/snmp.h"

#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace ewr {

    // The printer's IEEE 1284 device ID, the same string the USB
    // GET_DEVICE_ID request returns.
    snmp::Oid SnmpDeviceIdOid();

    // The '@BDC ST2' status report as a plain MIB object.
    snmp::Oid SnmpStatusOid();

    // An EPSON-CTRL command as an OID: Epson's control node followed by the
    // command, one arc per byte.
    snmp::Oid SnmpControlOid(const std::vector<unsigned char>& command);

    // A network printer behind the same seam as a USB one. Status, EEPROM
    // reads and EEPROM writes all travel as SNMP GETs - a write is a GET whose
    // OID carries the write command, answered with the same ':42:OK;'.
    //
    // Each packet is the D4 data packet the generator built; its EPSON-CTRL
    // payload is lifted out and sent as an OID, and the answer is framed back
    // into a D4 data packet, so everything above the gateway - Session, the
    // status and EEPROM parsers - reads it as it reads USB.
    //
    // The two paths stay apart: RunQuery never sends a write, whatever it is
    // handed, and RunReset sends nothing but writes, and only from a
    // generated sequence whose every write it can confirm.
    class SnmpDeviceGateway final : public IDeviceGateway
    {
    public:
        // Production: UDP to `host`, tracing to ewr_trace.log. Neither is
        // opened until the run lock is claimed, so a second run cannot
        // truncate the trace of the one it was refused for.
        explicit SnmpDeviceGateway(const std::string& host);

        // Tests: a scripted channel, and a trace stream or none. Takes no run
        // lock: a live run on the same machine would fail every test.
        SnmpDeviceGateway(const std::string& host,
                          std::unique_ptr<snmp::IDatagramChannel> channel,
                          std::ostream* trace = nullptr);

        // UsbDeviceGateway::ClaimPrinter's twin, and the same lock: one run
        // at a time machine-wide, whichever transport it uses. Every device
        // call claims first, so a host cannot skip it by forgetting.
        bool ClaimPrinter();

        // Non-empty when no channel could be opened at all (a host name that
        // does not resolve). Every call then fails with it.
        const std::string& OpenError() const { return m_openError; }

        // Whether the printer has answered any request so far, including with
        // an SNMP error. Separates "nothing at this address speaks SNMP" from
        // "it answers, just not to this".
        bool Answered() const { return m_answered; }

        // No request ever left this machine: every send was refused locally.
        // That is this machine - a VPN, a firewall, or on macOS a terminal
        // without the Local Network permission - not a printer that is off.
        bool SendBlocked() const { return m_sendFailed && !m_sendSucceeded; }

        bool OverNetwork() const override { return true; }

        // Why nothing came back, in the words the CLI prints: the open
        // error, a send this machine refused, or plain silence.
        std::string SilenceError() const;

        DeviceIdQueryResult QueryDeviceId();

        QueryRunResult RunQuery(
            const std::vector<std::vector<unsigned char>>& handshake,
            const std::vector<std::vector<unsigned char>>& queries,
            const ExecutorOptions& options) override;

        ResetRunResult RunReset(
            const std::vector<std::vector<unsigned char>>& sequence,
            const ExecutorOptions& options) override;

    private:
        // False on silence. True with an empty `value` when the printer
        // answered with an SNMP error or something other than a string.
        bool Get(const snmp::Oid& oid, std::vector<unsigned char>& value);
        void Trace(const std::string& text);

        std::string m_host;
        std::string m_openError;
        std::unique_ptr<snmp::IDatagramChannel> m_channel;
        std::ofstream m_traceFile;
        std::ostream* m_trace = nullptr;
        std::unique_ptr<RunLock> m_runLock;
        int32_t m_nextRequestId = 1;
        bool m_claimsRunLock = false;
        bool m_started = false;
        bool m_answered = false;
        bool m_sendFailed = false;
        bool m_sendSucceeded = false;
    };

} // namespace ewr
