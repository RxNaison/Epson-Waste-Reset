#include "ewr/json_out.h"

namespace ewr {

    const char* JsonLevelName(log::Level level)
    {
        switch (level)
        {
            case log::Level::Trace:   return "trace";
            case log::Level::Info:    return "info";
            case log::Level::Warning: return "warning";
            case log::Level::Error:   return "error";
        }

        return "info";
    }

    const char* JsonStageName(log::Stage stage)
    {
        switch (stage)
        {
            case log::Stage::General:   return "general";
            case log::Stage::Database:  return "database";
            case log::Stage::Update:    return "update";
            case log::Stage::Detect:    return "detect";
            case log::Stage::Handshake: return "handshake";
            case log::Stage::Read:      return "read";
            case log::Stage::Write:     return "write";
            case log::Stage::Verify:    return "verify";
            case log::Stage::Commit:    return "commit";
        }

        return "general";
    }

    nlohmann::json JsonCounterValues(const std::vector<std::pair<uint16_t, int>>& values)
    {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& value : values)
        {
            nlohmann::json entry;
            entry["address"] = value.first;
            entry["value"] = (value.second >= 0) ? nlohmann::json(value.second) : nlohmann::json(nullptr);
            out.push_back(std::move(entry));
        }
        return out;
    }

    nlohmann::json JsonPadUsage(const DbPrinterModel& model,
                                const std::vector<std::pair<uint16_t, int>>& values)
    {
        nlohmann::json out = nlohmann::json::array();

        for (const auto& spec : model.GetAllCounters())
        {
            const CounterReading reading = EvaluateCounter(spec, values);
            if (!reading.complete)
                continue;

            const int percent = reading.Percent();

            nlohmann::json pad;
            pad["name"] = reading.description.empty() ? std::string("Counter") : reading.description;
            // The key that says which pad this is without reading the label.
            pad["kind"] = spec.kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(spec.kind);
            pad["used"] = static_cast<unsigned>(reading.value);
            pad["max"] = (reading.max_value > 0) ? nlohmann::json(static_cast<unsigned>(reading.max_value))
                                                 : nlohmann::json(nullptr);
            pad["percent"] = (percent >= 0) ? nlohmann::json(percent) : nlohmann::json(nullptr);
            out.push_back(std::move(pad));
        }

        return out;
    }

    nlohmann::json JsonResetCoverage(const DbPrinterModel& model)
    {
        nlohmann::json out = nlohmann::json::array();
        for (const PadCoverage& pad : model.GetResetCoverage())
        {
            nlohmann::json entry;
            entry["kind"] = pad.kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(pad.kind);
            entry["name"] = pad.name;
            entry["readable"] = pad.readable;
            out.push_back(std::move(entry));
        }
        return out;
    }

    nlohmann::json JsonPrinterStatus(const PrinterStatus& status)
    {
        if (!status.valid)
            return nullptr;

        nlohmann::json inks = nlohmann::json::array();
        for (const InkReading& ink : status.inks)
        {
            nlohmann::json one;
            one["color"] = ink.colorName;
            one["code"] = ink.colorCode;
            one["level"] = (ink.level >= 0) ? nlohmann::json(ink.level) : nlohmann::json(nullptr);
            one["status"] = ink.statusText;
            inks.push_back(std::move(one));
        }

        nlohmann::json out;
        out["state"] = status.stateName;
        out["state_code"] = status.stateCode;
        out["error"] = status.hasError ? nlohmann::json(status.errorName) : nlohmann::json(nullptr);
        out["error_code"] = status.hasError ? nlohmann::json(status.errorCode) : nlohmann::json(nullptr);
        out["truncated"] = status.truncated;
        out["serial"] = status.serial.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.serial);
        out["maintenance_box"] = (status.maintenanceBoxLevel >= 0)
            ? nlohmann::json(status.maintenanceBoxLevel) : nlohmann::json(nullptr);
        // A box can report a condition without a level, so the text is its own
        // key rather than a second meaning for the number above.
        out["maintenance_box_status"] = status.maintenanceBoxText.empty()
            ? nlohmann::json(nullptr) : nlohmann::json(status.maintenanceBoxText);
        out["inks"] = std::move(inks);
        return out;
    }

    nlohmann::json JsonStateData(const DbPrinterModel& model, const StateSnapshot& state)
    {
        nlohmann::json out;
        out["model"] = model.name;
        // Filled in by a host that ran detection; null says "not asked", not
        // "no match", so a caller never reads silence as agreement.
        out["detected_model"] = nullptr;
        out["printer"] = state.available ? JsonPrinterStatus(state.status) : nlohmann::json(nullptr);
        out["counters"] = state.available ? JsonCounterValues(state.values) : nlohmann::json::array();
        out["pads"] = state.available ? JsonPadUsage(model, state.values) : nlohmann::json::array();
        // How many pads the model has, so an empty `pads` can be told apart
        // from a model that reports none: a group whose bytes did not all come
        // back is left out of `pads` rather than reported with a wrong total.
        out["pads_total"] = model.GetAllCounters().size();
        // What a waste-pad reset covers, readable or not. From the database, so
        // it is there whether the read worked or not: `pads` says how full the
        // readable pads are, this says which pads a reset would clear.
        out["reset_covers"] = JsonResetCoverage(model);
        return out;
    }

    const char* JsonResetPhaseName(ResetPhase phase)
    {
        switch (phase)
        {
            case ResetPhase::NotStarted:     return "not_started";
            case ResetPhase::Aborted:        return "aborted";
            case ResetPhase::DeviceNotFound: return "device_not_found";
            case ResetPhase::WriteFailed:    return "write_failed";
            case ResetPhase::Done:           return "done";
        }

        return "not_started";
    }

    nlohmann::json JsonResetData(const DbPrinterModel& model, bool ink, const ResetOutcome& outcome)
    {
        nlohmann::json out;
        out["model"] = model.name;
        out["target"] = ink ? "ink" : "waste";
        out["phase"] = JsonResetPhaseName(outcome.phase);
        out["writes"] = {
            { "verified", outcome.writesVerified },
            { "total", outcome.writesTotal },
        };
        out["alternate_key_used"] = outcome.alternateKeyUsed;
        out["committed"] = outcome.committed;
        out["verification"] = {
            { "ran", outcome.verificationRan },
            { "mismatches", outcome.verifyMismatches },
            { "unread", outcome.verifyUnread },
        };
        out["before"] = JsonCounterValues(outcome.before.values);
        out["after"] = JsonCounterValues(outcome.after.values);
        return out;
    }

    void JsonEmitter::Write(const char* type, nlohmann::json line)
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_start).count();

        nlohmann::json envelope;
        envelope["v"] = kContractVersion;
        envelope["type"] = type;
        envelope["seq"] = m_seq++;
        envelope["t"] = static_cast<long long>(ms);

        // The envelope first, so a human reading the stream sees what a line is
        // before what it says. nlohmann keeps insertion order off by default,
        // so this is about the merge order, not the printed order.
        envelope.update(line);

        // dump() escapes and never emits a raw newline, so one object per line
        // holds. A flush per line: a caller reading the pipe live is the point
        // of the mode.
        m_out << envelope.dump() << "\n" << std::flush;
    }

    void JsonEmitter::Hello(const std::string& ewrVersion,
                            const std::string& platform,
                            const std::string& command,
                            const nlohmann::json& model,
                            const nlohmann::json& flags)
    {
        nlohmann::json line;
        line["ewr"] = ewrVersion;
        line["platform"] = platform;
        line["command"] = command;
        line["model"] = model;
        line["flags"] = flags;
        Write("hello", std::move(line));
    }

    void JsonEmitter::Event(const log::Event& event)
    {
        if (event.level == log::Level::Trace)
            return;

        nlohmann::json line;
        line["code"] = event.code;
        line["level"] = JsonLevelName(event.level);
        line["stage"] = JsonStageName(event.stage);
        line["message"] = event.message;
        line["index"] = event.HasProgress() ? nlohmann::json(event.index) : nlohmann::json(nullptr);
        line["total"] = event.HasProgress() ? nlohmann::json(event.total) : nlohmann::json(nullptr);
        line["fields"] = event.fields;
        Write("event", std::move(line));
    }

    void JsonEmitter::Result(const std::string& command,
                             bool ok,
                             int exitCode,
                             const nlohmann::json& errorCode,
                             const nlohmann::json& error,
                             const nlohmann::json& data)
    {
        if (m_resultEmitted)
            return;

        m_resultEmitted = true;

        nlohmann::json line;
        line["command"] = command;
        line["ok"] = ok;
        line["exit"] = exitCode;
        line["error_code"] = errorCode;
        line["error"] = error;
        line["data"] = data.is_null() ? nlohmann::json::object() : data;
        Write("result", std::move(line));
    }

} // namespace ewr
