#include "ewr/session.h"

#include <cstdio>

namespace ewr {

    namespace {

        // These are the states EWR exists to fix and they still accept EEPROM
        // writes. Foreign locks (ink out, jam, open cover) are the ones the
        // firmware answers with ':42:NA;'.
        constexpr int kServiceRequest = 0x10;
        constexpr int kCartridgeOverflow = 0x2C;

        // A foreign lock for the waste reset, the precondition for the ink one.
        constexpr int kInkOut = 0x05;

        // Busy firmware can omit active error entries entirely: an R220 with
        // an empty cartridge reports INK OUT when idle but a clean BUSY status
        // during warm-up.
        constexpr int kStateBusy = 0x02;

    } // namespace

    bool IsExpectedWastePadError(int errorCode)
    {
        return errorCode == kServiceRequest || errorCode == kCartridgeOverflow;
    }

    std::optional<Blocker> EvaluateBlocker(const PrinterStatus& status)
    {
        // A clean-looking BUSY report is not proof there is no blocker, so
        // surface it rather than treating it as a green light.
        if (status.valid && !status.hasError && status.stateCode == kStateBusy)
        {
            Blocker blocker;
            blocker.errorName = "PRINTER BUSY";
            blocker.explanation =
                "The printer is busy (initializing, printing or cleaning), and busy printers\n"
                "    can omit active errors (e.g. INK OUT) from their status report. Waiting\n"
                "    until the printer is idle and running EWR again gives a trustworthy check.";
            return blocker;
        }

        if (!status.valid || !status.hasError)
            return std::nullopt;

        if (IsExpectedWastePadError(status.errorCode))
            return std::nullopt;

        Blocker blocker;
        blocker.errorName = status.errorName;
        blocker.errorCode = status.errorCode;
        blocker.explanation = "Printers locked by an active " + status.errorName +
                              " error usually refuse factory EEPROM writes (reply ':42:NA;'). "
                              "Clear the error first, then run the reset again.";
        return blocker;
    }

    std::optional<Blocker> EvaluateInkBlocker(const PrinterStatus& status)
    {
        // One more expected state: an empty cartridge cannot gate the very
        // reset that clears it.
        if (status.valid && status.hasError && status.errorCode == kInkOut)
            return std::nullopt;

        return EvaluateBlocker(status);
    }

    // ------------------------------------------------------------------
    //  UsbDeviceGateway
    // ------------------------------------------------------------------

    bool UsbDeviceGateway::NextCallAppends()
    {
        const bool append = m_traceStarted;
        m_traceStarted = true;
        return append;
    }

    DeviceIdQueryResult UsbDeviceGateway::QueryDeviceId()
    {
        return QueryPrinterDeviceId(NextCallAppends());
    }

    std::vector<InterfaceInfo> UsbDeviceGateway::ListInterfaces()
    {
        return ListPrinterInterfaces(NextCallAppends());
    }

    QueryRunResult UsbDeviceGateway::RunQuery(
        const std::vector<std::vector<unsigned char>>& handshake,
        const std::vector<std::vector<unsigned char>>& queries,
        const ExecutorOptions& options)
    {
        return ExecuteQuerySessionWithFallback(handshake, queries, options, NextCallAppends());
    }

    ResetRunResult UsbDeviceGateway::RunReset(
        const std::vector<std::vector<unsigned char>>& sequence,
        const ExecutorOptions& options)
    {
        return ExecutePayloadSequenceWithFallback(sequence, options, NextCallAppends());
    }

    // ------------------------------------------------------------------
    //  Model-free status read
    // ------------------------------------------------------------------

    StateSnapshot ReadPrinterStatus(IDeviceGateway& gateway, const ExecutorOptions& options)
    {
        StateSnapshot out;

        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(UniversalGenerator::GenerateStatusQueryPacket());

        const QueryRunResult run =
            gateway.RunQuery(UniversalGenerator::GenerateHandshake(), queries, options);

        if (!run.deviceFound || run.query.handshakeFailed || run.query.replies.empty())
            return out;

        out.available = true;
        out.status = ParseStatusReply(run.query.replies[0]);
        return out;
    }

    // ------------------------------------------------------------------
    //  Session
    // ------------------------------------------------------------------

    Session::Session(const DbPrinterModel& model, IDeviceGateway& gateway,
                     log::Reporter& reporter, ExecutorOptions queryOptions)
        : m_model(model)
        , m_gateway(gateway)
        , m_reporter(reporter)
        , m_queryOptions(std::move(queryOptions))
    {
    }

    StateSnapshot Session::ReadState() const
    {
        return ReadStateFor(nullptr);
    }

    StateSnapshot Session::ReadAddresses(const std::vector<uint16_t>& addresses) const
    {
        return ReadStateFor(&addresses);
    }

    // `addressOverride` reads an explicit address list instead of the model's
    // own counter bytes, which is what the commit step needs.
    StateSnapshot Session::ReadStateFor(const std::vector<uint16_t>* addressOverride) const
    {
        StateSnapshot out;

        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(UniversalGenerator::GenerateStatusQueryPacket());

        // A schema 4 counter can need a byte that is never written (a nibble
        // shared by two pads), hence the union rather than the write list.
        std::vector<uint16_t> addresses;
        const std::vector<uint16_t> wanted =
            addressOverride ? *addressOverride : m_model.GetReadAddresses();

        for (uint16_t addr : wanted)
        {
            if (addr <= m_model.mem_high)
            {
                addresses.push_back(addr);
                queries.push_back(UniversalGenerator::GenerateReadPacket(
                    m_model.rkey, addr, m_model.ReadAddressLength()));
            }
        }

        const QueryRunResult run =
            m_gateway.RunQuery(UniversalGenerator::GenerateHandshake(), queries, m_queryOptions);

        if (!run.deviceFound || run.query.handshakeFailed || run.query.replies.empty())
            return out;

        out.available = true;
        out.status = ParseStatusReply(run.query.replies[0]);

        for (size_t i = 0; i < addresses.size(); ++i)
        {
            int value = -1;
            if (i + 1 < run.query.replies.size())
            {
                uint8_t v = 0;
                if (ParseEepromReadReply(run.query.replies[i + 1], v, static_cast<int>(addresses[i])))
                    value = v;
            }
            out.values.push_back({ addresses[i], value });
        }

        return out;
    }

    // Generated sequences have a known shape, so every protocol safeguard is
    // on. Replay dumps, whose shape is unknown, get none of them.
    ExecutorOptions Session::BuildWriteOptions() const
    {
        ExecutorOptions options = m_queryOptions;
        options.validateHandshake = true;
        options.resendCreditOnRetry = true;
        options.verifyWrites = true;
        options.useSessionLayer = true;
        options.writeKey = m_model.wkey;
        options.alternateWriteKey = m_model.wkey1;

        // Carry the model's RCMODE channel into the write session. Empty and
        // inert for models that need no recovery step.
        if (m_model.HasRecoveryChannel())
        {
            options.recoveryService = m_model.recovery.service;
            options.recoveryEnter = m_model.recovery.enter;
            options.recoveryClose = m_model.recovery.close;
            options.recoveryReply = m_model.recovery.reply;
        }

        return options;
    }

    // The shared handshake plus the credit pair every EPSON-CTRL packet needs.
    std::vector<std::vector<unsigned char>> Session::BuildWriteSession(
        const std::vector<std::pair<uint16_t, uint8_t>>& writes) const
    {
        auto sequence = UniversalGenerator::GenerateHandshake();

        for (const auto& w : writes)
        {
            sequence.push_back(UniversalGenerator::CreditGrantPacket());
            sequence.push_back(UniversalGenerator::CreditRequestPacket());
            sequence.push_back(UniversalGenerator::GenerateWritePacket(
                m_model.rkey, w.first, w.second, m_model.wkey, m_model.WriteAddressLength()));
        }

        return sequence;
    }

    // Read a flag byte, apply the model's mask, write it back. Some firmwares
    // only latch the new counter values once that happens; without it they
    // restore the old ones on the next power cycle, which looks exactly like a
    // reset that silently did nothing.
    bool Session::RunCommit(const ExecutorOptions& writeOptions)
    {
        std::vector<uint16_t> addresses;
        for (const auto& op : m_model.close_ops)
        {
            bool seen = false;
            for (uint16_t known : addresses)
                seen = seen || (known == op.address);

            if (!seen && op.address <= m_model.mem_high && m_model.CanEncodeWriteAddress(op.address))
                addresses.push_back(op.address);
        }

        if (addresses.empty())
            return true;

        m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.commit",
                       "\n[*] Commit step: latching the new counter values...");

        const StateSnapshot current = ReadStateFor(&addresses);
        if (!current.available)
        {
            m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.commit_skipped",
                           "[!] Commit step skipped: the printer did not answer the read query.");
            return false;
        }

        std::vector<std::pair<uint16_t, uint8_t>> writes;
        for (const auto& op : m_model.close_ops)
        {
            int raw = -1;
            for (const auto& entry : current.values)
            {
                if (entry.first == op.address)
                {
                    raw = entry.second;
                    break;
                }
            }

            if (raw < 0)
            {
                char line[96];
                snprintf(line, sizeof(line), "[!] Commit step skipped: EEPROM 0x%04X did not answer.", op.address);
                m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.commit_skipped", line);
                return false;
            }

            const uint8_t updated = op.Apply(static_cast<uint8_t>(raw));
            if (updated != static_cast<uint8_t>(raw))
                writes.push_back({ op.address, updated });
        }

        if (writes.empty())
        {
            m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.commit_noop",
                           "[i] Commit step: the flag byte already holds the committed value.");
            return true;
        }

        const ResetRunResult run = m_gateway.RunReset(BuildWriteSession(writes), writeOptions);

        if (run.deviceFound && run.exec.success)
        {
            m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.committed",
                           "[SUCCESS] Commit step confirmed by the printer.");
            return true;
        }

        // The counters are already written, so a refusal here is worth
        // reporting but must not fail the run.
        m_reporter.Log(log::Level::Info, log::Stage::Commit, "session.commit_failed",
                       "[!] Commit step did not complete (" +
                           (run.exec.error.empty() ? std::string("no acknowledgement") : run.exec.error) +
                           ").\n    If the counters come back after a power cycle, please report it at:\n"
                           "    https://github.com/RxNaison/Epson-Waste-Reset/issues");
        return false;
    }

    ResetOutcome Session::Reset(const ResetHandlers& handlers)
    {
        // ---- Plan: build the write sequence from the database model.
        m_reporter.Log(log::Level::Info, log::Stage::Database, "session.generating",
                       "[*] Generating safe Smart Protocol R/W sequence for " + m_model.name + "...");

        UniversalGenerator generator;

        ResetFlow flow;
        flow.sequence = generator.GenerateSequence(m_model);
        flow.verifyAddresses = m_model.GetAllAddresses();
        flow.verifyValues = m_model.GetAllResetValues();
        flow.classify = [](const PrinterStatus& status) { return EvaluateBlocker(status); };
        flow.expectedErrorNote = " - that is the waste-pad error EWR resets. Proceeding.";
        flow.commit = m_model.HasCloseOps();

        return RunLifecycle(flow, handlers);
    }

    ResetOutcome Session::ResetInk(const ResetHandlers& handlers)
    {
        ResetOutcome out;

        // Structured refusal so a host can render it without parsing logs.
        if (!m_model.HasInkReset())
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "session.ink_unmapped",
                           "[ERROR] " + m_model.name + " has no cartridge ink map in the database.");
            out.error = "No cartridge ink map for this model.";
            return out;
        }

        m_reporter.Log(log::Level::Info, log::Stage::Database, "session.ink_generating",
                       "[*] Generating cartridge ink reset writes for " + m_model.name + "...");

        const std::vector<uint16_t> addresses = m_model.GetInkAddresses();
        const std::vector<uint8_t> resets = m_model.GetInkResetValues();

        // Same encoding guards the commit step applies to its own writes.
        std::vector<std::pair<uint16_t, uint8_t>> writes;
        for (size_t i = 0; i < addresses.size() && i < resets.size(); ++i)
        {
            if (addresses[i] <= m_model.mem_high && m_model.CanEncodeWriteAddress(addresses[i]))
                writes.push_back({ addresses[i], resets[i] });
        }

        if (writes.empty())
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "session.ink_unwritable",
                           "[ERROR] None of the ink counter addresses are writable on " + m_model.name + ".");
            out.error = "The model's ink counter addresses cannot be encoded for writing.";
            return out;
        }

        if (writes.size() < addresses.size())
            m_reporter.Log(log::Level::Warning, log::Stage::Database, "session.ink_partial",
                           "[!] " + std::to_string(addresses.size() - writes.size()) +
                               " ink counter address(es) fall outside this model's writable range and were skipped.");

        std::vector<uint16_t> writeAddresses;
        std::vector<uint8_t> writeValues;
        for (const auto& w : writes)
        {
            writeAddresses.push_back(w.first);
            writeValues.push_back(w.second);
        }

        ResetFlow flow;
        flow.sequence = BuildWriteSession(writes);
        flow.readAddresses = writeAddresses;
        flow.verifyAddresses = writeAddresses;
        flow.verifyValues = writeValues;
        flow.classify = [](const PrinterStatus& status) { return EvaluateInkBlocker(status); };
        flow.expectedErrorNote = " - an empty cartridge is exactly the state this reset clears. Proceeding.";
        flow.counterNoun = "ink counter";
        flow.recheckCommand = "'ewr --dump'";
        flow.commit = false; // persistence comes from the power cycle, not a close op
        flow.requirePreflight = true; // the unproven ink write path never runs blind

        ResetOutcome result = RunLifecycle(flow, handlers);

        if (result.success)
        {
            m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.ink_power_cycle",
                           "\n[i] Power cycle the printer now: OFF, wait ~10 seconds, ON. Then verify\n"
                           "    with --status or --dump. If the counters return to their old values,\n"
                           "    this model mirrors ink levels from the cartridge chips - a USB EEPROM\n"
                           "    reset cannot hold on it. Please report the model either way at:\n"
                           "    https://github.com/RxNaison/Epson-Waste-Reset/issues");
        }

        return result;
    }

    ResetOutcome Session::RunLifecycle(const ResetFlow& flow, const ResetHandlers& handlers)
    {
        ResetOutcome out;

        // ---- Preflight. A printer locked by a foreign error refuses writes
        // with ':42:NA;', so surface that up front rather than failing halfway.
        m_reporter.Log(log::Level::Info, log::Stage::Read, "session.preflight",
                       "\n[*] Preflight: reading printer status before writing (read-only)...");

        out.before = flow.readAddresses.has_value() ? ReadStateFor(&flow.readAddresses.value())
                                                     : ReadState();
        const bool preflightRan = out.before.available;

        if (preflightRan)
        {
            if (handlers.onPreflight)
                handlers.onPreflight(out.before);

            const std::optional<Blocker> blocker = flow.classify
                ? flow.classify(out.before.status)
                : EvaluateBlocker(out.before.status);
            if (blocker.has_value())
            {
                const bool proceed = handlers.onBlocker && handlers.onBlocker(*blocker);
                if (!proceed)
                {
                    m_reporter.Log(log::Level::Info, log::Stage::Read, "session.aborted",
                                   "[i] Aborted before any EEPROM write. Nothing was changed.");
                    out.phase = ResetPhase::Aborted;
                    out.error = "Aborted on an active " + blocker->errorName + " error.";
                    return out;
                }
            }
            else if (out.before.status.valid && out.before.status.hasError)
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.expected_error",
                               "\n[i] The printer reports " + out.before.status.errorName +
                                   flow.expectedErrorNote);
            }
        }
        else if (flow.requirePreflight)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Read, "session.preflight_required",
                           "[ERROR] The printer did not answer the read-only preflight, and this reset\n"
                           "        does not run blind: no preflight means no error check and no\n"
                           "        read-back verification. Nothing was written.");
            out.phase = ResetPhase::Aborted;
            out.error = "Preflight unavailable; this reset does not run without one.";
            return out;
        }
        else
        {
            m_reporter.Log(log::Level::Info, log::Stage::Read, "session.preflight_unavailable",
                           "[i] Status preflight unavailable on this printer - continuing with the reset.");
        }

        // ---- Database trust gate. The build flags a model when independent
        // sources disagree about its write path. Same policy as a status
        // blocker: no decision callback means no writes.
        if (m_model.conflict)
        {
            m_reporter.Log(log::Level::Warning, log::Stage::Database, "session.db_conflict",
                           "[!] The database flags " + m_model.name +
                               ": independent sources disagree about its write path.");

            Blocker conflict;
            conflict.errorName = "DATABASE CONFLICT";
            conflict.explanation =
                "Independent sources disagree about this model's EEPROM write path, so the\n"
                "    shipped values may be wrong for some units.\n"
                "    If you proceed and the reset verifies cleanly, please report it at:\n"
                "    https://github.com/RxNaison/Epson-Waste-Reset/issues";

            const bool proceed = handlers.onBlocker && handlers.onBlocker(conflict);
            if (!proceed)
            {
                m_reporter.Log(log::Level::Info, log::Stage::Database, "session.aborted",
                               "[i] Aborted before any EEPROM write. Nothing was changed.");
                out.phase = ResetPhase::Aborted;
                out.error = "Aborted on a database conflict: sources disagree about this model's write path.";
                return out;
            }
        }

        // ---- Ask once, after every conditional gate has had its say. Those
        // only fire when a blocker is visible and a BUSY preflight can hide
        // one, so this hook always fires when wired.
        if (handlers.confirmWrite && !handlers.confirmWrite(out.before))
        {
            m_reporter.Log(log::Level::Info, log::Stage::Read, "session.declined",
                           "[i] Aborted before any EEPROM write. Nothing was changed.");
            out.phase = ResetPhase::Aborted;
            out.error = "The reset was declined at the final confirmation.";
            return out;
        }

        m_reporter.Log(log::Level::Info, log::Stage::Detect, "session.scanning",
                       "Scanning USB ports for Epson device...");

        const ExecutorOptions writeOptions = BuildWriteOptions();
        const ResetRunResult run = m_gateway.RunReset(flow.sequence, writeOptions);

        if (!run.deviceFound)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Detect, "session.device_not_found",
                           "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?");
            out.phase = ResetPhase::DeviceNotFound;
            out.error = "No Epson USB interface answered.";
            return out;
        }

        out.alternateKeyUsed = run.exec.alternateKeyUsed;

        // Surfacing this turns a silent recovery into a reportable DB fix.
        if (run.exec.alternateKeyUsed)
        {
            m_reporter.Log(log::Level::Info, log::Stage::Write, "session.alternate_key",
                           "\n[i] This printer rejected the database's primary write keyword and accepted the\n"
                           "    alternate one ('wkey1'). Please report this model at:\n"
                           "    https://github.com/RxNaison/Epson-Waste-Reset/issues");
        }

        if (!run.exec.success)
        {
            out.phase = ResetPhase::WriteFailed;
            out.error = run.exec.error;
            return out;
        }

        out.success = true;

        // ---- Commit. On some firmwares the counter bytes are not final
        // until the flag byte is rewritten afterwards.
        if (flow.commit)
            out.committed = RunCommit(writeOptions);

        // ---- Read-back verification.
        if (preflightRan)
        {
            m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verifying",
                           "\n[*] Verifying the reset by reading the counters back...");

            out.after = flow.readAddresses.has_value() ? ReadStateFor(&flow.readAddresses.value())
                                                       : ReadState();

            if (out.after.available && !out.after.values.empty())
            {
                out.verificationRan = true;

                if (handlers.onVerify)
                    handlers.onVerify(out.after);

                const std::vector<uint16_t>& addrs = flow.verifyAddresses;
                const std::vector<uint8_t>& resets = flow.verifyValues;

                for (const auto& entry : out.after.values)
                {
                    int expected = -1;
                    for (size_t k = 0; k < addrs.size() && k < resets.size(); ++k)
                    {
                        if (addrs[k] == entry.first)
                        {
                            expected = resets[k];
                            break;
                        }
                    }

                    if (entry.second < 0)
                        out.verifyUnread++;
                    else if (expected >= 0 && entry.second != expected)
                        out.verifyMismatches++;
                }

                if (out.verifyMismatches == 0 && out.verifyUnread == 0)
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verified",
                                   "[SUCCESS] Read-back verification: every " + flow.counterNoun +
                                       " now holds its reset value.");
                else if (out.verifyMismatches > 0)
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verify_mismatch",
                                   "[!] Read-back verification: " + std::to_string(out.verifyMismatches) +
                                       " " + flow.counterNoun + "(s) do not match their reset value. Power-cycle the printer and run " +
                                       flow.recheckCommand + " to re-check.");
                else
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verify_incomplete",
                                   "[i] Read-back verification incomplete: " + std::to_string(out.verifyUnread) +
                                       " " + flow.counterNoun + "(s) did not answer.");
            }
            else
            {
                m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verify_unavailable",
                               "[i] Read-back verification unavailable (the printer did not answer the read queries).");
            }
        }

        out.phase = ResetPhase::Done;
        return out;
    }

} // namespace ewr
