#include "ewr/parser.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <iostream>

namespace fs = std::filesystem;

namespace ewr {

    std::vector<PrinterModel> ScanModelsFolder(const std::string& folderPath)
    {
        std::vector<PrinterModel> availableModels;
        std::regex filenameRegex(R"((.+)\.(txt|c)$)");

        if (!fs::exists(folderPath))
        {
            fs::create_directory(folderPath);
            return availableModels;
        }

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

    std::vector<PayloadPair> ParseWiresharkDump(const std::string& filepath)
    {
        std::vector<PayloadPair> sequence;
        std::ifstream file(filepath);

        if (!file.is_open())
        {
            std::cerr << "Error: Could not open payload file." << std::endl;
            return sequence;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        std::regex arrayRegex(R"(\{([^}]+)\})");
        auto array_begin = std::sregex_iterator(content.begin(), content.end(), arrayRegex);
        auto array_end = std::sregex_iterator();

        std::vector<std::vector<unsigned char>> all_packets;
        std::regex byteRegex(R"(0x[0-9a-fA-F]{1,2})");

        for (std::sregex_iterator i = array_begin; i != array_end; ++i)
        {
            std::string arrayContent = i->str(1);
            std::vector<unsigned char> current_packet;

            auto byte_begin = std::sregex_iterator(arrayContent.begin(), arrayContent.end(), byteRegex);
            for (std::sregex_iterator b = byte_begin; b != array_end; ++b) 
            {
                unsigned char hexByte = static_cast<unsigned char>(std::stoul(b->str(), nullptr, 16));
                current_packet.push_back(hexByte);
            }

            if (current_packet.size() >= 27 && current_packet[0] == 0x1B && current_packet[1] == 0x00) 
            {
                current_packet.erase(current_packet.begin(), current_packet.begin() + 27);
            }

            if (!current_packet.empty())
            {
                all_packets.push_back(current_packet);
            }
        }

        for (size_t i = 0; i < all_packets.size() - 1; i += 2)
        {
            PayloadPair pair;
            pair.command = all_packets[i];
            pair.query = all_packets[i + 1];
            sequence.push_back(pair);
        }

        return sequence;
    }
}