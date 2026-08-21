#include "ewr/usb.h"
#include "ewr/executor.h"
#include "ewr/generator.h"
// Distro packages install the header under libusb-1.0/; Homebrew relies on the
// pkg-config include dir already pointing inside that folder.
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace ewr {

    namespace {

        struct LinuxDeviceContext
        {
            libusb_device_handle* handle = nullptr;
            int interfaceNumber = -1;
            unsigned char epIn = 0;
            unsigned char epOut = 0;
        };

        class LinuxUsbTransport final : public ITransport
        {
        public:
            explicit LinuxUsbTransport(LinuxDeviceContext& ctx) : ctx_(ctx) {}

            bool Send(const std::vector<unsigned char>& packet) override
            {
                int transferred = 0;
                int status = libusb_bulk_transfer(ctx_.handle, ctx_.epOut,
                    const_cast<unsigned char*>(packet.data()),
                    static_cast<int>(packet.size()), &transferred, 2000);

                return status == 0 && transferred == static_cast<int>(packet.size());
            }

            std::vector<unsigned char> Drain() override
            {
                std::vector<unsigned char> data;
                unsigned char buffer[256];

                while (true)
                {
                    int received = 0;
                    int status = libusb_bulk_transfer(ctx_.handle, ctx_.epIn,
                        buffer, sizeof(buffer), &received, 250);

                    if (received > 0)
                        data.insert(data.end(), buffer, buffer + received);

                    if (status != 0 || received == 0)
                        break;
                }

                return data;
            }

        private:
            LinuxDeviceContext& ctx_;
        };

    } // namespace

    EwrDeviceHandle AutoConnectEpsonPrinter()
    {
        if (libusb_init(nullptr) < 0)
        {
            std::cerr << "Failed to initialize libusb." << std::endl;
            return nullptr;
        }

        libusb_device** devs = nullptr;
        ssize_t cnt = libusb_get_device_list(nullptr, &devs);
        if (cnt < 0)
        {
            libusb_exit(nullptr);
            return nullptr;
        }

        std::ofstream logFile("ewr_trace.log", std::ios::out | std::ios::trunc);
        LinuxDeviceContext* connected = nullptr;

        for (ssize_t i = 0; i < cnt && !connected; i++)
        {
            libusb_device_descriptor desc;

            if (libusb_get_device_descriptor(devs[i], &desc) < 0)
                continue;

            if (desc.idVendor != EPSON_VID)
                continue;

            libusb_device_handle* handle = nullptr;
            if (libusb_open(devs[i], &handle) != 0)
                continue;

            libusb_config_descriptor* config = nullptr;
            if (libusb_get_active_config_descriptor(devs[i], &config) != 0 || !config)
            {
                libusb_close(handle);
                continue;
            }

            int selected_interface = -1;
            unsigned char selected_ep_in = 0;
            unsigned char selected_ep_out = 0;
            bool found_printer_class = false;

            for (int iface_idx = 0; iface_idx < config->bNumInterfaces; iface_idx++)
            {
                const libusb_interface_descriptor* interdesc = &config->interface[iface_idx].altsetting[0];

                if (interdesc->bInterfaceClass == LIBUSB_CLASS_PRINTER || interdesc->bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC)
                {
                    unsigned char ep_in = 0;
                    unsigned char ep_out = 0;

                    for (int e = 0; e < interdesc->bNumEndpoints; e++)
                    {
                        const libusb_endpoint_descriptor* epdesc = &interdesc->endpoint[e];
                        if ((epdesc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK)
                        {
                            if (epdesc->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                                ep_in = epdesc->bEndpointAddress;
                            else
                                ep_out = epdesc->bEndpointAddress;
                        }
                    }

                    if (ep_in != 0 && ep_out != 0)
                    {
                        if (interdesc->bInterfaceClass == LIBUSB_CLASS_PRINTER)
                        {
                            selected_interface = iface_idx;
                            selected_ep_in = ep_in;
                            selected_ep_out = ep_out;
                            found_printer_class = true;
                            break;
                        }
                        else if (!found_printer_class && selected_interface == -1)
                        {
                            selected_interface = iface_idx;
                            selected_ep_in = ep_in;
                            selected_ep_out = ep_out;
                        }
                    }
                }
            }

            libusb_free_config_descriptor(config);

            if (selected_interface == -1)
            {
                libusb_close(handle);
                continue;
            }

            std::cout << "[SUCCESS] Auto-detected Epson Printer (PID: " << std::hex << std::setfill('0') << std::setw(4) << desc.idProduct << ")" << std::dec << std::endl;

            if (logFile.is_open())
            {
                logFile << "EWR Hardware Trace Log (libusb)\n";
                logFile << "==================================================\n";
                logFile << "Target VID: 0x04B8 | PID: 0x" << std::hex << desc.idProduct << std::dec << "\n";
                logFile << "Target Interface: " << selected_interface << (found_printer_class ? " (Printer Class)" : " (Vendor-Specific Class)") << "\n";
                logFile << "Endpoint OUT: 0x" << std::hex << (int)selected_ep_out << " | Endpoint IN: 0x" << (int)selected_ep_in << std::dec << "\n";
                logFile << "==================================================\n\n";
            }

            if (libusb_kernel_driver_active(handle, selected_interface) == 1)
            {
                std::cout << "Detaching kernel driver (CUPS) for exclusive access..." << std::endl;
                libusb_detach_kernel_driver(handle, selected_interface);
            }

            if (libusb_claim_interface(handle, selected_interface) < 0)
            {
                std::cerr << "[!] Failed to claim USB interface." << std::endl;
                libusb_close(handle);
                continue;
            }

            connected = new LinuxDeviceContext{ handle, selected_interface, selected_ep_in, selected_ep_out };
        }

        libusb_free_device_list(devs, 1);

        if (!connected)
            libusb_exit(nullptr);

        return static_cast<EwrDeviceHandle>(connected);
    }

    void DisconnectPrinter(EwrDeviceHandle hPrinter)
    {
        if (!hPrinter)
            return;

        LinuxDeviceContext* ctx = static_cast<LinuxDeviceContext*>(hPrinter);

        if (ctx->interfaceNumber != -1)
        {
            libusb_release_interface(ctx->handle, ctx->interfaceNumber);
            libusb_attach_kernel_driver(ctx->handle, ctx->interfaceNumber);
        }

        libusb_close(ctx->handle);
        libusb_exit(nullptr);
        delete ctx;
    }

    bool ExecutePayloadSequence(EwrDeviceHandle hPrinter, const std::vector<std::vector<unsigned char>>& sequence)
    {
        std::cout << "\nExecuting universal libusb hardware state machine..." << std::endl;
        std::cout << "[i] Saving hardware trace to ewr_trace.log for diagnostics." << std::endl;

        LinuxDeviceContext* ctx = static_cast<LinuxDeviceContext*>(hPrinter);

        std::ofstream logFile("ewr_trace.log", std::ios::app);
        logFile << "\n==================================================\n";
        logFile << "BEGIN PAYLOAD SEQUENCE EXECUTION\n";
        logFile << "Total Packets: " << sequence.size() << "\n";
        logFile << "==================================================\n\n";

        LinuxUsbTransport transport(*ctx);
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
