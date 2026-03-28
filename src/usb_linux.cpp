#include "ewr/usb.h"
#include <libusb-1.0/libusb.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>

namespace ewr {

    unsigned char EP_IN = 0;
    unsigned char EP_OUT = 0;

    EwrDeviceHandle AutoConnectEpsonPrinter()
    {
        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) < 0)
        {
            std::cerr << "Failed to initialize libusb." << std::endl;
            return nullptr;
        }

        libusb_device** devs;
        ssize_t cnt = libusb_get_device_list(ctx, &devs);
        if (cnt < 0) return nullptr;

        libusb_device_handle* handle = nullptr;

        for (ssize_t i = 0; i < cnt; i++)
        {
            libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(devs[i], &desc) < 0)
                continue;

            if (desc.idVendor == 0x04b8)
            {
                std::cout << "[SUCCESS] Auto-detected Epson Printer (PID: "
                    << std::hex << std::setfill('0') << std::setw(4) << desc.idProduct
                    << ")" << std::dec << std::endl;

                if (libusb_open(devs[i], &handle) == 0)
                {

                    libusb_config_descriptor* config;
                    libusb_get_active_config_descriptor(devs[i], &config);
                    const libusb_interface_descriptor* interdesc = &config->interface[0].altsetting[0];

                    for (int e = 0; e < interdesc->bNumEndpoints; e++)
                    {
                        const libusb_endpoint_descriptor* epdesc = &interdesc->endpoint[e];
                        if (epdesc->bEndpointAddress & LIBUSB_ENDPOINT_IN) EP_IN = epdesc->bEndpointAddress;
                        else EP_OUT = epdesc->bEndpointAddress;
                    }
                    libusb_free_config_descriptor(config);

                    if (libusb_kernel_driver_active(handle, 0) == 1)
                    {
                        std::cout << "Detaching kernel driver (CUPS) for exclusive access..." << std::endl;
                        libusb_detach_kernel_driver(handle, 0);
                    }

                    if (libusb_claim_interface(handle, 0) < 0)
                    {
                        std::cerr << "[!] Failed to claim USB interface." << std::endl;
                        libusb_close(handle);
                        handle = nullptr;
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }

        libusb_free_device_list(devs, 1);

        if (!handle)
            libusb_exit(ctx);

        return static_cast<EwrDeviceHandle>(handle);
    }

    void DisconnectPrinter(EwrDeviceHandle hPrinter)
    {
        if (!hPrinter)
            return;

        libusb_device_handle* handle = static_cast<libusb_device_handle*>(hPrinter);

        libusb_release_interface(handle, 0);
        libusb_attach_kernel_driver(handle, 0);

        libusb_context* ctx = libusb_get_context(libusb_get_device(handle));
        libusb_close(handle);
        libusb_exit(ctx);
    }

    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<PayloadPair>& sequence)
    {
        std::cout << "\nExecuting native Linux hardware state machine..." << std::endl;
        libusb_device_handle* handle = static_cast<libusb_device_handle*>(hPrinter);

        int actual_length;
        unsigned char readBuffer[256];

        for (size_t i = 0; i < sequence.size(); ++i)
        {
            const auto& pair = sequence[i];

            libusb_bulk_transfer(handle, EP_OUT, (unsigned char*)pair.command.data(), pair.command.size(), &actual_length, 2000);
            libusb_bulk_transfer(handle, EP_OUT, (unsigned char*)pair.query.data(), pair.query.size(), &actual_length, 2000);

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            int totalBytesReturned = 0;
            while (true)
            {
                int res = libusb_bulk_transfer(handle, EP_IN, readBuffer, sizeof(readBuffer), &actual_length, 50);
                if (res == LIBUSB_ERROR_TIMEOUT || actual_length == 0) break;
                if (res == 0) totalBytesReturned += actual_length;
                else break;
            }

            std::cout << "-> Pair " << i + 1 << " / " << sequence.size()
                << " | Cleared " << totalBytesReturned << " bytes from pipe." << std::endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }
}