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

    // Every 2-byte read key the database does not already carry at
    // `addressLength`, ordered so the likeliest fit comes first. The key's low
    // byte is sent first and marks a platform family (every modern ET/L/WF key
    // shares one of a few low bytes), so the low bytes `known` uses lead, most
    // models first, then their byte-neighbours - a new family sits beside the
    // last one (0x404C followed 0x???B keys) - then the rest. For a printer no
    // entry matches and whose own key the 131-key `CollectCandidates` sweep
    // did not find; read-only, like `Search`.
    //
    // `addressLength` is the address field's width, not the key's - the key is
    // always 2 bytes. Both widths need a pass: a 1-byte model refuses every
    // 2-byte-address read whatever key it holds, so a width-2-only sweep can
    // never find its key (#73). Discovery reads address 0, which both widths
    // encode; the narrow field only limits which counters a later reset could
    // reach.
    std::vector<Candidate> AllKeyCandidates(const std::vector<DbPrinterModel>& known,
                                            uint8_t addressLength = 2);

    // Reads address 0 once with each candidate's key. Read commands only: a
    // key gates access, it cannot turn a read into a write. `progress` is
    // called with (tried, total) as the search advances. `stopAtFirstValue`
    // ends the search the moment a key answers with data - for the full-key
    // sweep, where trying every remaining key after a hit is wasted time;
    // `probes` then holds only the keys actually tried.
    Result Search(IDeviceGateway& gateway,
                  const std::vector<Candidate>& candidates,
                  const ExecutorOptions& options = ExecutorOptions::ForQueries(),
                  const std::function<void(size_t, size_t)>& progress = {},
                  bool stopAtFirstValue = false);

} // namespace ewr::keysearch
