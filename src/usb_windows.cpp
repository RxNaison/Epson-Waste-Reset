#include "ewr/usb.h"
#include "ewr/executor.h"
#include "ewr/generator.h"
#include <setupapi.h>
#include <initguid.h>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_USBPRINT, 0x28d78fad, 0x5a12, 0x11d1, 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2);

namespace ewr {

    void LogToTrace(const std::string& message)
    {
        std::ofstream logFile("ewr_trace.log", std::ios::app);
        if (logFile.is_open())
            logFile << message << "\n";
    }

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
            default:
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "Error Code %lu", errorCode);
                return buf;
            }
        }
    }

    void DisconnectPrinter(EwrDeviceHandle hPrinter)
    {
        if (hPrinter && hPrinter != INVALID_HANDLE_VALUE)
        {
            CloseHandle(static_cast<HANDLE>(hPrinter));
            std::cout << "Hardware lock released." << std::endl;
            LogToTrace("[SUCCESS] Hardware lock released via CloseHandle.");
        }
    }

    EwrDeviceHandle AutoConnectEpsonPrinter()
    {
        HANDLE hPrinter = INVALID_HANDLE_VALUE;
        {
            std::ofstream logFile("ewr_trace.log", std::ios::out | std::ios::trunc);
            if (logFile.is_open()) 
            {
                logFile << "==================================================\n";
                logFile << "EWR HARDWARE TRACE LOG (Windows Mode)\n";
                logFile << "==================================================\n\n";
            }
        }

        struct GuidEntry
        {
            GUID guid;
            const char* name;
            int classPriority; // Lower number = higher priority for printer maintenance
        };

        const GuidEntry SCAN_GUIDS[] = {
            // GUID_DEVINTERFACE_USBPRINT (Standard USB Printer class)
            { { 0x28d78fad, 0x5a12, 0x11d1, { 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2 } }, "USBPRINT", 0 },
            // GUID_DEVINTERFACE_IMAGE (Standard USB Scanner/Image class)
            { { 0x6bdd1fc6, 0x810f, 0x11d0, { 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } }, "IMAGE", 10 },
            // GUID_DEVINTERFACE_USB_DEVICE (Generic USB device class)
            { { 0xa5cd7fef, 0x35b7, 0x11d0, { 0xb4, 0x20, 0x00, 0xc0, 0x4f, 0x79, 0xaa, 0xf1 } }, "USB_DEVICE", 20 },
            // GUID_DEVINTERFACE_WINUSB (Standard WinUSB class)
            { { 0xdee0c8d9, 0xba4e, 0x46c5, { 0x9a, 0x2a, 0x7d, 0x35, 0x9e, 0x80, 0xb4, 0xeb } }, "WINUSB", 30 }
        };

        struct DeviceCandidate
        {
            std::string path;
            std::string className;
            int classPriority;
            int interfaceIndex;
        };

        std::vector<DeviceCandidate> candidates;
        size_t totalDevices = 0;

        LogToTrace("[i] Scanning for Epson USB interfaces across multiple GUID classes...");

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

            LogToTrace("  [i] Enrolling class: " + std::string(guidStr) + " (" + entry.name + ")");

            for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &entry.guid, i, &devInterfaceData); ++i)
            {
                totalDevices++;
                DWORD requiredSize = 0;
                SetupDiGetDeviceInterfaceDetail(hDevInfo, &devInterfaceData, NULL, 0, &requiredSize, NULL);

                std::vector<BYTE> detailDataBuffer(requiredSize);
                PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)detailDataBuffer.data();
                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

                if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &devInterfaceData, detailData, requiredSize, NULL, NULL))
                {
                    std::string devicePath = detailData->DevicePath;
                    std::string devicePathLower = devicePath;
                    std::transform(devicePathLower.begin(), devicePathLower.end(), devicePathLower.begin(), ::tolower);

                    if (devicePathLower.find("vid_04b8") != std::string::npos)
                    {
                        LogToTrace("     -> Matches Epson Vendor ID (vid_04b8): " + devicePath + " [class: " + entry.name + "]");

                        bool alreadyKnown = false;
                        for (const auto& c : candidates)
                        {
                            if (c.path == devicePath)
                            {
                                alreadyKnown = true;
                                break;
                            }
                        }

                        if (!alreadyKnown)
                        {
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

                            candidates.push_back({ devicePath, entry.name, entry.classPriority, ifaceIdx });
                        }
                    }
                }
                else
                {
                    DWORD err = GetLastError();
                    LogToTrace("     [!] SetupDiGetDeviceInterfaceDetail failed. " + GetWindowsErrorString(err));
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }

        LogToTrace("[i] Total unique Epson (VID_04B8) candidates discovered: " + std::to_string(candidates.size()));

        for (const auto& c : candidates)
            LogToTrace("     [" + c.className + " | mi_" + (c.interfaceIndex >= 0 ? std::to_string(c.interfaceIndex) : "N/A") + "] " + c.path);

        // CLASS-AWARE INTERFACE SELECTION
        //
        // Priority order (highest to lowest):
        //   1. USBPRINT class + mi_00  (ideal: printer engine on primary interface)
        //   2. USBPRINT class + any mi (printer engine on secondary interface, e.g. L3250)
        //   3. USBPRINT class + non-composite (single-function printer)
        //   4. Non-USBPRINT class + mi_00 (last resort: might be scanner - logs a warning)
        //   5. First detected path (absolute fallback)

        std::string selectedPath;
        std::string selectionReason;

        if (!candidates.empty())
        {
            std::sort(candidates.begin(), candidates.end(), [](const DeviceCandidate& a, const DeviceCandidate& b)
                {
                    if (a.classPriority != b.classPriority)
                        return a.classPriority < b.classPriority;

                    int aIdx = (a.interfaceIndex >= 0) ? a.interfaceIndex : 9999;
                    int bIdx = (b.interfaceIndex >= 0) ? b.interfaceIndex : 9999;
                    return aIdx < bIdx;
                });

            selectedPath = candidates[0].path;
            selectionReason = "Selected via class-priority sort: " + candidates[0].className + " class";
            if (candidates[0].interfaceIndex >= 0)
                selectionReason += ", interface mi_0" + std::to_string(candidates[0].interfaceIndex);
            else
                selectionReason += ", non-composite device";

            if (candidates[0].classPriority > 0)
            {
                LogToTrace("[WARNING] No USBPRINT class interface found. Using " + candidates[0].className + " class as fallback.");
                LogToTrace("          This may target the scanner instead of the printer maintenance engine.");
                LogToTrace("          Consider reinstalling official Epson printer drivers.");
            }
        }
        else
        {
            LogToTrace("[!] Auto-detection error: No Epson devices detected. Please verify USB connection, power state, and drivers.");
        }

        if (!selectedPath.empty())
        {
            std::string selectedPathLower = selectedPath;
            std::transform(selectedPathLower.begin(), selectedPathLower.end(), selectedPathLower.begin(), ::tolower);
            size_t pidPos = selectedPathLower.find("pid_");
            std::string pid = (pidPos != std::string::npos && pidPos + 8 <= selectedPathLower.length()) ? selectedPathLower.substr(pidPos + 4, 4) : "UNKNOWN";

            std::cout << "[SUCCESS] Auto-detected Epson Printer (PID: " << pid << ")" << std::endl;

            LogToTrace("\n[Selection Decision]");
            LogToTrace("  Selected Path: " + selectedPath);
            LogToTrace("  Reason:        " + selectionReason);
            LogToTrace("  Product ID:    0x" + pid + "\n");

            LogToTrace("[i] Attempting to acquire hardware lock via CreateFile...");
            hPrinter = CreateFile(selectedPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

            if (hPrinter == INVALID_HANDLE_VALUE)
            {
                DWORD err = GetLastError();
                LogToTrace("[FATAL] CreateFile failed! " + GetWindowsErrorString(err));

                if (err == ERROR_SHARING_VIOLATION)
                {
                    std::cerr << "\n[!] HARDWARE LOCK FAILED: The printer is busy." << std::endl;
                    std::cerr << "    Please go to your Windows system tray (bottom right)," << std::endl;
                    std::cerr << "    right-click the Epson icon, and exit 'Epson Status Monitor'." << std::endl;
                }
                else if (err == ERROR_ACCESS_DENIED) 
                {
                    std::cerr << "\n[!] ACCESS DENIED: Ensure you have administrator rights or the printer is not active in another app." << std::endl;
                }
                else 
                {
                    std::cerr << "\n[!] CONNECTION ERROR: " << GetWindowsErrorString(err) << std::endl;
                }
            }
            else
            {
                LogToTrace("[SUCCESS] Hardware lock acquired successfully. Handle: " + std::to_string((uintptr_t)hPrinter));
            }
        }

        return hPrinter == INVALID_HANDLE_VALUE ? nullptr : static_cast<EwrDeviceHandle>(hPrinter);
    }

    namespace {

        bool AsyncWrite(HANDLE hPrinter, const std::vector<unsigned char>& data)
        {
            DWORD bytesWritten = 0;
            OVERLAPPED osWrite = { 0 };
            osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            if (!osWrite.hEvent)
            {
                DWORD err = GetLastError();
                LogToTrace("[!] AsyncWrite: CreateEvent failed. " + GetWindowsErrorString(err));
                return false;
            }

            bool success = WriteFile(hPrinter, data.data(), data.size(), &bytesWritten, &osWrite);
            if (!success)
            {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING)
                {
                    DWORD waitResult = WaitForSingleObject(osWrite.hEvent, 2000);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        success = GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, FALSE);
                        if (!success) 
                        {
                            DWORD overlapErr = GetLastError();
                            LogToTrace("[!] AsyncWrite: GetOverlappedResult failed after completion. " + GetWindowsErrorString(overlapErr));
                        }
                    }
                    else if (waitResult == WAIT_TIMEOUT)
                    {
                        LogToTrace("[!] AsyncWrite: WaitForSingleObject timed out (2000ms limit reached). Cancelling I/O...");
                        CancelIo(hPrinter);
                        GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, TRUE);
                        success = false;
                    } 
                    else 
                    {
                        DWORD waitErr = GetLastError();
                        LogToTrace("[!] AsyncWrite: WaitForSingleObject failed with error: " + std::to_string(waitResult) + ". " + GetWindowsErrorString(waitErr));
                        CancelIo(hPrinter);
                        GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, TRUE);
                        success = false;
                    }
                }
                else 
                {
                    LogToTrace("[!] AsyncWrite: WriteFile failed immediately. " + GetWindowsErrorString(err));
                }
            }

            CloseHandle(osWrite.hEvent);

            if (success && bytesWritten != data.size()) 
            {
                LogToTrace("[!] AsyncWrite: Write reported success but bytesWritten (" + std::to_string(bytesWritten) + ") != expected size (" + std::to_string(data.size()) + ").");
                return false;
            }

            return success;
        }

        std::vector<unsigned char> AsyncDrainBuffer(HANDLE hPrinter)
        {
            std::vector<unsigned char> totalData;
            BYTE buffer[256];

            while (true)
            {
                DWORD bytesRead = 0;
                OVERLAPPED osRead = { 0 };
                osRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

                if (!osRead.hEvent)
                {
                    DWORD err = GetLastError();
                    LogToTrace("[!] AsyncDrainBuffer: CreateEvent failed. " + GetWindowsErrorString(err));
                    break;
                }

                bool success = ReadFile(hPrinter, buffer, sizeof(buffer), &bytesRead, &osRead);
                if (!success)
                {
                    DWORD err = GetLastError();
                    if (err == ERROR_IO_PENDING)
                    {
                        DWORD waitResult = WaitForSingleObject(osRead.hEvent, 250);
                        if (waitResult == WAIT_OBJECT_0)
                        {
                            success = GetOverlappedResult(hPrinter, &osRead, &bytesRead, FALSE);
                            if (!success)
                            {
                                DWORD overlapErr = GetLastError();
                                LogToTrace("[!] AsyncDrainBuffer: GetOverlappedResult failed after completion. " + GetWindowsErrorString(overlapErr));
                            }
                        } 
                        else if (waitResult == WAIT_TIMEOUT)
                        {
                            CancelIo(hPrinter);
                            GetOverlappedResult(hPrinter, &osRead, &bytesRead, TRUE);
                        }
                        else 
                        {
                            DWORD waitErr = GetLastError();
                            LogToTrace("[!] AsyncDrainBuffer: WaitForSingleObject failed. " + GetWindowsErrorString(waitErr));
                            CancelIo(hPrinter);
                            GetOverlappedResult(hPrinter, &osRead, &bytesRead, TRUE);
                        }
                    }
                    else
                    {
                        LogToTrace("[!] AsyncDrainBuffer: ReadFile failed immediately. " + GetWindowsErrorString(err));
                    }
                }

                CloseHandle(osRead.hEvent);

                if (bytesRead == 0)
                    break;

                totalData.insert(totalData.end(), buffer, buffer + bytesRead);
            }
            return totalData;
        }

        class WindowsUsbTransport final : public ITransport
        {
        public:
            explicit WindowsUsbTransport(HANDLE handle) : handle_(handle) {}

            bool Send(const std::vector<unsigned char>& packet) override
            {
                return AsyncWrite(handle_, packet);
            }

            std::vector<unsigned char> Drain() override
            {
                return AsyncDrainBuffer(handle_);
            }

        private:
            HANDLE handle_;
        };

    } // namespace

    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence)
    {
        std::cout << "\nExecuting universal Windows hardware state machine..." << std::endl;
        std::cout << "[i] Saving hardware trace to ewr_trace.log for diagnostics." << std::endl;

        std::ofstream logFile("ewr_trace.log", std::ios::app);
        logFile << "\n==================================================\n";
        logFile << "BEGIN PAYLOAD SEQUENCE EXECUTION\n";
        logFile << "Total Packets: " << sequence.size() << "\n";
        logFile << "==================================================\n\n";

        WindowsUsbTransport transport(static_cast<HANDLE>(hPrinter));
        ExecutionResult result = ExecuteSequence(transport, sequence, std::cout, logFile);

        logFile << "==================================================\n";
        logFile << "SEQUENCE COMPLETE\n";
        logFile << "Packets sent:       " << result.packetsSent << " / " << sequence.size() << "\n";
        logFile << "EEPROM writes:      " << result.writesVerified << " verified / " << result.writesTotal << " total\n";
        logFile << "Result:             " << (result.success ? "SUCCESS" : ("FAILED - " + result.error)) << "\n";
        logFile << "==================================================\n";

        if (!result.success)
        {
            std::cerr << "\n[ERROR] " << result.error << std::endl;
            std::cerr << "[!] The waste counter was NOT confirmed as reset." << std::endl;
            std::cerr << "    Check ewr_trace.log for the full hardware trace." << std::endl;
        }

        return result.success;
    }

} // namespace ewr
