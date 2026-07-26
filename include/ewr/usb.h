#pragma once
#include "ewr/payload.h"
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ewr {

    constexpr uint16_t EPSON_VID = 0x04B8;

    typedef void* EwrDeviceHandle;

    EwrDeviceHandle AutoConnectEpsonPrinter();
    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence);
    void DisconnectPrinter(EwrDeviceHandle hPrinter);

}
