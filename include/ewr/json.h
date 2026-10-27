#pragma once
#include "ewr/info.h"
#include "ewr/keysearch.h"
#include "ewr/log.h"
#include "ewr/maintenance.h"
#include "ewr/network.h"
#include "ewr/session.h"
#include "ewr/usb.h"

#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The `--json` contract and the `data` objects it carries.
namespace ewr::json {

    // The `--json` contract, written to a caller-owned stream: one JSON object
    // per line, a `hello` first and a `result` last, whatever happened in
    // between. docs/json-output.md is the contract itself and the promises
    // made to callers; this class is only what writes it.
    //
    // In the library rather than the CLI because the stream is the caller's:
    // a GUI can point it at a pipe, the tests at a stringstream. Nothing here
    // knows about stdout.
    class Emitter
    {
    public:
        // Bumped only for a breaking change, and never quietly: see the
        // versioning promise in docs/json-output.md.
        static constexpr int kContractVersion = 1;

        explicit Emitter(std::ostream& out) : m_out(out), m_start(std::chrono::steady_clock::now()) {}

        void Hello(const std::string& ewrVersion,
                   const std::string& platform,
                   const std::string& command,
                   const nlohmann::json& model,   // string, or null
                   const nlohmann::json& flags);

        // Sink for log::Reporter. Trace events are not part of the contract -
        // they belong to ewr_trace.log - so they are dropped here.
        void Event(const log::Event& event);

        // Exactly one per run: the first call wins, so an exit path that runs
        // twice cannot end the stream with two verdicts.
        void Result(const std::string& command,
                    bool ok,
                    int exitCode,
                    const nlohmann::json& errorCode, // string, or null
                    const nlohmann::json& error,     // string, or null
                    const nlohmann::json& data);

        bool ResultEmitted() const { return m_resultEmitted; }

    private:
        void Write(const char* type, nlohmann::json line);

        std::ostream& m_out;
        std::chrono::steady_clock::time_point m_start;
        int m_seq = 0;
        bool m_resultEmitted = false;
    };

    // The contract's spellings, which are not the enums' and must not drift
    // with them.
    const char* Name(log::Level level);
    const char* Name(log::Stage stage);
    const char* Name(ResetPhase phase);

    // ------------------------------------------------------------------
    //  The `data` objects of docs/json-output.md
    // ------------------------------------------------------------------
    //
    // Here rather than in the CLI because the C API answers with the same
    // shapes: one definition, so a caller that moves from spawning ewr to
    // linking against it reads the same JSON.

    // [{"address": 12, "value": 0}, ...]; an unread byte has a null value.
    nlohmann::json CounterValues(const std::vector<std::pair<uint16_t, int>>& values);

    // [{"name": ..., "used": ..., "max": ..., "percent": ...}, ...]. A pad
    // group that could not be read whole is left out.
    nlohmann::json PadUsage(const DbPrinterModel& model,
                            const std::vector<std::pair<uint16_t, int>>& values);

    // [{"kind": ..., "name": ..., "readable": ...}, ...] - every pad a waste-pad
    // reset covers, including the ones EWR cannot read. See
    // DbPrinterModel::GetResetCoverage for how it is worked out.
    nlohmann::json ResetCoverage(const DbPrinterModel& model);

    // The '@BDC ST2' report, or null when nothing parsed.
    nlohmann::json Status(const PrinterStatus& status);

    // One --list / ewr_list_interfaces entry. `modelMatch` is the database
    // entry the device ID names, empty when none does.
    nlohmann::json InterfaceEntry(const usb::InterfaceInfo& info, const std::string& modelMatch);

    // One --list `network_printers` / ewr_discover_network entry:
    // {"address", "device_id", "reported_model", "model_match"}.
    nlohmann::json NetworkEntry(const NetworkPrinter& printer, const std::string& modelMatch);

    // What a blocker callback is asked: {"error", "error_code", "explanation"}.
    nlohmann::json BlockerData(const Blocker& blocker);

    // model / printer / counters / pads, as `status` and `dry-run` carry them.
    nlohmann::json StateData(const DbPrinterModel& model, const StateSnapshot& state);

    // {file, bytes, complete, values}: what the backup step kept, null when
    // there was no preflight to take it from.
    nlohmann::json BackupData(const ResetOutcome& outcome);

    // What a reset ended up doing: phase, writes, verification, before/after.
    nlohmann::json ResetData(const DbPrinterModel& model, bool ink, const ResetOutcome& outcome);

    // `info`: firmware, serial, the printer's own counters and its last fatal
    // error codes. `model` may be null.
    nlohmann::json InfoData(const DbPrinterModel* model, const PrinterInfo& info);

    // `nozzle-check`, `head-clean`, `power-clean`: what was asked, which way it
    // went, and what the printer's status said about it.
    nlohmann::json MaintenanceData(const DbPrinterModel* model, const MaintenanceRequest& request,
                                   const MaintenanceOutcome& outcome);

    // What a restore was asked to do before it touched the printer: the file
    // it came from, the bytes it would write, the ones it left alone and why,
    // and the model's bytes the file did not hold.
    nlohmann::json RestorePlanData(const Backup& source, const RestorePlan& plan, const std::string& file);

    // ResetData of the restore's outcome, with the plan's keys, target
    // "restore", and the bytes left alone because they already matched.
    nlohmann::json RestoreData(const DbPrinterModel& model, const Backup& source, const RestorePlan& plan,
                               const std::string& file, const ResetOutcome& outcome);

    // What `find-key` found: how every key was answered, and the ones that
    // work. `printerReports` empty becomes null - the printer named nothing.
    nlohmann::json KeySearchData(const std::string& printerReports, const keysearch::Result& search);

} // namespace ewr::json
