#include "ewr/session.h"
#include "ewr/gates.h"
#include "ewr/format.h"
#include "ewr/status.h"
#include "ewr/restore.h"
#include "protocol/ctrl.h"
#include "protocol/d4session.h"
#include "ewr/deviceid.h"
#include "ewr/version.h"
#include "protocol/jig.h"

#include <algorithm>
#include <map>

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

        // `replies` is padded one-for-one with the queries, an empty vector
        // standing in for every query that drew no answer - so a non-empty
        // `replies` means only that queries were sent. A snapshot is evidence
        // about the printer only once something actually came back.
        bool AnyReplyArrived(const std::vector<std::vector<unsigned char>>& replies)
        {
            for (const auto& reply : replies)
            {
                if (!reply.empty())
                    return true;
            }

            return false;
        }

    } // namespace

    bool gates::IsExpectedWastePadError(int errorCode)
    {
        return errorCode == kServiceRequest || errorCode == kCartridgeOverflow;
    }

    std::optional<Blocker> gates::Evaluate(const PrinterStatus& status)
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

        // Same reasoning for a report that stopped mid-field: the error entry
        // may be one of the fields that never arrived. An incomplete report
        // without an error is not a report without an error.
        if (status.valid && status.truncated && !status.hasError)
        {
            Blocker blocker;
            blocker.errorName = "INCOMPLETE STATUS REPORT";
            blocker.explanation =
                "The printer's status report ended mid-field, so the error entry may simply\n"
                "    be part of what never arrived. Running EWR again - on the interface that\n"
                "    answers cleanly - gives a trustworthy check.";
            return blocker;
        }

        if (!status.valid || !status.hasError)
            return std::nullopt;

        if (gates::IsExpectedWastePadError(status.errorCode))
            return std::nullopt;

        Blocker blocker;
        blocker.errorName = status.errorName;
        blocker.errorCode = status.errorCode;
        blocker.explanation = "Printers with an active " + status.errorName +
                              " error usually refuse the reset.\n"
                              "    Clear the error first, then run EWR again.";
        return blocker;
    }

    std::optional<Blocker> gates::EvaluateInk(const PrinterStatus& status)
    {
        // One more expected state: an empty cartridge cannot gate the very
        // reset that clears it.
        if (status.valid && status.hasError && status.errorCode == kInkOut)
            return std::nullopt;

        return gates::Evaluate(status);
    }

    // ------------------------------------------------------------------
    //  Model-free status read
    // ------------------------------------------------------------------

    StateSnapshot status::Read(IDeviceGateway& gateway, const ExecutorOptions& options)
    {
        StateSnapshot out;

        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(ctrl::StatusQueryPacket());

        const QueryRunResult run =
            gateway.RunQuery(d4::Handshake(), queries, options);

        if (!run.deviceFound || run.query.handshakeFailed || !AnyReplyArrived(run.query.replies))
            return out;

        out.available = true;
        out.status = ctrl::ParseStatusReply(run.query.replies[0]);
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
        // A host may say more (how the entry was chosen); none may say less.
        if (m_queryOptions.trace.selectedEntry.empty())
            m_queryOptions.trace.selectedEntry = m_model.name;
    }

    StateSnapshot Session::ReadState() const
    {
        return ReadStateFor(nullptr);
    }

    namespace {

        // Below this a range read saves less than its probe costs.
        constexpr size_t kRangeReadMinimum = 24;
        // One reply carries 2 hex digits per byte: 32 keeps it well inside
        // the 256-byte packets the session negotiates.
        constexpr uint8_t kRangeChunk = 32;
        constexpr uint8_t kRangeProbe = 8;

    } // namespace

    StateSnapshot Session::ReadRanges(const std::vector<std::pair<uint16_t, uint8_t>>& chunks,
                                      std::vector<bool>& parsed) const
    {
        StateSnapshot out;
        parsed.assign(chunks.size(), false);

        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(ctrl::StatusQueryPacket());
        for (const auto& chunk : chunks)
            queries.push_back(ctrl::RangeReadPacket(
                m_model.rkey, chunk.first, chunk.second, m_model.ReadAddressLength()));

        const QueryRunResult run =
            m_gateway.RunQuery(d4::Handshake(), queries, m_queryOptions);

        if (!run.deviceFound || run.query.handshakeFailed || !AnyReplyArrived(run.query.replies))
            return out;

        out.available = true;
        out.status = ctrl::ParseStatusReply(run.query.replies[0]);

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            if (i + 1 >= run.query.replies.size())
                break;

            std::vector<uint8_t> bytes;
            if (!ctrl::ParseRangeReadReply(run.query.replies[i + 1], chunks[i].first, chunks[i].second, bytes))
                continue;

            parsed[i] = true;
            for (size_t k = 0; k < bytes.size(); ++k)
                out.values.push_back({ static_cast<uint16_t>(chunks[i].first + k), bytes[k] });
        }

        return out;
    }

    StateSnapshot Session::ReadAddresses(const std::vector<uint16_t>& addresses) const
    {
        std::vector<uint16_t> wanted;
        for (uint16_t addr : addresses)
        {
            if (addr <= m_model.mem_high)
                wanted.push_back(addr);
        }

        // A JIG model has no key for a range read to carry.
        if (m_model.jig || m_rangeReads == RangeReads::Refused || wanted.size() < kRangeReadMinimum)
            return ReadStateFor(&addresses);

        // Runs of consecutive addresses, each cut to what one reply carries.
        std::vector<std::pair<uint16_t, uint8_t>> chunks;
        for (size_t i = 0; i < wanted.size();)
        {
            size_t j = i + 1;
            while (j < wanted.size() && wanted[j] == wanted[j - 1] + 1 && j - i < kRangeChunk)
                ++j;
            chunks.push_back({ wanted[i], static_cast<uint8_t>(j - i) });
            i = j;
        }

        if (m_rangeReads == RangeReads::Unknown)
        {
            // The same bytes both ways. Anything but full agreement - a
            // refusal, a reply of another shape, a different value - and this
            // session reads byte by byte.
            const uint16_t start = chunks.front().first;
            const uint8_t count = std::min<uint8_t>(chunks.front().second, kRangeProbe);

            std::vector<uint16_t> probe;
            for (uint8_t k = 0; k < count; ++k)
                probe.push_back(static_cast<uint16_t>(start + k));

            std::vector<bool> parsed;
            const StateSnapshot ranged = ReadRanges({ { start, count } }, parsed);
            const StateSnapshot single = ReadStateFor(&probe);

            bool agree = ranged.available && single.available && parsed[0]
                         && ranged.values.size() == single.values.size();
            for (size_t k = 0; agree && k < single.values.size(); ++k)
                agree = single.values[k].second >= 0 && single.values[k] == ranged.values[k];

            m_rangeReads = agree ? RangeReads::Work : RangeReads::Refused;

            m_reporter.Log(log::Level::Info, log::Stage::Read, agree ? "session.range_reads" : "session.single_reads",
                           agree ? "[i] The printer answers range reads: reading " + std::to_string(wanted.size()) +
                                       " bytes in " + std::to_string(chunks.size()) + " request(s)."
                                 : std::string("[i] Reading byte by byte: this printer does not answer range reads"
                                               " the way it answers single ones."),
                           { { "bytes", std::to_string(wanted.size()) }, { "requests", std::to_string(chunks.size()) } });

            if (!agree)
                return ReadStateFor(&addresses);
        }

        std::vector<bool> parsed;
        StateSnapshot out = ReadRanges(chunks, parsed);
        if (!out.available)
            return ReadStateFor(&addresses);

        // A chunk that did not come back as values is read byte by byte, and
        // ends range reads for the session: one odd reply is enough doubt.
        std::vector<uint16_t> leftover;
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            if (parsed[i])
                continue;
            for (uint8_t k = 0; k < chunks[i].second; ++k)
                leftover.push_back(static_cast<uint16_t>(chunks[i].first + k));
        }

        if (!leftover.empty())
        {
            m_rangeReads = RangeReads::Refused;
            const StateSnapshot single = ReadStateFor(&leftover);
            out.values.insert(out.values.end(), single.values.begin(), single.values.end());
        }

        // The caller's order, -1 for anything that never answered.
        StateSnapshot ordered;
        ordered.available = true;
        ordered.status = out.status;
        for (uint16_t addr : wanted)
        {
            int value = -1;
            for (const auto& entry : out.values)
            {
                if (entry.first == addr)
                {
                    value = entry.second;
                    break;
                }
            }
            ordered.values.push_back({ addr, value });
        }

        return ordered;
    }

    // `addressOverride` reads an explicit address list instead of the model's
    // own counter bytes, which is what the commit step needs.
    StateSnapshot Session::ReadStateFor(const std::vector<uint16_t>* addressOverride) const
    {
        if (m_model.jig)
            return ReadJigStateFor(addressOverride);

        StateSnapshot out;

        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(ctrl::StatusQueryPacket());

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
                queries.push_back(ctrl::ReadPacket(
                    m_model.rkey, addr, m_model.ReadAddressLength()));
            }
        }

        const QueryRunResult run =
            m_gateway.RunQuery(d4::Handshake(), queries, m_queryOptions);

        if (!run.deviceFound || run.query.handshakeFailed || !AnyReplyArrived(run.query.replies))
            return out;

        out.available = true;
        out.status = ctrl::ParseStatusReply(run.query.replies[0]);

        for (size_t i = 0; i < addresses.size(); ++i)
        {
            int value = -1;
            if (i + 1 < run.query.replies.size())
            {
                uint8_t v = 0;
                if (ctrl::ParseReadReply(run.query.replies[i + 1], v, static_cast<int>(addresses[i])))
                    value = v;
            }
            out.values.push_back({ addresses[i], value });
        }

        return out;
    }

    StateSnapshot Session::ReadJigStateFor(const std::vector<uint16_t>* addressOverride) const
    {
        StateSnapshot out = status::Read(m_gateway, m_queryOptions);
        if (!out.available)
            return out;

        std::vector<uint16_t> addresses;
        std::vector<std::vector<unsigned char>> queries;
        for (uint16_t addr : addressOverride ? *addressOverride : m_model.GetReadAddresses())
        {
            if (addr <= m_model.mem_high)
            {
                addresses.push_back(addr);
                queries.push_back(jig::BuildReadPacket(addr));
            }
        }

        if (queries.empty())
            return out;

        ExecutorOptions jigOptions = m_queryOptions;
        jigOptions.jigMode = true;
        const QueryRunResult run = m_gateway.RunQuery({}, queries, jigOptions);

        for (size_t i = 0; i < addresses.size(); ++i)
        {
            uint8_t value = 0;
            const bool read = i < run.query.replies.size() && jig::ParseReadReply(run.query.replies[i], value);
            out.values.push_back({ addresses[i], read ? static_cast<int>(value) : -1 });
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

        return options;
    }

    // The shared handshake plus the credit pair every EPSON-CTRL packet needs.
    std::vector<std::vector<unsigned char>> Session::BuildWriteSession(
        const std::vector<std::pair<uint16_t, uint8_t>>& writes) const
    {
        auto sequence = d4::Handshake();

        for (const auto& w : writes)
        {
            sequence.push_back(d4::CreditGrantPacket());
            sequence.push_back(d4::CreditRequestPacket());
            sequence.push_back(ctrl::WritePacket(
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

        ResetFlow flow;
        flow.sequence = ctrl::ResetSequence(m_model);
        flow.target = "waste";

        // What ctrl::ResetSequence writes: the same two guards, the same order.
        for (uint16_t addr : m_model.GetAllAddresses())
        {
            if (addr <= m_model.mem_high && m_model.CanEncodeWriteAddress(addr))
                flow.writeAddresses.push_back(addr);
        }

        flow.verifyAddresses = m_model.GetAllAddresses();
        flow.verifyValues = m_model.GetAllResetValues();
        flow.classify = [](const PrinterStatus& status) { return gates::Evaluate(status); };
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
        flow.writeAddresses = writeAddresses;
        flow.target = "ink";
        flow.readAddresses = writeAddresses;
        flow.verifyAddresses = writeAddresses;
        flow.verifyValues = writeValues;
        flow.classify = [](const PrinterStatus& status) { return gates::EvaluateInk(status); };
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

    ResetOutcome Session::Restore(const RestorePlan& plan, const ResetHandlers& handlers)
    {
        ResetOutcome out;

        if (plan.model != m_model.name)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "session.restore_wrong_model",
                           "[ERROR] The restore was planned for " + plan.model + ", not " + m_model.name + ".");
            out.error = "The restore was planned for " + plan.model + ", not " + m_model.name + ".";
            return out;
        }

        if (plan.writes.empty())
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "session.restore_empty",
                           "[ERROR] The file holds none of the bytes EWR writes on " + m_model.name +
                               ", so there is nothing it could restore.");
            out.error = "The file holds none of the bytes EWR writes on " + m_model.name + ".";
            return out;
        }

        // As the ink reset itself: it has never been sent over the network.
        if (plan.touchesInk && m_gateway.OverNetwork())
        {
            m_reporter.Log(log::Level::Error, log::Stage::Database, "session.restore_ink_needs_usb",
                           "[!] The file holds cartridge ink bytes, and writing those needs USB. Nothing was written.");
            out.phase = ResetPhase::NotSupported;
            out.error = "Restoring cartridge ink bytes needs USB.";
            return out;
        }

        m_reporter.Log(log::Level::Info, log::Stage::Database, "session.restore_generating",
                       "[*] Planning the restore of " + std::to_string(plan.writes.size()) +
                           " byte(s) on " + m_model.name + "...");

        ResetFlow flow;
        flow.target = "restore";

        // The counters' own bytes too, so the host can show what the restore
        // does to each pad, not only which bytes it writes - and for a full
        // restore the bytes that prove which unit this is.
        std::vector<uint16_t> reads = m_model.GetReadAddresses();
        auto addRead = [&reads](uint16_t address) {
            if (std::find(reads.begin(), reads.end(), address) == reads.end())
                reads.push_back(address);
        };
        for (const auto& w : plan.writes)
            addRead(w.first);
        for (const auto& p : plan.unitProof)
            addRead(p.first);
        flow.readAddresses = reads;

        const bool wasteBytes = plan.touchesWaste;
        flow.classify = [wasteBytes](const PrinterStatus& status) {
            return wasteBytes ? gates::Evaluate(status) : gates::EvaluateInk(status);
        };
        flow.expectedErrorNote = " - the restore does not need it cleared. Proceeding.";
        flow.counterNoun = "byte";
        flow.expectedValueNoun = "restored value";
        flow.runNoun = "restore";
        flow.recheckCommand = "'ewr --dump'";
        flow.commit = wasteBytes && m_model.HasCloseOps();
        flow.requirePreflight = true; // it has to know what it is about to change

        flow.afterPreflight = [this, &plan](const StateSnapshot& before, ResetFlow& f, ResetOutcome& o,
                                            const ResetHandlers& h) -> bool
        {
            const std::string& printerSerial = before.status.serial;

            // A full restore writes bytes no reset writes. No gate can be
            // argued past this one: it runs on the unit the dump came from
            // or not at all.
            if (plan.requireSameUnit)
            {
                std::string unproven;
                if (!plan.serial.empty() && !printerSerial.empty())
                {
                    if (plan.serial != printerSerial)
                        unproven = "the dump names serial " + plan.serial + " and this printer reports " + printerSerial;
                }
                else if (plan.unitProof.size() < restore::kMinimumUnitProof)
                {
                    unproven = "neither a serial number nor enough of the printer's own identity bytes are on"
                               " record to show it is the unit the dump came from";
                }
                else
                {
                    for (const auto& p : plan.unitProof)
                    {
                        int now = -1;
                        for (const auto& entry : before.values)
                        {
                            if (entry.first == p.first)
                                now = entry.second;
                        }
                        if (now != p.second)
                        {
                            char where[8];
                            snprintf(where, sizeof(where), "0x%04X", p.first);
                            unproven = std::string("identity byte ") + where + " differs from the dump's";
                            break;
                        }
                    }
                }

                if (!unproven.empty())
                {
                    m_reporter.Log(log::Level::Error, log::Stage::Read, "session.restore_all_other_unit",
                                   "[!] Nothing was written: " + unproven + ". A full restore runs only on the\n"
                                   "    printer the dump was taken from.");
                    o.phase = ResetPhase::NotSupported;
                    o.error = "Nothing was written: " + unproven + ".";
                    return false;
                }

                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.restore_unit_confirmed",
                               (!plan.serial.empty() && !printerSerial.empty())
                                   ? "[i] Same printer: the serial number matches the dump's."
                                   : "[i] Same printer: all " + std::to_string(plan.unitProof.size()) +
                                         " identity bytes match the dump's.");
            }

            if (plan.requireSameUnit)
            {
                // Proven above; the softer checks below are for --restore.
            }
            else if (!plan.serial.empty() && !printerSerial.empty() && plan.serial != printerSerial)
            {
                m_reporter.Log(log::Level::Warning, log::Stage::Read, "session.restore_other_printer",
                               "[!] The file was taken from the printer with serial " + plan.serial +
                                   ", and this one reports " + printerSerial + ".",
                               { { "file_serial", plan.serial }, { "printer_serial", printerSerial } });

                Blocker other;
                other.errorName = "DIFFERENT PRINTER";
                other.explanation =
                    "These are another printer's counter values. Writing them here does not\n"
                    "    restore this printer's own state; it gives it the other one's.";

                if (!(h.onBlocker && h.onBlocker(other)))
                {
                    m_reporter.Log(log::Level::Info, log::Stage::Read, "session.aborted",
                                   "[i] Aborted before any EEPROM write. Nothing was changed.");
                    o.phase = ResetPhase::Aborted;
                    o.error = "Aborted: the file comes from another printer (serial " + plan.serial + ").";
                    return false;
                }

                o.overrides.push_back({ "unit_mismatch", plan.serial, -1 });
            }
            else if (plan.serial.empty() || printerSerial.empty())
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.restore_unit_unknown",
                               std::string("[i] EWR cannot confirm this is the printer the file came from: ") +
                                   (plan.serial.empty() ? "the file" : "the printer") + " names no serial number.");
            }

            std::vector<std::pair<uint16_t, uint8_t>> writes;
            for (const auto& w : plan.writes)
            {
                int now = -1;
                for (const auto& entry : before.values)
                {
                    if (entry.first == w.first)
                    {
                        now = entry.second;
                        break;
                    }
                }

                if (now == w.second)
                    o.unchanged.push_back(w.first);
                else
                    writes.push_back(w);
            }

            if (writes.empty())
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.restore_unchanged",
                               "[i] Nothing to restore: every byte already holds the file's value. Nothing was written.");
                o.phase = ResetPhase::Done;
                o.success = true;
                return false;
            }

            f.sequence = BuildWriteSession(writes);
            f.writeAddresses.clear();
            f.verifyAddresses.clear();
            f.verifyValues.clear();
            for (const auto& w : writes)
            {
                f.writeAddresses.push_back(w.first);
                f.verifyAddresses.push_back(w.first);
                f.verifyValues.push_back(w.second);
            }

            return true;
        };

        return RunLifecycle(flow, handlers);
    }

    bool Session::KeepBackup(const ResetFlow& flow, const ResetHandlers& handlers, ResetOutcome& out)
    {
        Backup& backup = out.backup;
        backup.model = m_model.name;
        backup.target = flow.target;
        backup.created = format::UtcTimestamp();
        backup.ewrVersion = EWR_VERSION;
        backup.transport = m_gateway.OverNetwork() ? "network" : "usb";
        backup.serial = out.before.status.serial;
        backup.addressLength = m_model.WriteAddressLength();

        for (uint16_t addr : flow.writeAddresses)
        {
            bool seen = false;
            for (const auto& entry : backup.bytes)
                seen = seen || (entry.first == addr);
            if (seen)
                continue;

            int value = -1;
            for (const auto& entry : out.before.values)
            {
                if (entry.first == addr)
                {
                    value = entry.second;
                    break;
                }
            }
            backup.bytes.push_back({ addr, value });
        }

        out.backupTaken = true;

        if (!handlers.saveBackup)
            return true;

        const std::string answered = std::to_string(backup.Answered());
        const std::string total = std::to_string(backup.bytes.size());

        // Same policy as a printer error: nobody to ask means no write.
        auto askToGoOn = [&](const char* name, const std::string& explanation, const char* detail) -> bool
        {
            Blocker blocker;
            blocker.errorName = name;
            blocker.explanation = explanation;

            if (!(handlers.onBlocker && handlers.onBlocker(blocker)))
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.aborted",
                               "[i] Aborted before any EEPROM write. Nothing was changed.");
                out.phase = ResetPhase::Aborted;
                out.error = std::string("Aborted: ") + (std::string(detail) == "incomplete"
                    ? "the backup is incomplete." : "the backup could not be saved.");
                return false;
            }

            out.overrides.push_back({ "backup", detail, -1 });
            return true;
        };

        if (!backup.Complete())
        {
            m_reporter.Log(log::Level::Warning, log::Stage::Read, "session.backup_incomplete",
                           "[!] The preflight answered " + answered + " of the " + total +
                               " byte(s) this run overwrites, so a backup could not undo all of it.",
                           { { "answered", answered }, { "total", total } });

            if (!askToGoOn("BACKUP INCOMPLETE",
                           "Running EWR again usually reads every byte; a backup that is missing some\n"
                           "    cannot put them back if this write has to be undone.",
                           "incomplete"))
                return false;
        }

        std::string location;
        if (handlers.saveBackup(backup, location))
        {
            out.backupLocation = location;
            m_reporter.Log(log::Level::Info, log::Stage::Read, "session.backup_saved",
                           "[*] Backup of the " + total + " byte(s) this run overwrites: " +
                               (location.empty() ? std::string("kept by the host") : location),
                           { { "file", location }, { "bytes", total } });
            return true;
        }

        m_reporter.Log(log::Level::Warning, log::Stage::Read, "session.backup_failed",
                       "[!] The backup of the " + total + " byte(s) this run overwrites could not be saved.",
                       { { "bytes", total } });

        return askToGoOn("BACKUP NOT SAVED",
                         "Without it this write cannot be undone. Free some disk space, or run EWR from\n"
                         "    a folder it can write to, and try again.",
                         "not_saved");
    }

    ResetOutcome Session::RunLifecycle(ResetFlow flow, const ResetHandlers& handlers)
    {
        ResetOutcome out;

        // No JIG write command is known. Refused before anything is read or
        // sent, so no host can carry a keyless model into the keyed writes.
        if (m_model.jig)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Write, "session.jig_reset_unsupported",
                           "[!] " + m_model.name + " is read in JIG mode, and EWR cannot reset it yet:"
                           " its write command is not known. Nothing was sent.");
            out.error = m_model.name + " cannot be reset yet: no JIG write command is known.";
            return out;
        }

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

            // No gate can be argued past this one: a write that cannot be read
            // back cannot be verified, and the gateway has seen the reads fail.
            const std::string unreachable = m_gateway.EepromUnreachable();
            if (!unreachable.empty())
            {
                m_reporter.Log(log::Level::Error, log::Stage::Read, "session.eeprom_unreachable",
                               "[!] Nothing was written: " + unreachable);
                out.phase = ResetPhase::NotSupported;
                out.error = "Nothing was written: " + unreachable;
                return out;
            }

            const std::optional<Blocker> blocker = flow.classify
                ? flow.classify(out.before.status)
                : gates::Evaluate(out.before.status);
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

                out.overrides.push_back({ "blocker", blocker->errorName, blocker->errorCode });
            }
            else if (out.before.status.valid && out.before.status.hasError)
            {
                m_reporter.Log(log::Level::Info, log::Stage::Read, "session.expected_error",
                               "\n[i] The printer reports " + out.before.status.errorName +
                                   flow.expectedErrorNote);
            }

            if (flow.afterPreflight && !flow.afterPreflight(out.before, flow, out, handlers))
                return out;
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

            out.overrides.push_back({ "db_conflict", "", -1 });
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

        // ---- Backup, after every gate and before the first write: a run
        // that stopped at a gate leaves no file behind.
        if (preflightRan)
        {
            if (!KeepBackup(flow, handlers, out))
                return out;
        }
        else
        {
            m_reporter.Log(log::Level::Info, log::Stage::Read, "session.backup_unavailable",
                           "[i] No backup: the printer did not answer the preflight, so there are no\n"
                           "    values to keep. The reset goes ahead as it always has.");
        }

        const bool overUsb = !m_gateway.OverNetwork();

        m_reporter.Log(log::Level::Info, log::Stage::Detect, "session.scanning",
                       overUsb ? "Scanning USB ports for Epson device..."
                               : "Sending the reset to the printer over the network...");

        const ExecutorOptions writeOptions = BuildWriteOptions();
        const ResetRunResult run = m_gateway.RunReset(flow.sequence, writeOptions);

        if (!run.deviceFound)
        {
            m_reporter.Log(log::Level::Error, log::Stage::Detect, "session.device_not_found",
                           overUsb ? "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?"
                                   : "[ERROR] The printer stopped answering on the network.");
            out.phase = ResetPhase::DeviceNotFound;
            out.error = overUsb ? "No Epson USB interface answered."
                                : (run.exec.error.empty() ? "The printer did not answer on the network." : run.exec.error);
            return out;
        }

        out.alternateKeyUsed = run.exec.alternateKeyUsed;
        out.writesVerified = run.exec.writesVerified;
        out.writesTotal = run.exec.writesTotal;

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
                           "\n[*] Verifying the " + flow.runNoun + " by reading the " + flow.counterNoun + "s back...");

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
                                       " now holds its " + flow.expectedValueNoun + ".");
                else if (out.verifyMismatches > 0)
                    m_reporter.Log(log::Level::Info, log::Stage::Verify, "session.verify_mismatch",
                                   "[!] Read-back verification: " + std::to_string(out.verifyMismatches) +
                                       " " + flow.counterNoun + "(s) do not match their " + flow.expectedValueNoun + ". Power-cycle the printer and run " +
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
