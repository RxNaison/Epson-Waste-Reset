#include "cli.h"
#include "ewr/log.h"
#include "ewr/session.h"

#include <iostream>

namespace ewr::cli {

    int RunStatus(RunContext& ctx)
    {
        ewr::IDeviceGateway& gateway = ctx.gateway;
        const MenuOption& selected = ctx.selected;
        const std::string& detectedMatch = ctx.detectedMatch;
        ewr::ExecutorOptions& sessionOptions = ctx.sessionOptions;

        std::cout << "\n[*] Querying printer status (read-only, no EEPROM writes)..." << std::endl;

        ewr::StateSnapshot state;
        if (selected.isReplay)
        {
            // No read key in a dump: the '@BDC ST2' status is all we can ask.
            state = ewr::status::Read(gateway, sessionOptions);
        }
        else
        {
            ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
            state = session.ReadState();
        }

        // Built by the one function that builds it everywhere else, so the
        // status payload cannot quietly lack a key the dry run has - it lacked
        // pads_total exactly that way (#42). Built before the read is judged,
        // so a caller that got nothing still learns which model it asked about
        // and how many pads that model has.
        if (selected.isReplay)
        {
            // No read key in a dump, so there is no model to count pads on.
            g_jsonData["model"] = selected.replayModel.name;
            g_jsonData["printer"] = ewr::json::Status(state.status);
            g_jsonData["counters"] = nlohmann::json::array();
            g_jsonData["pads"] = nlohmann::json::array();
            g_jsonData["pads_total"] = 0;
            // Opaque bytes: which pads a replay covers is unknown, not none.
            g_jsonData["reset_covers"] = nullptr;
        }
        else
        {
            g_jsonData = ewr::json::StateData(selected.smartModel, state);
        }

        g_jsonData["detected_model"] = detectedMatch.empty() ? nlohmann::json(nullptr)
                                                            : nlohmann::json(detectedMatch);
        g_jsonData["planned_writes"] = nullptr;

        if (!state.available)
        {
            std::cerr << (ctx.overNetwork ? "[ERROR] Could not read the printer status over the network."
                                     : "[ERROR] Could not read the printer status. Is it turned on and plugged in?") << std::endl;
            std::cerr << "        Check ewr_trace.log for the hardware trace." << std::endl;
            JsonFail("read_failed", "The printer did not answer the status query.");
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);
        PrintCounterValues(state.values, "Waste counter EEPROM values:");

        if (!selected.isReplay)
        {
            PrintCounterSummary(selected.smartModel, state.values);
            PrintResetCoverage(selected.smartModel);
        }

        if (selected.isReplay)
            std::cout << "[i] Counter values are not available for Replay models (no read key in the dump)." << std::endl;

        return FinishRun(0);
    }

} // namespace ewr::cli
