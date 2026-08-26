#include "ewr/deviceid.h"
#include <algorithm>
#include <cctype>

namespace ewr {

    namespace {

        std::string Trim(const std::string& s)
        {
            size_t b = 0;
            size_t e = s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
            return s.substr(b, e - b);
        }

        std::string ToUpper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        // Uppercases, collapses punctuation runs into single spaces and drops
        // the marketing noise (an "EPSON" prefix, a "SERIES" suffix) so that
        // "EPSON ET-2800 Series" and "ET-2800" both normalize to "ET 2800".
        std::string NormalizeModelName(const std::string& name)
        {
            const std::string upper = ToUpper(name);

            std::string collapsed;
            bool lastWasSpace = true;
            for (char c : upper)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                {
                    collapsed += c;
                    lastWasSpace = false;
                }
                else if (!lastWasSpace)
                {
                    collapsed += ' ';
                    lastWasSpace = true;
                }
            }
            collapsed = Trim(collapsed);

            const std::string prefix = "EPSON ";
            if (collapsed.rfind(prefix, 0) == 0)
                collapsed = collapsed.substr(prefix.size());

            const std::string suffix = " SERIES";
            if (collapsed.size() > suffix.size()
                && collapsed.compare(collapsed.size() - suffix.size(), suffix.size(), suffix) == 0)
                collapsed = collapsed.substr(0, collapsed.size() - suffix.size());

            return Trim(collapsed);
        }

        bool ContainsWholeWords(const std::string& haystack, const std::string& needle)
        {
            const std::string h = " " + haystack + " ";
            const std::string n = " " + needle + " ";
            return h.find(n) != std::string::npos;
        }

    } // namespace

    std::string ExtractDeviceIdString(const unsigned char* data, size_t length)
    {
        if (!data || length == 0)
            return "";

        // IEEE 1284 replies start with a two-byte big-endian length prefix.
        // Realistic IDs are far below 8 KiB, so a prefixed reply always starts
        // with a non-printable byte; a printable first byte means the driver
        // already stripped the prefix.
        size_t start = 0;
        if (length >= 2 && data[0] < 0x20)
            start = 2;

        std::string out;
        out.reserve(length - start);
        for (size_t i = start; i < length; ++i)
        {
            const unsigned char c = data[i];
            if (c == '\0')
                break;
            if (c >= 0x20 && c < 0x7F)
                out += static_cast<char>(c);
        }

        return Trim(out);
    }

    DeviceIdInfo ParseIeee1284DeviceId(const std::string& deviceId)
    {
        DeviceIdInfo info;
        info.raw = Trim(deviceId);

        size_t pos = 0;
        while (pos < info.raw.size())
        {
            size_t sep = info.raw.find(';', pos);
            if (sep == std::string::npos)
                sep = info.raw.size();

            const std::string field = info.raw.substr(pos, sep - pos);
            pos = sep + 1;

            const size_t colon = field.find(':');
            if (colon == std::string::npos)
                continue;

            const std::string key = ToUpper(Trim(field.substr(0, colon)));
            const std::string value = Trim(field.substr(colon + 1));

            if (key == "MFG" || key == "MANUFACTURER")
                info.manufacturer = value;
            else if (key == "MDL" || key == "MODEL")
                info.model = value;
            else if (key == "CMD" || key == "COMMAND SET")
                info.commandSet = value;
            else if (key == "DES" || key == "DESCRIPTION")
                info.description = value;
        }

        return info;
    }

    std::vector<std::string> MatchModelEntries(const std::string& mdlField,
                                               const std::vector<ModelNameEntry>& entries)
    {
        const std::string mdlNorm = NormalizeModelName(mdlField);
        if (mdlNorm.empty())
            return {};

        // Tiers: 0 own-name exact, 1 alias exact, 2 own-name whole-word
        // partial, 3 alias whole-word partial. Within the partial tiers a
        // longer matched candidate is more specific ("XP-2200" beats "XP").
        struct Ranked
        {
            std::string name;
            int tier{4};
            size_t matchedLen{0};
        };

        std::vector<Ranked> ranked;

        for (const auto& entry : entries)
        {
            Ranked best;
            best.name = entry.name;

            auto consider = [&](const std::string& candidate, bool isAlias)
            {
                const std::string norm = NormalizeModelName(candidate);
                if (norm.empty())
                    return;

                int tier;
                if (norm == mdlNorm)
                    tier = isAlias ? 1 : 0;
                else if (ContainsWholeWords(mdlNorm, norm))
                    tier = isAlias ? 3 : 2;
                else
                    return;

                if (tier < best.tier || (tier == best.tier && norm.size() > best.matchedLen))
                {
                    best.tier = tier;
                    best.matchedLen = norm.size();
                }
            };

            consider(entry.name, false);
            for (const std::string& alias : entry.aliases)
                consider(alias, true);

            if (best.tier < 4)
                ranked.push_back(best);
        }

        std::stable_sort(ranked.begin(), ranked.end(),
            [](const Ranked& a, const Ranked& b)
            {
                if (a.tier != b.tier)
                    return a.tier < b.tier;
                if (a.tier >= 2 && a.matchedLen != b.matchedLen)
                    return a.matchedLen > b.matchedLen;
                return false;
            });

        std::vector<std::string> out;
        out.reserve(ranked.size());
        for (const auto& r : ranked)
            out.push_back(r.name);

        return out;
    }

    std::vector<std::string> MatchModelNames(const std::string& mdlField,
                                             const std::vector<std::string>& knownModels)
    {
        std::vector<ModelNameEntry> entries;
        entries.reserve(knownModels.size());
        for (const std::string& name : knownModels)
            entries.push_back({ name, {} });

        return MatchModelEntries(mdlField, entries);
    }

} // namespace ewr
