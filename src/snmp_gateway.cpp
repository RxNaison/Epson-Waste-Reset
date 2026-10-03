#include "ewr/snmp_gateway.h"
#include "ewr/d4session.h"
#include "ewr/deviceid.h"
#include "ewr/version.h"

#include <chrono>

namespace ewr {

    namespace {

        // Epson printers answer reads on the default community.
        const char* const kCommunity = "public";

        // A printer on Wi-Fi can drop a datagram or be slow to wake, so each
        // request is sent up to this many times. A read is idempotent, and the
        // repeat carries the same request ID, so a late answer to an earlier
        // send is still the answer.
        constexpr int kAttempts = 3;
        constexpr int kAttemptTimeoutMs = 1000;

        // Frames a reply the way the D4 session hands one up: a data packet
        // on the EPSON-CTRL socket, end-of-message set.
        std::vector<unsigned char> FrameAsD4Data(const std::vector<unsigned char>& value)
        {
            constexpr std::size_t kHeader = 6;
            const std::size_t body = (value.size() > 0xFFFF - kHeader) ? (0xFFFF - kHeader) : value.size();
            const std::size_t total = kHeader + body;

            std::vector<unsigned char> packet = {
                EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL,
                static_cast<unsigned char>((total >> 8) & 0xFF),
                static_cast<unsigned char>(total & 0xFF),
                0x00, 0x01
            };
            packet.insert(packet.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(body));
            return packet;
        }

        bool IsStatusCommand(const std::vector<unsigned char>& command)
        {
            return command.size() >= 2 && command[0] == 's' && command[1] == 't';
        }

        // UsbDeviceGateway's wording, so a host sees one reason either way.
        const char* const kAnotherRunError =
            "Another EWR run is already driving a printer on this machine.";

    } // namespace

    snmp::Oid SnmpDeviceIdOid()
    {
        return { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 1, 1, 1, 1, 1 };
    }

    snmp::Oid SnmpStatusOid()
    {
        return { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 1, 1, 1, 4, 1 };
    }

    snmp::Oid SnmpControlOid(const std::vector<unsigned char>& command)
    {
        snmp::Oid oid = { 1, 3, 6, 1, 4, 1, 1248, 1, 2, 2, 44, 1, 1, 2, 1 };
        oid.insert(oid.end(), command.begin(), command.end());
        return oid;
    }

    SnmpDeviceGateway::SnmpDeviceGateway(const std::string& host)
        : m_host(host), m_claimsRunLock(true)
    {
    }

    SnmpDeviceGateway::SnmpDeviceGateway(const std::string& host,
                                         std::unique_ptr<snmp::IDatagramChannel> channel,
                                         std::ostream* trace)
        : m_host(host), m_channel(std::move(channel)), m_trace(trace), m_started(true)
    {
    }

    bool SnmpDeviceGateway::ClaimPrinter()
    {
        if (!m_claimsRunLock)
            return true;

        if (!m_runLock)
            m_runLock = std::make_unique<RunLock>();

        if (!m_runLock->Held())
        {
            log::Log(log::Level::Error, log::Stage::Detect, "snmp.another_run",
                     "[!] " + std::string(kAnotherRunError) + "\n"
                     "    Two runs could be driving the same printer, one of them mid-write, so this\n"
                     "    one stops here. Close the other run - or look for a leftover ewr process\n"
                     "    waiting at a prompt - and try again.");
            return false;
        }

        if (!m_started)
        {
            m_started = true;

            m_traceFile.open("ewr_trace.log", std::ios::out | std::ios::trunc);
            m_trace = &m_traceFile;

            Trace("==================================================\n"
                  "EWR NETWORK TRACE LOG (SNMP v1)\n"
                  "EWR Version: " + std::string(EWR_VERSION) + "\n"
                  "Printer address: " + m_host + ", UDP " + std::to_string(snmp::kPort) + "\n"
                  "==================================================\n\n");

            m_channel = snmp::OpenUdpChannel(m_host, snmp::kPort, m_openError);
            if (!m_channel)
                Trace("[!] " + m_openError + "\n");
        }

        return true;
    }

    void SnmpDeviceGateway::Trace(const std::string& text)
    {
        if (!m_trace)
            return;

        (*m_trace) << text;
        m_trace->flush();
    }

    std::string SnmpDeviceGateway::SilenceError() const
    {
        if (!m_openError.empty())
            return m_openError;

        if (SendBlocked())
            return "This computer refused to send to " + m_host + " (UDP " + std::to_string(snmp::kPort)
                + "): it is blocked on this machine.";

        return "No SNMP answer from " + m_host + " (UDP " + std::to_string(snmp::kPort) + ").";
    }

    bool SnmpDeviceGateway::Get(const snmp::Oid& oid, std::vector<unsigned char>& value)
    {
        value.clear();

        if (!m_channel)
            return false;

        const int32_t requestId = m_nextRequestId++;
        const std::vector<unsigned char> request = snmp::EncodeGetRequest(kCommunity, requestId, oid);
        if (request.empty())
            return false;

        Trace("[OUT] GET " + snmp::FormatOid(oid) + "\n");

        for (int attempt = 1; attempt <= kAttempts; ++attempt)
        {
            if (attempt > 1)
                Trace("[RETRY] No answer - sending the request again (attempt " + std::to_string(attempt)
                      + " of " + std::to_string(kAttempts) + ").\n");

            if (!m_channel->Send(request))
            {
                m_sendFailed = true;
                Trace("[!] SEND FAILED: this machine refused to send the datagram.\n");
                continue;
            }

            m_sendSucceeded = true;

            // Anything that is not the answer to this request - a late reply
            // to an earlier one, a malformed datagram - is skipped without
            // restarting the wait.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAttemptTimeoutMs);
            for (;;)
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0)
                    break;

                const std::vector<unsigned char> datagram = m_channel->Receive(static_cast<int>(left));
                if (datagram.empty())
                    break;

                snmp::GetResponse response;
                if (!snmp::DecodeGetResponse(datagram, response) || response.requestId != requestId)
                {
                    Trace("[i] Skipped a datagram that does not answer this request.\n");
                    continue;
                }

                m_answered = true;

                if (response.errorStatus != 0)
                {
                    Trace("[IN]  SNMP error-status " + std::to_string(response.errorStatus)
                          + (response.errorStatus == 2 ? " (noSuchName: the printer does not serve this OID)" : "")
                          + "\n\n");
                    return true;
                }

                if (response.valueTag != snmp::kTagOctetString)
                {
                    Trace("[IN]  A value that is not a string (BER tag "
                          + std::to_string(response.valueTag) + ") - ignored.\n\n");
                    return true;
                }

                value = response.value;
                Trace("[IN]  " + std::to_string(value.size()) + " bytes:\n"
                      + HexDumpCapped(value.data(), value.size(), kTraceDumpCapBytes) + "\n");
                return true;
            }
        }

        Trace("[!] No answer.\n\n");
        return false;
    }

    DeviceIdQueryResult SnmpDeviceGateway::QueryDeviceId()
    {
        DeviceIdQueryResult out;
        if (!ClaimPrinter())
            return out;

        Trace("---- IEEE 1284 device ID ----\n");

        std::vector<unsigned char> value;
        if (!Get(SnmpDeviceIdOid(), value) || value.empty())
            return out;

        out.deviceId = ExtractDeviceIdString(value.data(), value.size());
        out.found = !out.deviceId.empty();
        return out;
    }

    QueryRunResult SnmpDeviceGateway::RunQuery(
        const std::vector<std::vector<unsigned char>>& /*handshake*/,
        const std::vector<std::vector<unsigned char>>& queries,
        const ExecutorOptions& options)
    {
        QueryRunResult run;
        if (!ClaimPrinter())
        {
            run.query.error = kAnotherRunError;
            return run;
        }

        Trace("==================================================\n"
              "BEGIN QUERY SESSION (read-only)\n"
              + DescribeTraceContext(options.trace)
              + "Queries:       " + std::to_string(queries.size()) + "\n"
              "==================================================\n\n");

        // One-for-one with the queries, empty where nothing came back.
        run.query.replies.assign(queries.size(), {});

        for (std::size_t i = 0; i < queries.size(); ++i)
        {
            std::vector<unsigned char> command;
            if (!ExtractDataPayload(queries[i], command))
                continue;

            // The query path stays read-only, whatever it is handed.
            if (IsWritePacket(queries[i]))
            {
                Trace("[!] Query " + std::to_string(i + 1) + " is an EEPROM write - not sent.\n");
                continue;
            }

            std::vector<unsigned char> value;
            bool heard = false;

            if (IsStatusCommand(command))
            {
                heard = Get(SnmpStatusOid(), value);
                // Some firmware serves the status only as a control command.
                if (heard && value.empty())
                    heard = Get(SnmpControlOid(command), value);
            }
            else
            {
                heard = Get(SnmpControlOid(command), value);
            }

            run.query.packetsSent++;

            if (!value.empty())
                run.query.replies[i] = FrameAsD4Data(value);

            // Nothing has ever answered at this address: the remaining
            // queries would only wait out the same timeouts.
            if (!heard && !m_answered)
                break;
        }

        // There is no channel to open on this transport; a printer that
        // answered SNMP at all stands in for the handshake.
        run.deviceFound = m_answered;
        run.candidatesTried = m_answered ? 1 : 0;
        run.query.handshakeConfirmed = m_answered;
        run.query.handshakeFailed = !m_answered;
        run.query.success = m_answered;
        if (!m_answered)
            run.query.error = SilenceError();

        Trace("==================================================\n"
              "QUERY SESSION COMPLETE\n"
              "Requests sent:      " + std::to_string(run.query.packetsSent) + "\n"
              "Result:             " + (run.query.success ? std::string("SUCCESS") : ("FAILED - " + run.query.error)) + "\n"
              "==================================================\n\n");

        return run;
    }

    ResetRunResult SnmpDeviceGateway::RunReset(
        const std::vector<std::vector<unsigned char>>& sequence,
        const ExecutorOptions& options)
    {
        ResetRunResult run;
        if (!ClaimPrinter())
        {
            run.exec.error = kAnotherRunError;
            return run;
        }

        ExecutionResult& result = run.exec;
        log::Reporter& reporter = log::Default();

        // The handshake and credit packets of the sequence belong to a D4
        // channel that does not exist here; the writes are all that travels.
        std::vector<CtrlCommand> writes;
        for (CtrlCommand& command : ExtractCtrlCommands(sequence))
        {
            if (command.isWrite)
                writes.push_back(std::move(command));
        }

        Trace("==================================================\n"
              "BEGIN WRITE SESSION\n"
              + DescribeTraceContext(options.trace)
              + "Writes:        " + std::to_string(writes.size()) + "\n"
              "==================================================\n\n");

        auto finish = [&]() -> ResetRunResult&
        {
            run.deviceFound = m_answered;
            run.candidatesTried = m_answered ? 1 : 0;
            result.handshakeConfirmed = m_answered;
            result.handshakeFailed = !m_answered;

            Trace("==================================================\n"
                  "WRITE SESSION COMPLETE\n"
                  "Writes verified:    " + std::to_string(result.writesVerified) + " of "
                  + std::to_string(result.writesTotal) + "\n"
                  "Result:             " + (result.success ? std::string("SUCCESS") : ("FAILED - " + result.error)) + "\n"
                  "==================================================\n\n");

            // The console hides the per-write verdicts and leaves the reason
            // to this, as usb.reset_not_confirmed does on USB. A printer that
            // never answered is session.device_not_found's to report.
            if (!result.success && m_answered)
            {
                reporter.Log(log::Level::Error, log::Stage::Write, "snmp.reset_not_confirmed",
                             "\n[ERROR] " + result.error + "\n[!] The waste counter was NOT confirmed as reset.\n"
                             "    Check ewr_trace.log for the full network trace.");
            }
            return run;
        };

        // A replay dump is opaque bytes with nothing to confirm them by, and
        // this transport sends no write it cannot confirm.
        if (!options.verifyWrites)
        {
            result.error = "Replay dumps cannot be sent over the network: only database models, whose"
                           " writes are confirmed one by one.";
            return finish();
        }

        // The loop's retries and verdicts, into this run's trace as on USB.
        struct SinkGuard
        {
            log::Reporter& reporter;
            int id;
            ~SinkGuard() { reporter.RemoveSink(id); }
        };
        const SinkGuard traceSink{ reporter, m_trace ? reporter.AddSink(log::OStreamSink(*m_trace, log::Level::Trace)) : 0 };

        // A GET whose OID carries the command. Silence before anything ever
        // answered means nothing at this address will.
        const CtrlExchange exchange = [this](const std::vector<unsigned char>& command,
                                             std::vector<unsigned char>& reply, std::string& error)
        {
            if (Get(SnmpControlOid(command), reply) || m_answered)
                return true;

            error = SilenceError();
            return false;
        };

        RunCtrlCommands(exchange, writes, reporter, options, result);
        return finish();
    }

} // namespace ewr
