#pragma once
#include "ewr/usb_backend.h"

#include <memory>
#include <ostream>

// Members of the Windows composite. `CreateUsbBackend` in ewr/usb_backend.h
// is the only entry point a host needs.

namespace ewr {

    std::unique_ptr<UsbBackend> CreateLibusbBackend(std::ostream& trace);

#ifdef _WIN32
    std::unique_ptr<UsbBackend> CreateWindowsUsbBackend(std::ostream& trace);
#endif

} // namespace ewr
