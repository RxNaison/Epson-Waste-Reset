#pragma once
#include <cstddef>
#include <string>
#include <vector>

// IEEE 1284 device IDs, and matching their model names to database entries.
namespace ewr::deviceid {

    struct Info
    {
        std::string raw;          // sanitized raw string
        std::string manufacturer; // MFG / MANUFACTURER
        std::string model;        // MDL / MODEL
        std::string commandSet;   // CMD / COMMAND SET
        std::string description;  // DES / DESCRIPTION
    };

    // Handles the optional two-byte big-endian length prefix, which some
    // drivers strip and some do not, and drops non-printable bytes.
    std::string ExtractString(const unsigned char* data, size_t length);

    // ';' between fields, ':' between key and value. Unknown keys ignored.
    Info Parse(const std::string& deviceId);

    // Best first. An exact match wins, ignoring case, punctuation, an "EPSON"
    // prefix and a "Series" suffix; otherwise database names appearing as whole
    // words inside MDL, longest first ("Stylus Photo R220" matches "R220").
    std::vector<std::string> MatchNames(const std::string& mdlField,
                                        const std::vector<std::string>& knownModels);

    // A model name plus the aliases the database build attached to it.
    struct ModelNameEntry
    {
        std::string name;                 // database key, returned on a match
        std::vector<std::string> aliases; // marketing / family names
    };

    // Like MatchNames, but a hit on any alias counts for its owning entry
    // and each entry appears once. A name match outranks an alias match, so a
    // model never shadows another model's real name.
    std::vector<std::string> MatchEntries(const std::string& mdlField,
                                          const std::vector<ModelNameEntry>& entries);

}
