#include "ewr/usb.h"
#include "ewr/usb_backend.h"
#include "ewr/executor.h"
#include "ewr/log.h"
#include "ewr/version.h"

#include <algorithm>
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
// usb_libusb.cpp, merged by usb_composite.cpp on Windows) only enumerate
// interfaces and move raw bytes; see ewr/usb_backend.h for the seam.

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

        bool HasPositiveMaintenanceCandidate(const std::vector<UsbCandidate>& candidates)
        {
            return std::any_of(candidates.begin(), candidates.end(),
                [](const UsbCandidate& cand)
                {
                    return cand.role == UsbCandidateRole::Printer
                        || cand.role == UsbCandidateRole::Maintenance;
                });
        }

        bool IsAutomaticMaintenanceCandidate(const UsbCandidate& cand,
                                               const std::vector<UsbCandidate>& candidates)
        {
            return IsAutomaticMaintenanceRole(cand.role, HasPositiveMaintenanceCandidate(candidates));
        }

        std::size_t CountAutomaticMaintenanceCandidates(const std::vector<UsbCandidate>& candidates)
        {
            const bool hasPositiveRole = HasPositiveMaintenanceCandidate(candidates);
            std::size_t count = 0;
            for (const auto& cand : candidates)
                if (IsAutomaticMaintenanceRole(cand.role, hasPositiveRole))
                    ++count;
            return count;
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
            trace << "  Candidate:     " << (idx + 1) << " of " << total << "\n";
            trace << "  Interface:     " << backend.Describe(cand.ordinal) << "\n";
            trace << "  Product ID:    0x" << cand.pid << "\n";
            trace << "  OS role:       " << UsbCandidateRoleName(cand.role) << "\n";
            if (!cand.serviceName.empty()) trace << "  PnP service:   " << cand.serviceName << "\n";
            if (!cand.pnpClass.empty()) trace << "  PnP class:     " << cand.pnpClass << "\n";
            if (!cand.instanceId.empty()) trace << "  Instance ID:   " << cand.instanceId << "\n";
            trace << "\n";
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
                info.role = UsbCandidateRoleName(cand.role);

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

        struct FallbackAttempt
        {
            bool attempted = false;
            bool success = false;
            size_t writesTotal = 0;
            size_t writesVerified = 0;
            std::string error;
        };

        // The non-D4 write paths, in the order they are tried on an interface
        // whose D4 handshake went nowhere.
        enum class NonD4Path
        {
            // Epson's own direct-control framing, after a packet-mode flush
            // sized from the device ID's DDS field.
            End4,
            // ESC/P Remote carrying the same '||' command. Stateless where the
            // other two are not: ESC @ before every write, one self-contained
            // block, nothing carried between attempts. It is the only path that
            // does not assume EWR owns the pipe - on Windows it never does,
            // since usbprint.sys cannot be detached and the spooler shares it.
            EscRemote,
        };

        struct PathLabels
        {
            const char* tag;          // trace prefix
            const char* attemptCode;  // event code for the attempt line
            const char* announce;     // user-facing line
        };

        PathLabels LabelsFor(NonD4Path path)
        {
            if (path == NonD4Path::End4)
            {
                return { "[END4]", "usb.end4_attempt",
                         "[!] D4 handshake silent - attempting the END4 direct-control fallback (no driver, no D4 framing)..." };
            }

            return { "[ESC/P]", "usb.esc_remote_attempt",
                     "[!] END4 silent as well - attempting the ESC/P Remote fallback (no handshake, no session)..." };
        }

        // Last resort on an interface whose D4 handshake stayed silent: some
        // composite ET-2xxx units answer a non-D4 direct-control path over the
        // same handle even when they never open a D4 channel.
        FallbackAttempt TryNonD4Fallback(UsbBackend& backend,
                                         const UsbCandidate& cand,
                                         const std::vector<std::vector<unsigned char>>& sequence,
                                         const ExecutorOptions& options,
                                         std::ostream& trace,
                                         NonD4Path path)
        {
            FallbackAttempt outcome;
            const PathLabels labels = LabelsFor(path);

            const std::vector<std::vector<unsigned char>> factoryCommands =
                ExtractFactoryWriteCommands(sequence);
            if (factoryCommands.empty())
            {
                trace << labels.tag << " No factory EEPROM writes in this sequence; the fallback is not applicable.\n\n";
                return outcome; // attempted == false -> normal interface fallback continues
            }

            outcome.attempted = true;

            // The DDS flush length lives in the device ID, and that IOCTL needs
            // its own handle - so ask before opening this interface. ESC/P
            // Remote sends no flush and has no use for it.
            const std::string deviceId = (path == NonD4Path::End4)
                ? backend.QueryDeviceId(cand.ordinal)
                : std::string();

            trace << labels.tag << " D4 handshake stayed silent on " << backend.Describe(cand.ordinal)
                  << "; attempting a non-D4 direct-control fallback on this interface.\n";
            log::Log(log::Level::Info, log::Stage::Handshake, labels.attemptCode, labels.announce);

            ITransport* transport = backend.Open(cand.ordinal, options.usbSoftResetOnOpen);
            if (!transport)
            {
                outcome.error = "Could not reopen the interface for the fallback.";
                trace << labels.tag << " " << outcome.error << "\n\n";
                return outcome;
            }

            ScopedBackendClose closeGuard(backend);

            log::Reporter& reporter = log::Default();
            End4Result e4;
            {
                ScopedTraceSink traceSink(reporter, trace);
                e4 = (path == NonD4Path::End4)
                    ? ExecuteEnd4Sequence(*transport, deviceId, factoryCommands, reporter, options)
                    : ExecuteEscRemoteSequence(*transport, factoryCommands, reporter, options);
            }
            closeGuard.Close();

            outcome.success = e4.success;
            outcome.writesTotal = e4.writesTotal;
            outcome.writesVerified = e4.writesVerified;
            outcome.error = e4.error;

            trace << labels.tag << " Fallback result: "
                  << (e4.success
                          ? ("SUCCESS - " + std::to_string(e4.writesVerified) + "/" + std::to_string(e4.writesTotal) + " writes verified")
                          : ("FAILED - " + e4.error))
                  << "\n\n";

            return outcome;
        }

        struct QueryFallbackAttempt
        {
            bool attempted = false;
            QuerySessionResult result;
        };

        // Read-side counterpart to TryNonD4Fallback. Keep the ladder and all
        // open / trace-sink / close ownership here so adding an END4 query
        // executor later is one branch, not a second copy of this scaffolding.
        QueryFallbackAttempt TryNonD4QueryFallback(UsbBackend& backend,
                                                    const UsbCandidate& cand,
                                                    const std::vector<std::vector<unsigned char>>& queries,
                                                    const ExecutorOptions& options,
                                                    std::ostream& trace,
                                                    NonD4Path path)
        {
            QueryFallbackAttempt outcome;
            outcome.result.handshakeFailed = true;

            const std::vector<std::vector<unsigned char>> factoryCommands =
                ExtractFactoryQueryCommands(queries);
            if (factoryCommands.empty())
            {
                outcome.result.error = "No factory-control queries are available for the non-D4 read fallback.";
                return outcome;
            }

            // END4 write fallback already exists, but there is no read/query
            // executor for that framing yet. Deliberately keep it as a rung in
            // this shared ladder: when END4 reads are implemented they belong
            // in this function and inherit the same lifetime/trace handling.
            if (path == NonD4Path::End4)
            {
                trace << "[END4 QUERY] Read-only END4 fallback is not implemented yet; "
                         "continuing to the next non-D4 path.\n";
                return outcome;
            }

            outcome.attempted = true;

            trace << "[ESC/P QUERY] Reopening the same printer interface for a read-only direct-control fallback.\n";
            log::Log(log::Level::Info, log::Stage::Handshake, "usb.esc_remote_query_attempt",
                     "[!] D4 handshake silent - trying read-only ESC/P Remote on the same printer interface...");

            ITransport* transport = backend.Open(cand.ordinal, options.usbSoftResetOnOpen);
            if (!transport)
            {
                outcome.result.error = "Could not reopen the printer interface for the non-D4 query fallback.";
                return outcome;
            }

            ScopedBackendClose closeGuard(backend);
            log::Reporter& reporter = log::Default();
            {
                ScopedTraceSink traceSink(reporter, trace);

                switch (path)
                {
                    case NonD4Path::EscRemote:
                        outcome.result = ExecuteEscRemoteQuerySequence(
                            *transport, factoryCommands, reporter, options);
                        break;

                    case NonD4Path::End4:
                        // Kept unreachable until an END4 query executor exists;
                        // the early return above prevents opening a handle for a
                        // protocol we cannot execute yet.
                        break;
                }
            }
            closeGuard.Close();

            trace << "[ESC/P QUERY] Result: "
                  << (outcome.result.handshakeFailed ? "NO RESPONSE"
                                                     : (outcome.result.success ? "SUCCESS" : "PARTIAL"))
                  << (outcome.result.error.empty() ? "" : (" - " + outcome.result.error))
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
        bool fallbackAttempted = false;

        const auto firstEligible = std::find_if(candidates.begin(), candidates.end(),
            [&candidates](const UsbCandidate& cand) { return IsAutomaticMaintenanceCandidate(cand, candidates); });
        if (firstEligible != candidates.end())
        {
            log::Log(log::Level::Info, log::Stage::Detect, "usb.device_detected",
                     "[SUCCESS] Auto-detected Epson Printer (PID: " + firstEligible->pid + ")");
        }
        else
        {
            run.exec.error = "Only Epson scanner interfaces were found; no printer maintenance interface is available.";
            log::Log(log::Level::Error, log::Stage::Detect, "usb.only_scanner",
                     "[ERROR] Only Epson scanner interfaces were found. Reinstall the official Epson printer driver and reconnect USB.");
            return run;
        }
        log::Log(log::Level::Info, log::Stage::General, "usb.sequence_begin",
                 std::string("\nExecuting universal ") + backend->PlatformName() + " hardware state machine...");
        log::Log(log::Level::Info, log::Stage::General, "usb.trace_log",
                 "[i] Saving hardware trace to ewr_trace.log for diagnostics.");

        for (std::size_t idx = 0; idx < candidates.size(); ++idx)
        {
            const UsbCandidate& cand = candidates[idx];

            if (pinned >= 1 && static_cast<int>(idx) + 1 != pinned)
                continue;

            if (cand.role == UsbCandidateRole::Scanner && pinned >= 1)
            {
                run.exec.error = "The pinned USB interface is positively identified as a scanner; refusing EEPROM writes.";
                trace << "[SAFETY] Refusing maintenance writes to pinned scanner interface #" << pinned
                      << ". Use --list and select the printer/unknown function instead.\n";
                log::Log(log::Level::Error, log::Stage::Detect, "usb.scanner_write_blocked",
                         "[ERROR] Interface " + std::to_string(pinned) +
                             " is the Epson scanner function. EWR will not send EEPROM writes to a scanner interface.");
                break;
            }

            if (pinned < 1 && !IsAutomaticMaintenanceCandidate(cand, candidates))
            {
                trace << "[SAFETY] Skipping automatic maintenance candidate "
                      << backend->Describe(cand.ordinal) << ": role="
                      << UsbCandidateRoleName(cand.role)
                      << (HasPositiveMaintenanceCandidate(candidates)
                              ? " (a positively identified maintenance function is available).\n"
                              : " (scanner functions are never auto-maintained).\n");
                continue;
            }

            WriteSelectionDecision(trace, *backend, cand, idx, candidates.size());

            // A silent handshake is never repaired on a live handle: the retry
            // is a brand-new session that inherits nothing from the failed one.
            ExecutionResult result;
            bool opened = false;
            const int attempts = backend->AttemptsPerCandidate(cand.ordinal);
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                trace << "[Session] Candidate " << (idx + 1) << " of " << candidates.size()
                      << ", attempt " << (attempt + 1) << " of " << attempts
                      << ": starting a fresh session.\n";

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
            if (result.handshakeFailed && !fallbackAttempted)
            {
                fallbackAttempted = true;

                // END4 first: it is Epson's own framing and the one the captures
                // show. ESC/P Remote follows because it assumes least about the
                // transport, so it is what is left standing when END4 draws
                // nothing back.
                bool counterReset = false;
                for (const NonD4Path path : { NonD4Path::End4, NonD4Path::EscRemote })
                {
                    const FallbackAttempt attempt =
                        TryNonD4Fallback(*backend, cand, sequence, options, trace, path);
                    if (!attempt.attempted)
                        break; // no factory writes at all - neither path applies

                    if (attempt.success)
                    {
                        run.exec.success = true;
                        run.exec.handshakeFailed = false;
                        run.exec.writesTotal = attempt.writesTotal;
                        run.exec.writesVerified = attempt.writesVerified;
                        run.exec.error.clear();
                        log::Log(log::Level::Info, log::Stage::Write, "usb.fallback_success",
                                 "[SUCCESS] Direct-control fallback reset the counter ("
                                     + std::to_string(attempt.writesVerified) + "/"
                                     + std::to_string(attempt.writesTotal) + " writes verified).");
                        counterReset = true;
                        break;
                    }

                    // Keep the silent-handshake status so the interface fallback
                    // below still runs, but surface this path's real reason.
                    run.exec.error = attempt.error;
                }

                if (counterReset)
                    break;
            }

            if (result.handshakeFailed && pinned < 1 && idx + 1 < candidates.size())
            {
                log::Log(log::Level::Info, log::Stage::Handshake, "usb.interface_fallback",
                         "[!] Interface " + std::to_string(idx + 1) + "/" + std::to_string(candidates.size())
                             + " stayed silent to D4, END4 and ESC/P Remote. Trying the next USB interface...");
                trace << "[!] Handshake silent (D4, END4 and ESC/P Remote) on this interface."
                         " Trying the next USB interface candidate.\n\n";
                continue;
            }

            if (result.handshakeFailed && pinned >= 1)
                trace << "[!] Handshake silent on the pinned interface. Interface fallback is disabled by the pin (END4 was still attempted here).\n\n";

            break;
        }

        if (run.candidatesTried == 0)
        {
            if (!run.exec.error.empty())
                return run;

            if (CountAutomaticMaintenanceCandidates(candidates) == 0)
            {
                run.exec.error = "Only scanner interfaces were found; no safe printer maintenance interface is available.";
                return run;
            }

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

            // Automatic read preflight must stay on the exact same role set
            // that can later receive writes. A user may still pin a non-printer
            // interface explicitly for read-only diagnostics.
            if (pinned < 1 && !IsAutomaticMaintenanceCandidate(cand, candidates))
            {
                trace << "[SAFETY] Skipping non-maintenance interface during automatic query: "
                      << backend->Describe(cand.ordinal) << "\n";
                continue;
            }

            WriteSelectionDecision(trace, *backend, cand, idx, candidates.size());

            QuerySessionResult result;
            bool opened = false;
            const int attempts = backend->AttemptsPerCandidate(cand.ordinal);
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                trace << "[Session] Candidate " << (idx + 1) << " of " << candidates.size()
                      << ", attempt " << (attempt + 1) << " of " << attempts
                      << ": starting a fresh session.\n";

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

            // L3210 / ET-28xx style MFPs can expose a perfectly valid printer
            // function that stays silent to D4 while still accepting a non-D4
            // direct-control protocol. Keep reads on the same ordered ladder as
            // writes, and never let automatic preflight/read-back move onto the
            // WIA scanner function.
            if (result.handshakeFailed)
            {
                for (const NonD4Path path : { NonD4Path::End4, NonD4Path::EscRemote })
                {
                    QueryFallbackAttempt direct =
                        TryNonD4QueryFallback(*backend, cand, queries, options, trace, path);
                    if (!direct.attempted)
                        continue;

                    if (!direct.result.handshakeFailed)
                    {
                        result = std::move(direct.result);
                        run.query = result;
                        break;
                    }

                    if (!direct.result.error.empty())
                        run.query.error = direct.result.error;
                }
            }

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
        {
            if (pinned < 1 && CountAutomaticMaintenanceCandidates(candidates) == 0)
                run.query.error = "Only scanner interfaces were found; no printer maintenance interface is available.";
            else
                run.query.error = backend->DescribeOpenFailure(false);
        }

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
