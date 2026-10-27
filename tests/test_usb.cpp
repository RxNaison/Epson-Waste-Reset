#include "support.h"

// Two EWR runs on one printer take each other's replies, and the one that
// loses a reply may be the one mid-write (issue #39). The second run is meant
// to find the lock taken and stop, and a run that ends must leave it free -
// including one that was killed, which is why it is an OS lock and not a file
// anybody has to clean up.
void test_run_lock_admits_one_run_at_a_time()
{
    std::cout << "[TEST] test_run_lock_admits_one_run_at_a_time" << std::endl;

    {
        ewr::RunLock first;
        CHECK(first.Held());

        ewr::RunLock second;
        CHECK(!second.Held());
    }

    // Both are gone: the next run gets it.
    ewr::RunLock afterwards;
    CHECK(afterwards.Held());
}

void test_composite_merges_candidates_in_member_order()
{
    std::cout << "[TEST] test_composite_merges_candidates_in_member_order" << std::endl;

    ewr::usb::Candidate printer = MakeCandidate("USBPRINT", 1);
    printer.port = "USB001";
    printer.queues = { { "EPSON XP-332 335 Series", "USB001", "Microsoft IPP Class Driver", 1 } };

    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ printer, MakeCandidate("IMAGE", 0) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("VENDOR", 2), MakeCandidate("VENDOR", 3) });

    std::vector<ewr::usb::BackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::usb::Backend> composite = ewr::usb::CreateCompositeBackend(std::move(members), trace);

    CHECK(std::string(composite->PlatformName()) == "usbprint + libusb");
    CHECK(composite->InitError().empty());

    const std::vector<ewr::usb::Candidate> merged = composite->Enumerate();
    CHECK(merged.size() == 4);

    // Order is the guarantee: a printer that already worked never reaches
    // libusb.
    CHECK(merged[0].className == "USBPRINT");
    CHECK(merged[1].className == "IMAGE");
    CHECK(merged[2].className == "libusb:VENDOR");
    CHECK(merged[3].className == "libusb:VENDOR");

    // --interface N and the fallback ladder both address candidates by this.
    for (std::size_t i = 0; i < merged.size(); ++i)
        CHECK(merged[i].ordinal == i);

    CHECK(merged[2].interfaceNumber == 2);
    CHECK(merged[2].pid == "1187");
    CHECK(merged[0].path == "USBPRINT#if_1");

    // The driver loop reads the silent port's queues off the merged list.
    CHECK(merged[0].port == "USB001");
    CHECK(merged[0].queues.size() == 1 && merged[0].queues[0].waitingJobs == 1);
    CHECK(merged[2].port.empty() && merged[2].queues.empty());
}

void test_print_queues_are_matched_by_port()
{
    std::cout << "[TEST] test_print_queues_are_matched_by_port" << std::endl;

    const std::vector<ewr::PrintQueue> queues = {
        { "Microsoft Print to PDF", "PORTPROMPT:", "Microsoft Print To PDF", 0 },
        { "EPSON R220", "USB002", "Epson ESC/P Standard 2 V4 Class Driver", 0 },
        { "EPSON XP-332 335 Series", "USB001", "Microsoft IPP Class Driver", 0 },
        { "Pooled", "USB003, usb004", "MICROSOFT IPP CLASS DRIVER", 0 },
    };

    const std::vector<ewr::PrintQueue> onUsb001 = ewr::usb::QueuesOnPort(queues, "USB001");
    CHECK(onUsb001.size() == 1 && onUsb001[0].name == "EPSON XP-332 335 Series");

    const std::vector<ewr::PrintQueue> onUsb002 = ewr::usb::QueuesOnPort(queues, "usb002");
    CHECK(onUsb002.size() == 1 && onUsb002[0].name == "EPSON R220");

    // A pooled queue names each of its ports, in any case.
    CHECK(ewr::usb::QueuesOnPort(queues, "USB004").size() == 1);
    CHECK(ewr::usb::QueuesOnPort(queues, "USB003").size() == 1);

    // A prefix of a port is not that port, and no port is no queue.
    CHECK(ewr::usb::QueuesOnPort(queues, "USB00").empty());
    CHECK(ewr::usb::QueuesOnPort(queues, "").empty());

    CHECK(ewr::usb::IsReadyPrintDriver("Microsoft IPP Class Driver"));
    CHECK(ewr::usb::IsReadyPrintDriver(" MICROSOFT IPP CLASS DRIVER "));
    CHECK(!ewr::usb::IsReadyPrintDriver("Epson ESC/P Standard 2 V4 Class Driver"));
    CHECK(!ewr::usb::IsReadyPrintDriver(""));
}

// #55: an XP-332 never answered D4 Init under a Ready Print queue, and on an
// R220 a waiting print job stalled every write.
void test_silent_port_warns_about_waiting_jobs_and_ready_print()
{
    std::cout << "[TEST] test_silent_port_warns_about_waiting_jobs_and_ready_print" << std::endl;

    std::vector<ewr::log::Event> events;
    const int sinkId = ewr::log::Default().AddSink([&](const ewr::log::Event& e) { events.push_back(e); });

    ewr::SilentPortQueues quiet;
    quiet.port = "USB001";
    quiet.queues = { { "EPSON R220", "USB001", "Epson ESC/P Standard 2 V4 Class Driver", 0 } };
    CHECK(!ewr::usb::ReportSilentPortQueues(quiet));
    CHECK(!ewr::usb::ReportSilentPortQueues(ewr::SilentPortQueues{}));
    CHECK(events.empty());

    ewr::SilentPortQueues note;
    note.port = "USB001";
    note.queues = {
        { "EPSON XP-332 335 Series", "USB001", "Microsoft IPP Class Driver", 1 },
        { "EPSON XP-332 (copy 1)", "USB001", "EPSON XP-332 335 Series", 3 },
    };
    CHECK(ewr::usb::ReportSilentPortQueues(note));

    ewr::log::Default().RemoveSink(sinkId);

    // Jobs first: cancelling one is the cheaper fix.
    CHECK(events.size() == 3);
    if (events.size() == 3)
    {
        CHECK(events[0].code == "usb.print_jobs_waiting");
        CHECK(events[0].level == ewr::log::Level::Warning);
        CHECK(events[0].fields.at("queue") == "EPSON XP-332 335 Series");
        CHECK(events[0].fields.at("port") == "USB001");
        CHECK(events[0].fields.at("jobs") == "1");
        CHECK(events[0].message.find("1 print job is waiting") != std::string::npos);

        CHECK(events[1].code == "usb.print_jobs_waiting");
        CHECK(events[1].fields.at("jobs") == "3");
        CHECK(events[1].message.find("3 print jobs are waiting") != std::string::npos);

        CHECK(events[2].code == "usb.ready_print_driver");
        CHECK(events[2].fields.at("queue") == "EPSON XP-332 335 Series");
        CHECK(events[2].fields.count("jobs") == 0);
    }
}

void test_composite_identifies_a_shared_interface_by_number()
{
    std::cout << "[TEST] test_composite_identifies_a_shared_interface_by_number" << std::endl;

    // mi_01 is bound to usbprint.sys and still visible to libusb; listing it
    // twice only spends a failed claim on a handle usbprint already holds.
    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("USBPRINT", 1) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{
            MakeCandidate("PRINTER", 1),              // the same interface
            MakeCandidate("VENDOR", 1, "1234"),       // same number, different device
            MakeCandidate("VENDOR", 2) });

    std::vector<ewr::usb::BackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::usb::Backend> composite = ewr::usb::CreateCompositeBackend(std::move(members), trace);

    const std::vector<ewr::usb::Candidate> merged = composite->Enumerate();
    CHECK(merged.size() == 3);
    CHECK(merged[0].className == "USBPRINT");
    CHECK(merged[1].className == "libusb:VENDOR" && merged[1].pid == "1234");
    CHECK(merged[2].className == "libusb:VENDOR" && merged[2].interfaceNumber == 2);

    for (std::size_t i = 0; i < merged.size(); ++i)
        CHECK(merged[i].ordinal == i);

    // No mi_XX means non-composite: one function, so libusb's interface 0 on
    // that PID is the same one.
    auto lonePrimary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("USBPRINT", -1) });
    auto loneSecondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("PRINTER", 0) });

    std::vector<ewr::usb::BackendMember> loneMembers;
    loneMembers.push_back(Member(std::move(lonePrimary), ""));
    loneMembers.push_back(Member(std::move(loneSecondary), "libusb"));

    std::unique_ptr<ewr::usb::Backend> lone = ewr::usb::CreateCompositeBackend(std::move(loneMembers), trace);
    CHECK(lone->Enumerate().size() == 1);
    CHECK(lone->Enumerate()[0].className == "USBPRINT");

    // Two of the same printer share a PID and an interface number and are
    // still two printers.
    auto twins = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("USBPRINT", 1), MakeCandidate("USBPRINT", 1) });

    std::vector<ewr::usb::BackendMember> twinMembers;
    twinMembers.push_back(Member(std::move(twins), ""));

    std::unique_ptr<ewr::usb::Backend> pair = ewr::usb::CreateCompositeBackend(std::move(twinMembers), trace);
    CHECK(pair->Enumerate().size() == 2);
}

void test_composite_routes_every_call_to_the_owning_member()
{
    std::cout << "[TEST] test_composite_routes_every_call_to_the_owning_member" << std::endl;

    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("USBPRINT", 1), MakeCandidate("IMAGE", 0) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("VENDOR", 2), MakeCandidate("VENDOR", 3) });

    FakeBackend* primaryRaw = primary.get();
    FakeBackend* secondaryRaw = secondary.get();

    // usbprint retries a silent handshake on a fresh handle; libusb does not.
    primaryRaw->attemptsBase = 2;
    secondaryRaw->attemptsBase = 1;

    std::vector<ewr::usb::BackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::usb::Backend> composite = ewr::usb::CreateCompositeBackend(std::move(members), trace);
    composite->Enumerate();

    // Composite ordinal 3 is libusb's own ordinal 1.
    CHECK(composite->Describe(3) == "[libusb] libusb candidate 1");
    CHECK(composite->QueryDeviceId(3) == "libusb id 1");
    CHECK(composite->AttemptsPerCandidate(3) == 2); // 1 + local ordinal 1
    CHECK(composite->AttemptsPerCandidate(2) == 1); // 1 + local ordinal 0
    CHECK(composite->AttemptsPerCandidate(1) == 3); // 2 + local ordinal 1

    CHECK(composite->Open(3) == &secondaryRaw->transport);
    CHECK(secondaryRaw->opened == std::vector<std::size_t>{ 1 });
    CHECK(primaryRaw->opened.empty());

    // The reset goes to the transport holding the handle, and its "nothing
    // was reset" answer comes back unchanged.
    secondaryRaw->softResetSucceeds = false;
    CHECK(!composite->SoftReset());
    CHECK(secondaryRaw->softResets == 1);
    CHECK(primaryRaw->softResets == 0);

    composite->Close();
    CHECK(secondaryRaw->closes == 1);
    CHECK(primaryRaw->closes == 0);

    // Refused, not routed to member zero.
    CHECK(composite->Open(99) == nullptr);
    CHECK(composite->Describe(99) == "<invalid candidate>");
    CHECK(composite->QueryDeviceId(99).empty());
    CHECK(secondaryRaw->opened.size() == 1);

    // A failed open still leaves the diagnosis with the transport that tried.
    secondaryRaw->openSucceeds = false;
    CHECK(composite->Open(2) == nullptr);
    CHECK(composite->DescribeOpenFailure(true) == "libusb could not open.");
}

void test_composite_runs_on_the_transports_that_came_up()
{
    std::cout << "[TEST] test_composite_runs_on_the_transports_that_came_up" << std::endl;

    // libusb failing to initialise must not stop a run usbprint can finish.
    auto primary = std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("USBPRINT", 1) });
    auto secondary = std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{ MakeCandidate("VENDOR", 2) }, "Failed to initialize libusb.");

    FakeBackend* secondaryRaw = secondary.get();

    std::vector<ewr::usb::BackendMember> members;
    members.push_back(Member(std::move(primary), ""));
    members.push_back(Member(std::move(secondary), "libusb"));

    std::ostringstream trace;
    std::unique_ptr<ewr::usb::Backend> composite = ewr::usb::CreateCompositeBackend(std::move(members), trace);

    CHECK(composite->InitError().empty());
    CHECK(std::string(composite->PlatformName()) == "usbprint");
    CHECK(composite->Enumerate().size() == 1);
    CHECK(secondaryRaw->enumerations == 0);
    CHECK(trace.str().find("Failed to initialize libusb.") != std::string::npos);

    // Every transport down is the only fatal case, and the reason survives.
    std::vector<ewr::usb::BackendMember> deadMembers;
    deadMembers.push_back(Member(std::make_unique<FakeBackend>("usbprint",
        std::vector<ewr::usb::Candidate>{}, "SetupAPI enumeration failed."), ""));
    deadMembers.push_back(Member(std::make_unique<FakeBackend>("libusb",
        std::vector<ewr::usb::Candidate>{}, "Failed to initialize libusb."), "libusb"));

    std::unique_ptr<ewr::usb::Backend> dead = ewr::usb::CreateCompositeBackend(std::move(deadMembers), trace);
    CHECK(dead->InitError() == "SetupAPI enumeration failed.");
    CHECK(dead->Enumerate().empty());
}
