#pragma once
#include "ewr/log.h"
#include "ewr/session.h"

#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ewr {

    // The `--json` contract, written to a caller-owned stream: one JSON object
    // per line, a `hello` first and a `result` last, whatever happened in
    // between. docs/json-output.md is the contract itself and the promises
    // made to callers; this class is only what writes it.
    //
    // In the library rather than the CLI because the stream is the caller's:
    // a GUI can point it at a pipe, the tests at a stringstream. Nothing here
    // knows about stdout.
    class JsonEmitter
    {
    public:
        // Bumped only for a breaking change, and never quietly: see the
        // versioning promise in docs/json-output.md.
        static constexpr int kContractVersion = 1;

        explicit JsonEmitter(std::ostream& out) : m_out(out), m_start(std::chrono::steady_clock::now()) {}

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

    // The contract's spellings, which are not the enum's and must not drift
    // with it.
    const char* JsonLevelName(log::Level level);
    const char* JsonStageName(log::Stage stage);

    // ------------------------------------------------------------------
    //  The `data` objects of docs/json-output.md
    // ------------------------------------------------------------------
    //
    // Here rather than in the CLI because the C API answers with the same
    // shapes: one definition, so a caller that moves from spawning ewr to
    // linking against it reads the same JSON.

    // [{"address": 12, "value": 0}, ...]; an unread byte has a null value.
    nlohmann::json JsonCounterValues(const std::vector<std::pair<uint16_t, int>>& values);

    // [{"name": ..., "used": ..., "max": ..., "percent": ...}, ...]. A pad
    // group that could not be read whole is left out.
    nlohmann::json JsonPadUsage(const DbPrinterModel& model,
                                const std::vector<std::pair<uint16_t, int>>& values);

    // [{"kind": ..., "name": ..., "readable": ...}, ...] - every pad a waste-pad
    // reset covers, including the ones EWR cannot read. See
    // DbPrinterModel::GetResetCoverage for how it is worked out.
    nlohmann::json JsonResetCoverage(const DbPrinterModel& model);

    // The '@BDC ST2' report, or null when nothing parsed.
    nlohmann::json JsonPrinterStatus(const PrinterStatus& status);

    // model / printer / counters / pads, as `status` and `dry-run` carry them.
    nlohmann::json JsonStateData(const DbPrinterModel& model, const StateSnapshot& state);

    // What a reset ended up doing: phase, writes, verification, before/after.
    nlohmann::json JsonResetData(const DbPrinterModel& model, bool ink, const ResetOutcome& outcome);

    // The contract's spelling for a reset phase.
    const char* JsonResetPhaseName(ResetPhase phase);

} // namespace ewr
