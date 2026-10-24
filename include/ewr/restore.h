#pragma once
#include "ewr/backup.h"
#include "ewr/generator.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ewr {

    // A byte of the file a restore leaves alone, and why. The reasons are
    // contract strings ("skipped" in the JSON result): never reworded.
    struct RestoreSkip
    {
        uint16_t address = 0;
        // "unmapped"     - not a byte this model's reset writes
        // "unread"       - the file has no value for it ('--', null)
        // "above_limit"  - above the model's mem_high
        // "unencodable"  - beyond what its address field can name
        std::string reason;
    };

    // What a restore of `source` onto one model may write. Only the bytes
    // the database already writes for that model - the waste and cartridge
    // ink counters - so a restore is never more of a write than a reset of
    // the same model: same addresses, values the printer itself once held.
    struct RestorePlan
    {
        std::string model;   // the database entry the plan was made for
        std::string serial;  // the file's, empty when it did not say
        std::vector<std::pair<uint16_t, uint8_t>> writes; // file order
        std::vector<RestoreSkip> skipped;
        bool touchesWaste = false;
        bool touchesInk = false;
        // A pre-1.6 dump read only up to 0xFF: the model's bytes above it
        // that the file could not hold. Listed so a partial restore says so.
        std::vector<uint16_t> missing;

        // A full restore (PlanRestoreAll) writes bytes no reset ever writes,
        // so it runs only on the very unit the dump came from: the serial
        // both name, or else these - the dump's own serial and head-ID bytes
        // - all read back unchanged from the printer.
        bool requireSameUnit = false;
        std::vector<std::pair<uint16_t, uint8_t>> unitProof;
    };

    RestorePlan PlanRestore(const DbPrinterModel& model, const Backup& source);

    // Fewer proof bytes than this prove nothing about which unit it is.
    constexpr size_t kMinimumUnitProof = 4;

    // Every byte of a dump, not only the mapped ones: the expert restore
    // (--restore-all). Never the serial, head-ID, USB-ID or MAC bytes
    // ("identity"), never past mem_high or the address field. Not for a
    // backup, which holds only what a reset wrote: --restore takes those.
    RestorePlan PlanRestoreAll(const DbPrinterModel& model, const Backup& dump);

    // What --dump reads, in address order: 0x00 up to mem_high or 0xFF (the
    // classic range), then every byte the database names for the model above
    // that - its counters, ink bytes and commit flag - which a dump made
    // before 1.6 stopped short of. `full` is all of 0..mem_high. A model with
    // a 1-byte read address cannot name anything above 0xFF, so it never gets
    // one: the read would land on the address's low byte instead.
    std::vector<uint16_t> DumpAddresses(const DbPrinterModel& model, bool full = false);

} // namespace ewr
