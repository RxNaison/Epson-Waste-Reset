#include "ewr/discover.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace ewr {

    namespace {

        // -1 (no reply) anywhere in the series disqualifies the address: a
        // missing read is not a value that moved.
        bool Series(const std::vector<EepromSnapshot>& snapshots, uint16_t addr,
                    std::vector<int>& out)
        {
            out.clear();
            for (const EepromSnapshot& snap : snapshots)
            {
                int value = -1;
                for (const auto& entry : snap)
                {
                    if (entry.first == addr)
                    {
                        value = entry.second;
                        break;
                    }
                }

                if (value < 0)
                    return false;

                out.push_back(value);
            }

            return true;
        }

        // A trend is every interval moving the same way. One flat interval is
        // enough to disqualify: a byte that sat still through a cleaning is not
        // tracking it.
        bool Trend(const std::vector<uint32_t>& values, std::vector<int64_t>& deltas, bool& rising)
        {
            deltas.clear();
            if (values.size() < 3) // two intervals minimum
                return false;

            rising = values[1] > values[0];

            for (std::size_t i = 1; i < values.size(); ++i)
            {
                if (values[i] == values[i - 1])
                    return false;
                if ((values[i] > values[i - 1]) != rising)
                    return false;

                deltas.push_back(static_cast<int64_t>(values[i]) - static_cast<int64_t>(values[i - 1]));
            }

            return true;
        }

    } // namespace

    std::vector<ByteTrend> FindTrendingBytes(const std::vector<EepromSnapshot>& snapshots)
    {
        std::vector<ByteTrend> found;
        if (snapshots.size() < 3)
            return found;

        std::set<uint16_t> addresses;
        for (const EepromSnapshot& snap : snapshots)
        {
            for (const auto& entry : snap)
                addresses.insert(entry.first);
        }

        std::set<uint16_t> claimed;
        std::vector<int> low, high;
        std::vector<uint32_t> values;
        std::vector<int64_t> deltas;
        bool rising = false;

        // Pairs first, so a wrapping low byte is read as part of its counter
        // rather than thrown away for breaking its own run.
        for (uint16_t addr : addresses)
        {
            if (!addresses.count(static_cast<uint16_t>(addr + 1)))
                continue;
            if (!Series(snapshots, addr, low) || !Series(snapshots, static_cast<uint16_t>(addr + 1), high))
                continue;

            values.clear();
            for (std::size_t i = 0; i < low.size(); ++i)
                values.push_back(static_cast<uint32_t>(low[i]) | (static_cast<uint32_t>(high[i]) << 8));

            if (!Trend(values, deltas, rising))
                continue;

            ByteTrend t;
            t.addresses = { addr, static_cast<uint16_t>(addr + 1) };
            t.values = values;
            t.deltas = deltas;
            t.rising = rising;
            found.push_back(t);

            claimed.insert(addr);
            claimed.insert(static_cast<uint16_t>(addr + 1));
        }

        for (uint16_t addr : addresses)
        {
            if (claimed.count(addr) || !Series(snapshots, addr, low))
                continue;

            values.assign(low.begin(), low.end());
            if (!Trend(values, deltas, rising))
                continue;

            ByteTrend t;
            t.addresses = { addr };
            t.values = values;
            t.deltas = deltas;
            t.rising = rising;
            found.push_back(t);
        }

        std::sort(found.begin(), found.end(),
            [](const ByteTrend& a, const ByteTrend& b)
            {
                return a.addresses.front() < b.addresses.front();
            });

        return found;
    }

    std::string FormatDiscoveryJson(const std::string& modelName,
                                    const std::string& modelBodyJson,
                                    const std::vector<ByteTrend>& trends)
    {
        std::ostringstream out;

        out << "{\n";
        out << "    \"schema_version\": 4,\n";
        out << "    \"models\": {\n";
        out << "        \"" << modelName << "\": {\n";
        out << modelBodyJson;

        // Left empty on purpose. Which of the readings below is a waste counter,
        // and what it resets to, is a judgement no dump can make.
        out << "            \"pad_groups\": []\n";
        out << "        }\n";
        out << "    },\n";

        out << "    \"observations\": {\n";
        out << "        \"note\": \"values read before the first cleaning, then after each one."
               " a waste counter only rises; a falling byte is something else. no reset value"
               " is observable this way.\",\n";
        out << "        \"passes\": " << (trends.empty() ? 0 : trends.front().values.size()) << ",\n";
        out << "        \"trending\": [\n";

        for (std::size_t i = 0; i < trends.size(); ++i)
        {
            const ByteTrend& t = trends[i];

            out << "            { \"addresses\": [";
            for (std::size_t a = 0; a < t.addresses.size(); ++a)
                out << (a ? ", " : "") << t.addresses[a];
            out << "], \"pair\": " << (t.IsPair() ? "true" : "false");

            out << ", \"values\": [";
            for (std::size_t v = 0; v < t.values.size(); ++v)
                out << (v ? ", " : "") << t.values[v];
            out << "], \"deltas\": [";
            for (std::size_t d = 0; d < t.deltas.size(); ++d)
                out << (d ? ", " : "") << t.deltas[d];
            out << "], \"direction\": \"" << (t.rising ? "rising" : "falling") << "\" }";

            out << (i + 1 < trends.size() ? "," : "") << "\n";
        }

        out << "        ]\n";
        out << "    }\n";
        out << "}\n";

        return out.str();
    }

} // namespace ewr
