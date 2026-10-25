#pragma once
#include "ewr/database.h"
#include "ewr/gateway.h"
#include "ewr/status.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Read-key search: for a printer the database has no entry for.
namespace ewr::keysearch {

    // One read key to try, with the address width its models use. The same
    // key at the other width is a different candidate: the width changes the
    // packet, so a printer can refuse one and answer the other.
    struct Candidate
    {
        uint16_t rkey = 0;
        uint8_t addressLength = 2;
        std::vector<std::string> models; // database entries that use it
    };

    struct Probe
    {
        Candidate candidate;
        ReadAnswer answer = ReadAnswer::Silent;
        uint8_t value = 0; // the byte at address 0, when answer is Value
    };

    struct Result
    {
        bool deviceFound = false;
        // False when the search stopped before every candidate was tried;
        // `error` says why. `probes` holds the ones that were.
        bool completed = false;
        std::vector<Probe> probes;
        std::string error;

        size_t Count(ReadAnswer answer) const;
    };

    // Every distinct (read key, address width) in `models`, the most widely
    // shared first: a key many entries use is the likeliest to fit a sibling.
    std::vector<Candidate> CollectCandidates(const std::vector<DbPrinterModel>& models);

    // Reads address 0 once with each candidate's key. Read commands only: a
    // key gates access, it cannot turn a read into a write. `progress` is
    // called with (tried, total) as the search advances.
    Result Search(IDeviceGateway& gateway,
                  const std::vector<Candidate>& candidates,
                  const ExecutorOptions& options = ExecutorOptions::ForQueries(),
                  const std::function<void(size_t, size_t)>& progress = {});

} // namespace ewr::keysearch
