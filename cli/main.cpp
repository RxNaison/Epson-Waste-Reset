#include <iostream>
#include <algorithm>
#include <string>
#include <cctype>
#include "ewr/payload.h"
#include "ewr/parser.h"
#include "ewr/usb.h"
#include "ewr/generator.h"

struct MenuOption
{
    std::string displayName;
    bool isReplay;
    ewr::PrinterModel replayModel;
    ewr::DbPrinterModel smartModel;
};

std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "       EWR - Epson Waste Reset          " << std::endl;
    std::cout << "========================================\n" << std::endl;

    ewr::UniversalGenerator generator;

    std::cout << "[i] Checking for OTA database updates... ";

    if (generator.SyncDatabaseOTA())
        std::cout << "SUCCESS." << std::endl;
    else
        std::cout << "OFFLINE (Using local cache)." << std::endl;

    generator.LoadDatabase("database.json");

    auto replayModels = ewr::ScanModelsFolder("models");
    auto smartModels = generator.GetAvailableModels();

    if (replayModels.empty() && smartModels.empty())
    {
        std::cerr << "\n[!] No payloads found. You need internet access on first run, or a 'models' folder with payload dumps." << std::endl;
        std::cin.get();
        return 1;
    }

    std::vector<MenuOption> options;
    size_t hiddenModels = 0;

    for (const auto& sm : smartModels)
    {
        if (!sm.HasResettableCounters())
        {
            hiddenModels++;
            continue;
        }
        options.push_back({ sm.name + " (Smart Protocol)", false, {}, sm });
    }

    std::cout << "[i] Loaded " << (smartModels.size() - hiddenModels) << " Smart Protocol payloads." << std::endl;

    if (hiddenModels > 0)
        std::cout << "[i] " << hiddenModels << " database entries have no USB-resettable counters and are hidden." << std::endl;

    std::cout << "[i] Loaded " << replayModels.size() << " Custom payloads.\n" << std::endl;

    for (const auto& lm : replayModels)
        options.push_back({ lm.name + " (Replay)", true, lm, {} });

    std::sort(options.begin(), options.end(), [](const MenuOption& a, const MenuOption& b)
        {
            return a.displayName < b.displayName;
        });

    MenuOption selected;
    bool hasSelected = false;

    while (!hasSelected)
    {
        std::cout << "\nEnter printer model to search (e.g., 'L3150' or 'XP') or type 'exit' to quit: ";
        std::string searchQuery;
        std::getline(std::cin, searchQuery);

        if (searchQuery.empty())
            continue;

        std::string searchLower = toLower(searchQuery);
        if (searchLower == "exit" || searchLower == "quit")
            return 0;

        std::vector<MenuOption> filteredOptions;
        for (const auto& opt : options)
        {
            if (toLower(opt.displayName).find(searchLower) != std::string::npos)
                filteredOptions.push_back(opt);
        }

        if (filteredOptions.empty())
        {
            std::cout << "[-] No printers found matching '" << searchQuery << "'. Please try again.\n";
            continue;
        }

        std::cout << "\nFound " << filteredOptions.size() << " matching printers:\n";

        for (size_t i = 0; i < filteredOptions.size(); ++i)
            std::cout << "[" << i + 1 << "] " << filteredOptions[i].displayName << "\n";

        std::cout << "[0] Search again...\n";

        std::cout << "\nSelect your printer [0-" << filteredOptions.size() << "]: ";
        std::string choiceStr;
        std::getline(std::cin, choiceStr);

        try
        {
            int choice = std::stoi(choiceStr);
            if (choice == 0)
            {
                continue;
            }
            else if (choice >= 1 && choice <= static_cast<int>(filteredOptions.size()))
            {
                selected = filteredOptions[choice - 1];
                hasSelected = true;
            }
            else
            {
                std::cout << "[-] Invalid selection. Please try again.\n";
            }
        }
        catch (...)
        {
            std::cout << "[-] Invalid input. Please enter a number.\n";
        }
    }

    std::vector<std::vector<unsigned char>> executionSequence;

    if (selected.isReplay)
    {
        std::cout << "\n[!] Parsing replay Wireshark dump for " << selected.displayName << "..." << std::endl;
        executionSequence = ewr::ParseWiresharkDump(selected.replayModel.filepath);
    }
    else
    {
        if (selected.smartModel.IsPlatenOnly())
        {
            std::cout << "\n================================================================================" << std::endl;
            std::cout << "[!] NOTICE FOR " << selected.smartModel.name << ":" << std::endl;
            std::cout << "    This printer model ONLY has EEPROM counters for the PLATEN PAD (borderless ink pad)." << std::endl;
            std::cout << "    The MAIN WASTE INK BOX on this printer uses a physical hardware chip on the" << std::endl;
            std::cout << "    maintenance tank and CANNOT be reset over USB EEPROM." << std::endl;
            std::cout << "    To reset the Main Waste Ink Box, replace the maintenance box or use a physical chip resetter." << std::endl;
            std::cout << "================================================================================\n" << std::endl;
        }

        std::cout << "[*] Generating safe Smart Protocol R/W sequence for " << selected.displayName << "..." << std::endl;
        executionSequence = generator.GenerateSequence(selected.smartModel);
    }

    if (executionSequence.empty())
    {
        std::cerr << "[-] Failed to construct payload. Exiting.\n";
        std::cin.get();
        return 1;
    }

    std::cout << "Scanning USB ports for Epson device..." << std::endl;
    ewr::EwrDeviceHandle hPrinter = ewr::AutoConnectEpsonPrinter();

    if (!hPrinter)
    {
        std::cerr << "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?" << std::endl;
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    const bool resetOk = ewr::ExecutePayloadSequence(hPrinter, executionSequence);

    if (resetOk)
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << " SUCCESS! Turn the printer OFF, then ON." << std::endl;
        std::cout << "========================================" << std::endl;
    }
    else
    {
        std::cerr << "\n========================================" << std::endl;
        std::cerr << " RESET FAILED. See messages above and" << std::endl;
        std::cerr << " ewr_trace.log for details." << std::endl;
        std::cerr << "========================================" << std::endl;
    }

    ewr::DisconnectPrinter(hPrinter);

    std::cout << "\nPress Enter to exit..." << std::endl;
    std::cin.get();

    return resetOk ? 0 : 1;
}
