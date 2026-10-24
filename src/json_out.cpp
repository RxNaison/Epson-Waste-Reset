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

        // A report is valid once any field decodes, so the state field itself
        // may be the one that never came.
        nlohmann::json out;
        out["state"] = status.stateName.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.stateName);
        out["state_code"] = status.stateCode >= 0 ? nlohmann::json(status.stateCode) : nlohmann::json(nullptr);
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

    nlohmann::json JsonInterface(const InterfaceInfo& info, const std::string& modelMatch)
    {
        nlohmann::json out;
        out["index"] = info.index;
        out["class"] = info.className;
        // -1 in the struct means "not a composite interface".
        out["interface_number"] = info.interfaceNumber >= 0 ? nlohmann::json(info.interfaceNumber)
                                                            : nlohmann::json(nullptr);
        out["path"] = info.path;
        out["device_id"] = info.deviceId.empty() ? nlohmann::json(nullptr) : nlohmann::json(info.deviceId);
        out["model_match"] = modelMatch.empty() ? nlohmann::json(nullptr) : nlohmann::json(modelMatch);
        return out;
    }

    nlohmann::json JsonNetworkPrinter(const NetworkPrinter& printer, const std::string& modelMatch)
    {
        nlohmann::json out;
        out["address"] = printer.address;
        out["device_id"] = printer.deviceId;
        out["reported_model"] = printer.model.empty() ? nlohmann::json(nullptr) : nlohmann::json(printer.model);
        out["model_match"] = modelMatch.empty() ? nlohmann::json(nullptr) : nlohmann::json(modelMatch);
        return out;
    }

    nlohmann::json JsonBlocker(const Blocker& blocker)
    {
        nlohmann::json out;
        out["error"] = blocker.errorName;
        // -1 means the objection is not a printer error (busy, a cut-short
        // report, a database conflict), which has no printer code to give.
        out["error_code"] = blocker.errorCode >= 0 ? nlohmann::json(blocker.errorCode) : nlohmann::json(nullptr);
        out["explanation"] = blocker.explanation;
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
            case ResetPhase::NotSupported:   return "not_supported";
        }

        return "not_started";
    }

    nlohmann::json JsonKeySearchData(const std::string& printerReports, const ReadKeySearch& search)
    {
        nlohmann::json out;
        out["printer"] = printerReports.empty() ? nlohmann::json(nullptr) : nlohmann::json(printerReports);
        out["completed"] = search.completed;
        out["tried"] = search.probes.size();

        out["answers"] = {
            { "value", search.Count(ReadAnswer::Value) },
            { "refused", search.Count(ReadAnswer::Refused) },
            { "empty", search.Count(ReadAnswer::Empty) },
            { "silent", search.Count(ReadAnswer::Silent) },
            { "other", search.Count(ReadAnswer::Other) },
        };

        nlohmann::json keys = nlohmann::json::array();
        for (const ReadKeyProbe& probe : search.probes)
        {
            if (probe.answer != ReadAnswer::Value)
                continue;

            keys.push_back({
                { "read_key", probe.candidate.rkey },
                { "address_length", probe.candidate.addressLength },
                { "value", probe.value },
                { "models", probe.candidate.models },
            });
        }
        out["keys"] = std::move(keys);

        return out;
    }

    nlohmann::json JsonBackup(const ResetOutcome& outcome)
    {
        if (!outcome.backupTaken)
            return nullptr;

        return {
            { "file", outcome.backupLocation.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(outcome.backupLocation) },
            { "bytes", outcome.backup.bytes.size() },
            { "complete", outcome.backup.Complete() },
            { "values", JsonCounterValues(outcome.backup.bytes) },
        };
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

        // One shape for every gate, null where a key does not apply.
        nlohmann::json overrides = nlohmann::json::array();
        for (const auto& passed : outcome.overrides)
        {
            const bool mismatch = passed.gate == "model_mismatch";
            const bool blocker = passed.gate == "blocker";
            overrides.push_back({
                { "gate", passed.gate },
                { "detected_model", mismatch ? nlohmann::json(passed.detail) : nlohmann::json(nullptr) },
                { "error", blocker ? nlohmann::json(passed.detail) : nlohmann::json(nullptr) },
                { "error_code", blocker && passed.errorCode >= 0 ? nlohmann::json(passed.errorCode)
                                                                 : nlohmann::json(nullptr) },
                // Why a gate without a key of its own objected ("not_saved").
                { "detail", (mismatch || blocker || passed.detail.empty()) ? nlohmann::json(nullptr)
                                                                            : nlohmann::json(passed.detail) },
            });
        }
        out["overrides"] = overrides;
        out["backup"] = JsonBackup(outcome);
        return out;
    }

    nlohmann::json JsonPrinterInfo(const DbPrinterModel* model, const PrinterInfo& info)
    {
        auto orNull = [](const std::string& text) {
            return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
        };

        nlohmann::json counters = nlohmann::json::array();
        for (const auto& counter : info.counters)
            counters.push_back({ { "key", counter.key },
                                 { "value", counter.value >= 0 ? nlohmann::json(counter.value) : nlohmann::json(nullptr) } });

        nlohmann::json fatal = nlohmann::json::array();
        for (const auto& slot : info.fatal)
            fatal.push_back({ { "kind", slot.kind }, { "slot", slot.slot },
                              { "code", slot.code >= 0 ? nlohmann::json(slot.code) : nlohmann::json(nullptr) } });

        nlohmann::json out;
        out["model"] = model ? nlohmann::json(model->name) : nlohmann::json(nullptr);
        out["printer"] = info.available ? JsonPrinterStatus(info.status) : nlohmann::json(nullptr);
        out["firmware"] = orNull(info.firmware);
        out["serial"] = orNull(info.serial);
        out["serial_source"] = orNull(info.serialSource);
        out["counters"] = std::move(counters);
        out["first_use"] = info.firstUse >= 0 ? nlohmann::json(info.firstUse) : nlohmann::json(nullptr);
        out["fatal_errors"] = std::move(fatal);
        return out;
    }

    nlohmann::json JsonMaintenanceData(const DbPrinterModel* model, const MaintenanceRequest& request,
                                       const MaintenanceOutcome& outcome)
    {
        auto orNull = [](const std::string& text) {
            return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
        };

        const bool clean = request.action != MaintenanceAction::NozzleCheck;
        const char* group = request.group == CleanGroup::Black ? "black"
                          : request.group == CleanGroup::Color ? "color" : "all";

        nlohmann::json overrides = nlohmann::json::array();
        for (const auto& passed : outcome.overrides)
            overrides.push_back({ { "gate", passed.gate }, { "error", passed.detail },
                                  { "error_code", passed.errorCode >= 0 ? nlohmann::json(passed.errorCode)
                                                                         : nlohmann::json(nullptr) } });

        nlohmann::json out;
        out["model"] = model ? nlohmann::json(model->name) : nlohmann::json(nullptr);
        out["action"] = MaintenanceActionName(request.action);
        out["level"] = (request.action == MaintenanceAction::HeadClean) ? nlohmann::json(request.level)
                                                                         : nlohmann::json(nullptr);
        out["group"] = clean ? nlohmann::json(group) : nlohmann::json(nullptr);
        out["phase"] = MaintenancePhaseName(outcome.phase);
        out["route"] = orNull(outcome.route);
        out["sent"] = outcome.sent;
        out["reply"] = orNull(outcome.reply);
        out["started"] = outcome.started;
        out["seconds"] = outcome.seconds;
        out["printer_before"] = outcome.before.available ? JsonPrinterStatus(outcome.before.status) : nlohmann::json(nullptr);
        out["printer_after"] = outcome.last.valid ? JsonPrinterStatus(outcome.last) : nlohmann::json(nullptr);
        out["pads_before"] = (model && outcome.before.available) ? JsonPadUsage(*model, outcome.before.values)
                                                                 : nlohmann::json::array();
        out["pads_after"] = (model && outcome.after.available) ? JsonPadUsage(*model, outcome.after.values)
                                                               : nlohmann::json::array();
        out["overrides"] = std::move(overrides);
        return out;
    }

    nlohmann::json JsonRestorePlan(const Backup& source, const RestorePlan& plan, const std::string& file)
    {
        auto orNull = [](const std::string& text) {
            return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
        };

        nlohmann::json planned = nlohmann::json::array();
        for (const auto& w : plan.writes)
            planned.push_back({ { "address", w.first }, { "value", w.second } });

        nlohmann::json skipped = nlohmann::json::array();
        for (const auto& skip : plan.skipped)
            skipped.push_back({ { "address", skip.address }, { "reason", skip.reason } });

        nlohmann::json out;
        out["model"] = plan.model;
        out["target"] = "restore";
        out["source"] = {
            { "file", orNull(file) },
            { "kind", source.source },
            { "model", source.model },
            { "created", orNull(source.created) },
            { "serial", orNull(source.serial) },
        };
        out["planned_writes"] = std::move(planned);
        out["skipped"] = std::move(skipped);
        out["missing"] = plan.missing;
        return out;
    }

    nlohmann::json JsonRestoreData(const DbPrinterModel& model, const Backup& source, const RestorePlan& plan,
                                   const std::string& file, const ResetOutcome& outcome)
    {
        nlohmann::json out = JsonResetData(model, plan.touchesInk && !plan.touchesWaste, outcome);
        for (const auto& item : JsonRestorePlan(source, plan, file).items())
            out[item.key()] = item.value();
        out["unchanged"] = outcome.unchanged;
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
