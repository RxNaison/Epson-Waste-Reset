#include <iostream>
#include "ewr/payload.h"
#include "ewr/parser.h"
#include "ewr/usb.h"

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "       EWR - Epson Waste Reset          " << std::endl;
    std::cout << "========================================\n" << std::endl;

    auto models = ewr::ScanModelsFolder("models");

    if (models.empty())
    {
        std::cout << "No payloads found. \nAction Required: Create a 'models' folder next to this .exe and drop your Wireshark dump (.txt or .c) inside it." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "Available Printer Payloads:\n";
    for (size_t i = 0; i < models.size(); ++i)
        std::cout << "[" << i + 1 << "] " << models[i].name << "\n";

    int choice;
    std::cout << "\nSelect your printer: ";
    std::cin >> choice;

    std::cin.clear();
    std::cin.ignore(256, '\n');

    if (choice < 1 || choice > models.size())
    {
        std::cout << "Invalid selection. Exiting.\n";
        std::cin.get();
        return 1;
    }

    ewr::PrinterModel selectedModel = models[choice - 1];
    std::cout << "\nParsing Wireshark dump for " << selectedModel.name << "..." << std::endl;

    std::vector<std::vector<unsigned char>> executionSequence = ewr::ParseWiresharkDump(selectedModel.filepath);

    if (executionSequence.empty())
    {
        std::cout << "Failed to parse payloads. Exiting.\n";
        std::cin.get();
        return 1;
    }

    std::cout << "Successfully loaded " << executionSequence.size() << " raw operational packets." << std::endl;

    std::cout << "\nScanning USB ports for Epson device..." << std::endl;
    ewr::EwrDeviceHandle hPrinter = ewr::AutoConnectEpsonPrinter();

    if (!hPrinter)
    {
        std::cerr << "[ERROR] Could not find an Epson printer. Is it turned on and plugged in?" << std::endl;
        std::cin.get();
        return 1;
    }

    if (ewr::ExecutePayloadSequence(hPrinter, executionSequence))
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << " SUCCESS! Turn the printer OFF, then ON." << std::endl;
        std::cout << "========================================" << std::endl;
    }

    ewr::DisconnectPrinter(hPrinter);

    std::cin.get();
    return 0;
}