#include "cli.h"
#include "ewr/usb.h"

#include <iostream>
#include <sstream>

namespace ewr::cli {

    // No update checks, no D4 traffic, no menu - diagnostics only.
    int RunList(const std::string& databasePath)
    {
        ewr::Database listDb;
        std::vector<ewr::deviceid::ModelNameEntry> listEntries;
        if (listDb.Load(databasePath))
        {
            for (const auto& m : listDb.Models())
                listEntries.push_back({ m.name, m.aliases });
        }

        std::cout << "[*] Scanning Epson USB interfaces (read-only)..." << std::endl;
        ewr::UsbDeviceGateway listGateway;

        if (!listGateway.ClaimPrinter())
        {
            JsonFail("another_run", "Another EWR run is already driving a printer on this machine.");
            return FinishRun(1);
        }

        const std::vector<ewr::usb::InterfaceInfo> interfaces = listGateway.ListInterfaces();
        nlohmann::json jsonInterfaces = nlohmann::json::array();

        if (interfaces.empty())
            std::cout << "\nNo Epson USB interfaces found." << std::endl;
        else
            std::cout << "\nDetected Epson USB interfaces (in automatic fallback order):" << std::endl;

        for (const auto& iface : interfaces)
        {
            std::cout << "\n  [" << iface.index << "] " << iface.className;
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;
            std::cout << "\n      " << iface.path << std::endl;

            if (iface.deviceId.empty())
            {
                std::cout << "      IEEE 1284 device ID: (no reply)" << std::endl;
                jsonInterfaces.push_back(ewr::json::InterfaceEntry(iface, ""));
                continue;
            }

            const ewr::deviceid::Info devId = ewr::deviceid::Parse(iface.deviceId);
            std::cout << "      IEEE 1284 device ID: " << (devId.model.empty() ? iface.deviceId : "MDL \"" + devId.model + "\"") << std::endl;

            std::string match;
            if (!devId.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::deviceid::MatchEntries(devId.model, listEntries);
                if (!matches.empty())
                {
                    std::cout << "      Database entry:      " << matches[0] << std::endl;
                    match = matches[0];
                }
            }

            jsonInterfaces.push_back(ewr::json::InterfaceEntry(iface, match));
        }

        if (!interfaces.empty())
            std::cout << "\nUse --interface <n> to pin a run to one specific interface." << std::endl;
        g_jsonData["interfaces"] = std::move(jsonInterfaces);

        std::cout << "\n[*] Searching the local network for Epson printers (SNMP, read-only)..." << std::endl;
        std::string searchError;
        std::ostringstream searchTrace;
        const std::vector<ewr::NetworkPrinter> network =
            ewr::snmp::DiscoverPrinters(ewr::snmp::kDiscoveryWaitMs, searchError, &searchTrace);
        listGateway.AppendTrace(searchTrace.str());
        nlohmann::json jsonNetwork = nlohmann::json::array();

        if (!searchError.empty())
            std::cerr << "\n[!] " << searchError << std::endl;
        else if (network.empty())
            std::cout << "\nNo Epson printer answered on the network." << std::endl;
        else
            std::cout << "\nEpson printers on the network:" << std::endl;

        for (const auto& printer : network)
        {
            std::cout << "\n  " << printer.address << std::endl;
            std::cout << "      IEEE 1284 device ID: "
                      << (printer.model.empty() ? printer.deviceId : "MDL \"" + printer.model + "\"") << std::endl;

            std::string match;
            if (!printer.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::deviceid::MatchEntries(printer.model, listEntries);
                if (!matches.empty())
                {
                    std::cout << "      Database entry:      " << matches[0] << std::endl;
                    match = matches[0];
                }
            }

            jsonNetwork.push_back(ewr::json::NetworkEntry(printer, match));
        }

        if (!network.empty())
            std::cout << "\nUse --ip <address> to reach one of them." << std::endl;
        g_jsonData["network_printers"] = std::move(jsonNetwork);

        if (interfaces.empty() && network.empty())
        {
            std::cout << "\nIs the printer on, and plugged in or on this network?" << std::endl;
            PrintNetworkSearchHints();
            JsonFail("device_not_found", "No Epson printer found on USB or on the network.");
            return FinishRun(1);
        }

        return FinishRun(0);
    }

} // namespace ewr::cli
