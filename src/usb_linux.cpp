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
        if (libusb_init(nullptr) < 0) 
        {
            std::cerr << "Failed to initialize libusb." << std::endl;
            return nullptr;
        }

        libusb_device** devs;
        ssize_t cnt = libusb_get_device_list(nullptr, &devs);
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
            libusb_exit(nullptr);

        return static_cast<EwrDeviceHandle>(handle);
    }

    void DisconnectPrinter(EwrDeviceHandle hPrinter)
    {
        if (!hPrinter)
            return;

        libusb_device_handle* handle = static_cast<libusb_device_handle*>(hPrinter);

        libusb_release_interface(handle, 0);
        libusb_attach_kernel_driver(handle, 0);

        libusb_close(handle);
        libusb_exit(nullptr);
    }

    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence) {
        std::cout << "\nExecuting universal Linux hardware state machine..." << std::endl;
        libusb_device_handle* handle = static_cast<libusb_device_handle*>(hPrinter);

        int actual_length;
        unsigned char readBuffer[256];

        for (size_t i = 0; i < sequence.size(); ++i) {

            int write_status = libusb_bulk_transfer(handle, EP_OUT, (unsigned char*)sequence[i].data(), sequence[i].size(), &actual_length, 2000);

            if (write_status != 0) {
                std::cerr << "Failed to send packet " << i + 1 << " (libusb error: " << write_status << ")" << std::endl;
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            int totalBytesReturned = 0;
            while (true) {
                int read_status = libusb_bulk_transfer(handle, EP_IN, readBuffer, sizeof(readBuffer), &actual_length, 50);

                if (read_status == LIBUSB_ERROR_TIMEOUT || actual_length == 0) {
                    break;
                }

                if (read_status == 0) {
                    totalBytesReturned += actual_length;
                }
                else {
                    break;
                }
            }

            if (totalBytesReturned > 0) {
                std::cout << "-> Packet " << i + 1 << " / " << sequence.size()
                    << " | Triggered ACK: Cleared " << totalBytesReturned << " bytes." << std::endl;
            }
            else {
                std::cout << "-> Packet " << i + 1 << " / " << sequence.size()
                    << " | Sent. (No ACK)" << std::endl;
            }
        }
        return true;
    }
}
