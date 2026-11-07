#include "ewr/restore.h"

#include <algorithm>

namespace ewr {

    RestorePlan restore::Plan(const DbPrinterModel& model, const Backup& source)
    {
        RestorePlan plan;
        plan.model = model.name;
        plan.serial = source.serial;

        const std::vector<uint16_t> waste = model.GetAllAddresses();
        const std::vector<uint16_t> ink = model.GetInkAddresses();

        auto contains = [](const std::vector<uint16_t>& list, uint16_t address) {
            return std::find(list.begin(), list.end(), address) != list.end();
        };

        std::vector<uint16_t> seen;
        for (const auto& entry : source.bytes)
        {
            const uint16_t address = entry.first;
            if (contains(seen, address))
                continue;
            seen.push_back(address);

            const bool isWaste = contains(waste, address);
            const bool isInk = contains(ink, address);

            if (!isWaste && !isInk)
                plan.skipped.push_back({ address, "unmapped" });
            else if (entry.second < 0)
                plan.skipped.push_back({ address, "unread" });
            else if (address > model.mem_high)
                plan.skipped.push_back({ address, "above_limit" });
            else if (!model.CanEncodeWriteAddress(address))
                plan.skipped.push_back({ address, "unencodable" });
            else
            {
                plan.writes.push_back({ address, static_cast<uint8_t>(entry.second) });
                plan.touchesWaste = plan.touchesWaste || isWaste;
                plan.touchesInk = plan.touchesInk || isInk;
            }
        }

        // Bytes the model writes that the file does not hold at all: a dump
        // from before 1.6 stopped at 0xFF. A backup covers only what its own
        // run wrote, so a waste backup is not missing the ink bytes, and a
        // restore's backup is missing nothing.
        const bool dump = source.source != "backup";
        std::vector<uint16_t> expected;
        if (dump || source.target == "waste")
            expected.insert(expected.end(), waste.begin(), waste.end());
        if (dump || source.target == "ink")
            expected.insert(expected.end(), ink.begin(), ink.end());

        for (uint16_t address : expected)
        {
            if (!contains(seen, address) && !contains(plan.missing, address))
                plan.missing.push_back(address);
        }

        return plan;
    }

    RestorePlan restore::PlanAll(const DbPrinterModel& model, const Backup& dump)
    {
        RestorePlan plan;
        plan.model = model.name;
        plan.serial = dump.serial;
        plan.requireSameUnit = true;

        std::vector<uint16_t> identity = model.identity_addresses;
        identity.insert(identity.end(), model.serial_addresses.begin(), model.serial_addresses.end());

        const std::vector<uint16_t> waste = model.GetAllAddresses();
        const std::vector<uint16_t> ink = model.GetInkAddresses();

        auto contains = [](const std::vector<uint16_t>& list, uint16_t address) {
            return std::find(list.begin(), list.end(), address) != list.end();
        };

        std::vector<uint16_t> seen;
        for (const auto& entry : dump.bytes)
        {
            const uint16_t address = entry.first;
            if (contains(seen, address))
                continue;
            seen.push_back(address);

            if (contains(identity, address))
            {
                plan.skipped.push_back({ address, "identity" });
                if (entry.second >= 0)
                    plan.unitProof.push_back({ address, static_cast<uint8_t>(entry.second) });
            }
            else if (entry.second < 0)
                plan.skipped.push_back({ address, "unread" });
            else if (address > model.mem_high)
                plan.skipped.push_back({ address, "above_limit" });
            else if (!model.CanEncodeWriteAddress(address))
                plan.skipped.push_back({ address, "unencodable" });
            else
            {
                plan.writes.push_back({ address, static_cast<uint8_t>(entry.second) });
                plan.touchesWaste = plan.touchesWaste || contains(waste, address);
                plan.touchesInk = plan.touchesInk || contains(ink, address);
            }
        }

        return plan;
    }

    std::vector<uint16_t> restore::DumpAddresses(const DbPrinterModel& model, bool full)
    {
        const uint32_t reachable = (model.ReadAddressLength() == 1)
            ? std::min<uint32_t>(model.mem_high, 0xFF) : model.mem_high;
        const uint32_t end = full ? reachable : std::min<uint32_t>(reachable, 0xFF);

        std::vector<uint16_t> addresses;
        for (uint32_t a = 0; a <= end; ++a)
            addresses.push_back(static_cast<uint16_t>(a));

        if (full)
            return addresses;

        std::vector<uint16_t> named = model.GetReadAddresses();
        const std::vector<uint16_t> ink = model.GetInkAddresses();
        named.insert(named.end(), ink.begin(), ink.end());
        for (const auto& op : model.close_ops)
            named.push_back(op.address);

        std::sort(named.begin(), named.end());
        named.erase(std::unique(named.begin(), named.end()), named.end());

        for (uint16_t address : named)
        {
            if (address > end && address <= reachable)
                addresses.push_back(address);
        }

        return addresses;
    }

} // namespace ewr
