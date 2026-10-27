#pragma once
#include "transport/usb_backend.h"

#include <memory>
#include <ostream>

// Members of the Windows composite. `CreateBackend` in ewr/usb_backend.h
// is the only entry point a host needs.

namespace ewr::usb {

    std::unique_ptr<Backend> CreateLibusbBackend(std::ostream& trace);

#ifdef _WIN32
    std::unique_ptr<Backend> CreateWindowsBackend(std::ostream& trace);
#endif

} // namespace ewr::usb
