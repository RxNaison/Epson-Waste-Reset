#pragma once
#include "ewr/database.h"
#include "ewr/log.h"
#include "ewr/session.h"
#include "ewr/status.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Head cleaning, ink charge and the nozzle check: what the printer's own
// buttons do, behind a flag and an API call. None of it writes the EEPROM,
// but all of it acts on the printer - a cleaning uses ink and fills the waste
// pads - so it runs the way a reset does: preflight, gates, one question,
// then the command exactly once, and the printer's own status afterwards
// says whether it ran.
namespace ewr {

    enum class MaintenanceAction
    {
        NozzleCheck,
        HeadClean,
        PowerClean, // the "ink charge": far more ink than a cleaning
    };

    enum class CleanGroup
    {
        All,
        Black,
        Color,
    };

    struct MaintenanceRequest
    {
        MaintenanceAction action = MaintenanceAction::HeadClean;
        std::string level = "normal"; // gentle, normal, medium, strong: what the model lists
        CleanGroup group = CleanGroup::All;
        int nozzleVariant = 0;        // 1: the alternate pattern
        // The print-data route even where the model has a factory level.
        bool remoteOnly = false;
    };

    enum class MaintenancePhase
    {
        NotStarted,     // nothing was sent
        Aborted,        // a gate was declined
        NotSupported,   // no way to send this to this printer
        DeviceNotFound,
        SendFailed,     // it could not be sent, or the printer refused it
        NeverStarted,   // sent, but the printer never left IDLE
        Error,          // the printer reported an error while at it
        TimedOut,       // it started and had not finished in time
        Done,           // seen working, then IDLE again
    };

    struct MaintenanceOutcome
    {
        MaintenancePhase phase = MaintenancePhase::NotStarted;
        bool success = false;
        // "factory" (the keyed command 0x84) or "remote" (print data).
        std::string route;
        bool sent = false;
        std::string reply;     // the factory command's answer, e.g. "||:84:OK;"
        bool started = false;  // the printer was seen at work
        int seconds = 0;       // from the send to the last status read
        StateSnapshot before;  // status, and the counters when the model is known
        StateSnapshot after;
        PrinterStatus last;    // the last status read while watching
        std::vector<GateOverride> overrides;
        std::string error;
    };

    struct MaintenanceHandlers
    {
        std::function<void(const StateSnapshot&)> onPreflight;
        DecisionCallback onBlocker;
        // Asked once, after every gate, with the route about to be used.
        // False aborts; unset proceeds.
        std::function<bool(const StateSnapshot&, const std::string& route)> confirm;
        // Each status read while the printer works.
        std::function<void(const PrinterStatus&, int seconds)> onProgress;
    };

    // The contract's spellings.
    const char* ToString(MaintenanceAction action);
    const char* ToString(MaintenancePhase phase);

    class Maintenance
    {
    public:
        // `model` may be null: a nozzle check and the print-data cleaning
        // need no database entry. When given it must outlive this object.
        Maintenance(IDeviceGateway& gateway, const DbPrinterModel* model,
                    log::Reporter& reporter = log::Default(),
                    ExecutorOptions options = ExecutorOptions::ForQueries());

        // The route `request` would take - "factory" or "remote" - or empty,
        // with `why`, when there is none.
        std::string Route(const MaintenanceRequest& request, std::string& why) const;

        MaintenanceOutcome Run(const MaintenanceRequest& request, const MaintenanceHandlers& handlers = {});

        // How the printer is watched once the command is out. Tests shorten them.
        int pollIntervalMs = 2000;
        int startWindowSeconds = 45;   // to leave IDLE at all
        int cleanTimeoutSeconds = 240;
        int chargeTimeoutSeconds = 600;
        int nozzleTimeoutSeconds = 180;

    private:
        // The factory payload for `request`, empty when the model has none.
        std::vector<uint8_t> FactoryPayload(const MaintenanceRequest& request) const;

        void Watch(const MaintenanceRequest& request, const MaintenanceHandlers& handlers,
                   MaintenanceOutcome& out);

        IDeviceGateway& m_gateway;
        const DbPrinterModel* m_model;
        log::Reporter& m_reporter;
        ExecutorOptions m_options;
    };

} // namespace ewr

namespace ewr::gates {

    // nullopt when the printer is in a state to do this.
    std::optional<Blocker> EvaluateMaintenance(const PrinterStatus& status, MaintenanceAction action);

} // namespace ewr::gates
