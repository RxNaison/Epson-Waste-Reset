#pragma once
#include "ewr/executor.h"
#include "ewr/generator.h"
#include "ewr/log.h"
#include "ewr/status.h"
#include "ewr/usb.h"

#include <cstdint>
#include <functional>
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
    };

    struct ResetOutcome
    {
        ResetPhase phase = ResetPhase::NotStarted;

        bool success = false;
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

        std::string error; // failure detail when the run did not complete
    };

    // ------------------------------------------------------------------
    //  Device gateway: the only thing between a Session and the hardware
    // ------------------------------------------------------------------

    // The device I/O a session needs, behind an interface so the lifecycle
    // is unit-testable with a scripted fake and USB-driven in production.
    struct IDeviceGateway
    {
        virtual ~IDeviceGateway() = default;

        virtual QueryRunResult RunQuery(
            const std::vector<std::vector<unsigned char>>& handshake,
            const std::vector<std::vector<unsigned char>>& queries,
            const ExecutorOptions& options) = 0;

        virtual ResetRunResult RunReset(
            const std::vector<std::vector<unsigned char>>& sequence,
            const ExecutorOptions& options) = 0;
    };

    // The first device call of a run starts ewr_trace.log fresh and every
    // later one appends, so a program run yields one coherent trace file.
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

    private:
        // False exactly once: the call that must start the trace file fresh.
        bool NextCallAppends();

        bool m_traceStarted = false;
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
        // the --dump diagnostic.
        StateSnapshot ReadAddresses(const std::vector<uint16_t>& addresses) const;

        ResetOutcome Reset(const ResetHandlers& handlers = {});

        // Same lifecycle over ink_groups, with two differences: INK OUT does
        // not gate it, and there is no commit step. On chipped cartridges the
        // chip is authoritative, so the reset will not hold.
        ResetOutcome ResetInk(const ResetHandlers& handlers = {});

    private:
        // Everything that differs between the waste-pad and ink resets, so
        // RunLifecycle() stays the single hardware-verified write path.
        struct ResetFlow
        {
            std::vector<std::vector<unsigned char>> sequence;
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
            // The read-only re-check to suggest on a verify mismatch.
            std::string recheckCommand = "'ewr --status'";
            // Schema-4 close step after the writes (waste only).
            bool commit = false;
            // Abort when the preflight cannot be read: ink never runs blind.
            bool requirePreflight = false;
        };

        ResetOutcome RunLifecycle(const ResetFlow& flow, const ResetHandlers& handlers);

        StateSnapshot ReadStateFor(const std::vector<uint16_t>* addressOverride) const;
        ExecutorOptions BuildWriteOptions() const;
        std::vector<std::vector<unsigned char>> BuildWriteSession(
            const std::vector<std::pair<uint16_t, uint8_t>>& writes) const;
        bool RunCommit(const ExecutorOptions& writeOptions);

        const DbPrinterModel& m_model;
        IDeviceGateway& m_gateway;
        log::Reporter& m_reporter;
        ExecutorOptions m_queryOptions;
    };

} // namespace ewr
