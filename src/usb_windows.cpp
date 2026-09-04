#include "ewr/usb.h"
#include "ewr/usb_backend.h"
#include "usb_backends_internal.h"
#include "ewr/usb_timing.h"
#include "ewr/deviceid.h"
#include "ewr/log.h"
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_USBPRINT, 0x28d78fad, 0x5a12, 0x11d1, 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2);

// From usbprint.h (WDK): CTL_CODE(FILE_DEVICE_UNKNOWN, 13, METHOD_BUFFERED,
// FILE_ANY_ACCESS). Defined manually so the desktop SDK alone builds EWR.
#ifndef IOCTL_USBPRINT_GET_1284_ID
#define IOCTL_USBPRINT_GET_1284_ID 0x220034
#endif

// usbprint.h again: CTL_CODE(FILE_DEVICE_UNKNOWN, 16, METHOD_BUFFERED,
// FILE_ANY_ACCESS) - asks usbprint.sys to soft-reset the USB pipe.
#ifndef IOCTL_USBPRINT_SOFT_RESET
#define IOCTL_USBPRINT_SOFT_RESET 0x220040
#endif

// Windows USB backend: SetupAPI enumeration across the USBPRINT / IMAGE /
// USB_DEVICE / WINUSB interface classes plus raw overlapped I/O against
// usbprint.sys.
//
// Tried first; usb_composite.cpp appends the vendor-specific interfaces only
// libusb can open.
//
// A session is never repaired in place: a silent handshake means close and
// reopen, and the driver loop owns that policy. The run choreography (pin,
// retry, fallback, banners) lives in usb_driver.cpp; see ewr/usb_backend.h
// for the seam.

namespace ewr {

    std::string GetWindowsErrorString(DWORD errorCode)
    {
        switch (errorCode)
        {
            case ERROR_SUCCESS: return "ERROR_SUCCESS (0): Success";
            case ERROR_FILE_NOT_FOUND: return "ERROR_FILE_NOT_FOUND (2): The system cannot find the file specified.";
            case ERROR_ACCESS_DENIED: return "ERROR_ACCESS_DENIED (5): Access is denied. Check Administrator rights or exclusive locks.";
            case ERROR_INVALID_HANDLE: return "ERROR_INVALID_HANDLE (6): The handle is invalid.";
            case ERROR_SHARING_VIOLATION: return "ERROR_SHARING_VIOLATION (32): The process cannot access the file because it is being used by another process (e.g. Spooler or Status Monitor).";
            case ERROR_SEM_TIMEOUT: return "ERROR_SEM_TIMEOUT (121): The semaphore timeout period has expired (USB communication timeout).";
            case ERROR_GEN_FAILURE: return "ERROR_GEN_FAILURE (31): A device attached to the system is not functioning.";
            case ERROR_IO_PENDING: return "ERROR_IO_PENDING (997): Overlapped I/O operation is in progress.";
            case ERROR_OPERATION_ABORTED: return "ERROR_OPERATION_ABORTED (995): The I/O operation has been aborted (canceled request).";
            case ERROR_INVALID_FUNCTION: return "ERROR_INVALID_FUNCTION (1): The device driver does not implement this request.";
            default:
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "Error Code %lu", errorCode);
                return buf;
            }
        }
    }

    namespace {

        using namespace usb_timing;

        struct DeviceCandidate
        {
            std::string path;
            std::string className;
            int classPriority;
            int interfaceIndex;
            UsbCandidateRole role = UsbCandidateRole::Unknown;
            std::string serviceName;
            std::string pnpClass;
            std::string instanceId;
        };

        std::string DescribeCandidate(const DeviceCandidate& c)
        {
            std::string text = "[" + c.className + " | " + UsbCandidateRoleName(c.role) + " | mi_"
                 + (c.interfaceIndex >= 0 ? std::to_string(c.interfaceIndex) : "N/A") + "] " + c.path;
            if (!c.serviceName.empty())
                text += " | service=" + c.serviceName;
            if (!c.pnpClass.empty())
                text += " | pnpClass=" + c.pnpClass;
            if (!c.instanceId.empty())
                text += " | instance=" + c.instanceId;
            return text;
        }

        std::string ExtractPid(const std::string& devicePath)
        {
            std::string lower = UsbAsciiLower(devicePath);
            size_t pidPos = lower.find("pid_");
            return (pidPos != std::string::npos && pidPos + 8 <= lower.length()) ? lower.substr(pidPos + 4, 4) : "UNKNOWN";
        }

        std::string GetDeviceRegistryString(HDEVINFO hDevInfo,
                                            SP_DEVINFO_DATA& devInfoData,
                                            DWORD property)
        {
            DWORD dataType = 0;
            DWORD required = 0;

            SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devInfoData, property,
                                              &dataType, nullptr, 0, &required);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
                return {};

            std::vector<BYTE> buffer(required + 1, 0);
            if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devInfoData, property,
                                                   &dataType, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()), nullptr))
                return {};

            if (dataType != REG_SZ && dataType != REG_EXPAND_SZ)
                return {};

            return reinterpret_cast<const char*>(buffer.data());
        }

        std::string GetDeviceInstanceId(HDEVINFO hDevInfo, SP_DEVINFO_DATA& devInfoData)
        {
            DWORD required = 0;
            SetupDiGetDeviceInstanceIdA(hDevInfo, &devInfoData, nullptr, 0, &required);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
                return {};

            std::vector<char> buffer(required + 1, '\0');
            if (!SetupDiGetDeviceInstanceIdA(hDevInfo, &devInfoData, buffer.data(),
                                             static_cast<DWORD>(buffer.size()), nullptr))
                return {};

            return buffer.data();
        }

        // Interface enumeration priority (highest to lowest):
        //   1. Positively identified printer functions (USBPRINT / usbprint)
        //   2. Unknown Epson USB functions kept for compatibility fallback
        //   3. Positively identified scanner functions (IMAGE / usbscan)
        //
        // mi_XX is only a tiebreaker inside the same role. On L3210/ET-280x
        // layouts the scanner may be mi_00 and the printer mi_01, so interface
        // number must never outweigh OS-level role classification.
        std::vector<DeviceCandidate> EnumerateEpsonCandidates(std::ostream& trace)
        {
            struct GuidEntry
            {
                GUID guid;
                const char* name;
                int classPriority; // Lower number = higher priority for printer maintenance
            };

            const GuidEntry SCAN_GUIDS[] = {
                // GUID_DEVINTERFACE_USBPRINT
                { { 0x28d78fad, 0x5a12, 0x11d1, { 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2 } }, "USBPRINT", 0 },
                // GUID_DEVINTERFACE_IMAGE
                { { 0x6bdd1fc6, 0x810f, 0x11d0, { 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } }, "IMAGE", 10 },
                // GUID_DEVINTERFACE_USB_DEVICE
                { { 0xa5cd7fef, 0x35b7, 0x11d0, { 0xb4, 0x20, 0x00, 0xc0, 0x4f, 0x79, 0xaa, 0xf1 } }, "USB_DEVICE", 20 },
                // GUID_DEVINTERFACE_WINUSB
                { { 0xdee0c8d9, 0xba4e, 0x46c5, { 0x9a, 0x2a, 0x7d, 0x35, 0x9e, 0x80, 0xb4, 0xeb } }, "WINUSB", 30 }
            };

            std::vector<DeviceCandidate> candidates;

            trace << "[i] Scanning for Epson USB interfaces across multiple GUID classes...\n";

            for (const auto& entry : SCAN_GUIDS)
            {
                HDEVINFO hDevInfo = SetupDiGetClassDevs(&entry.guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

                if (hDevInfo == INVALID_HANDLE_VALUE)
                    continue;

                SP_DEVICE_INTERFACE_DATA devInterfaceData;
                devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

                char guidStr[64];
                snprintf(guidStr, sizeof(guidStr), "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                         entry.guid.Data1, entry.guid.Data2, entry.guid.Data3,
                         entry.guid.Data4[0], entry.guid.Data4[1], entry.guid.Data4[2], entry.guid.Data4[3],
                         entry.guid.Data4[4], entry.guid.Data4[5], entry.guid.Data4[6], entry.guid.Data4[7]);

                trace << "  [i] Enrolling class: " << guidStr << " (" << entry.name << ")\n";

                for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &entry.guid, i, &devInterfaceData); ++i)
                {
                    DWORD requiredSize = 0;
                    SetupDiGetDeviceInterfaceDetailA(hDevInfo, &devInterfaceData, NULL, 0, &requiredSize, NULL);

                    std::vector<BYTE> detailDataBuffer(requiredSize);
                    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData =
                        reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_A>(detailDataBuffer.data());
                    detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

                    SP_DEVINFO_DATA devInfoData = {};
                    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

                    if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &devInterfaceData, detailData, requiredSize, NULL, &devInfoData))
                    {
                        std::string devicePath = detailData->DevicePath;
                        std::string devicePathLower = UsbAsciiLower(devicePath);

                        if (devicePathLower.find("vid_04b8") != std::string::npos)
                        {
                            const std::string serviceName = GetDeviceRegistryString(hDevInfo, devInfoData, SPDRP_SERVICE);
                            const std::string pnpClass = GetDeviceRegistryString(hDevInfo, devInfoData, SPDRP_CLASS);
                            const std::string instanceId = GetDeviceInstanceId(hDevInfo, devInfoData);
                            const UsbCandidateRole role = ClassifyWindowsUsbCandidateRole(entry.name, serviceName, pnpClass);

                            trace << "     -> Matches Epson Vendor ID (vid_04b8): " << devicePath
                                  << " [class: " << entry.name
                                  << ", role: " << UsbCandidateRoleName(role);
                            if (!serviceName.empty()) trace << ", service: " << serviceName;
                            if (!pnpClass.empty()) trace << ", pnp: " << pnpClass;
                            trace << "]\n";

                            int ifaceIdx = -1;
                            size_t miPos = devicePathLower.find("mi_");
                            if (miPos != std::string::npos && miPos + 5 <= devicePathLower.length())
                            {
                                try
                                {
                                    ifaceIdx = std::stoi(devicePathLower.substr(miPos + 3, 2));
                                }
                                catch (...)
                                {
                                    ifaceIdx = -1;
                                }
                            }

                            DeviceCandidate incoming{ devicePath, entry.name, entry.classPriority, ifaceIdx,
                                                      role, serviceName, pnpClass, instanceId };

                            // The same physical function can be exposed under
                            // multiple interface GUIDs. Deduplicate by PnP
                            // instance ID and retain the representation with the
                            // strongest printer role / most specific class.
                            auto existing = candidates.end();
                            if (!instanceId.empty())
                            {
                                existing = std::find_if(candidates.begin(), candidates.end(),
                                    [&](const DeviceCandidate& c)
                                    {
                                        return !c.instanceId.empty() && UsbEqualsNoCase(c.instanceId, instanceId);
                                    });
                            }
                            else
                            {
                                existing = std::find_if(candidates.begin(), candidates.end(),
                                    [&](const DeviceCandidate& c) { return UsbEqualsNoCase(c.path, devicePath); });
                            }

                            if (existing == candidates.end())
                            {
                                candidates.push_back(std::move(incoming));
                            }
                            else
                            {
                                const int incomingRole = UsbCandidateRolePriority(incoming.role);
                                const int existingRole = UsbCandidateRolePriority(existing->role);
                                if (incomingRole < existingRole ||
                                    (incomingRole == existingRole && incoming.classPriority < existing->classPriority))
                                {
                                    trace << "        [i] Same PnP function already seen; keeping the stronger representation: "
                                          << incoming.className << "/" << UsbCandidateRoleName(incoming.role) << "\n";
                                    *existing = std::move(incoming);
                                }
                            }
                        }
                    }
                    else
                    {
                        DWORD err = GetLastError();
                        trace << "     [!] SetupDiGetDeviceInterfaceDetail failed. " << GetWindowsErrorString(err) << "\n";
                    }
                }
                SetupDiDestroyDeviceInfoList(hDevInfo);
            }

            trace << "[i] Total unique Epson (VID_04B8) candidates discovered: " << candidates.size() << "\n";

            for (const auto& c : candidates)
                trace << "     " << DescribeCandidate(c) << "\n";

            std::sort(candidates.begin(), candidates.end(), [](const DeviceCandidate& a, const DeviceCandidate& b)
                {
                    const int aRole = UsbCandidateRolePriority(a.role);
                    const int bRole = UsbCandidateRolePriority(b.role);
                    if (aRole != bRole)
                        return aRole < bRole;

                    if (a.classPriority != b.classPriority)
                        return a.classPriority < b.classPriority;

                    int aIdx = (a.interfaceIndex >= 0) ? a.interfaceIndex : 9999;
                    int bIdx = (b.interfaceIndex >= 0) ? b.interfaceIndex : 9999;
                    return aIdx < bIdx;
                });

            const bool hasPrinter = std::any_of(candidates.begin(), candidates.end(),
                [](const DeviceCandidate& c) { return c.role == UsbCandidateRole::Printer; });
            if (!hasPrinter && !candidates.empty())
            {
                trace << "[WARNING] No positively identified Epson printer function was found.\n";
                trace << "          Unknown USB functions remain available as compatibility fallbacks,\n";
                trace << "          but scanner functions are blocked from automatic maintenance writes.\n";
                trace << "          Reinstall the official Epson printer driver if the print function is missing.\n";
            }

            return candidates;
        }

        // A stalled write records how many bytes were accepted before the
        // cancel, which separates a printer that ignores our bytes from one
        // that refuses them outright.
        bool AsyncWrite(HANDLE hPrinter, const std::vector<unsigned char>& data, std::ostream& trace)
        {
            const ULONGLONG start = GetTickCount64();
            const char* mode = "completed synchronously";

            DWORD bytesWritten = 0;
            OVERLAPPED osWrite = { 0 };
            osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            if (!osWrite.hEvent)
            {
                DWORD err = GetLastError();
                trace << "[!] AsyncWrite: CreateEvent failed. " << GetWindowsErrorString(err) << "\n";
                return false;
            }

            bool success = WriteFile(hPrinter, data.data(), data.size(), &bytesWritten, &osWrite);
            if (!success)
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    DWORD waitResult = WaitForSingleObject(osWrite.hEvent, kWriteTimeoutMs);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        mode = "went pending, completed";
                        success = GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, FALSE);
                        if (!success)
                        {
                            DWORD overlapErr = GetLastError();
                            trace << "[!] AsyncWrite: GetOverlappedResult failed after completion. " << GetWindowsErrorString(overlapErr) << "\n";
                        }
                    }
                    else if (waitResult == WAIT_TIMEOUT)
                    {
                        trace << "[!] AsyncWrite: WaitForSingleObject timed out (" << kWriteTimeoutMs << "ms limit reached). Canceling I/O...\n";
                        CancelIo(hPrinter);
                        DWORD recovered = 0;
                        if (GetOverlappedResult(hPrinter, &osWrite, &recovered, TRUE))
                        {
                            trace << "[io] OUT stalled: " << recovered << "/" << data.size() << " bytes accepted in "
                                  << (GetTickCount64() - start) << " ms; canceled cleanly.\n";
                        }
                        else
                        {
                            DWORD cancelErr = GetLastError();
                            trace << "[io] OUT stalled: " << recovered << "/" << data.size() << " bytes accepted in "
                                  << (GetTickCount64() - start) << " ms; canceled (" << GetWindowsErrorString(cancelErr) << ").\n";
                        }
                        bytesWritten = recovered;
                        success = false;
                    }
                    else
                    {
                        DWORD waitErr = GetLastError();
                        trace << "[!] AsyncWrite: WaitForSingleObject failed with error: " << waitResult << ". " << GetWindowsErrorString(waitErr) << "\n";
                        CancelIo(hPrinter);
                        GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, TRUE);
                        success = false;
                    }
                }
                else
                {
                    trace << "[!] AsyncWrite: WriteFile failed immediately. " << GetWindowsErrorString(err) << "\n";
                }
            }

            CloseHandle(osWrite.hEvent);

            if (success && bytesWritten != data.size())
            {
                trace << "[!] AsyncWrite: Write reported success but bytesWritten (" << bytesWritten << ") != expected size (" << data.size() << ").\n";
                return false;
            }

            if (success)
            {
                trace << "[io] OUT " << bytesWritten << "/" << data.size() << " bytes in "
                      << (GetTickCount64() - start) << " ms (" << mode << ").\n";
            }

            return success;
        }

        // SyncEmpty and PendingTimeout are both "no bytes" but mean very
        // different things about the pipe, so they are counted apart.
        enum class ReadOutcome
        {
            SyncData,        // ReadFile returned TRUE with payload
            SyncEmpty,       // ReadFile returned TRUE with 0 bytes ("not yet")
            PendingData,     // went pending, delivered payload (or raced in at cancel)
            PendingEmpty,    // went pending, completed with 0 bytes
            PendingTimeout,  // went pending, nothing arrived before the deadline
            HardFail         // I/O failure that makes further reads pointless
        };

        const char* DescribeReadOutcome(ReadOutcome outcome)
        {
            switch (outcome)
            {
                case ReadOutcome::SyncData:       return "completed synchronously with data";
                case ReadOutcome::SyncEmpty:      return "completed synchronously, empty";
                case ReadOutcome::PendingData:    return "went pending, completed with data";
                case ReadOutcome::PendingEmpty:   return "went pending, completed empty";
                case ReadOutcome::PendingTimeout: return "went pending, timed out";
                default:                          return "failed";
            }
        }

        ReadOutcome ReadChunk(HANDLE hPrinter, BYTE* buffer, DWORD bufferSize, ULONGLONG deadline, DWORD& bytesRead, std::ostream& trace)
        {
            bytesRead = 0;

            OVERLAPPED osRead = { 0 };
            osRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            if (!osRead.hEvent)
            {
                DWORD err = GetLastError();
                trace << "[!] AsyncDrainBuffer: CreateEvent failed. " << GetWindowsErrorString(err) << "\n";
                return ReadOutcome::HardFail;
            }

            ReadOutcome outcome = ReadOutcome::HardFail;

            bool success = ReadFile(hPrinter, buffer, bufferSize, &bytesRead, &osRead);
            if (success)
            {
                outcome = (bytesRead > 0) ? ReadOutcome::SyncData : ReadOutcome::SyncEmpty;
            }
            else
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    // usbprint.sys holds the read until data arrives, so wait
                    // out whatever remains of the deadline.
                    const ULONGLONG now = GetTickCount64();
                    const DWORD waitMs = (deadline > now) ? (DWORD)(deadline - now) : 0;

                    DWORD waitResult = WaitForSingleObject(osRead.hEvent, waitMs);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        if (GetOverlappedResult(hPrinter, &osRead, &bytesRead, FALSE))
                        {
                            outcome = (bytesRead > 0) ? ReadOutcome::PendingData : ReadOutcome::PendingEmpty;
                        }
                        else
                        {
                            DWORD overlapErr = GetLastError();
                            trace << "[!] AsyncDrainBuffer: GetOverlappedResult failed after completion. " << GetWindowsErrorString(overlapErr) << "\n";
                            outcome = ReadOutcome::HardFail;
                        }
                    }
                    else
                    {
                        if (waitResult != WAIT_TIMEOUT)
                        {
                            DWORD waitErr = GetLastError();
                            trace << "[!] AsyncDrainBuffer: WaitForSingleObject failed. " << GetWindowsErrorString(waitErr) << "\n";
                        }

                        // Expiry is normal, not a failure: cancel, but keep
                        // anything that raced in before the cancel landed.
                        CancelIo(hPrinter);
                        DWORD recovered = 0;
                        if (GetOverlappedResult(hPrinter, &osRead, &recovered, TRUE))
                            bytesRead = recovered;
                        else
                            bytesRead = 0;

                        outcome = (bytesRead > 0) ? ReadOutcome::PendingData : ReadOutcome::PendingTimeout;
                    }
                }
                else
                {
                    trace << "[!] AsyncDrainBuffer: ReadFile failed immediately. " << GetWindowsErrorString(err) << "\n";
                    outcome = ReadOutcome::HardFail;
                }
            }

            CloseHandle(osRead.hEvent);
            return outcome;
        }

        // `firstReadTimeoutMs` bounds only the wait for the first bytes; once
        // data flows, a kDrainFollowUpTimeoutMs pause ends the burst.
        //
        // usbprint.sys can complete a read synchronously with 0 bytes while a
        // reply is still on its way, so an empty completion means "not yet",
        // not "no reply", and the read is re-posted. The deadline, the settle
        // window, kMaxDrainBytes and kMaxDrainReads each bound the loop, so a
        // mute printer fails rather than spinning.
        std::vector<unsigned char> AsyncDrainBuffer(HANDLE hPrinter, int firstReadTimeoutMs, std::ostream& trace)
        {
            std::vector<unsigned char> totalData;
            BYTE buffer[kDrainReadChunkBytes];

            const ULONGLONG start = GetTickCount64();
            ULONGLONG deadline = start + ((firstReadTimeoutMs > 0) ? (ULONGLONG)firstReadTimeoutMs : 0);
            int dataReads = 0;

            int syncEmptyPolls = 0;
            int pendingEmpty = 0;
            int pendingTimeouts = 0;
            long long firstDataMs = -1;

            while (totalData.size() < kMaxDrainBytes && dataReads < kMaxDrainReads)
            {
                DWORD bytesRead = 0;
                const ReadOutcome outcome = ReadChunk(hPrinter, buffer, (DWORD)sizeof(buffer), deadline, bytesRead, trace);

                if (outcome == ReadOutcome::HardFail)
                    break;

                if (bytesRead > 0)
                {
                    if (firstDataMs < 0)
                        firstDataMs = (long long)(GetTickCount64() - start);

                    trace << "[io] IN " << bytesRead << " bytes at +" << (GetTickCount64() - start)
                          << " ms (" << DescribeReadOutcome(outcome) << ").\n";

                    totalData.insert(totalData.end(), buffer, buffer + bytesRead);
                    ++dataReads;

                    deadline = GetTickCount64() + kDrainFollowUpTimeoutMs;
                    continue;
                }

                if (outcome == ReadOutcome::SyncEmpty)
                    ++syncEmptyPolls;
                else if (outcome == ReadOutcome::PendingEmpty)
                    ++pendingEmpty;
                else if (outcome == ReadOutcome::PendingTimeout)
                    ++pendingTimeouts;

                if (GetTickCount64() >= deadline)
                    break; // the printer truly had nothing (more) to say

                Sleep(kDrainPollIntervalMs);
            }

            trace << "[io] Drain summary: " << totalData.size() << " bytes in " << dataReads
                  << " data read(s) over " << (GetTickCount64() - start) << " ms (first-byte window "
                  << firstReadTimeoutMs << " ms); ";
            if (firstDataMs >= 0)
                trace << "first data at +" << firstDataMs << " ms; ";
            else
                trace << "no data arrived; ";
            trace << "empty polls: " << syncEmptyPolls << " sync, " << pendingEmpty
                  << " pending-empty, " << pendingTimeouts << " pending-timeout.\n";

            // Logged here rather than at the call sites: a caller may discard
            // a drain without ever looking at it.
            if (!totalData.empty())
                trace << "[io] IN payload (" << totalData.size() << " bytes):\n"
                      << HexDumpCapped(totalData.data(), totalData.size(), kTraceDumpCapBytes);

            return totalData;
        }

        class WindowsUsbTransport final : public ITransport
        {
        public:
            WindowsUsbTransport(HANDLE handle, std::ostream& trace) : handle_(handle), trace_(trace) {}

            bool Send(const std::vector<unsigned char>& packet) override
            {
                return AsyncWrite(handle_, packet, trace_);
            }

            std::vector<unsigned char> Drain(int timeoutMs) override
            {
                return AsyncDrainBuffer(handle_, timeoutMs, trace_);
            }

        private:
            HANDLE handle_;
            std::ostream& trace_;
        };

        // Only usbprint.sys implements the IOCTL; IMAGE and WinUSB would fail
        // with ERROR_INVALID_FUNCTION, so those return "" without opening.
        std::string QueryCandidateDeviceId(const DeviceCandidate& cand, std::ostream& trace)
        {
            if (cand.className != "USBPRINT")
                return "";

            // Synchronous: one quick metadata request, not bulk traffic.
            HANDLE hPrinter = CreateFile(cand.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

            if (hPrinter == INVALID_HANDLE_VALUE)
            {
                trace << "[!] Device ID query: CreateFile failed for " << cand.path << ". " << GetWindowsErrorString(GetLastError()) << "\n";
                return "";
            }

            unsigned char buffer[2048] = { 0 };
            DWORD bytesReturned = 0;
            const BOOL ok = DeviceIoControl(hPrinter, IOCTL_USBPRINT_GET_1284_ID,
                NULL, 0, buffer, sizeof(buffer) - 1, &bytesReturned, NULL);
            const DWORD ioctlError = ok ? ERROR_SUCCESS : GetLastError();
            CloseHandle(hPrinter);

            if (!ok || bytesReturned == 0)
            {
                trace << "[!] IOCTL_USBPRINT_GET_1284_ID failed on " << DescribeCandidate(cand) << ". " << GetWindowsErrorString(ioctlError) << "\n";
                return "";
            }

            const std::string id = ExtractDeviceIdString(buffer, bytesReturned);
            if (id.empty())
                trace << "[!] Device ID reply was empty on " << DescribeCandidate(cand) << ".\n";

            return id;
        }

        // Opt-in (--usb-soft-reset): on ET-2xxx units this reset leaves the
        // bulk-OUT pipe unable to accept a write, and the stall outlives the
        // handle. A failure is logged but never aborts the attempt - the
        // handshake renders the verdict either way.
        void SoftResetOnOpen(HANDLE hPrinter, const DeviceCandidate& cand, std::ostream& trace)
        {
            if (cand.className != "USBPRINT")
            {
                trace << "[i] Session-start soft reset skipped: class " << cand.className << " is not driven by usbprint.sys.\n";
                return;
            }

            const ULONGLONG start = GetTickCount64();
            const char* mode = "completed synchronously";

            OVERLAPPED ov = { 0 };
            ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (!ov.hEvent)
            {
                trace << "[!] Session-start soft reset: CreateEvent failed. " << GetWindowsErrorString(GetLastError()) << "\n";
                return;
            }

            unsigned char outBuffer[1024] = { 0 };
            DWORD bytes = 0;
            BOOL ok = DeviceIoControl(hPrinter, IOCTL_USBPRINT_SOFT_RESET, NULL, 0, outBuffer, sizeof(outBuffer), &bytes, &ov);
            DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();

            if (!ok && lastError == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(ov.hEvent, kSoftResetTimeoutMs) == WAIT_OBJECT_0)
                {
                    mode = "went pending, completed";
                    ok = GetOverlappedResult(hPrinter, &ov, &bytes, FALSE);
                    lastError = ok ? ERROR_SUCCESS : GetLastError();
                }
                else
                {
                    mode = "went pending, timed out";
                    CancelIo(hPrinter);
                    if (!GetOverlappedResult(hPrinter, &ov, &bytes, TRUE))
                        lastError = GetLastError();
                    ok = FALSE;
                }
            }

            CloseHandle(ov.hEvent);

            if (ok)
            {
                trace << "[RESET] Session-start USB pipe soft reset OK (" << mode << ", "
                      << (GetTickCount64() - start) << " ms, " << bytes
                      << " bytes returned). Building the D4 session on a clean channel.\n";
            }
            else
            {
                trace << "[!] Session-start USB pipe soft reset FAILED (" << mode << ", "
                      << (GetTickCount64() - start) << " ms). " << GetWindowsErrorString(lastError)
                      << " Continuing anyway.\n";
            }
        }

        class WindowsUsbBackend final : public UsbBackend
        {
        public:
            explicit WindowsUsbBackend(std::ostream& trace) : trace_(trace) {}

            ~WindowsUsbBackend() override { Close(); }

            const char* PlatformName() const override { return "Windows"; }

            const std::string& InitError() const override { return initError_; }

            std::vector<UsbCandidate> Enumerate() override
            {
                candidates_ = EnumerateEpsonCandidates(trace_);

                std::vector<UsbCandidate> out;
                for (std::size_t i = 0; i < candidates_.size(); ++i)
                {
                    UsbCandidate c;
                    c.ordinal = i;
                    c.className = candidates_[i].className;
                    c.interfaceNumber = candidates_[i].interfaceIndex;
                    c.path = candidates_[i].path;
                    c.pid = ExtractPid(candidates_[i].path);
                    c.role = candidates_[i].role;
                    c.serviceName = candidates_[i].serviceName;
                    c.pnpClass = candidates_[i].pnpClass;
                    c.instanceId = candidates_[i].instanceId;
                    out.push_back(c);
                }
                return out;
            }

            std::string Describe(std::size_t ordinal) const override
            {
                return (ordinal < candidates_.size()) ? DescribeCandidate(candidates_[ordinal]) : std::string("<invalid candidate>");
            }

            ITransport* Open(std::size_t ordinal, bool softReset) override
            {
                if (ordinal >= candidates_.size())
                    return nullptr;

                ++openCount_;
                trace_ << "[i] Attempting to acquire hardware lock via CreateFile (open #" << openCount_ << " this run)...\n";

                const ULONGLONG start = GetTickCount64();

                // Shared because the Spooler and Epson Status Monitor routinely
                // hold this device open and an exclusive open just fails.
                HANDLE handle = CreateFile(candidates_[ordinal].path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, NULL);

                if (handle == INVALID_HANDLE_VALUE)
                {
                    lastOpenError_ = GetLastError();
                    trace_ << "[!] CreateFile failed! " << GetWindowsErrorString(lastOpenError_) << "\n";
                    return nullptr;
                }

                trace_ << "[SUCCESS] Hardware lock acquired successfully (open #" << openCount_
                       << ", flags OVERLAPPED|WRITE_THROUGH, " << (GetTickCount64() - start) << " ms).\n";

                if (softReset)
                    SoftResetOnOpen(handle, candidates_[ordinal], trace_);

                handle_ = handle;
                transport_ = std::make_unique<WindowsUsbTransport>(handle_, trace_);
                return transport_.get();
            }

            void Close() override
            {
                if (handle_ == INVALID_HANDLE_VALUE)
                    return;

                transport_.reset();
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
                trace_ << "[SUCCESS] Hardware lock released via CloseHandle.\n";
            }

            // A silent handshake earns one more session on a fresh handle
            // before the driver falls through to the next candidate.
            int AttemptsPerCandidate(std::size_t) const override { return 2; }

            std::string QueryDeviceId(std::size_t ordinal) override
            {
                return (ordinal < candidates_.size()) ? QueryCandidateDeviceId(candidates_[ordinal], trace_) : std::string();
            }

            std::string DescribeOpenFailure(bool payloadRun) override
            {
                if (payloadRun)
                {
                    trace_ << "[FATAL] Could not open any Epson interface. Last error: " << GetWindowsErrorString(lastOpenError_) << "\n";

                    if (lastOpenError_ == ERROR_SHARING_VIOLATION)
                    {
                        log::Log(log::Level::Warning, log::Stage::Detect, "usb.busy_status_monitor",
                                 "\n[!] HARDWARE LOCK FAILED: The printer is busy.\n"
                                 "    Please go to your Windows system tray (bottom right),\n"
                                 "    right-click the Epson icon, and exit 'Epson Status Monitor'.");
                    }
                    else if (lastOpenError_ == ERROR_ACCESS_DENIED)
                    {
                        log::Log(log::Level::Warning, log::Stage::Detect, "usb.access_denied",
                                 "\n[!] ACCESS DENIED: Ensure you have administrator rights or the printer is not active in another app.");
                    }
                    else
                    {
                        log::Log(log::Level::Warning, log::Stage::Detect, "usb.open_failed",
                                 "\n[!] CONNECTION ERROR: " + GetWindowsErrorString(lastOpenError_));
                    }
                }
                else
                {
                    trace_ << "[FATAL] Could not open any Epson interface for the query session. Last error: " << GetWindowsErrorString(lastOpenError_) << "\n";
                }

                return "Could not open any Epson USB interface. " + GetWindowsErrorString(lastOpenError_);
            }

        private:
            std::ostream& trace_;
            std::string initError_;
            std::vector<DeviceCandidate> candidates_;
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            std::unique_ptr<WindowsUsbTransport> transport_;
            DWORD lastOpenError_ = ERROR_SUCCESS;
            unsigned openCount_ = 0;
        };

    } // namespace

    std::unique_ptr<UsbBackend> CreateWindowsUsbBackend(std::ostream& trace)
    {
        return std::make_unique<WindowsUsbBackend>(trace);
    }

} // namespace ewr
