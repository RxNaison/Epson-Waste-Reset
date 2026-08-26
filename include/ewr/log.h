#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// EWR is a library first and a CLI second: nothing under src/ may write to
// std::cout or std::cerr. Code emits Events into a Reporter instead, and the
// host decides what they mean - the CLI prints them, the trace log writes them
// to a file, a GUI turns them into progress bars, status rows and dialogs.
//
// A default-constructed Reporter has no sinks and is therefore silent, which
// is the only correct default for a library: an embedder that installs nothing
// gets nothing on its stdout.
//
// Every event carries a stable `code` next to the human-readable `message`.
// UIs switch on the code and on the structured fields; they must never parse
// the message, so wording stays free to change and to be translated later.

namespace ewr::log
{
    enum class Level
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
    };

    // Lets a UI route an event without reading its text: Detect and Database
    // belong in a status line, Write and Commit in the progress panel.
    enum class Stage
    {
        General,
        Database,
        Update,
        Detect,
        Handshake,
        Read,
        Write,
        Verify,
        Commit,
    };

    struct Event
    {
        Level level = Level::Info;
        Stage stage = Stage::General;

        // e.g. "db.entry_skipped". Never reworded, never translated: the only
        // part UIs are allowed to depend on.
        std::string code;

        // Human-readable English. Free to change; do not parse.
        std::string message;

        // -1 when the event is not one step of a known-length sequence.
        int index = -1;
        int total = -1;

        // {"model", "L3260"}, {"address", "0x2F"}. Keys are as stable as `code`.
        std::map<std::string, std::string> fields;

        bool HasProgress() const { return index >= 0 && total > 0; }
    };

    using Sink = std::function<void(const Event&)>;

    class Reporter
    {
    public:
        int AddSink(Sink sink)
        {
            if (!sink)
                return 0;

            std::lock_guard<std::mutex> lock(m_mutex);
            const int id = m_nextId++;
            m_sinks.emplace_back(id, std::move(sink));
            return id;
        }

        void RemoveSink(int id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_sinks.begin(); it != m_sinks.end(); ++it)
            {
                if (it->first == id)
                {
                    m_sinks.erase(it);
                    return;
                }
            }
        }

        void ClearSinks()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sinks.clear();
        }

        bool HasSinks() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return !m_sinks.empty();
        }

        // Safe to call from any thread: the background updater emits from its
        // own thread while a reset runs on the main one. Sinks are copied out
        // under the lock and invoked outside it, so a sink may log, install
        // another sink, or block without deadlocking the emitter.
        void Emit(const Event& event) const
        {
            std::vector<Sink> sinks;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                sinks.reserve(m_sinks.size());
                for (const auto& entry : m_sinks)
                    sinks.push_back(entry.second);
            }

            for (const auto& sink : sinks)
                sink(event);
        }

        void Log(Level level, Stage stage, std::string code, std::string message) const
        {
            Event event;
            event.level = level;
            event.stage = stage;
            event.code = std::move(code);
            event.message = std::move(message);
            Emit(event);
        }

    private:
        mutable std::mutex m_mutex;
        std::vector<std::pair<int, Sink>> m_sinks;
        int m_nextId = 1;
    };

    // Where library code emits when the caller passes no reporter. Hosts
    // install their sinks here once at startup.
    inline Reporter& Default()
    {
        static Reporter reporter;
        return reporter;
    }

    inline void Log(Level level, Stage stage, std::string code, std::string message)
    {
        Default().Log(level, stage, std::move(code), std::move(message));
    }

    inline const char* ToString(Level level)
    {
        switch (level)
        {
        case Level::Trace:   return "trace";
        case Level::Debug:   return "debug";
        case Level::Info:    return "info";
        case Level::Warning: return "warning";
        case Level::Error:   return "error";
        }
        return "info";
    }

    inline const char* ToString(Stage stage)
    {
        switch (stage)
        {
        case Stage::General:   return "general";
        case Stage::Database:  return "database";
        case Stage::Update:    return "update";
        case Stage::Detect:    return "detect";
        case Stage::Handshake: return "handshake";
        case Stage::Read:      return "read";
        case Stage::Write:     return "write";
        case Stage::Verify:    return "verify";
        case Stage::Commit:    return "commit";
        }
        return "general";
    }

    // Plain message to one stream; what the trace log installs.
    inline Sink OStreamSink(std::ostream& out, Level minimum = Level::Info)
    {
        std::ostream* stream = &out;
        return [stream, minimum](const Event& event)
        {
            if (static_cast<int>(event.level) < static_cast<int>(minimum))
                return;

            (*stream) << event.message << std::endl;
        };
    }

    // Warnings and errors to stderr so they survive a redirected stdout.
    inline Sink ConsoleSink(std::ostream& normal, std::ostream& errors, Level minimum = Level::Info)
    {
        std::ostream* out = &normal;
        std::ostream* err = &errors;
        return [out, err, minimum](const Event& event)
        {
            if (static_cast<int>(event.level) < static_cast<int>(minimum))
                return;

            std::ostream* target = (event.level >= Level::Warning) ? err : out;
            (*target) << event.message << std::endl;
        };
    }

} // namespace ewr::log
