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
