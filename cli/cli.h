#pragma once
#include "ewr/backup.h"
#include "ewr/database.h"
#include "ewr/deviceid.h"
#include "ewr/gateway.h"
#include "ewr/json.h"
#include "ewr/network.h"
#include "ewr/replay.h"
#include "ewr/status.h"
#include "ewr/version.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ewr::cli {

    struct MenuOption
    {
        std::string displayName;
        bool isReplay;
        ewr::replay::Capture replayModel;
        ewr::DbPrinterModel smartModel;
    };

    inline std::string toLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    struct CliOptions
    {
        bool statusOnly = false;     // --status: read-only status + counters
        bool listOnly = false;       // --list: interface survey, then exit
        bool dryRun = false;         // --dry-run: everything except the writes
        bool dump = false;           // --dump: read-only EEPROM dump to a file
        bool dumpFull = false;       // --full (with --dump): all of 0..mem_high
        bool findAddresses = false;  // --find-addresses: repeated dumps around head cleanings
        bool findKey = false;        // --find-key: try every read key in the database, read-only
        bool noUpdate = false;       // --no-update: offline run, nothing checked or swapped
        bool assumeYes = false;      // --yes: answer the write confirmation, nothing else
        bool forceYes = false;       // --force-yes: also overrule the gates that would stop it
        bool cartridge = false;      // --cartridge: reset ink levels instead of the waste pads
        int interfaceCandidate = 0;  // --interface <n>: 1-based pin, 0 = auto
        bool usbSoftReset = false;   // --usb-soft-reset: reset the USB channel once, then wait
        bool json = false;           // --json: the machine-readable contract on stdout
        std::string modelOverride;   // --model <name>: skip the menu
        std::string ip;              // --ip <address>: read a network printer over SNMP
        bool ipAuto = false;         // --ip auto: search the network even with a printer on USB
        std::string restoreFile;     // --restore <file>: write back a backup's or a dump's counter bytes
        bool restoreAll = false;     // --restore-all <dump>: every differing byte, same unit only
        bool info = false;           // --info: firmware, serial, the printer's own counters, fatal codes
        bool nozzleCheck = false;    // --nozzle-check [alt]: print the nozzle check pattern
        int nozzleVariant = 0;
        bool headClean = false;      // --head-clean [level]: a head cleaning
        std::string cleanLevel = "normal";
        std::string cleanGroup;      // --clean-group all|black|color
        bool powerClean = false;     // --power-clean: the ink charge
        bool remoteOnly = false;     // --print-data: the print-data route even where a factory level exists
    };

    // What every mode works with once the printer and its model are settled.
    struct RunContext
    {
        CliOptions& cli;
        ewr::IDeviceGateway& gateway;
        bool overNetwork;
        const MenuOption& selected;
        std::string detectedMatch;
        std::string deviceId;
        ewr::ExecutorOptions sessionOptions;
        std::string mismatchPassed;
    };

    // Never hardcode the version here: the updater compares this string against
    // release tags, and it comes from project(EWR VERSION ...) via version.h.
    inline const std::string kEwrCurrentVersion = EWR_VERSION;

    // ---- The run: --json state, exit paths, the terminal (run.cpp) ----------

    // Filled in as the run goes; spent by the single result line.
    extern std::unique_ptr<ewr::json::Emitter> g_json;
    extern std::string g_jsonCommand;
    extern std::string g_jsonErrorCode;
    extern std::string g_jsonErrorText;
    extern nlohmann::json g_jsonData;
    extern bool g_jsonHelloSent;
    extern bool g_exitPause;

    void StartJsonOutput();
    void JsonHello(const CliOptions& cli);
    void JsonFail(const char* errorCode, const std::string& detail = "");
    int FinishRun(int exitCode);
    int FinishReset(bool resetOk);
    int UsageError(const CliOptions& cli, const std::string& message);

    void SetWorkingDirectoryToExecutable();
    std::string ChooseDataLocation();
    bool StdinIsInteractive();
    bool StdoutIsTerminal();
    void InstallColor();
    void SpinUntil(const std::function<bool()>& done);
    void PrintNewVersionBanner(const std::string& latestVersion);

    // ---- The command line (args.cpp) ----------------------------------------

    void PrintUsage();
    std::optional<int> ParseCommandLine(int argc, char* argv[], CliOptions& cli);

    // ---- What the person reads (render.cpp) ---------------------------------

    void PrintPrinterStatus(const ewr::PrinterStatus& st);
    void PrintCounterValues(const std::vector<std::pair<uint16_t, int>>& values, const char* label);
    void PrintCounterSummary(const ewr::DbPrinterModel& model,
                             const std::vector<std::pair<uint16_t, int>>& values);
    void PrintResetCoverage(const ewr::DbPrinterModel& model);
    void PrintNetworkSearchHints();

    // ---- Which printer, which model (select.cpp) ----------------------------

    // ChooseNetworkPrinter's answers besides an index: "none of these", and no
    // stdin to ask, which needs telling apart - its fix is --ip.
    constexpr int kNoPrinterChosen = -1;
    constexpr int kNoInputForPrinter = -2;

    int ChooseNetworkPrinter(const std::vector<ewr::NetworkPrinter>& found,
                             const std::vector<ewr::deviceid::ModelNameEntry>& entries);
    std::optional<int> SelectModel(const CliOptions& cli, const std::vector<MenuOption>& options,
                                   const std::string& detectedMdl, const std::string& detectedMatch,
                                   bool overNetwork, MenuOption& selected, std::string& selectedBy);

    // ---- The modes, one file each -------------------------------------------

    int RunList(const std::string& databasePath);
    int RunStatus(RunContext& ctx);
    int RunDump(RunContext& ctx);
    int RunReadKeySearch(ewr::IDeviceGateway& gateway,
                         const std::vector<ewr::DbPrinterModel>& models,
                         const CliOptions& cli,
                         const std::string& detectedMdl,
                         const std::string& detectedMatch);
    int RunFindAddresses(RunContext& ctx);
    int RunInfo(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options);
    int RunMaintenance(const ewr::DbPrinterModel* model, ewr::IDeviceGateway& gateway,
                       const ewr::ExecutorOptions& options, const CliOptions& cli,
                       const std::string& mismatchPassed);
    int RunRestore(const ewr::DbPrinterModel& model, const ewr::Backup& source, const std::string& file,
                   ewr::IDeviceGateway& gateway, const ewr::ExecutorOptions& options, const CliOptions& cli,
                   const std::string& deviceId, const std::string& mismatchPassed);
    int RunDryRun(RunContext& ctx);
    int RunReplay(RunContext& ctx);
    int RunReset(RunContext& ctx);

    int Run(int argc, char* argv[]);

} // namespace ewr::cli
