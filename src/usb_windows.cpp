#include "ewr/usb.h"
#include <setupapi.h>
#include <initguid.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_USBPRINT, 0x28d78fad, 0x5a12, 0x11d1, 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2);

namespace ewr {

    void DisconnectPrinter(EwrDeviceHandle hPrinter)
    {
        if (hPrinter && hPrinter != INVALID_HANDLE_VALUE)
        {
            CloseHandle(static_cast<HANDLE>(hPrinter));
            std::cout << "Hardware lock released." << std::endl;
        }
    }

    HANDLE AutoConnectEpsonPrinter()
    {
        HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_USBPRINT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE)
        {
            std::cerr << "Failed to query Windows USB devices." << std::endl;
            return nullptr;
        }

        SP_DEVICE_INTERFACE_DATA devInterfaceData;
        devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        HANDLE hPrinter = INVALID_HANDLE_VALUE;

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_USBPRINT, i, &devInterfaceData); ++i)
        {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetail(hDevInfo, &devInterfaceData, NULL, 0, &requiredSize, NULL);

            std::vector<BYTE> detailDataBuffer(requiredSize);
            PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)detailDataBuffer.data();
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &devInterfaceData, detailData, requiredSize, NULL, NULL))
            {
                std::string devicePath = detailData->DevicePath;
                std::transform(devicePath.begin(), devicePath.end(), devicePath.begin(), ::tolower);

                if (devicePath.find("vid_04b8") != std::string::npos)
                {
                    size_t pidPos = devicePath.find("pid_");
                    if (pidPos != std::string::npos && pidPos + 8 <= devicePath.length())
                        std::cout << "[SUCCESS] Auto-detected Epson Printer (PID: " << devicePath.substr(pidPos + 4, 4) << ")" << std::endl;

                    hPrinter = CreateFile(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

                    if (hPrinter == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
                    {
                        std::cerr << "\n[!] HARDWARE LOCK FAILED: The printer is busy." << std::endl;
                        std::cerr << "    Please go to your Windows system tray (bottom right)," << std::endl;
                        std::cerr << "    right-click the Epson icon, and exit 'Epson Status Monitor'." << std::endl;
                        std::cerr << "    Also ensure no documents are stuck in the print queue." << std::endl;
                    }
                    break;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);

        if (hPrinter == INVALID_HANDLE_VALUE) {
            return nullptr;
        }

        return hPrinter;
    }

    bool AsyncWrite(HANDLE hPrinter, const std::vector<unsigned char>& data)
    {
        DWORD bytesWritten = 0;
        OVERLAPPED osWrite = { 0 };
        osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        bool success = WriteFile(hPrinter, data.data(), data.size(), &bytesWritten, &osWrite);
        if (!success && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(osWrite.hEvent, 2000) == WAIT_OBJECT_0)
            {
                success = GetOverlappedResult(hPrinter, &osWrite, &bytesWritten, FALSE);
            }
            else
            {
                CancelIo(hPrinter);
                success = false;
            }
        }
        CloseHandle(osWrite.hEvent);
        return success && (bytesWritten == data.size());
    }

    DWORD AsyncDrainBuffer(HANDLE hPrinter)
    {
        DWORD totalRead = 0;
        BYTE buffer[256];

        while (true)
        {
            DWORD bytesRead = 0;
            OVERLAPPED osRead = { 0 };
            osRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            bool success = ReadFile(hPrinter, buffer, sizeof(buffer), &bytesRead, &osRead);
            if (!success && GetLastError() == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(osRead.hEvent, 50) == WAIT_OBJECT_0)
                {
                    GetOverlappedResult(hPrinter, &osRead, &bytesRead, FALSE);
                }
                else
                {
                    CancelIo(hPrinter);
                    GetOverlappedResult(hPrinter, &osRead, &bytesRead, FALSE);
                }
            }
            CloseHandle(osRead.hEvent);

            if (bytesRead == 0)
                break;

            totalRead += bytesRead;
        }
        return totalRead;
    }

    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<PayloadPair>& sequence)
    {
        std::cout << "\nExecuting native hardware state machine..." << std::endl;

        HANDLE winHandle = static_cast<HANDLE>(hPrinter);

        for (size_t i = 0; i < sequence.size(); ++i)
        {
            const auto& pair = sequence[i];

            if (!AsyncWrite(winHandle, pair.command))
            {
                std::cerr << "Failed to send command on pair " << i + 1 << std::endl;
                return false;
            }

            if (!AsyncWrite(winHandle, pair.query))
            {
                std::cerr << "Failed to send query on pair " << i + 1 << std::endl;
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            DWORD totalBytesReturned = AsyncDrainBuffer(winHandle);

            std::cout << "-> Pair " << i + 1 << " / " << sequence.size()
                << " | Cleared " << totalBytesReturned << " bytes from pipe." << std::endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }
}