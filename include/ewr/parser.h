#pragma once
#include "ewr/payload.h"
#include <string>
#include <vector>

namespace ewr {
    std::vector<PrinterModel> ScanModelsFolder(const std::string& folderPath);
    std::vector<PayloadPair> ParseWiresharkDump(const std::string& filepath);
}