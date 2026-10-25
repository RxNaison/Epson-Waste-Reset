#include "cli.h"

#include <iostream>

namespace ewr::cli {

    // The network search's menu. Even a lone printer is picked, not assumed: the
    // network may hold someone else's Epson.
    int ChooseNetworkPrinter(const std::vector<ewr::NetworkPrinter>& found,
                                    const std::vector<ewr::deviceid::ModelNameEntry>& entries)
    {
        std::cout << std::endl;
        for (std::size_t i = 0; i < found.size(); ++i)
        {
            const ewr::NetworkPrinter& printer = found[i];
            std::cout << "  [" << i + 1 << "] " << (printer.model.empty() ? "(no model name)" : printer.model)
                      << "   " << printer.address;

            const std::vector<std::string> matches = printer.model.empty()
                ? std::vector<std::string>{} : ewr::deviceid::MatchEntries(printer.model, entries);
            if (!matches.empty())
                std::cout << "   (database: " << matches[0] << ")";
            std::cout << std::endl;
        }
        std::cout << "  [0] None of these" << std::endl;

        while (true)
        {
            std::cout << "\nSelect a printer [0-" << found.size() << "]: ";
            std::string line;
            if (!std::getline(std::cin, line))
                return kNoInputForPrinter;

            try
            {
                const int choice = std::stoi(line);
                if (choice == 0)
                    return kNoPrinterChosen;
                if (choice >= 1 && choice <= static_cast<int>(found.size()))
                    return choice - 1;
            }
            catch (...)
            {
            }

            std::cout << "[-] Please enter a number from 0 to " << found.size() << "." << std::endl;
        }
    }

    // --model, the detected model under --yes or --json, or the menu. An exit
    // code when the run ends here instead.
    std::optional<int> SelectModel(const CliOptions& cli, const std::vector<MenuOption>& options,
                                   const std::string& detectedMdl, const std::string& detectedMatch,
                                   bool overNetwork, MenuOption& selected, std::string& selectedBy)
    {
        bool hasSelected = false;

        // Same matcher detection uses, so names, aliases and families all work.
        if (!cli.modelOverride.empty())
        {
            const std::string wantedLower = toLower(cli.modelOverride);

            // Exact equality is never ambiguous, even when the same text also
            // partial-matches other entries.
            std::string resolvedName;
            for (const auto& opt : options)
            {
                const std::string ownName = opt.isReplay ? opt.replayModel.name : opt.smartModel.name;
                if (toLower(ownName) == wantedLower)
                {
                    resolvedName = ownName;
                    break;
                }

                if (!opt.isReplay)
                {
                    for (const auto& alias : opt.smartModel.aliases)
                    {
                        if (toLower(alias) == wantedLower)
                        {
                            resolvedName = opt.smartModel.name;
                            break;
                        }
                    }
                }

                if (!resolvedName.empty())
                    break;
            }

            // Fall back to the fuzzy matcher: normalization, aliases, families.
            if (resolvedName.empty())
            {
                std::vector<ewr::deviceid::ModelNameEntry> allEntries;
                for (const auto& opt : options)
                {
                    if (opt.isReplay)
                        allEntries.push_back({ opt.replayModel.name, {} });
                    else
                        allEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
                }

                const std::vector<std::string> matches = ewr::deviceid::MatchEntries(cli.modelOverride, allEntries);

                std::vector<std::string> uniqueNames;
                for (const auto& match : matches)
                {
                    bool seen = false;
                    for (const auto& name : uniqueNames)
                        seen = seen || (name == match);
                    if (!seen)
                        uniqueNames.push_back(match);
                }

                if (uniqueNames.empty())
                {
                    std::cerr << "[!] --model \"" << cli.modelOverride << "\" matches no usable model." << std::endl;
                    std::cerr << "    (Database models without resettable waste counters are not offered.)" << std::endl;
                    std::cerr << "    Run without --model and use the menu search, or check 'ewr --list'." << std::endl;
                    JsonFail("model_unknown", "--model \"" + cli.modelOverride + "\" matches no usable model.");
                    return FinishRun(1);
                }

                if (uniqueNames.size() > 1)
                {
                    std::cerr << "[!] --model \"" << cli.modelOverride << "\" is ambiguous. Closest matches:" << std::endl;
                    for (size_t i = 0; i < uniqueNames.size() && i < 6; ++i)
                        std::cerr << "      " << uniqueNames[i] << std::endl;
                    std::cerr << "    Be more specific - the full name always works." << std::endl;
                    JsonFail("model_ambiguous", "--model \"" + cli.modelOverride + "\" matches "
                                                + std::to_string(uniqueNames.size()) + " database entries.");
                    return FinishRun(1);
                }

                resolvedName = uniqueNames[0];
            }

            // Same name on a Smart Protocol and a Replay entry: prefer Smart.
            for (const auto& opt : options)
            {
                if (!opt.isReplay && opt.smartModel.name == resolvedName)
                {
                    selected = opt;
                    hasSelected = true;
                    break;
                }
            }

            if (!hasSelected)
            {
                for (const auto& opt : options)
                {
                    if (opt.isReplay && opt.replayModel.name == resolvedName)
                    {
                        selected = opt;
                        hasSelected = true;
                        break;
                    }
                }
            }

            if (hasSelected)
            {
                selectedBy = "--model \"" + cli.modelOverride + "\"";
                std::cout << "\n[i] --model: using " << selected.displayName << "." << std::endl;
            }
        }

        // --yes or --json with no --model: the printer named itself, so take that
        // entry instead of opening a menu nobody is there to answer. It is also
        // why neither can write to the wrong model on its own - the selection and
        // the detection are then the same answer.
        if (!hasSelected && (cli.assumeYes || cli.json))
        {
            const char* flag = cli.assumeYes ? "--yes" : "--json";

            if (detectedMatch.empty())
            {
                std::cerr << "\n[!] " << flag << " needs a model and the printer did not supply one." << std::endl;
                if (!detectedMdl.empty())
                    std::cerr << "    It reports \"" << detectedMdl
                              << "\", which matches no database entry." << std::endl;
                else
                    std::cerr << "    No Epson interface answered the device ID query." << std::endl;
                std::cerr << "    Pass --model <name>, or run 'ewr --list' to see what is connected."
                          << std::endl;
                JsonFail("model_required", "The printer supplied no model that matches the database.");
                return FinishRun(1);
            }

            for (const auto& opt : options)
            {
                if (!opt.isReplay && opt.smartModel.name == detectedMatch)
                {
                    selected = opt;
                    hasSelected = true;
                    selectedBy = std::string(flag) + ": the detected model";
                    std::cout << "\n[i] " << flag << ": using the detected model " << opt.smartModel.name << "."
                              << std::endl;
                    break;
                }
            }
        }

        // --json never prompts: there is nobody to ask, and a menu on a silenced
        // stdout would look like a hang. Reached only when the detected entry is
        // not one this run can use.
        if (!hasSelected && cli.json)
        {
            std::cerr << "[!] --json needs the model on the command line: pass --model <name>." << std::endl;
            JsonFail("model_required", "--json cannot open the model menu; pass --model <name>.");
            return FinishRun(1);
        }

        while (!hasSelected)
        {
            if (!detectedMatch.empty())
                std::cout << "\nPress Enter to use the detected model [" << detectedMatch
                          << "], enter another model to search, or type 'exit' to quit: ";
            else
                std::cout << "\nEnter printer model to search (e.g., 'L3150' or 'XP') or type 'exit' to quit: ";

            std::string searchQuery;
            if (!std::getline(std::cin, searchQuery))
            {
                // No stdin to read (a pipe, a redirect, a service context). Every
                // other prompt here needs a typed word and so aborts on its own;
                // this one treats empty as "search again" and would spin forever.
                std::cerr << "\n[ERROR] No input available: EWR needs a model to work with and stdin\n"
                             "        is closed. Pass --model <name> to choose one non-interactively." << std::endl;
                JsonFail("model_required", "No model was given and stdin is closed.");
                return FinishRun(1);
            }

            if (searchQuery.empty())
            {
                if (detectedMatch.empty())
                    continue;

                for (const auto& opt : options)
                {
                    if (!opt.isReplay && opt.smartModel.name == detectedMatch)
                    {
                        selected = opt;
                        hasSelected = true;
                        selectedBy = "the detected model, confirmed at the prompt";
                        break;
                    }
                }
                continue;
            }

            std::string searchLower = toLower(searchQuery);
            if (searchLower == "exit" || searchLower == "quit")
                return FinishRun(0);

            std::vector<MenuOption> filteredOptions;
            for (const auto& opt : options)
            {
                // A Replay dump is bytes for a USB channel; over the network there
                // is nothing it could be sent through.
                if (overNetwork && opt.isReplay)
                    continue;

                if (toLower(opt.displayName).find(searchLower) != std::string::npos)
                    filteredOptions.push_back(opt);
            }

            if (filteredOptions.empty())
            {
                std::cout << "[-] No printers found matching '" << searchQuery << "'. Please try again.\n";
                continue;
            }

            std::cout << "\nFound " << filteredOptions.size() << " matching printers:\n";

            for (size_t i = 0; i < filteredOptions.size(); ++i)
                std::cout << "[" << i + 1 << "] " << filteredOptions[i].displayName << "\n";

            std::cout << "[0] Search again...\n";

            std::cout << "\nSelect your printer [0-" << filteredOptions.size() << "]: ";
            std::string choiceStr;
            std::getline(std::cin, choiceStr);

            try
            {
                int choice = std::stoi(choiceStr);
                if (choice == 0)
                {
                    continue;
                }
                else if (choice >= 1 && choice <= static_cast<int>(filteredOptions.size()))
                {
                    selected = filteredOptions[choice - 1];
                    hasSelected = true;
                    selectedBy = "picked from the menu, search \"" + searchQuery + "\"";
                }
                else
                {
                    std::cout << "[-] Invalid selection. Please try again.\n";
                }
            }
            catch (...)
            {
                std::cout << "[-] Invalid input. Please enter a number.\n";
            }
        }

        return std::nullopt;
    }

} // namespace ewr::cli
