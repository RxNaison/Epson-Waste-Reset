#include "ewr/parser.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include "ewr/log.h"

namespace fs = std::filesystem;

namespace ewr {

    std::vector<PrinterModel> ScanModelsFolder(const std::string& folderPath)
    {
        std::vector<PrinterModel> availableModels;
        std::regex filenameRegex(R"((.+)\.(txt|c)$)");

        // Deliberately does not create the folder: replay dumps are an opt-in
        // escape hatch, so an absent folder means "no replay models", not
        // "make me one".
        if (!fs::exists(folderPath))
            return availableModels;

        for (const auto& entry : fs::directory_iterator(folderPath))
        {
            if (entry.is_regular_file())
            {
                std::string filename = entry.path().filename().string();
                std::smatch match;

                if (std::regex_match(filename, match, filenameRegex))
                {
                    PrinterModel model;
                    model.name = match[1].str();
                    model.filepath = entry.path().string();
                    availableModels.push_back(model);
                }
            }
        }
        return availableModels;
    }

    std::vector<std::vector<unsigned char>> ParseWiresharkDump(const std::string& filepath)
    {
        std::vector<std::vector<unsigned char>> sequence;
        std::ifstream file(filepath);

        if (!file.is_open())
        {
            ewr::log::Log(ewr::log::Level::Error, ewr::log::Stage::General,
                          "payload.open_failed", "Error: Could not open payload file.");
            return sequence;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        std::regex arrayRegex(R"(\{([^}]+)\})");
        auto array_begin = std::sregex_iterator(content.begin(), content.end(), arrayRegex);
        const auto regex_end = std::sregex_iterator(); // shared end sentinel for both loops

        std::vector<std::vector<unsigned char>> all_packets;
        std::regex byteRegex(R"(0x[0-9a-fA-F]{1,2})");

        for (std::sregex_iterator i = array_begin; i != regex_end; ++i)
        {
            std::string arrayContent = i->str(1);
            std::vector<unsigned char> current_packet;

            // The inner loop must terminate against its own iterator's end
            // sentinel, never against an iterator tied to a different string.
            auto byte_begin = std::sregex_iterator(arrayContent.begin(), arrayContent.end(), byteRegex);
            for (std::sregex_iterator b = byte_begin; b != regex_end; ++b)
            {
                unsigned char hexByte = static_cast<unsigned char>(std::stoul(b->str(), nullptr, 16));
                current_packet.push_back(hexByte);
            }

            // Strip a 27-byte USBPcap pseudo-header (header length is encoded
            // little-endian in the first two bytes: 0x1B 0x00). Only strip when
            // a real payload remains afterwards, so a legitimate 27-byte packet
            // that merely starts with 0x1B 0x00 is not swallowed whole.
            if (current_packet.size() > 27 && current_packet[0] == 0x1B && current_packet[1] == 0x00)
            {
                current_packet.erase(current_packet.begin(), current_packet.begin() + 27);
            }

            if (!current_packet.empty())
            {
                all_packets.push_back(current_packet);
            }
        }

        return all_packets;
    }
}