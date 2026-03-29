#pragma once
#include <string>
#include <vector>

namespace ewr {

    struct PrinterModel
    {
        std::string name;
        std::string filepath;
    };

    struct PayloadPair
    {
        std::vector<unsigned char> command;
        std::vector<unsigned char> query;
    };

}