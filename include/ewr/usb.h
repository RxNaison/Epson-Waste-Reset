#pragma once
#include "ewr/payload.h"
#include <windows.h>
#include <vector>

namespace ewr {
    typedef void* EwrDeviceHandle;

    EwrDeviceHandle AutoConnectEpsonPrinter();
    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<PayloadPair>& sequence);
    void DisconnectPrinter(EwrDeviceHandle hPrinter);
}