#pragma once
#include "ewr/payload.h"
#include <string>
#include <vector>

namespace ewr {
    std::vector<PrinterModel> ScanModelsFolder(const std::string& folderPath);
    std::vector<std::vector<unsigned char>> ParseWiresharkDump(const std::string& filepath);
}