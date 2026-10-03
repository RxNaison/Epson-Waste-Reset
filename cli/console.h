#pragma once

#include "ewr/log.h"

#include <memory>
#include <ostream>
#include <string>

// How library events read at the keyboard. --json and the C API still get every
// event, and ewr_trace.log every device step. A person needs what is happening
// to the printer and what to decide: an interface that failed on the way to one
// that worked still printed "Handshake FAILED", and made a working reset look
// broken.
namespace ewr::cli
{
    enum class ConsoleAction
    {
        Show,       // the library's own message
        Hide,       // machinery
        Progress,   // one step of a write sequence, folded into one line
        Say,        // shown in the CLI's own words
    };

    struct ConsoleRule
    {
        ConsoleAction action{ ConsoleAction::Show };
        const char* text{ nullptr };
    };

    inline ConsoleRule ConsoleRuleFor(const std::string& code)
    {
        static const char* const kHidden[] = {
            "session.scanning", "session.generating", "session.ink_generating",
            "usb.device_detected", "usb.sequence_begin", "usb.trace_log", "usb.lock_released",
            "usb.kernel_driver_detach", "usb.claim_failed",
            // One interface or attempt failing is the fallback working. If they
            // all fail, usb.reset_not_confirmed (snmp. over the network)
            // carries the reason.
            "exec.handshake_failed",
            "exec.write_retry", "exec.write_key_retry", "exec.write_rejected", "exec.write_refused",
            "escr.success", "end4.success",
            "session.commit", "session.commit_noop", "session.committed",
            // Logged from the update worker while the CLI's own update line is
            // still spinning; the CLI prints the outcome there instead.
            "update.database_applied", "update.database_failed",
        };

        static const char* const kProgress[] = {
            "exec.packet_sent", "exec.packet_acked", "exec.write_verified",
            "escr.write_verified", "end4.write_verified",
        };

        for (const char* hidden : kHidden)
            if (code == hidden)
                return { ConsoleAction::Hide };

        for (const char* step : kProgress)
            if (code == step)
                return { ConsoleAction::Progress };

        if (code == "usb.interface_fallback")
            return { ConsoleAction::Say, "[i] No answer on that USB connection - trying the next one..." };

        if (code == "session.preflight")
            return { ConsoleAction::Say, "\n[*] Reading the printer's current state (nothing is written yet)..." };

        return {};
    }

    // Write progress is one line redrawn in place on a terminal, and nothing
    // when stdout is piped, where the redraws would pile up in the log.
    inline log::Sink ConsoleFor(std::ostream& out, std::ostream& err, bool terminal)
    {
        struct State
        {
            std::ostream* out;
            std::ostream* err;
            bool terminal;
            bool lineOpen{ false };
            bool inCommit{ false };

            void EndLine()
            {
                if (lineOpen)
                    (*out) << std::endl;
                lineOpen = false;
            }

            void Handle(const log::Event& event)
            {
                if (event.level < log::Level::Info)
                    return;

                // The commit is a second device session of one write. Its
                // progress would read as a second reset, and its failure is
                // not the reset's: session.commit_failed says what it means.
                if (event.code == "session.commit")
                    inCommit = true;
                else if (event.code.rfind("session.commit", 0) == 0)
                    inCommit = false;

                if (inCommit && (event.code == "usb.reset_not_confirmed"
                                 || event.code == "snmp.reset_not_confirmed"))
                    return;

                const ConsoleRule rule = ConsoleRuleFor(event.code);
                switch (rule.action)
                {
                    case ConsoleAction::Hide:
                        return;

                    case ConsoleAction::Progress:
                        if (!terminal || inCommit || !event.HasProgress())
                            return;
                        (*out) << "\r[*] Writing to the printer... " << event.index << "/" << event.total
                               << std::flush;
                        lineOpen = true;
                        if (event.index >= event.total)
                            EndLine();
                        return;

                    case ConsoleAction::Say:
                        EndLine();
                        (*out) << rule.text << std::endl;
                        return;

                    case ConsoleAction::Show:
                        EndLine();
                        (*(event.level >= log::Level::Warning ? err : out)) << event.message << std::endl;
                        return;
                }
            }
        };

        auto state = std::make_shared<State>(State{ &out, &err, terminal });
        return [state](const log::Event& event) { state->Handle(event); };
    }

} // namespace ewr::cli
