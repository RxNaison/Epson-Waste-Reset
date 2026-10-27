#include "cli.h"
#include "ewr/format.h"
#include "ewr/log.h"
#include "ewr/paths.h"
#include "ewr/restore.h"
#include "ewr/session.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace ewr::cli {

    // Diffing two dumps taken around a single change locates counters on
    // printers the database has not mapped yet.
    static std::string WriteEepromDump(const ewr::DbPrinterModel& model,
                                       const std::vector<std::pair<uint16_t, int>>& values,
                                       const std::string& deviceId, const std::string& serial)
    {
        std::vector<std::pair<uint16_t, std::string>> notes;
        for (const auto& g : model.pad_groups)
        {
            const std::string label = g.description.empty() ? std::string("waste counter") : g.description;
            for (uint16_t a : g.addresses)
                notes.push_back({ a, label });
        }
        for (const auto& g : model.ink_groups)
        {
            for (uint16_t a : g.addresses)
                notes.push_back({ a, "cartridge ink: " + (g.color.empty() ? std::string("?") : g.color) });
        }

        auto noteFor = [&notes](uint16_t addr) -> std::string {
            for (const auto& n : notes)
            {
                if (n.first == addr)
                    return n.second;
            }
            return "";
        };

        std::string safeName;
        for (char c : model.name)
            safeName += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';

        const std::string base = "ewr_dump_" + safeName + "_" +
            std::to_string(static_cast<long long>(std::time(nullptr)));

        // Epoch granularity: two dumps in the same second must not collide, since
        // diffing #1 against #2 is the entire point.
        std::string filename = ewr::paths::InDataDir(base + ".txt");
        for (int n = 2; fs::exists(filename) && n < 100; ++n)
            filename = ewr::paths::InDataDir(base + "_" + std::to_string(n) + ".txt");

        std::ofstream out(filename);
        if (!out)
            return "";

        size_t answered = 0;
        for (const auto& v : values)
            answered += (v.second >= 0) ? 1 : 0;

        // Format 2 adds the header lines after "model"; a reader of format 1
        // skips '#' lines it does not know, and the body is unchanged, so two
        // dumps of either format still diff line for line.
        out << "# EWR EEPROM dump\n";
        out << "# format: 2\n";
        out << "# model: " << model.name << "\n";
        out << "# ewr: " << kEwrCurrentVersion << "\n";
        out << "# created: " << ewr::format::UtcTimestamp() << "\n";
        if (!deviceId.empty())
            out << "# device_id: " << deviceId << "\n";
        if (!serial.empty())
            out << "# serial: " << serial << "\n";
        out << "# address_length: " << static_cast<int>(model.ReadAddressLength()) << "\n";
        out << "# answered: " << answered << " of " << values.size() << "\n";
        out << "# address,value,note ('--' = no reply: unknown)\n";
        for (const auto& v : values)
        {
            char line[24];
            if (v.second < 0)
                snprintf(line, sizeof(line), "0x%02X,--", v.first);
            else
                snprintf(line, sizeof(line), "0x%02X,0x%02X", v.first, v.second & 0xFF);

            out << line << "," << noteFor(v.first) << "\n";
        }

        return filename;
    }

    // Dump: read a range of the EEPROM to a file, then stop.
    // Needs a Smart Protocol model: the read key lives in the database.
    int RunDump(RunContext& ctx)
    {
        CliOptions& cli = ctx.cli;
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;

        if (selected.isReplay)
        {
            std::cerr << "[!] --dump needs a Smart Protocol model: it reads the EEPROM with the\n"
                         "    database read key, and Replay dumps carry none." << std::endl;
            JsonFail("not_supported", "--dump needs a Smart Protocol model; a Replay dump carries no read key.");
            return FinishRun(1);
        }

        // A dump walks 0x00-0xFF, and JIG memory is only known from 0x100.
        if (selected.smartModel.jig)
        {
            std::cerr << "[!] --dump does not support JIG-mode printers yet. Use --status to read\n"
                         "    the waste counters." << std::endl;
            JsonFail("not_supported", "--dump does not support JIG-mode printers yet.");
            return FinishRun(1);
        }

        std::cout << "\n[*] DUMP for " << selected.smartModel.name
                  << ": reading the EEPROM (read-only, no writes)..." << std::endl;

        // The low page, where the classic models keep everything, plus every
        // byte the database names above it - a restore needs those.
        const std::vector<uint16_t> addresses = ewr::restore::DumpAddresses(selected.smartModel, cli.dumpFull);
        if (cli.dumpFull)
            std::cout << "    --full: all " << addresses.size() << " bytes up to mem_high. This takes a while." << std::endl;

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadAddresses(addresses);

        if (!state.available)
        {
            std::cerr << "[ERROR] Could not read the printer. Is it powered on and connected?" << std::endl;
            std::cerr << "        See ewr_trace.log for the hardware trace." << std::endl;
            JsonFail("read_failed", "The printer did not answer the EEPROM read.");
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);

        size_t answered = 0;
        for (const auto& v : state.values)
            answered += (v.second >= 0) ? 1 : 0;

        g_jsonData["model"] = selected.smartModel.name;
        g_jsonData["answered"] = answered;
        g_jsonData["total"] = state.values.size();
        g_jsonData["values"] = ewr::json::CounterValues(state.values);
        g_jsonData["file"] = nullptr;

        const std::string path = WriteEepromDump(selected.smartModel, state.values, ctx.deviceId,
                                                 state.status.serial);
        if (path.empty())
        {
            std::cerr << "[ERROR] Read the EEPROM, but could not write the dump file." << std::endl;
            JsonFail("io_error", "Read the EEPROM, but could not write the dump file.");
            return FinishRun(1);
        }

        g_jsonData["file"] = path;

        // A diff reads '--' on both sides as "unchanged" when it means
        // "unknown", so a short dump must not look like a backup (#39).
        if (answered < state.values.size())
        {
            std::cerr << "\n[ERROR] Incomplete dump: the printer answered " << answered << " of "
                      << state.values.size() << " EEPROM byte(s). Saved to " << path << ",\n"
                      << "        but it is NOT a backup and must not be diffed: a '--' line is a\n"
                      << "        byte that was never read." << std::endl;
            if (answered == 0)
                std::cerr << "        No byte answered at all: the read key is likely wrong for this\n"
                             "        printer - try a sibling model from the same line." << std::endl;
            else
                std::cerr << "        Run --dump again; if it comes back short twice, power-cycle the\n"
                             "        printer first. See ewr_trace.log for the hardware trace." << std::endl;

            JsonFail("incomplete_dump", "The printer answered " + std::to_string(answered) + " of "
                                        + std::to_string(state.values.size()) + " EEPROM bytes.");
            return FinishRun(1);
        }

        std::cout << "\n[SUCCESS] Wrote " << answered << " of " << state.values.size()
                  << " EEPROM byte(s) to " << path << "." << std::endl;
        std::cout << "    To map a color: dump once, print that color (or swap its cartridge),\n"
                     "    dump again, then diff the two files. The bytes that changed are that\n"
                     "    color's ink counter.\n"
                     "    Mind the mirror trap: a byte that comes back by itself after a power\n"
                     "    cycle is rewritten by the firmware from the cartridge chip - that level\n"
                     "    lives on the chip and cannot be reset from the PC." << std::endl;
        return FinishRun(0);
    }

} // namespace ewr::cli
