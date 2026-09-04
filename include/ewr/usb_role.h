#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace ewr {

    // Trust level for one USB function on a composite Epson device. The role is
    // deliberately about *function*, not VID/PID: an MFP can expose a printer
    // and a scanner under the same physical USB device.
    enum class UsbCandidateRole
    {
        Printer,
        // A vendor-specific bulk interface deliberately surfaced by the
        // libusb backend as a maintenance candidate (ET-2xxx/#16). It is not
        // a Windows printer-class node, but it is still an intentional rung.
        Maintenance,
        Unknown,
        Scanner
    };

    inline const char* UsbCandidateRoleName(UsbCandidateRole role)
    {
        switch (role)
        {
            case UsbCandidateRole::Printer: return "printer";
            case UsbCandidateRole::Maintenance: return "maintenance";
            case UsbCandidateRole::Scanner: return "scanner";
            default: return "unknown";
        }
    }

    inline std::string UsbAsciiLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return text;
    }

    inline bool UsbEqualsNoCase(const std::string& a, const std::string& b)
    {
        return UsbAsciiLower(a) == UsbAsciiLower(b);
    }

    // Pure and platform-independent so Windows role classification can be
    // covered by the normal Linux/Windows unit-test matrix. The strings are the
    // values exposed by SetupAPI: interface GUID bucket, SPDRP_SERVICE and
    // SPDRP_CLASS respectively.
    inline UsbCandidateRole ClassifyWindowsUsbCandidateRole(
        const std::string& interfaceClass,
        const std::string& serviceName,
        const std::string& pnpClass)
    {
        if (UsbEqualsNoCase(interfaceClass, "USBPRINT") ||
            UsbEqualsNoCase(serviceName, "usbprint") ||
            UsbEqualsNoCase(pnpClass, "Printer") ||
            UsbEqualsNoCase(pnpClass, "PrintQueue"))
            return UsbCandidateRole::Printer;

        if (UsbEqualsNoCase(interfaceClass, "IMAGE") ||
            UsbEqualsNoCase(serviceName, "usbscan") ||
            UsbEqualsNoCase(pnpClass, "Image"))
            return UsbCandidateRole::Scanner;

        return UsbCandidateRole::Unknown;
    }

    // CollectCandidates() in the libusb backend has already filtered the set
    // to USB printer-class or Epson vendor-specific bulk functions. Keep this
    // mapping pure so CI can verify the Windows libusb merge policy without a
    // libusb device (or even libusb headers) being present.
    inline UsbCandidateRole ClassifyLibusbCandidateRole(bool printerClass)
    {
        return printerClass ? UsbCandidateRole::Printer : UsbCandidateRole::Maintenance;
    }

    inline int UsbCandidateRolePriority(UsbCandidateRole role)
    {
        switch (role)
        {
            case UsbCandidateRole::Printer: return 0;
            case UsbCandidateRole::Maintenance: return 5;
            case UsbCandidateRole::Unknown: return 10;
            case UsbCandidateRole::Scanner: return 100;
        }
        return 10;
    }

    // Once a positively identified maintenance-capable function exists, keep
    // automatic traffic on those functions and stop falling through to generic
    // composite parents. Printer-class and libusb vendor-maintenance functions
    // are both positive roles; Unknown stays eligible only on legacy/custom
    // layouts where neither was discoverable. Scanner is always blocked.
    inline bool IsAutomaticMaintenanceRole(UsbCandidateRole role, bool positiveRoleAvailable = false)
    {
        if (role == UsbCandidateRole::Scanner)
            return false;
        if (role == UsbCandidateRole::Printer || role == UsbCandidateRole::Maintenance)
            return true;
        if (positiveRoleAvailable)
            return false;
        return true;
    }

} // namespace ewr
