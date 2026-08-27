#include "ewr/usb.h"
#include "ewr/usb_backend.h"
#include "ewr/usb_timing.h"
#include "ewr/deviceid.h"
#include "ewr/log.h"
// Distro packages install the header under libusb-1.0/; Homebrew relies on
// the pkg-config include dir already pointing inside that folder.
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Linux USB backend: libusb enumeration (printer-class first,
// vendor-specific fallback) and raw bulk I/O. The run choreography (pin,
// retry, fallback, banners) lives in usb_driver.cpp; see ewr/usb_backend.h
// for the seam.

namespace ewr {

    namespace {

        using namespace usb_timing;

        struct LinuxCandidate
        {
            libusb_device* device = nullptr; // borrowed ref, owned by the device list
            uint16_t pid = 0;
            int interfaceNumber = -1;
            unsigned char epIn = 0;
            unsigned char epOut = 0;
            bool printerClass = false;
        };

        struct OpenInterface
        {
            libusb_device_handle* handle = nullptr;
            int interfaceNumber = -1;
            bool detachedKernelDriver = false;
        };

        class LinuxUsbTransport final : public ITransport
        {
        public:
            LinuxUsbTransport(libusb_device_handle* handle, unsigned char epIn, unsigned char epOut, std::ostream& trace)
                : handle_(handle), epIn_(epIn), epOut_(epOut), trace_(trace) {}

            bool Send(const std::vector<unsigned char>& packet) override
            {
                const auto start = std::chrono::steady_clock::now();

                int transferred = 0;
                int status = libusb_bulk_transfer(handle_, epOut_,
                    const_cast<unsigned char*>(packet.data()),
                    static_cast<int>(packet.size()), &transferred, kWriteTimeoutMs);

                const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();

                if (status == 0)
                {
                    trace_ << "[io] OUT " << transferred << "/" << packet.size() << " bytes in "
                           << elapsedMs << " ms.\n";
                }
                else
                {
                    trace_ << "[!] Bulk OUT failed after " << elapsedMs << " ms: "
                           << libusb_error_name(status) << " (" << transferred << "/"
                           << packet.size() << " bytes transferred).\n";
                }

                return status == 0 && transferred == static_cast<int>(packet.size());
            }

            std::vector<unsigned char> Drain(int timeoutMs) override
            {
                // timeoutMs bounds only the wait for the first bytes; follow-up
                // reads collect the rest of an already-flowing burst.
                //
                // Unlike usbprint.sys, libusb_bulk_transfer genuinely blocks
                // until data arrives or the window expires, so one read per
                // window is enough - no re-post polling here.
                std::vector<unsigned char> data;
                unsigned char buffer[kDrainReadChunkBytes];
                unsigned int readTimeoutMs = (timeoutMs > 0) ? static_cast<unsigned int>(timeoutMs) : 0;

                const auto start = std::chrono::steady_clock::now();
                const auto sinceStartMs = [&]() {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
                };

                int dataReads = 0;
                int lastStatus = 0;
                long long firstDataMs = -1;

                for (int reads = 0; reads < kMaxDrainReads && data.size() < kMaxDrainBytes; ++reads)
                {
                    int received = 0;
                    int status = libusb_bulk_transfer(handle_, epIn_,
                        buffer, sizeof(buffer), &received, readTimeoutMs);
                    lastStatus = status;

                    if (received > 0)
                    {
                        if (firstDataMs < 0)
                            firstDataMs = sinceStartMs();

                        trace_ << "[io] IN " << received << " bytes at +" << sinceStartMs()
                               << " ms (" << libusb_error_name(status) << ").\n";

                        data.insert(data.end(), buffer, buffer + received);
                        ++dataReads;
                    }

                    if (status != 0 || received == 0)
                        break;

                    readTimeoutMs = kDrainFollowUpTimeoutMs;
                }

                trace_ << "[io] Drain summary: " << data.size() << " bytes in " << dataReads
                       << " data read(s) over " << sinceStartMs() << " ms (first-byte window "
                       << timeoutMs << " ms); ";
                if (firstDataMs >= 0)
                    trace_ << "first data at +" << firstDataMs << " ms; ";
                else
                    trace_ << "no data arrived; ";
                trace_ << "last status: " << libusb_error_name(lastStatus) << ".\n";

                // Logged here rather than at the call sites: a caller may
                // discard a drain without ever looking at it.
                if (!data.empty())
                    trace_ << "[io] IN payload (" << data.size() << " bytes):\n"
                           << HexDumpCapped(data.data(), data.size(), kTraceDumpCapBytes);

                return data;
            }

        private:
            libusb_device_handle* handle_;
            unsigned char epIn_;
            unsigned char epOut_;
            std::ostream& trace_;
        };

        // Printer class first, vendor-specific as fallback; each candidate
        // needs both a bulk IN and a bulk OUT endpoint.
        std::vector<LinuxCandidate> CollectCandidates(libusb_device** devs, ssize_t cnt)
        {
            std::vector<LinuxCandidate> candidates;

            for (ssize_t i = 0; i < cnt; i++)
            {
                libusb_device_descriptor desc;

                if (libusb_get_device_descriptor(devs[i], &desc) < 0)
                    continue;

                if (desc.idVendor != EPSON_VID)
                    continue;

                libusb_config_descriptor* config = nullptr;
                if (libusb_get_active_config_descriptor(devs[i], &config) != 0 || !config)
                    continue;

                for (int iface_idx = 0; iface_idx < config->bNumInterfaces; iface_idx++)
                {
                    const libusb_interface_descriptor* interdesc = &config->interface[iface_idx].altsetting[0];

                    if (interdesc->bInterfaceClass != LIBUSB_CLASS_PRINTER
                        && interdesc->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC)
                        continue;

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
                        LinuxCandidate cand;
                        cand.device = devs[i];
                        cand.pid = desc.idProduct;
                        cand.interfaceNumber = iface_idx;
                        cand.epIn = ep_in;
                        cand.epOut = ep_out;
                        cand.printerClass = (interdesc->bInterfaceClass == LIBUSB_CLASS_PRINTER);
                        candidates.push_back(cand);
                    }
                }

                libusb_free_config_descriptor(config);
            }

            std::stable_sort(candidates.begin(), candidates.end(),
                [](const LinuxCandidate& a, const LinuxCandidate& b)
                {
                    if (a.printerClass != b.printerClass)
                        return a.printerClass;
                    return a.interfaceNumber < b.interfaceNumber;
                });

            return candidates;
        }

        bool OpenCandidate(const LinuxCandidate& cand, OpenInterface& open)
        {
            libusb_device_handle* handle = nullptr;
            if (libusb_open(cand.device, &handle) != 0)
                return false;

            bool detached = false;
            if (libusb_kernel_driver_active(handle, cand.interfaceNumber) == 1)
            {
                log::Log(log::Level::Info, log::Stage::Detect, "usb.kernel_driver_detach",
                         "Detaching kernel driver (CUPS) for exclusive access...");
                if (libusb_detach_kernel_driver(handle, cand.interfaceNumber) == 0)
                    detached = true;
            }

            if (libusb_claim_interface(handle, cand.interfaceNumber) < 0)
            {
                log::Log(log::Level::Warning, log::Stage::Detect, "usb.claim_failed",
                         "[!] Failed to claim USB interface " + std::to_string(cand.interfaceNumber) + ".");

                if (detached)
                    libusb_attach_kernel_driver(handle, cand.interfaceNumber);

                libusb_close(handle);
                return false;
            }

            open.handle = handle;
            open.interfaceNumber = cand.interfaceNumber;
            open.detachedKernelDriver = detached;
            return true;
        }

        void CloseCandidate(OpenInterface& open)
        {
            if (!open.handle)
                return;

            libusb_release_interface(open.handle, open.interfaceNumber);

            // Only hand the interface back to the kernel (CUPS) if we were the
            // ones who detached it; re-attaching a driver that was never bound
            // can bind usblp to an interface the user intentionally left free.
            if (open.detachedKernelDriver)
                libusb_attach_kernel_driver(open.handle, open.interfaceNumber);

            libusb_close(open.handle);
            open = OpenInterface{};
        }

        // GET_DEVICE_ID is a printer-class request, so vendor-specific
        // interfaces return "" rather than being asked.
        std::string QueryCandidateDeviceId(const LinuxCandidate& cand, std::ostream& trace)
        {
            if (!cand.printerClass)
                return "";

            OpenInterface open;
            if (!OpenCandidate(cand, open))
            {
                trace << "[!] Device ID query: could not open/claim interface " << cand.interfaceNumber << ".\n";
                return "";
            }

            // USB printer class GET_DEVICE_ID: class request 0 on the
            // interface, wIndex = (interface << 8) | alternate setting (0).
            unsigned char buffer[2048] = { 0 };
            const int received = libusb_control_transfer(open.handle,
                LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
                0 /* GET_DEVICE_ID */, 0 /* config */,
                static_cast<uint16_t>(cand.interfaceNumber << 8),
                buffer, sizeof(buffer), kDeviceIdTimeoutMs);

            CloseCandidate(open);

            if (received <= 0)
            {
                trace << "[!] GET_DEVICE_ID failed on interface " << cand.interfaceNumber << " (status " << received << ").\n";
                return "";
            }

            return ExtractDeviceIdString(buffer, static_cast<size_t>(received));
        }

        class LinuxUsbBackend final : public UsbBackend
        {
        public:
            explicit LinuxUsbBackend(std::ostream& trace) : trace_(trace)
            {
                if (libusb_init(nullptr) < 0)
                {
                    log::Log(log::Level::Error, log::Stage::Detect, "usb.libusb_init_failed", "Failed to initialize libusb.");
                    initError_ = "Failed to initialize libusb.";
                    return;
                }
                inited_ = true;

                deviceCount_ = libusb_get_device_list(nullptr, &devices_);
                if (deviceCount_ < 0)
                {
                    initError_ = "Failed to enumerate USB devices.";
                    devices_ = nullptr;
                }
            }

            ~LinuxUsbBackend() override
            {
                Close();
                if (devices_)
                    libusb_free_device_list(devices_, 1);
                if (inited_)
                    libusb_exit(nullptr);
            }

            // Names the transport, not the OS: this backend also serves macOS.
            const char* PlatformName() const override { return "libusb"; }

            const std::string& InitError() const override { return initError_; }

            std::vector<UsbCandidate> Enumerate() override
            {
                candidates_.clear();
                if (devices_ && deviceCount_ > 0)
                    candidates_ = CollectCandidates(devices_, deviceCount_);

                std::vector<UsbCandidate> out;
                for (std::size_t i = 0; i < candidates_.size(); ++i)
                {
                    UsbCandidate c;
                    c.ordinal = i;
                    c.className = candidates_[i].printerClass ? "PRINTER" : "VENDOR";
                    c.interfaceNumber = candidates_[i].interfaceNumber;
                    c.path = SummarizePath(candidates_[i]);
                    c.pid = FormatPid(candidates_[i].pid);
                    out.push_back(c);
                }
                return out;
            }

            std::string Describe(std::size_t ordinal) const override
            {
                if (ordinal >= candidates_.size())
                    return "<invalid candidate>";

                const LinuxCandidate& cand = candidates_[ordinal];
                std::ostringstream text;
                text << "Interface " << cand.interfaceNumber
                     << (cand.printerClass ? " (Printer Class)" : " (Vendor-Specific Class)")
                     << " | Endpoint OUT: 0x" << std::hex << (int)cand.epOut
                     << " / IN: 0x" << (int)cand.epIn << std::dec
                     << " | VID 0x04B8 PID 0x" << FormatPid(cand.pid);
                return text.str();
            }

            // softReset is a usbprint.sys concept; claiming the interface
            // already gives libusb an exclusive, clean pipe.
            ITransport* Open(std::size_t ordinal, bool /*softReset*/) override
            {
                if (ordinal >= candidates_.size())
                    return nullptr;

                if (!OpenCandidate(candidates_[ordinal], open_))
                {
                    trace_ << "[!] Could not open/claim interface " << candidates_[ordinal].interfaceNumber << ".\n";
                    return nullptr;
                }

                transport_ = std::make_unique<LinuxUsbTransport>(open_.handle, candidates_[ordinal].epIn, candidates_[ordinal].epOut, trace_);
                return transport_.get();
            }

            void Close() override
            {
                transport_.reset();
                CloseCandidate(open_);
            }

            // libusb reads block properly, so a silent handshake here means
            // the interface is genuinely mute - retrying it proves nothing.
            int AttemptsPerCandidate() const override { return 1; }

            std::string QueryDeviceId(std::size_t ordinal) override
            {
                return (ordinal < candidates_.size()) ? QueryCandidateDeviceId(candidates_[ordinal], trace_) : std::string();
            }

            std::string DescribeOpenFailure(bool payloadRun) override
            {
                trace_ << "[FATAL] Could not open or claim any Epson interface" << (payloadRun ? "" : " for the query session") << ".\n";

                if (payloadRun)
                {
                    log::Log(log::Level::Warning, log::Stage::Detect, "usb.claim_all_failed",
                             "[!] Found an Epson device but could not claim any interface (is another program using it? try running with sudo).");
                }

                return "Could not claim any Epson USB interface.";
            }

        private:
            static std::string FormatPid(uint16_t pid)
            {
                std::ostringstream text;
                text << std::hex << std::setfill('0') << std::setw(4) << pid;
                return text.str();
            }

            static std::string SummarizePath(const LinuxCandidate& cand)
            {
                std::ostringstream path;
                path << "vid_04b8&pid_" << FormatPid(cand.pid) << "&if_" << cand.interfaceNumber;
                return path.str();
            }

            std::ostream& trace_;
            std::string initError_;
            bool inited_ = false;
            libusb_device** devices_ = nullptr;
            ssize_t deviceCount_ = 0;
            std::vector<LinuxCandidate> candidates_;
            OpenInterface open_;
            std::unique_ptr<LinuxUsbTransport> transport_;
        };

    } // namespace

    std::unique_ptr<UsbBackend> CreateUsbBackend(std::ostream& trace)
    {
        return std::make_unique<LinuxUsbBackend>(trace);
    }

} // namespace ewr
