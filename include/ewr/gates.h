#pragma once
#include "ewr/status.h"

#include <functional>
#include <optional>
#include <string>

namespace ewr {

    // A foreign printer error (empty cartridge, paper jam, open cover, ...).
    // Firmware locked by one usually refuses factory writes with ':42:NA;'.
    struct Blocker
    {
        std::string errorName;   // ST2 error name, e.g. "INK OUT"
        int errorCode = -1;      // raw ST2 error code
        std::string explanation; // human-readable risk explanation
    };

    // True pushes past the blocker, false aborts before any EEPROM write.
    // The CLI wires this to a y/N prompt, a GUI to a dialog, an unattended
    // runner to a fixed answer.
    using DecisionCallback = std::function<bool(const Blocker&)>;

} // namespace ewr

// Whether the printer's state lets a run go ahead.
namespace ewr::gates {

    // SERVICE REQUEST / CARTRIDGE OVERFLOW - the errors EWR exists to clear.
    // Printers in those states still accept writes, so they must never gate.
    bool IsExpectedWastePadError(int errorCode);

    // nullopt when it is safe to proceed. A BUSY state with no error entry
    // blocks too: busy firmware can suppress the very error that would have
    // gated the write.
    std::optional<Blocker> Evaluate(const PrinterStatus& status);

    // INK OUT joins the expected list - an empty cartridge is the state this
    // reset clears. Foreign locks still block.
    std::optional<Blocker> EvaluateInk(const PrinterStatus& status);

} // namespace ewr::gates
