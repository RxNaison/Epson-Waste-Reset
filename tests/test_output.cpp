#include "support.h"

// A person sees what happens to the printer, not the fallback ladder: an
// interface that failed on the way to one that worked used to print "Handshake
// FAILED" on a reset that succeeded. The rules live in the CLI; --json and
// the C API still get every event.
// The filter sits on std::cout and std::cerr of every terminal run, so text
// that is not a tag must come through byte for byte.
void test_tags_are_colored_only_at_a_line_start()
{
    std::cout << "[TEST] test_tags_are_colored_only_at_a_line_start" << std::endl;

    namespace ansi = ewr::cli::ansi;
    const auto painted = [](const char* tag, const char* color) {
        return std::string(color) + tag + ansi::kReset;
    };

    std::ostringstream sink;
    ewr::cli::TagColorBuf buf(sink.rdbuf());
    std::ostream out(&buf);

    out << "[i] Detecting\n";
    out << "    [!] indented\n";
    out << "\n[ERROR] after a blank line\n";
    out << "plain [i] mid-line\n";
    out << "  [1] L3150 Series   (a menu entry)\n";
    out << "\r[*] Writing to the printer... 3/14" << std::flush;
    out << "\n[ERR";
    out << "OR] in two pieces\n";
    out << "Select a printer [0-2]: " << std::flush;

    const std::string text = sink.str();
    CHECK(text.find(painted("[i]", ansi::kCyan) + " Detecting\n") == 0);
    CHECK(text.find("    " + painted("[!]", ansi::kYellow) + " indented") != std::string::npos);
    CHECK(text.find("\n\n" + painted("[ERROR]", ansi::kRed) + " after a blank line") != std::string::npos);
    CHECK(text.find("plain [i] mid-line\n") != std::string::npos);
    CHECK(text.find("  [1] L3150 Series   (a menu entry)\n") != std::string::npos);
    CHECK(text.find("\r" + painted("[*]", ansi::kBlue) + " Writing") != std::string::npos);
    CHECK(text.find(painted("[ERROR]", ansi::kRed) + " in two pieces") != std::string::npos);

    // A prompt without a newline still reaches the screen on the flush before
    // the read, uncolored.
    CHECK(text.size() >= 24 && text.compare(text.size() - 24, 24, "Select a printer [0-2]: ") == 0);
}

void test_paint_leaves_text_alone_when_color_is_off()
{
    std::cout << "[TEST] test_paint_leaves_text_alone_when_color_is_off" << std::endl;

    namespace ansi = ewr::cli::ansi;
    const ewr::cli::ColorState saved = ewr::cli::Colors();

    ewr::cli::Colors() = {};
    CHECK(ewr::cli::Paint("OK", ansi::kGreen) == "OK");
    CHECK(ewr::cli::PaintErr("RESET FAILED", ansi::kRed) == "RESET FAILED");

    ewr::cli::Colors().out = true;
    CHECK(ewr::cli::Paint("OK", ansi::kGreen) == std::string(ansi::kGreen) + "OK" + ansi::kReset);
    CHECK(ewr::cli::PaintErr("RESET FAILED", ansi::kRed) == "RESET FAILED");
    CHECK(ewr::cli::Paint("", ansi::kGreen).empty());
    CHECK(ewr::cli::Paint("plain", nullptr) == "plain");

    ewr::cli::Colors() = saved;

    // A filling pad: green, yellow halfway, red at the service limit and past it.
    CHECK(std::string(ewr::cli::RampColor(0)) == "\033[38;5;46m");
    CHECK(std::string(ewr::cli::RampColor(50)) == "\033[38;5;226m");
    CHECK(std::string(ewr::cli::RampColor(100)) == "\033[38;5;196m");
    CHECK(std::string(ewr::cli::RampColor(101)) == "\033[38;5;196m");
    CHECK(std::string(ewr::cli::RampColor(-3)) == "\033[38;5;46m");
    CHECK(std::string(ewr::cli::RampColor(84)) == "\033[38;5;208m");
}

void test_console_hides_the_machinery()
{
    std::cout << "[TEST] test_console_hides_the_machinery" << std::endl;

    auto event = [](ewr::log::Level level, const char* code, const char* message, int index = -1, int total = -1)
    {
        ewr::log::Event e;
        e.level = level;
        e.code = code;
        e.message = message;
        e.index = index;
        e.total = total;
        return e;
    };

    using L = ewr::log::Level;

    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, false);

        sink(event(L::Info, "usb.sequence_begin", "Executing universal libusb hardware state machine..."));
        sink(event(L::Info, "exec.handshake_failed", "-> Handshake FAILED: no reply"));
        sink(event(L::Info, "usb.interface_fallback", "[!] Interface 1/3 stayed silent to D4, END4 and ESC/P Remote."));
        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 2 | EEPROM write verified (||:42:OK;).", 1, 2));
        sink(event(L::Info, "session.verified", "[SUCCESS] Read-back verification: every counter now holds its reset value."));
        sink(event(L::Trace, "exec.retry", "[RETRY] Command 1"));
        sink(event(L::Warning, "db.entry_skipped", "[!] skipped an entry"));

        const std::string text = out.str();
        CHECK(text.find("FAILED") == std::string::npos);
        CHECK(text.find("state machine") == std::string::npos);
        CHECK(text.find("stayed silent") == std::string::npos);
        CHECK(text.find("trying the next one") != std::string::npos);
        CHECK(text.find("[SUCCESS] Read-back verification") != std::string::npos);
        CHECK(text.find("RETRY") == std::string::npos);

        // Piped: no redraws to pile up in a log.
        CHECK(text.find("Writing to the printer") == std::string::npos);

        // Warnings keep going to stderr, where a redirected stdout cannot lose them.
        CHECK(err.str().find("skipped an entry") != std::string::npos);
        CHECK(text.find("skipped an entry") == std::string::npos);
    }

    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, true);

        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 2", 1, 2));
        sink(event(L::Info, "d4.session_recovering", "[!] The printer stopped answering - waiting for it to come back..."));
        sink(event(L::Info, "exec.write_verified", "-> Command 2 / 2", 2, 2));
        sink(event(L::Info, "session.verifying", "[*] Verifying"));

        const std::string text = out.str();
        CHECK(text.find("-> Command") == std::string::npos);

        // A message in the middle of the counter starts on its own line, and
        // the finished counter ends its own.
        CHECK(text.find("1/2\n[!] The printer stopped answering") != std::string::npos);
        CHECK(text.find("\r[*] Writing to the printer... 2/2\n[*] Verifying") != std::string::npos);
    }

    // The commit is a second device session of one write. Its counter would
    // read as a second reset and its failure is not the reset's, so both stay
    // quiet until the commit says how it went.
    {
        std::ostringstream out, err;
        ewr::log::Sink sink = ewr::cli::ConsoleFor(out, err, true);

        sink(event(L::Info, "session.commit", "[*] Commit step: latching the new counter values..."));
        sink(event(L::Info, "exec.write_verified", "-> Command 1 / 1", 1, 1));
        sink(event(L::Error, "usb.reset_not_confirmed", "[ERROR] no ack\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Error, "snmp.reset_not_confirmed", "[ERROR] no network ack\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Info, "session.commit_failed", "[!] Commit step did not complete (no ack)."));
        sink(event(L::Error, "usb.reset_not_confirmed", "[ERROR] the reset itself\n[!] The waste counter was NOT confirmed as reset."));
        sink(event(L::Error, "snmp.reset_not_confirmed", "[ERROR] the network reset itself\n[!] The waste counter was NOT confirmed as reset."));

        CHECK(out.str().find("Commit step: latching") == std::string::npos);
        CHECK(out.str().find("Writing to the printer") == std::string::npos);
        CHECK(out.str().find("Commit step did not complete") != std::string::npos);
        CHECK(err.str().find("no ack") == std::string::npos);
        CHECK(err.str().find("no network ack") == std::string::npos);
        CHECK(err.str().find("the reset itself") != std::string::npos);
        CHECK(err.str().find("the network reset itself") != std::string::npos);
    }

    // Everything the rules touch is Info except usb.claim_failed, which
    // usb.claim_all_failed repeats when no interface could be claimed. Hiding
    // any other warning or error would lose a real problem.
    const char* const hiddenOnPurpose[] = { "usb.claim_failed" };
    const char* const errorCodes[] = {
        "usb.reset_not_confirmed", "snmp.reset_not_confirmed", "usb.claim_all_failed", "usb.another_run",
        "snmp.another_run", "session.device_not_found",
        "session.preflight_required", "session.db_conflict", "db.parse_error", "usb.busy_status_monitor",
        "usb.access_denied", "usb.open_failed", "usb.soft_reset_settle_timeout", "usb.ready_print_driver", "usb.print_jobs_waiting",
    };
    for (const char* code : errorCodes)
        CHECK(ewr::cli::ConsoleRuleFor(code).action == ewr::cli::ConsoleAction::Show);
    for (const char* code : hiddenOnPurpose)
        CHECK(ewr::cli::ConsoleRuleFor(code).action == ewr::cli::ConsoleAction::Hide);
}

// "ERROR | ERROR: INK OUT (0x05)" said the state twice.
void test_printer_condition_names_an_error_once()
{
    std::cout << "[TEST] test_printer_condition_names_an_error_once" << std::endl;

    ewr::PrinterStatus st;
    st.valid = true;
    st.stateName = "ERROR";
    st.hasError = true;
    st.errorCode = 0x05;
    st.errorName = "INK OUT";
    CHECK(ewr::status::Describe(st) == "ERROR: INK OUT (0x05)");

    // A busy printer that also reports an error keeps both.
    st.stateName = "BUSY";
    CHECK(ewr::status::Describe(st) == "BUSY | ERROR: INK OUT (0x05)");

    st.hasError = false;
    st.stateName = "IDLE";
    CHECK(ewr::status::Describe(st) == "IDLE");
}
