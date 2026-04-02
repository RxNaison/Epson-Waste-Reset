#pragma once
#include "ewr/payload.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <vector>

namespace ewr {
    typedef void* EwrDeviceHandle;

    EwrDeviceHandle AutoConnectEpsonPrinter();
    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence);
    void DisconnectPrinter(EwrDeviceHandle hPrinter);
}
