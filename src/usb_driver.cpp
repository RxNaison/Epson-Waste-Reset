#include "ewr/usb.h"
#include "ewr/usb_backend.h"
#include "ewr/executor.h"
#include "ewr/log.h"
#include "ewr/version.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

// Platform-neutral driver loop for every USB entry point in usb.h.
//
// Candidate iteration, interface pinning, the fresh-session retry, the
// automatic fallback and the trace-log choreography all live here, once.
// Every attempt is a full session - open, handshake, close - never an
// in-place repair: resetting a live session desyncs the printer from the
// host-side channel state. The platform backends (usb_windows.cpp /
// usb_linux.cpp) only enumerate interfaces and move raw bytes; see
// ewr/usb_backend.h for the seam.

namespace ewr {

    namespace {

        // Prefixes every non-blank trace line with wall-clock milliseconds
        // since the run opened the log, so stalls, reply latencies and
        // timeout windows are readable straight from a field trace. Blank
        // lines stay unstamped so banners keep their shape.
        class TimestampBuf final : public std::streambuf
        {
        public:
            explicit TimestampBuf(std::streambuf* inner)
                : inner_(inner), start_(std::chrono::steady_clock::now()) {}

        protected:
            int overflow(int ch) override
            {
                if (ch == traits_type::eof())
                    return traits_type::not_eof(ch);

                if (atLineStart_ && ch != '\n')
                {
                    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_).count();
                    char stamp[24];
                    const int len = snprintf(stamp, sizeof(stamp), "[+%6lld ms] ", ms);
                    if (len > 0)
                        inner_->sputn(stamp, len);
                }

                atLineStart_ = (ch == '\n');
                return inner_->sputc(traits_type::to_char_type(ch));
            }

            int sync() override { return inner_->pubsync(); }

        private:
            std::streambuf* inner_;
            std::chrono::steady_clock::time_point start_;
            bool atLineStart_ = true;
        };

        // The run's single trace stream: ewr_trace.log opened once per run,
        // every line wall-clock stamped via TimestampBuf. Declaration order
        // matters: file, then buf, then stream.
        struct TraceLog
        {
            explicit TraceLog(bool appendTraceLog)
                : file("ewr_trace.log",
                       appendTraceLog ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc)),
                  buf(file.rdbuf()),
                  stream(&buf)
            {
            }

            std::ofstream file;
            TimestampBuf buf;
            std::ostream stream;
        };

        // The trace sink points into a TraceLog that dies with this run, while
        // log::Default() is process-global and outlives it. Anything that
        // leaves the executor call without removing the sink - a throw out of
        // a hex dump, a future early return - leaves that global holding a
        // pointer to a destroyed stream.
        class ScopedTraceSink
        {
        public:
            ScopedTraceSink(log::Reporter& reporter, std::ostream& trace)
                : reporter_(reporter)
                , id_(reporter.AddSink(log::OStreamSink(trace, log::Level::Trace)))
            {
            }

            ~ScopedTraceSink() { reporter_.RemoveSink(id_); }

            ScopedTraceSink(const ScopedTraceSink&) = delete;
            ScopedTraceSink& operator=(const ScopedTraceSink&) = delete;

        private:
            log::Reporter& reporter_;
            int id_;
        };

        // Same shape for the open interface: an attempt that leaves early must
        // not keep the device claimed. Close() is idempotent, so the normal
        // path still closes exactly where it always did and the destructor
        // only covers the paths that never reach it.
        class ScopedBackendClose
        {
        public:
            explicit ScopedBackendClose(UsbBackend& backend) : backend_(backend) {}

            ~ScopedBackendClose() { Close(); }

            void Close()
            {
                if (closed_)
                    return;

                closed_ = true;
                backend_.Close();
            }

            ScopedBackendClose(const ScopedBackendClose&) = delete;
            ScopedBackendClose& operator=(const ScopedBackendClose&) = delete;

        private:
            UsbBackend& backend_;
            bool closed_ = false;
        };

        void WriteTraceBanner(std::ostream& trace, const std::string& title)
        {
            trace << "==================================================\n";
            trace << title << "\n";
            trace << "EWR Version: " << EWR_VERSION << "\n";
            trace << "==================================================\n\n";
        }

        // Enumeration plus pin validation, shared by the payload and query
        // runs. `error` carries the user-facing reason on false.
        bool PrepareCandidates(UsbBackend& backend,
                               std::ostream& trace,
                               const ExecutorOptions& options,
                               std::vector<UsbCandidate>& candidates,
                               bool& deviceFound,
                               std::string& error)
        {
            if (!backend.InitError().empty())
            {
                trace << "[!] " << backend.InitError() << "\n";
                error = backend.InitError();
                return false;
            }

            candidates = backend.Enumerate();

            if (candidates.empty())
            {
                trace << "[!] Auto-detection error: No Epson devices detected. Please verify USB connection, power state, and drivers.\n";
                error = "No Epson USB device detected.";
                return false;
            }

            deviceFound = true;

            const int pinned = options.interfaceCandidate;
            if (pinned >= 1 && pinned > static_cast<int>(candidates.size()))
            {
                trace << "[!] Interface pin " << pinned << " is out of range: only "
                      << candidates.size() << " Epson interface candidate(s) present.\n";
                log::Log(log::Level::Warning, log::Stage::Detect, "usb.interface_pin_invalid",
                         "[!] Interface " + std::to_string(pinned) + " does not exist: only "
                             + std::to_string(candidates.size()) + " Epson interface(s) were found. Run 'ewr --list' to see them.");
                error = "Requested interface candidate does not exist.";
                return false;
            }

            if (pinned >= 1)
                trace << "[i] Interface pin active: only candidate #" << pinned << " will be driven (no fallback).\n";

            return true;
        }

        void WriteSelectionDecision(std::ostream& trace, const UsbBackend& backend,
                                    const UsbCandidate& cand, std::size_t idx, std::size_t total)
        {
            trace << "\n[Selection Decision]\n";
            trace << "  Attempt:       " << (idx + 1) << " of " << total << "\n";
            trace << "  Interface:     " << backend.Describe(cand.ordinal) << "\n";
            trace << "  Product ID:    0x" << cand.pid << "\n\n";
        }

        // Shared by ListPrinterInterfaces and QueryPrinterDeviceId so both
        // run over one open trace stream.
        std::vector<InterfaceInfo> SurveyInterfaces(UsbBackend& backend, std::ostream& trace)
        {
            std::vector<InterfaceInfo> out;

            if (!backend.InitError().empty())
            {
                trace << "[!] Interface survey: " << backend.InitError() << "\n";
                return out;
            }

            const std::vector<UsbCandidate> candidates = backend.Enumerate();

            for (std::size_t idx = 0; idx < candidates.size(); ++idx)
            {
                const UsbCandidate& cand = candidates[idx];

                InterfaceInfo info;
                info.index = static_cast<int>(idx) + 1;
                info.className = cand.className;
                info.interfaceNumber = cand.interfaceNumber;
                info.path = cand.path;
                info.deviceId = backend.QueryDeviceId(cand.ordinal);

                if (!info.deviceId.empty())
                {
                    trace << "[i] IEEE 1284 device ID from " << backend.Describe(cand.ordinal) << ":\n";
                    trace << "    " << info.deviceId << "\n";
                }

                out.push_back(info);
            }

            if (out.empty())
                trace << "[i] Interface survey: no Epson (VID_04B8) interfaces present.\n";

            return out;
        }

        struct End4Attempt
        {
            bool attempted = false;
            bool success = false;
            size_t writesTotal = 0;
            size_t writesVerified = 0;
            std::string error;
        };

        // Last resort on an interface whose D4 handshake stayed silent: some
        // composite ET-2xxx units answer Epson's non-D4 END4 direct-control
        // path over the same handle even when they never open a D4 channel.
        End4Attempt TryEnd4Fallback(UsbBackend& backend,
                                    const UsbCandidate& cand,
                                    const std::vector<std::vector<unsigned char>>& sequence,
                                    const ExecutorOptions& options,
                                    std::ostream& trace)
        {
            End4Attempt outcome;

            const std::vector<std::vector<unsigned char>> factoryCommands =
                ExtractFactoryWriteCommands(sequence);
            if (factoryCommands.empty())
            {
                trace << "[END4] No factory EEPROM writes in this sequence; END4 fallback is not applicable.\n\n";
                return outcome; // attempted == false -> normal interface fallback continues
            }

            outcome.attempted = true;

            // The DDS flush length lives in the device ID, and that IOCTL needs
            // its own handle - so ask before opening this interface.
            const std::string deviceId = backend.QueryDeviceId(cand.ordinal);

            trace << "[END4] D4 handshake stayed silent on " << backend.Describe(cand.ordinal)
                  << "; attempting the non-D4 END4 direct-control fallback on this interface.\n";
            log::Log(log::Level::Info, log::Stage::Handshake, "usb.end4_attempt",
                     "[!] D4 handshake silent - attempting the END4 direct-control fallback (no driver, no D4 framing)...");

            ITransport* transport = backend.Open(cand.ordinal, options.usbSoftResetOnOpen);
            if (!transport)
            {
                outcome.error = "END4: could not reopen the interface for the fallback.";
                trace << "[END4] " << outcome.error << "\n\n";
                return outcome;
            }

            ScopedBackendClose closeGuard(backend);

            log::Reporter& reporter = log::Default();
            End4Result e4;
            {
                ScopedTraceSink traceSink(reporter, trace);
                e4 = ExecuteEnd4Sequence(*transport, deviceId, factoryCommands, reporter, options);
            }
            closeGuard.Close();

            outcome.success = e4.success;
            outcome.writesTotal = e4.writesTotal;
            outcome.writesVerified = e4.writesVerified;
            outcome.error = e4.error;

            trace << "[END4] Fallback result: "
                  << (e4.success
                          ? ("SUCCESS - " + std::to_string(e4.writesVerified) + "/" + std::to_string(e4.writesTotal) + " writes verified")
                          : ("FAILED - " + e4.error))
                  << "\n\n";

            return outcome;
        }

    } // namespace

    ResetRunResult ExecutePayloadSequenceWithFallback(const std::vector<std::vector<unsigned char>>& sequence,
                                                      const ExecutorOptions& options,
                                                      bool appendTraceLog)
    {
        ResetRunResult run;

        TraceLog traceLog(appendTraceLog);
        std::ostream& trace = traceLog.stream;
        std::unique_ptr<UsbBackend> backend = CreateUsbBackend(trace);

        WriteTraceBanner(trace, std::string("EWR HARDWARE TRACE LOG (") + backend->PlatformName() + ")");

        std::vector<UsbCandidate> candidates;
        if (!PrepareCandidates(*backend, trace, options, candidates, run.deviceFound, run.exec.error))
            return run;

        const int pinned = options.interfaceCandidate;

        // Once per run, on the first interface whose D4 handshake goes silent.
        bool end4Attempted = false;

        log::Log(log::Level::Info, log::Stage::Detect, "usb.device_detected",
                 "[SUCCESS] Auto-detected Epson Printer (PID: " + candidates[0].pid + ")");
        log::Log(log::Level::Info, log::Stage::General, "usb.sequence_begin",
                 std::string("\nExecuting universal ") + backend->PlatformName() + " hardware state machine...");
        log::Log(log::Level::Info, log::Stage::General, "usb.trace_log",
                 "[i] Saving hardware trace to ewr_trace.log for diagnostics.");

        for (std::size_t idx = 0; idx < candidates.size(); ++idx)
        {
            const UsbCandidate& cand = candidates[idx];

            if (pinned >= 1 && static_cast<int>(idx) + 1 != pinned)
                continue;

            WriteSelectionDecision(trace, *backend, cand, idx, candidates.size());

            // A silent handshake is never repaired on a live handle: the retry
            // is a brand-new session that inherits nothing from the failed one.
            ExecutionResult result;
            bool opened = false;
            const int attempts = backend->AttemptsPerCandidate();
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                trace << "[Session] Interface " << (idx + 1) << ", attempt " << (attempt + 1)
                      << " of " << attempts << ": starting a fresh session.\n";

                ITransport* transport = backend->Open(cand.ordinal, options.usbSoftResetOnOpen);
                if (!transport)
                    break;

                opened = true;

                trace << "\n==================================================\n";
                trace << "BEGIN PAYLOAD SEQUENCE EXECUTION\n";
                trace << "Interface:     " << backend->Describe(cand.ordinal) << "\n";
                trace << "Total Packets: " << sequence.size() << "\n";
                trace << "==================================================\n\n";

                ScopedBackendClose closeGuard(*backend);

                // Host sinks plus this run's trace file, at Trace level so the
                // file gets full hex detail alongside the user-facing lines.
                log::Reporter& reporter = log::Default();
                {
                    ScopedTraceSink traceSink(reporter, trace);
                    result = ExecuteSequence(*transport, sequence, reporter, options);
                }

                trace << "==================================================\n";
                trace << "SEQUENCE COMPLETE\n";
                trace << "Data packets sent:  " << result.packetsSent << " (sequence: " << sequence.size() << " packets incl. handshake/credit)\n";
                trace << "EEPROM writes:      " << result.writesVerified << " verified / " << result.writesTotal << " total\n";
                trace << "Result:             " << (result.success ? "SUCCESS" : ("FAILED - " + result.error)) << "\n";
                trace << "==================================================\n";

                closeGuard.Close();
                log::Log(log::Level::Info, log::Stage::General, "usb.lock_released", "Hardware lock released.");

                if (!result.handshakeFailed)
                    break;

                if (attempt + 1 < attempts)
                    trace << "[!] Handshake silent - retrying this interface with a brand-new session (fresh open + session-start reset).\n";
            }

            if (!opened)
                continue;

            run.candidatesTried++;
            run.exec = result;

            // D4 stayed silent: try END4 on this same interface before moving
            // to the next candidate. Once per run, on the first silent one.
            if (result.handshakeFailed && !end4Attempted)
            {
                end4Attempted = true;
                const End4Attempt e4 = TryEnd4Fallback(*backend, cand, sequence, options, trace);
                if (e4.attempted)
                {
                    if (e4.success)
                    {
                        run.exec.success = true;
                        run.exec.handshakeFailed = false;
                        run.exec.writesTotal = e4.writesTotal;
                        run.exec.writesVerified = e4.writesVerified;
                        run.exec.error.clear();
                        log::Log(log::Level::Info, log::Stage::Write, "usb.end4_success",
                                 "[SUCCESS] END4 direct-control fallback reset the counter ("
                                     + std::to_string(e4.writesVerified) + "/" + std::to_string(e4.writesTotal) + " writes verified).");
                        break;
                    }

                    // Keep the silent-handshake status so the interface fallback
                    // below still runs, but surface END4's real reason.
                    run.exec.error = e4.error;
                }
            }

            if (result.handshakeFailed && pinned < 1 && idx + 1 < candidates.size())
            {
                log::Log(log::Level::Info, log::Stage::Handshake, "usb.interface_fallback",
                         "[!] Interface " + std::to_string(idx + 1) + "/" + std::to_string(candidates.size())
                             + " stayed silent to both D4 and END4. Trying the next USB interface...");
                trace << "[!] Handshake silent (D4 and END4) on this interface. Trying the next USB interface candidate.\n\n";
                continue;
            }

            if (result.handshakeFailed && pinned >= 1)
                trace << "[!] Handshake silent on the pinned interface. Interface fallback is disabled by the pin (END4 was still attempted here).\n\n";

            break;
        }

        if (run.candidatesTried == 0)
        {
            run.exec.error = backend->DescribeOpenFailure(true);
            return run;
        }

        if (!run.exec.success)
        {
            log::Log(log::Level::Error, log::Stage::Write, "usb.reset_not_confirmed",
                     "\n[ERROR] " + run.exec.error + "\n[!] The waste counter was NOT confirmed as reset.\n    Check ewr_trace.log for the full hardware trace.");
        }

        return run;
    }

    QueryRunResult ExecuteQuerySessionWithFallback(const std::vector<std::vector<unsigned char>>& handshake,
                                                   const std::vector<std::vector<unsigned char>>& queries,
                                                   const ExecutorOptions& options,
                                                   bool appendTraceLog)
    {
        QueryRunResult run;

        TraceLog traceLog(appendTraceLog);
        std::ostream& trace = traceLog.stream;
        std::unique_ptr<UsbBackend> backend = CreateUsbBackend(trace);

        WriteTraceBanner(trace, std::string("EWR STATUS/READ QUERY SESSION (") + backend->PlatformName() + ")");

        std::vector<UsbCandidate> candidates;
        if (!PrepareCandidates(*backend, trace, options, candidates, run.deviceFound, run.query.error))
            return run;

        const int pinned = options.interfaceCandidate;

        for (std::size_t idx = 0; idx < candidates.size(); ++idx)
        {
            const UsbCandidate& cand = candidates[idx];

            if (pinned >= 1 && static_cast<int>(idx) + 1 != pinned)
                continue;

            WriteSelectionDecision(trace, *backend, cand, idx, candidates.size());

            QuerySessionResult result;
            bool opened = false;
            const int attempts = backend->AttemptsPerCandidate();
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                trace << "[Session] Interface " << (idx + 1) << ", attempt " << (attempt + 1)
                      << " of " << attempts << ": starting a fresh session.\n";

                ITransport* transport = backend->Open(cand.ordinal, options.usbSoftResetOnOpen);
                if (!transport)
                    break;

                opened = true;

                trace << "\n==================================================\n";
                trace << "BEGIN QUERY SESSION (read-only)\n";
                trace << "Interface:     " << backend->Describe(cand.ordinal) << "\n";
                trace << "Queries:       " << queries.size() << "\n";
                trace << "==================================================\n\n";

                ScopedBackendClose closeGuard(*backend);

                log::Reporter& reporter = log::Default();
                {
                    ScopedTraceSink traceSink(reporter, trace);
                    result = ExecuteQuerySession(*transport, handshake, queries, reporter, options);
                }

                trace << "==================================================\n";
                trace << "QUERY SESSION COMPLETE\n";
                trace << "Packets sent:       " << result.packetsSent << "\n";
                trace << "Result:             " << (result.success ? "SUCCESS" : ("FAILED - " + result.error)) << "\n";
                trace << "==================================================\n";

                closeGuard.Close();

                if (!result.handshakeFailed)
                    break;

                if (attempt + 1 < attempts)
                    trace << "[!] Handshake silent - retrying this interface with a brand-new session (fresh open + session-start reset).\n";
            }

            if (!opened)
                continue;

            run.candidatesTried++;
            run.query = result;

            if (result.handshakeFailed && pinned < 1 && idx + 1 < candidates.size())
            {
                trace << "[!] Handshake silent on this interface. Falling back to the next candidate.\n\n";
                continue;
            }

            if (result.handshakeFailed && pinned >= 1)
                trace << "[!] Handshake silent on the pinned interface. Fallback is disabled by the interface pin.\n\n";

            break;
        }

        if (run.candidatesTried == 0)
            run.query.error = backend->DescribeOpenFailure(false);

        return run;
    }

    std::vector<InterfaceInfo> ListPrinterInterfaces(bool appendTraceLog)
    {
        TraceLog traceLog(appendTraceLog);
        std::ostream& trace = traceLog.stream;
        std::unique_ptr<UsbBackend> backend = CreateUsbBackend(trace);

        WriteTraceBanner(trace, std::string("EPSON USB INTERFACE SURVEY (") + backend->PlatformName() + ")");

        return SurveyInterfaces(*backend, trace);
    }

    DeviceIdQueryResult QueryPrinterDeviceId(bool appendTraceLog)
    {
        DeviceIdQueryResult out;

        TraceLog traceLog(appendTraceLog);
        std::ostream& trace = traceLog.stream;
        std::unique_ptr<UsbBackend> backend = CreateUsbBackend(trace);

        WriteTraceBanner(trace, std::string("EPSON USB INTERFACE SURVEY (") + backend->PlatformName() + ")");

        for (const InterfaceInfo& info : SurveyInterfaces(*backend, trace))
        {
            if (info.deviceId.empty())
                continue;

            out.found = true;
            out.deviceId = info.deviceId;
            break;
        }

        if (!out.found)
            trace << "[i] No interface answered the IEEE 1284 device ID query.\n";

        return out;
    }

} // namespace ewr
