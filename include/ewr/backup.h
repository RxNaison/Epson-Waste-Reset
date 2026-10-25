#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ewr {

    // The EEPROM bytes a write is about to overwrite, as the preflight read
    // them, kept so the write can be undone. Also what a --dump file reads
    // back as, which is how a restore takes either.
    struct Backup
    {
        // "backup" for a file EWR wrote before a write, "dump" for a --dump.
        std::string source = "backup";
        int dumpFormat = 0; // 1 or 2 for a dump, 0 for a backup

        std::string model;
        std::string target;     // "waste", "ink", "restore", ... ; empty for a dump
        std::string created;    // UTC, ISO 8601; empty when a v1 dump did not say
        std::string ewrVersion;
        std::string transport;  // "usb" or "network"; empty when unknown
        std::string host;       // the network address, empty over USB
        std::string deviceId;
        std::string serial;     // empty when the printer did not report one
        uint8_t addressLength = 0; // 0 when the file did not say

        // Address -> byte, -1 for a byte the printer did not answer.
        std::vector<std::pair<uint16_t, int>> bytes;

        size_t Answered() const;
        bool Complete() const { return !bytes.empty() && Answered() == bytes.size(); }
    };

} // namespace ewr

// Backup files: written before every write, read back for a restore.
namespace ewr::backup {

    std::string ToJson(const Backup& backup);

    // An EWR backup file (JSON) or an EWR --dump file (text, either format).
    // False with `error` set when it is neither, or names no model.
    bool Parse(const std::string& text, Backup& out, std::string& error);

    // "<model>_<serial or unknown>_<YYYYMMDDTHHMMSSZ>.json", every character
    // outside [A-Za-z0-9-] turned into '_'.
    std::string FileName(const Backup& backup);

    // Writes into `dir` (created if missing) through a temporary file and a
    // rename, so a run killed mid-write leaves no half backup that looks
    // whole. Never overwrites: a name already taken gets a numeric suffix.
    bool WriteFile(const Backup& backup, const std::string& dir,
                   std::string& pathOut, std::string& error);

} // namespace ewr::backup
