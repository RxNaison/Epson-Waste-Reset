#include "cli.h"
#include "color.h"

#include <cstdio>
#include <iostream>

namespace ewr::cli {

    // Only the filled cells take the color, so a bar keeps its width either way.
    static std::string GaugeBar(int percent, const char* color = nullptr)
    {
        if (percent < 0)
            percent = 0;
        if (percent > 100)
            percent = 100;

        const int filled = (percent + 9) / 10; // any non-zero percent shows a cell

        return "[" + ewr::cli::Paint(std::string(filled, '#'), color) + std::string(10 - filled, '-') + "]";
    }

    // A gauge in its own ink. Black keeps the terminal's text color: a black bar
    // vanishes on a dark background.
    static const char* InkColor(const std::string& name)
    {
        using namespace ewr::cli::ansi;
        if (name == "Cyan")          return kDarkCyan;
        if (name == "Light Cyan")    return kCyan;
        if (name == "Magenta")       return kDarkMagenta;
        if (name == "Light Magenta") return kMagenta;
        if (name == "Yellow")        return kYellow;
        if (name == "Dark Yellow" || name == "Orange") return kDarkYellow;
        if (name == "Red")           return kRed;
        if (name == "Blue")          return kBlue;
        if (name == "Gray" || name == "Light Gray" || name == "Light Black") return kGray;
        return nullptr;
    }

    static const char* LevelWordColor(const std::string& word)
    {
        using namespace ewr::cli::ansi;
        if (word == "OK")      return kGreen;
        if (word == "UNKNOWN") return kGray;
        if (word == "EMPTY" || word == "MISSING" || word == "FAIL") return kRed;
        return nullptr;
    }

    // How full a pad or maintenance box is, in the same shade as its bar.
    static std::string FillPercent(int percent)
    {
        char text[16];
        snprintf(text, sizeof(text), "%3d%%", percent);
        return ewr::cli::Paint(text, ewr::cli::RampColor(percent));
    }

    static const char* ConditionColor(const ewr::PrinterStatus& st)
    {
        using namespace ewr::cli::ansi;
        if (st.hasError)            return kRed;
        if (st.stateName == "IDLE") return kGreen;
        return kYellow;
    }

    void PrintPrinterStatus(const ewr::PrinterStatus& st)
    {
        if (!st.valid)
        {
            std::cout << "[i] The printer answered, but the status report could not be parsed." << std::endl;
            return;
        }

        std::cout << "\n----------- PRINTER STATUS -----------" << std::endl;
        std::cout << "  State:  " << ewr::cli::Paint(ewr::status::Describe(st), ConditionColor(st)) << std::endl;

        if (!st.serial.empty())
            std::cout << "  Serial: " << st.serial << std::endl;

        if (!st.inks.empty())
        {
            std::cout << "  Ink levels:" << std::endl;
            for (const auto& ink : st.inks)
            {
                const std::string word = ewr::cli::Paint(ink.statusText, LevelWordColor(ink.statusText));
                char line[192];
                if (ink.level >= 0)
                    snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s",
                             ink.colorName.c_str(), GaugeBar(ink.level, InkColor(ink.colorName)).c_str(),
                             ink.level, word.c_str());
                else
                    snprintf(line, sizeof(line), "    %-14.14s %s",
                             ink.colorName.c_str(), word.c_str());
                std::cout << line << std::endl;
            }
        }

        if (st.maintenanceBoxLevel >= 0 || !st.maintenanceBoxText.empty())
        {
            const std::string word = ewr::cli::Paint(st.maintenanceBoxText, LevelWordColor(st.maintenanceBoxText));
            char line[192];
            if (st.maintenanceBoxLevel >= 0)
                snprintf(line, sizeof(line), "    %-14.14s %s %s  %s", "Maint. box",
                         GaugeBar(st.maintenanceBoxLevel, ewr::cli::RampColor(st.maintenanceBoxLevel)).c_str(),
                         FillPercent(st.maintenanceBoxLevel).c_str(), word.c_str());
            else
                snprintf(line, sizeof(line), "    %-14.14s %s", "Maint. box", word.c_str());
            std::cout << line << std::endl;
        }

        std::cout << "--------------------------------------" << std::endl;
    }

    void PrintCounterValues(const std::vector<std::pair<uint16_t, int>>& values, const char* label)
    {
        if (values.empty())
            return;

        std::cout << "  " << label << std::endl;
        for (const auto& entry : values)
        {
            char line[64];
            if (entry.second >= 0)
                snprintf(line, sizeof(line), "    EEPROM 0x%04X = 0x%02X (%d)", entry.first, entry.second, entry.second);
            else
                snprintf(line, sizeof(line), "    EEPROM 0x%04X = (no reply)", entry.first);
            std::cout << line << std::endl;
        }
    }

    // Raw EEPROM bytes to "how full is this pad", via the per-counter masks,
    // weights and service limits carried by the database.
    void PrintCounterSummary(const ewr::DbPrinterModel& model,
                                    const std::vector<std::pair<uint16_t, int>>& values)
    {
        const auto specs = model.GetAllCounters();
        if (specs.empty() || values.empty())
            return;

        bool printedHeader = false;
        for (const auto& spec : specs)
        {
            const ewr::CounterReading reading = spec.Evaluate(values);
            if (!reading.complete)
                continue;

            if (!printedHeader)
            {
                std::cout << "  Waste pad usage:" << std::endl;
                printedHeader = true;
            }

            const std::string name = reading.description.empty() ? "Counter" : reading.description;
            const int percent = reading.Percent();

            char line[256];
            if (percent >= 0)
            {
                const std::string limit = (percent >= 100)
                    ? ewr::cli::Paint("  <-- service limit reached", ewr::cli::ansi::kRed)
                    : std::string();
                snprintf(line, sizeof(line), "    %-28.28s %s %s  (%u / %u)%s",
                         name.c_str(), GaugeBar(percent, ewr::cli::RampColor(percent)).c_str(),
                         FillPercent(percent).c_str(),
                         static_cast<unsigned>(reading.value),
                         static_cast<unsigned>(reading.max_value),
                         limit.c_str());
            }
            else
            {
                snprintf(line, sizeof(line), "    %-28.28s value %u (no service limit on record)",
                         name.c_str(), static_cast<unsigned>(reading.value));
            }
            std::cout << line << std::endl;
        }
    }

    // The gauges above show only pads with a counter, and 809 of the 1337
    // resettable models also reset a pad without one - an R220's platen pad, an
    // L6490's only pad - so without this a person sees one pad, or none, on a
    // printer where a reset clears more (#42). Same list as `reset_covers`.
    void PrintResetCoverage(const ewr::DbPrinterModel& model)
    {
        const std::vector<ewr::PadCoverage> covers = model.GetResetCoverage();
        if (covers.empty())
            return;

        std::cout << "  A waste pad reset clears:" << std::endl;
        for (const auto& pad : covers)
        {
            const std::string name = pad.name.empty() ? std::string("Waste pad") : pad.name;
            char line[200];
            snprintf(line, sizeof(line), "    %-28.28s %s", name.c_str(),
                     pad.readable ? "has a counter"
                                  : "no counter - only the printer's own error says when it is full");
            std::cout << line << std::endl;
        }
    }

    void PrintNetworkSearchHints()
    {
        std::cout << "    A printer on the network has to be on, on this computer's network, and have\n"
                     "    SNMP enabled in its web settings. A VPN, a firewall, or a system setting that\n"
                     "    keeps programs off the local network (on macOS, possibly Local Network under\n"
                     "    Privacy & Security) can stop the search reaching it. Its address is on its\n"
                     "    network status sheet: ewr --ip <address> goes straight to it." << std::endl;
    }

} // namespace ewr::cli
