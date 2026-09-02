#include "ewr/usb_backend.h"
#include "usb_backends_internal.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// Windows needs two transports: the ET-2xxx maintenance engine sits on a
// vendor-specific interface bound to WinUSB, which usbprint.sys cannot see and
// which does not answer ReadFile/WriteFile (issue #16). usbprint goes first, so
// a printer that already worked never reaches libusb.
//
// Compiled on every platform, not just Windows, so the tests cover the merge
// everywhere.

namespace ewr {

    namespace {

        // A composite ordinal indexes these; the driver loop never sees the
        // member's own ordinal.
        struct Slot
        {
            std::size_t member = 0;
            std::size_t local = 0;
        };

        class CompositeUsbBackend final : public UsbBackend
        {
        public:
            CompositeUsbBackend(std::vector<UsbBackendMember> members, std::ostream& trace)
                : members_(std::move(members)), trace_(trace)
            {
                // Only a total failure is fatal: libusb missing must not stop
                // a run usbprint alone can finish.
                bool anyUsable = false;

                for (const UsbBackendMember& member : members_)
                {
                    if (member.backend->InitError().empty())
                    {
                        anyUsable = true;

                        if (!platformName_.empty())
                            platformName_ += " + ";
                        platformName_ += member.backend->PlatformName();
                    }
                    else
                    {
                        trace_ << "[!] Transport " << member.backend->PlatformName()
                               << " is unavailable this run: " << member.backend->InitError() << "\n";

                        if (initError_.empty())
                            initError_ = member.backend->InitError();
                    }
                }

                if (anyUsable)
                    initError_.clear();
                else
                    platformName_ = "no transport";
            }

            const char* PlatformName() const override { return platformName_.c_str(); }

            const std::string& InitError() const override { return initError_; }

            std::vector<UsbCandidate> Enumerate() override
            {
                slots_.clear();
                std::vector<UsbCandidate> merged;

                for (std::size_t m = 0; m < members_.size(); ++m)
                {
                    if (!members_[m].backend->InitError().empty())
                        continue;

                    trace_ << "\n[i] Enumerating over " << members_[m].backend->PlatformName() << ".\n";

                    // Two of the same printer share a PID and an interface
                    // number, so a member is never matched against itself.
                    const std::size_t earlierCount = merged.size();

                    for (const UsbCandidate& cand : members_[m].backend->Enumerate())
                    {
                        // The earlier transport holds the handle once it
                        // opens, so a duplicate could only fail to claim.
                        if (AlreadyListed(merged, earlierCount, cand))
                        {
                            trace_ << "     -> mi_" << cand.interfaceNumber
                                   << " skipped: already listed by an earlier transport.\n";
                            continue;
                        }

                        UsbCandidate out = cand;
                        out.ordinal = slots_.size();
                        if (!members_[m].tag.empty())
                            out.className = members_[m].tag + ":" + cand.className;

                        slots_.push_back({ m, cand.ordinal });
                        merged.push_back(out);
                    }
                }

                return merged;
            }

            std::string Describe(std::size_t ordinal) const override
            {
                if (ordinal >= slots_.size())
                    return "<invalid candidate>";

                const Slot& slot = slots_[ordinal];
                return "[" + std::string(members_[slot.member].backend->PlatformName()) + "] "
                     + members_[slot.member].backend->Describe(slot.local);
            }

            ITransport* Open(std::size_t ordinal, bool softReset) override
            {
                if (ordinal >= slots_.size())
                    return nullptr;

                // Before the open, not after: a failure still has to route
                // Close() and the diagnosis to the transport that tried.
                activeMember_ = slots_[ordinal].member;
                return members_[activeMember_].backend->Open(slots_[ordinal].local, softReset);
            }

            void Close() override
            {
                if (activeMember_ < members_.size())
                    members_[activeMember_].backend->Close();
            }

            int AttemptsPerCandidate(std::size_t ordinal) const override
            {
                if (ordinal >= slots_.size())
                    return 1;

                const Slot& slot = slots_[ordinal];
                return members_[slot.member].backend->AttemptsPerCandidate(slot.local);
            }

            std::string QueryDeviceId(std::size_t ordinal) override
            {
                if (ordinal >= slots_.size())
                    return std::string();

                const Slot& slot = slots_[ordinal];
                return members_[slot.member].backend->QueryDeviceId(slot.local);
            }

            std::string DescribeOpenFailure(bool payloadRun) override
            {
                if (activeMember_ >= members_.size())
                    return "No Epson USB transport is available.";

                return members_[activeMember_].backend->DescribeOpenFailure(payloadRun);
            }

        private:
            // On PID and interface number, not path: the two transports
            // describe one interface with completely different strings.
            static bool AlreadyListed(const std::vector<UsbCandidate>& listed,
                                      std::size_t count,
                                      const UsbCandidate& cand)
            {
                for (std::size_t i = 0; i < count && i < listed.size(); ++i)
                {
                    if (listed[i].pid != cand.pid)
                        continue;

                    // No mi_XX means a non-composite device: its one function
                    // is whatever this candidate points at.
                    if (listed[i].interfaceNumber < 0 || listed[i].interfaceNumber == cand.interfaceNumber)
                        return true;
                }

                return false;
            }

            std::vector<UsbBackendMember> members_;
            std::ostream& trace_;
            std::string platformName_;
            std::string initError_;
            std::vector<Slot> slots_;
            std::size_t activeMember_ = 0;
        };

    } // namespace

    std::unique_ptr<UsbBackend> CreateCompositeUsbBackend(std::vector<UsbBackendMember> members,
                                                          std::ostream& trace)
    {
        return std::make_unique<CompositeUsbBackend>(std::move(members), trace);
    }

#ifdef _WIN32
    std::unique_ptr<UsbBackend> CreateUsbBackend(std::ostream& trace)
    {
        std::vector<UsbBackendMember> members;
        members.push_back(UsbBackendMember{ CreateWindowsUsbBackend(trace), "" });
        members.push_back(UsbBackendMember{ CreateLibusbBackend(trace), "libusb" });

        return CreateCompositeUsbBackend(std::move(members), trace);
    }
#endif

} // namespace ewr
