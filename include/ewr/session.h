#pragma once
#include "ewr/backup.h"
#include "ewr/gateway.h"
#include "ewr/database.h"
#include "ewr/log.h"
#include "ewr/restore.h"
#include "ewr/run_lock.h"
#include "ewr/status.h"
#include "ewr/usb.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ewr {

    // ------------------------------------------------------------------
    //  Blockers: conditions the host must decide about
    // ------------------------------------------------------------------

    // A foreign printer error (empty cartridge, paper jam, open cover, ...).
    // Firmware locked by one usually refuses factory writes with ':42:NA;'.
    struct Blocker
    {
        std::string errorName;   // ST2 error name, e.g. "INK OUT"
        int errorCode = -1;      // raw ST2 error code
        std::string explanation; // human-readable risk explanation
    };

    // True pushes past the blocker, false aborts before any EEPROM write.
    // The CLI wires this to a y/N prompt, a GUI to a dialog, an unattended
    // runner to a fixed answer.
    using DecisionCallback = std::function<bool(const Blocker&)>;

    // SERVICE REQUEST / CARTRIDGE OVERFLOW - the errors EWR exists to clear.
    // Printers in those states still accept writes, so they must never gate.
    bool IsExpectedWastePadError(int errorCode);

    // nullopt when it is safe to proceed. A BUSY state with no error entry
    // blocks too: busy firmware can suppress the very error that would have
    // gated the write.
    std::optional<Blocker> EvaluateBlocker(const PrinterStatus& status);

    // INK OUT joins the expected list - an empty cartridge is the state this
    // reset clears. Foreign locks still block.
    std::optional<Blocker> EvaluateInkBlocker(const PrinterStatus& status);

    // ------------------------------------------------------------------
    //  Snapshots and outcomes (structured data for any host to render)
    // ------------------------------------------------------------------

    // Parsed '@BDC ST2' status plus raw counter bytes (address -> value).
    struct StateSnapshot
    {
        bool available = false; // a device answered the query session
        PrinterStatus status;
        std::vector<std::pair<uint16_t, int>> values;
    };

    enum class ResetPhase
    {
        NotStarted,     // nothing was attempted yet
        Aborted,        // a blocker was declined - nothing was written
        DeviceNotFound, // no Epson interface answered at write time
        WriteFailed,    // the write session ran and did not succeed
        Done,           // the reset writes were acknowledged
        NotSupported,   // this way to the printer cannot reach its EEPROM - nothing was written
    };

    // A gate that objected and was told to go ahead: by --force-yes, a typed
    // yes or a host callback. A caller reading only the result has to be able
    // to see that the run wrote past an objection.
    struct GateOverride
    {
        std::string gate;   // "model_mismatch", "blocker" or "db_conflict"
        std::string detail; // the model the printer reported, or the blocker's name
        int errorCode = -1; // the printer's ST2 error code, for a printer error
    };

    struct ResetOutcome
    {
        ResetPhase phase = ResetPhase::NotStarted;

        bool success = false;
        // What the write session acknowledged. Both stay 0 when it never ran.
        size_t writesVerified = 0;
        size_t writesTotal = 0;
        // The database's primary keyword was rejected and 'wkey1' worked -
        // a reportable database fix.
        bool alternateKeyUsed = false;
        // Schema-4 close step: true when it confirmed or none was needed.
        bool committed = true;

        // Read-back verification (runs only when the preflight read worked).
        bool verificationRan = false;
        size_t verifyMismatches = 0; // counters differing from their reset value
        size_t verifyUnread = 0;     // counters that did not answer the read-back

        StateSnapshot before; // preflight snapshot
        StateSnapshot after;  // read-back snapshot

        // In the order they were passed. The model check lives in the host,
        // so the host puts its own entry first.
        std::vector<GateOverride> overrides;

        // The preflight value of every byte the writes overwrite, built once
        // the run is decided. False when there was no preflight to take it
        // from; `backupLocation` is where the host kept it, empty if nowhere.
        bool backupTaken = false;
        Backup backup;
        std::string backupLocation;

        // A restore leaves out the bytes that already held the file's value.
        std::vector<uint16_t> unchanged;

        std::string error; // failure detail when the run did not complete
    };

    // ------------------------------------------------------------------
    //  Device gateway: the only thing between a Session and the hardware
    // ------------------------------------------------------------------

    // A maintenance command - a head cleaning - sent once: no retry, no
    // second interface, whatever came back. A command that drew no reply may
    // still have run; only the printer's status afterwards can say.
    struct CommandRunResult
    {
        bool supported = false;   // this gateway can send one at all
        bool deviceFound = false;
        bool sent = false;        // it left this machine
        std::vector<unsigned char> reply; // as a query reply; empty when none came
        std::string error;
    };

    // Print data (ESC/P2), the way a nozzle check is printed. Nothing comes
    // back from a print channel.
    struct PrintDataResult
    {
        bool supported = false;
        bool deviceFound = false;
        bool sent = false;        // every byte was accepted
        std::string error;
    };

    // The device I/O a session needs, behind an interface so the lifecycle
    // is unit-testable with a scripted fake and USB-driven in production.
    struct IDeviceGateway
    {
        virtual ~IDeviceGateway() = default;

        // For the lines a session prints about how it reaches the printer.
        virtual bool OverNetwork() const { return false; }

        // Why no EEPROM write sent this way could land, once the gateway has
        // seen enough to say so; empty while it can, or cannot tell. A reset
        // stops on it before the first write, since none could be verified.
        virtual std::string EepromUnreachable() const { return {}; }

        virtual QueryRunResult RunQuery(
            const std::vector<std::vector<unsigned char>>& handshake,
            const std::vector<std::vector<unsigned char>>& queries,
            const ExecutorOptions& options) = 0;

        virtual ResetRunResult RunReset(
            const std::vector<std::vector<unsigned char>>& sequence,
            const ExecutorOptions& options) = 0;

        // Not every way to a printer can send these; the defaults say so.
        virtual CommandRunResult RunCommand(const std::vector<unsigned char>& command,
                                            const ExecutorOptions& options)
        {
            (void)command;
            (void)options;
            return {};
        }

        virtual PrintDataResult SendPrintData(const std::vector<unsigned char>& data,
                                              const ExecutorOptions& options)
        {
            (void)data;
            (void)options;
            return {};
        }
    };

    // The warnings for a USB run that ended silent: jobs waiting in a print
    // queue on the printer's port, a Ready Print queue driver. False when
    // neither applies.
    bool ReportSilentPortQueues(const SilentPortQueues& note);

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
        std::vector<InterfaceInfo> ListInterfaces();

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

    // Read-only session defaults: the IEEE 1284.4 session core is the
    // default transport.
    inline ExecutorOptions DefaultQueryOptions()
    {
        ExecutorOptions options;
        options.useSessionLayer = true;
        return options;
    }

    // Model-free status read: just the '@BDC ST2' report, no counter reads
    // (those need a model's read key). What the replay path can know about
    // a printer.
    StateSnapshot ReadPrinterStatus(IDeviceGateway& gateway,
                                    const ExecutorOptions& options = DefaultQueryOptions());

    // ------------------------------------------------------------------
    //  Read-key search: for a printer the database has no entry for
    // ------------------------------------------------------------------

    // One read key to try, with the address width its models use. The same
    // key at the other width is a different candidate: the width changes the
    // packet, so a printer can refuse one and answer the other.
    struct ReadKeyCandidate
    {
        uint16_t rkey = 0;
        uint8_t addressLength = 2;
        std::vector<std::string> models; // database entries that use it
    };

    struct ReadKeyProbe
    {
        ReadKeyCandidate candidate;
        ReadAnswer answer = ReadAnswer::Silent;
        uint8_t value = 0; // the byte at address 0, when answer is Value
    };

    struct ReadKeySearch
    {
        bool deviceFound = false;
        // False when the search stopped before every candidate was tried;
        // `error` says why. `probes` holds the ones that were.
        bool completed = false;
        std::vector<ReadKeyProbe> probes;
        std::string error;

        size_t Count(ReadAnswer answer) const;
    };

    // Every distinct (read key, address width) in `models`, the most widely
    // shared first: a key many entries use is the likeliest to fit a sibling.
    std::vector<ReadKeyCandidate> CollectReadKeyCandidates(const std::vector<DbPrinterModel>& models);

    // Reads address 0 once with each candidate's key. Read commands only: a
    // key gates access, it cannot turn a read into a write. `progress` is
    // called with (tried, total) as the search advances.
    ReadKeySearch SearchReadKey(IDeviceGateway& gateway,
                                const std::vector<ReadKeyCandidate>& candidates,
                                const ExecutorOptions& options = DefaultQueryOptions(),
                                const std::function<void(size_t, size_t)>& progress = {});

    // ------------------------------------------------------------------
    //  Session: the whole reset lifecycle behind one API
    // ------------------------------------------------------------------

    // Host hooks for the moments a reset needs its host. All optional; an
    // empty struct is a fully unattended reset that aborts on any blocker.
    struct ResetHandlers
    {
        std::function<void(const StateSnapshot&)> onPreflight;
        DecisionCallback onBlocker;
        // The unconditional ask-once gate, after every other gate and right
        // before the first EEPROM write. False aborts with nothing written;
        // unset means proceed, so unattended hosts keep their behavior.
        std::function<bool(const StateSnapshot&)> confirmWrite;
        // After confirmWrite and before the first write, with every byte the
        // writes overwrite as the preflight read it. False means the backup
        // could not be kept, which gates the run like a printer error does;
        // `location` is where it went. Unset: the backup is still built and
        // returned in the outcome, but nothing is gated on keeping it.
        std::function<bool(const Backup&, std::string& location)> saveBackup;
        std::function<void(const StateSnapshot&)> onVerify;
    };

    // The reset lifecycle over one database model: preflight read, blocker
    // policy, EEPROM writes, schema-4 commit, read-back verification.
    // Progress flows through log.h events, decisions through ResetHandlers.
    class Session
    {
    public:
        // `model` and `gateway` must outlive the session.
        Session(const DbPrinterModel& model,
                IDeviceGateway& gateway,
                log::Reporter& reporter = log::Default(),
                ExecutorOptions queryOptions = DefaultQueryOptions());

        StateSnapshot ReadState() const;

        // Reads an explicit address list with the model's read key. Backs
        // the --dump diagnostic. A long list is read with range reads (0x51)
        // once the printer has shown, on a probe of the first bytes, that it
        // answers them exactly as it answers single reads; any other answer
        // means single reads for the rest of this session. The reset itself
        // never reads this way: its lifecycle keeps the single reads every
        // verified reset was done with.
        StateSnapshot ReadAddresses(const std::vector<uint16_t>& addresses) const;

        // Whether range reads were found to work. Unknown until a long enough
        // ReadAddresses call has probed.
        enum class RangeReads { Unknown, Work, Refused };
        RangeReads RangeReadSupport() const { return m_rangeReads; }

        ResetOutcome Reset(const ResetHandlers& handlers = {});

        // Same lifecycle over ink_groups, with two differences: INK OUT does
        // not gate it, and there is no commit step. On chipped cartridges the
        // chip is authoritative, so the reset will not hold.
        ResetOutcome ResetInk(const ResetHandlers& handlers = {});

        // Writes back what PlanRestore kept of a backup or a dump, through the
        // same lifecycle: preflight (required), gates, backup of what it
        // overwrites, writes, commit, read-back against the file's values.
        // Only bytes that differ are written, and a file that names another
        // serial than the printer reports is a gate of its own.
        ResetOutcome Restore(const RestorePlan& plan, const ResetHandlers& handlers = {});

    private:
        // Everything that differs between the waste-pad and ink resets, so
        // RunLifecycle() stays the single hardware-verified write path.
        struct ResetFlow
        {
            std::vector<std::vector<unsigned char>> sequence;
            // Every address `sequence` writes, for the backup.
            std::vector<uint16_t> writeAddresses;
            // Backup::target.
            std::string target;
            // nullopt = the model's own counter bytes (the waste default).
            std::optional<std::vector<uint16_t>> readAddresses;
            // Parallel vectors: what the read-back must show.
            std::vector<uint16_t> verifyAddresses;
            std::vector<uint8_t> verifyValues;
            std::function<std::optional<Blocker>(const PrinterStatus&)> classify;
            // Appended to "The printer reports <ERROR>" for expected errors.
            std::string expectedErrorNote;
            // Noun for the verification verdicts ("counter", "ink counter").
            std::string counterNoun = "counter";
            // What the read-back compares against, in those verdicts.
            std::string expectedValueNoun = "reset value";
            // What the run is called in its progress lines.
            std::string runNoun = "reset";
            // The read-only re-check to suggest on a verify mismatch.
            std::string recheckCommand = "'ewr --status'";
            // Schema-4 close step after the writes (waste only).
            bool commit = false;
            // Abort when the preflight cannot be read: ink never runs blind.
            bool requirePreflight = false;
            // After the printer's own gate, with the preflight reading: may
            // narrow the writes to what that reading says is needed and ask a
            // gate of its own. False stops the run with `out` as it left it.
            std::function<bool(const StateSnapshot& before, ResetFlow& flow, ResetOutcome& out,
                               const ResetHandlers& handlers)> afterPreflight;
        };

        ResetOutcome RunLifecycle(ResetFlow flow, const ResetHandlers& handlers);

        // False when the run must stop: a backup that could not be kept, or
        // came back short, and the host declined to go on without it.
        bool KeepBackup(const ResetFlow& flow, const ResetHandlers& handlers, ResetOutcome& out);

        StateSnapshot ReadStateFor(const std::vector<uint16_t>* addressOverride) const;

        // One query session of range reads, one per (start, count) chunk.
        // `values` gets every byte that parsed; a chunk that did not is left
        // out, for the caller to read byte by byte.
        StateSnapshot ReadRanges(const std::vector<std::pair<uint16_t, uint8_t>>& chunks,
                                 std::vector<bool>& parsed) const;
        // The status still comes over D4; the counters only answer in JIG mode.
        StateSnapshot ReadJigStateFor(const std::vector<uint16_t>* addressOverride) const;
        ExecutorOptions BuildWriteOptions() const;
        std::vector<std::vector<unsigned char>> BuildWriteSession(
            const std::vector<std::pair<uint16_t, uint8_t>>& writes) const;
        bool RunCommit(const ExecutorOptions& writeOptions);

        const DbPrinterModel& m_model;
        IDeviceGateway& m_gateway;
        log::Reporter& m_reporter;
        ExecutorOptions m_queryOptions;
        mutable RangeReads m_rangeReads = RangeReads::Unknown;
    };

} // namespace ewr
