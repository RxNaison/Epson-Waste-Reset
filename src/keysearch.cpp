#include "ewr/keysearch.h"
#include "protocol/ctrl.h"
#include "protocol/d4session.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace ewr {

    size_t keysearch::Result::Count(ReadAnswer answer) const
    {
        size_t count = 0;
        for (const keysearch::Probe& probe : probes)
            count += (probe.answer == answer) ? 1 : 0;
        return count;
    }

    std::vector<keysearch::Candidate> keysearch::CollectCandidates(const std::vector<DbPrinterModel>& models)
    {
        std::map<std::pair<uint16_t, uint8_t>, keysearch::Candidate> byKey;
        for (const DbPrinterModel& model : models)
        {
            // A JIG model has no read key; its 0 would only be noise.
            if (model.jig)
                continue;

            keysearch::Candidate& candidate = byKey[{ model.rkey, model.ReadAddressLength() }];
            candidate.rkey = model.rkey;
            candidate.addressLength = model.ReadAddressLength();
            candidate.models.push_back(model.name);
        }

        std::vector<keysearch::Candidate> out;
        for (auto& entry : byKey)
            out.push_back(std::move(entry.second));

        // Ties keep the map's (key, width) order, so the list is stable.
        std::stable_sort(out.begin(), out.end(), [](const keysearch::Candidate& a, const keysearch::Candidate& b) {
            return a.models.size() > b.models.size();
        });

        return out;
    }

    std::vector<keysearch::Candidate> keysearch::AllKeyCandidates(const std::vector<DbPrinterModel>& known)
    {
        constexpr uint8_t kWidth = 2;

        // How many models share each low byte, and which (key, width) pairs the
        // database already holds - those were tried by the 131-key sweep.
        std::array<size_t, 256> weight{};
        std::set<std::pair<uint16_t, uint8_t>> inDb;
        for (const DbPrinterModel& model : known)
        {
            if (model.jig) // a JIG model has no read key.
                continue;
            weight[model.rkey & 0xFF]++;
            inDb.insert({ model.rkey, model.ReadAddressLength() });
        }

        std::vector<int> leadBytes;
        for (int b = 0; b < 256; ++b)
            if (weight[b] > 0)
                leadBytes.push_back(b);
        std::stable_sort(leadBytes.begin(), leadBytes.end(),
                         [&](int a, int b) { return weight[a] > weight[b]; });

        // Low-byte order: each family the database uses, the heaviest first,
        // and immediately with it the bytes either side - a new platform sits
        // beside an established family, so its neighbours are worth more than
        // the next, lighter family. Taking every known byte first instead
        // would bury the neighbours behind ~69 blocks, which is no better than
        // counting up from zero. Then whatever is left.
        std::array<bool, 256> placed{};
        std::vector<int> order;
        auto take = [&](int b) {
            if (b >= 0 && b < 256 && !placed[b])
            {
                placed[b] = true;
                order.push_back(b);
            }
        };

        for (int b : leadBytes)
        {
            take(b);
            take(b - 1);
            take(b + 1);
        }
        for (int b = 0; b < 256; ++b)
            take(b);

        std::vector<keysearch::Candidate> out;
        out.reserve(order.size() * 256);
        for (int lo : order)
            for (int hi = 0; hi < 256; ++hi)
            {
                const uint16_t rkey = static_cast<uint16_t>((hi << 8) | lo);
                if (inDb.count({ rkey, kWidth }) > 0)
                    continue;
                keysearch::Candidate candidate;
                candidate.rkey = rkey;
                candidate.addressLength = kWidth;
                out.push_back(std::move(candidate));
            }

        return out;
    }

    keysearch::Result keysearch::Search(IDeviceGateway& gateway,
                                const std::vector<keysearch::Candidate>& candidates,
                                const ExecutorOptions& options,
                                const std::function<void(size_t, size_t)>& progress,
                                bool stopAtFirstValue)
    {
        // One session per batch. A query session stops at the first exchange
        // that draws no reply, and the candidates after it are never sent.
        constexpr size_t kBatch = 16;

        // An unanswered exchange is not yet "the printer ignored this key":
        // on an SX110 the printer refused the D4 credit four times in one
        // search, so those reads never left - and one of the four keys was
        // the closest sibling's. Such a candidate is asked again in a later
        // pass, and is called silent only after this many tries.
        constexpr int kPasses = 3;

        keysearch::Result search;

        // Asking again inside one session buys nothing, and every retry
        // there costs a fresh session; the passes above do the retrying.
        ExecutorOptions sweep = options;
        sweep.maxWriteAttempts = 1;

        std::vector<keysearch::Probe> probes(candidates.size());
        std::vector<bool> answered(candidates.size(), false);
        size_t resolved = 0;

        std::vector<size_t> pending(candidates.size());
        for (size_t i = 0; i < pending.size(); ++i)
            pending[i] = i;

        bool foundValue = false;

        for (int pass = 1; pass <= kPasses && !pending.empty() && !foundValue; ++pass)
        {
            std::vector<size_t> unanswered;

            size_t next = 0;
            while (next < pending.size())
            {
                const size_t end = std::min(pending.size(), next + kBatch);

                std::vector<std::vector<unsigned char>> queries;
                for (size_t slot = next; slot < end; ++slot)
                {
                    const keysearch::Candidate& candidate = candidates[pending[slot]];
                    queries.push_back(ctrl::ReadPacket(candidate.rkey, 0, candidate.addressLength));
                }

                const QueryRunResult run = gateway.RunQuery(d4::Handshake(), queries, sweep);

                if (!run.deviceFound)
                {
                    search.error = run.query.error.empty() ? "No Epson printer answered." : run.query.error;
                    break;
                }

                search.deviceFound = true;

                if (run.query.handshakeFailed)
                {
                    search.error = run.query.error;
                    break;
                }

                // Replies are positional. The first empty one is the exchange
                // the session stopped at; everything after it was never sent
                // and stays in this pass.
                size_t consumed = 0;
                for (size_t slot = next; slot < end; ++slot)
                {
                    const size_t index = pending[slot];
                    const size_t position = slot - next;
                    const std::vector<unsigned char> reply =
                        position < run.query.replies.size() ? run.query.replies[position]
                                                            : std::vector<unsigned char>{};
                    consumed++;

                    if (reply.empty())
                    {
                        unanswered.push_back(index);
                        break;
                    }

                    probes[index].candidate = candidates[index];
                    probes[index].answer = ctrl::ClassifyReadReply(reply, probes[index].value, 0);
                    answered[index] = true;
                    resolved++;

                    if (stopAtFirstValue && probes[index].answer == ReadAnswer::Value)
                    {
                        foundValue = true;
                        break;
                    }
                }

                next += consumed;

                if (progress)
                    progress(resolved, candidates.size());

                if (foundValue)
                    break;
            }

            if (!search.error.empty())
                break;

            pending = std::move(unanswered);
        }

        // Stopped early, on an error or the first key that answered: report
        // only what was actually tried, in candidate order.
        if (!search.error.empty() || foundValue)
        {
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (answered[i])
                    search.probes.push_back(std::move(probes[i]));
            }
            search.completed = foundValue;
            return search;
        }

        // Still unanswered after every pass.
        for (size_t index : pending)
        {
            probes[index].candidate = candidates[index];
            probes[index].answer = ReadAnswer::Silent;
        }

        search.probes = std::move(probes);
        search.completed = true;

        if (progress)
            progress(candidates.size(), candidates.size());

        return search;
    }

} // namespace ewr
