#include "ewr/backup.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ewr {

    namespace {

        constexpr const char* kFormat = "ewr-backup";
        // A file is refused only when this goes up; new keys never raise it.
        constexpr int kFormatVersion = 1;

        std::string Trim(const std::string& text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return "";
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        std::string Sanitize(const std::string& text)
        {
            std::string out;
            for (char c : text)
                out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '_';
            return out;
        }

        // "0x1A2" or "1A2" -> 0x1A2. False on anything else.
        bool ParseHex(const std::string& text, unsigned long& value)
        {
            std::string digits = Trim(text);
            if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                digits = digits.substr(2);
            if (digits.empty() || digits.size() > 8)
                return false;

            for (char c : digits)
            {
                if (!std::isxdigit(static_cast<unsigned char>(c)))
                    return false;
            }

            value = std::stoul(digits, nullptr, 16);
            return true;
        }

        std::string StringOr(const nlohmann::json& object, const char* key)
        {
            const auto it = object.find(key);
            return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
        }

        bool ParseJson(const std::string& text, Backup& out, std::string& error)
        {
            nlohmann::json root;
            try
            {
                root = nlohmann::json::parse(text);
            }
            catch (const std::exception&)
            {
                error = "The file is not valid JSON.";
                return false;
            }

            if (!root.is_object() || StringOr(root, "format") != kFormat)
            {
                error = "The file is not an EWR backup.";
                return false;
            }

            const int version = root.value("version", 0);
            if (version < 1 || version > kFormatVersion)
            {
                error = "The backup was written in format version " + std::to_string(version)
                      + ", which this EWR does not read. Use the EWR that wrote it, or a newer one.";
                return false;
            }

            out = Backup();
            out.source = "backup";
            out.model = StringOr(root, "model");
            out.target = StringOr(root, "target");
            out.created = StringOr(root, "created");
            out.ewrVersion = StringOr(root, "ewr");

            const auto printer = root.find("printer");
            if (printer != root.end() && printer->is_object())
            {
                out.transport = StringOr(*printer, "transport");
                out.host = StringOr(*printer, "address");
                out.deviceId = StringOr(*printer, "device_id");
                out.serial = StringOr(*printer, "serial");
            }

            const int width = root.value("address_length", 0);
            out.addressLength = (width == 1 || width == 2) ? static_cast<uint8_t>(width) : 0;

            const auto bytes = root.find("bytes");
            if (bytes == root.end() || !bytes->is_array())
            {
                error = "The backup holds no bytes.";
                return false;
            }

            for (const auto& entry : *bytes)
            {
                if (!entry.is_object() || !entry.contains("address") || !entry["address"].is_number_unsigned())
                {
                    error = "The backup has a byte without a valid address.";
                    return false;
                }

                const auto address = entry["address"].get<unsigned long>();
                if (address > 0xFFFF)
                {
                    error = "The backup names an address beyond 0xFFFF.";
                    return false;
                }

                int value = -1;
                const auto v = entry.find("value");
                if (v != entry.end() && !v->is_null())
                {
                    if (!v->is_number_unsigned() || v->get<unsigned long>() > 0xFF)
                    {
                        error = "The backup holds a value that is not a byte.";
                        return false;
                    }
                    value = static_cast<int>(v->get<unsigned long>());
                }

                out.bytes.push_back({ static_cast<uint16_t>(address), value });
            }

            if (out.model.empty())
            {
                error = "The backup does not name a model.";
                return false;
            }

            return true;
        }

        // The text --dump writes: '#' header lines, then 'address,value,note'
        // with '--' for a byte that never answered.
        bool ParseDump(const std::string& text, Backup& out, std::string& error)
        {
            out = Backup();
            out.source = "dump";
            out.dumpFormat = 1;

            std::istringstream in(text);
            std::string line;
            size_t lineNumber = 0;
            while (std::getline(in, line))
            {
                lineNumber++;
                line = Trim(line);
                if (line.empty())
                    continue;

                if (line[0] == '#')
                {
                    const size_t colon = line.find(':');
                    if (colon == std::string::npos)
                        continue;

                    const std::string key = Trim(line.substr(1, colon - 1));
                    const std::string value = Trim(line.substr(colon + 1));

                    if (key == "model")
                        out.model = value;
                    else if (key == "format")
                        out.dumpFormat = std::atoi(value.c_str());
                    else if (key == "ewr")
                        out.ewrVersion = value;
                    else if (key == "created")
                        out.created = value;
                    else if (key == "device_id")
                        out.deviceId = value;
                    else if (key == "serial")
                        out.serial = value;
                    else if (key == "transport")
                        out.transport = value;
                    else if (key == "address_length")
                    {
                        const int width = std::atoi(value.c_str());
                        out.addressLength = (width == 1 || width == 2) ? static_cast<uint8_t>(width) : 0;
                    }
                    continue;
                }

                const size_t first = line.find(',');
                const std::string addressText = line.substr(0, first);
                std::string valueText;
                if (first != std::string::npos)
                {
                    const size_t second = line.find(',', first + 1);
                    valueText = line.substr(first + 1, second == std::string::npos ? std::string::npos
                                                                                 : second - first - 1);
                }

                unsigned long address = 0;
                if (!ParseHex(addressText, address) || address > 0xFFFF)
                {
                    error = "Line " + std::to_string(lineNumber) + " of the dump is not 'address,value'.";
                    return false;
                }

                int value = -1;
                if (Trim(valueText) != "--")
                {
                    unsigned long parsed = 0;
                    if (!ParseHex(valueText, parsed) || parsed > 0xFF)
                    {
                        error = "Line " + std::to_string(lineNumber) + " of the dump holds no byte value.";
                        return false;
                    }
                    value = static_cast<int>(parsed);
                }

                out.bytes.push_back({ static_cast<uint16_t>(address), value });
            }

            if (out.model.empty())
            {
                error = "The dump does not name a model ('# model:' line).";
                return false;
            }

            if (out.bytes.empty())
            {
                error = "The dump holds no bytes.";
                return false;
            }

            return true;
        }

    } // namespace

    size_t Backup::Answered() const
    {
        size_t answered = 0;
        for (const auto& entry : bytes)
            answered += (entry.second >= 0) ? 1 : 0;
        return answered;
    }

    std::string UtcTimestamp()
    {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return text;
    }

    std::string BackupToJson(const Backup& backup)
    {
        nlohmann::ordered_json root;
        root["format"] = kFormat;
        root["version"] = kFormatVersion;
        root["created"] = backup.created;
        root["ewr"] = backup.ewrVersion;
        root["model"] = backup.model;
        root["target"] = backup.target;

        auto orNull = [](const std::string& text) {
            return text.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(text);
        };

        root["printer"] = {
            { "transport", orNull(backup.transport) },
            { "address", orNull(backup.host) },
            { "device_id", orNull(backup.deviceId) },
            { "serial", orNull(backup.serial) },
        };
        root["address_length"] = backup.addressLength ? nlohmann::ordered_json(backup.addressLength)
                                                      : nlohmann::ordered_json(nullptr);

        nlohmann::ordered_json bytes = nlohmann::ordered_json::array();
        for (const auto& entry : backup.bytes)
        {
            bytes.push_back({
                { "address", entry.first },
                { "value", entry.second >= 0 ? nlohmann::ordered_json(entry.second) : nlohmann::ordered_json(nullptr) },
            });
        }
        root["bytes"] = std::move(bytes);
        root["complete"] = backup.Complete();

        return root.dump(2) + "\n";
    }

    bool ParseBackupText(const std::string& text, Backup& out, std::string& error)
    {
        const std::string trimmed = Trim(text);
        if (trimmed.empty())
        {
            error = "The file is empty.";
            return false;
        }

        if (trimmed[0] == '{')
            return ParseJson(trimmed, out, error);

        if (trimmed.rfind("# EWR EEPROM dump", 0) == 0)
            return ParseDump(trimmed, out, error);

        error = "The file is neither an EWR backup nor an EWR dump.";
        return false;
    }

    std::string BackupFileName(const Backup& backup)
    {
        std::string stamp;
        for (char c : backup.created)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
                stamp += c;
        }
        if (stamp.empty())
            stamp = "undated";

        const std::string model = backup.model.empty() ? std::string("unknown") : Sanitize(backup.model);
        const std::string serial = backup.serial.empty() ? std::string("unknown") : Sanitize(backup.serial);
        return model + "_" + serial + "_" + stamp + ".json";
    }

    bool WriteBackupFile(const Backup& backup, const std::string& dir,
                         std::string& pathOut, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        if (!fs::is_directory(fs::path(dir), ec))
        {
            error = "Could not create the folder " + dir + ".";
            return false;
        }

        const std::string name = BackupFileName(backup);
        const std::string stem = name.substr(0, name.size() - 5); // ".json"

        fs::path target = fs::path(dir) / name;
        for (int n = 2; fs::exists(target, ec) && n < 100; ++n)
            target = fs::path(dir) / (stem + "_" + std::to_string(n) + ".json");

        if (fs::exists(target, ec))
        {
            error = "Every backup file name for this second is taken in " + dir + ".";
            return false;
        }

        const fs::path temp = fs::path(target.string() + ".tmp");
        {
            std::ofstream out(temp, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out)
            {
                error = "Could not write to " + dir + ".";
                return false;
            }

            out << BackupToJson(backup);
            out.flush();
            if (!out)
            {
                out.close();
                fs::remove(temp, ec);
                error = "Writing the backup to " + dir + " failed.";
                return false;
            }
        }

        fs::rename(temp, target, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            error = "Could not finish the backup file in " + dir + ".";
            return false;
        }

        pathOut = target.string();
        return true;
    }

} // namespace ewr
