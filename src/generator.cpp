#include "ewr/generator.h"
#include <cstdio>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <urlmon.h>
#else
#include <curl/curl.h>
#endif

using json = nlohmann::json;

namespace ewr {

    namespace {

        constexpr int kMaxSupportedSchemaVersion = 3;

        bool IsValidDatabase(const std::string& path)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;

            try
            {
                json j;
                file >> j;

                if (!j.is_object() || j.empty())
                    return false;

                const json& models = (j.contains("models") && j["models"].is_object())
                    ? j["models"]
                    : j;

                for (const auto& [key, val] : models.items())
                {
                    if (val.is_object() && val.contains("wkey"))
                        return true;
                }
                return false;
            }
            catch (...)
            {
                return false;
            }
        }

    } // namespace

    bool UniversalGenerator::SyncDatabaseOTA()
    {
        const char* url = "https://raw.githubusercontent.com/RxNaison/Epson-Waste-Reset/main/database.json";
        const char* dest = "database.json";
        const char* tmp = "database.json.tmp";

#ifdef _WIN32
        if (URLDownloadToFileA(NULL, url, tmp, 0, NULL) != S_OK)
        {
            std::remove(tmp);
            return false;
        }
#else
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;

        FILE* fp = fopen(tmp, "wb");
        if (!fp)
        {
            curl_easy_cleanup(curl);
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        fclose(fp);

        if (res != CURLE_OK)
        {
            std::remove(tmp);
            return false;
        }
#endif

        if (!IsValidDatabase(tmp))
        {
            std::remove(tmp);
            return false;
        }

#ifdef _WIN32
        if (!MoveFileExA(tmp, dest, MOVEFILE_REPLACE_EXISTING))
        {
            std::remove(tmp);
            return false;
        }
#else
        if (std::rename(tmp, dest) != 0)
        {
            std::remove(tmp);
            return false;
        }
#endif
        return true;
    }

    bool UniversalGenerator::LoadDatabase(const std::string& filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
            return false;

        try
        {
            json j;
            file >> j;

            if (!j.is_object())
                return false;

            json& modelsRoot = (j.contains("models") && j["models"].is_object())
                ? j["models"]
                : j;

            const int schemaVersion = j.value("schema_version", 0);
            if (schemaVersion > kMaxSupportedSchemaVersion)
            {
                std::cerr << "[!] Database schema_version " << schemaVersion
                          << " is newer than this build supports ("
                          << kMaxSupportedSchemaVersion
                          << "). Loading best-effort; please update EWR." << std::endl;
            }

            for (auto& [key, val] : modelsRoot.items())
            {
                if (!val.is_object())
                    continue;

                DbPrinterModel model;
                model.name = key;
                model.rkey = val.value("rkey", 0);
                model.wkey = val.value("wkey", "");
                model.wkey1 = val.value("wkey1", "");
                model.rlen = val.value("rlen", 2);
                model.wlen = val.value("wlen", 2);
                model.mem_high = val.value("mem_high", 2047);

                if (val.contains("pad_groups") && val["pad_groups"].is_array())
                {
                    for (auto& pg_json : val["pad_groups"])
                    {
                        PadGroup pg;
                        pg.description = pg_json.value("desc", "");
                        pg.kind = pg_json.value("kind", "");

                        if (pg_json.contains("addresses") && pg_json["addresses"].is_array())
                        {
                            for (auto& addr : pg_json["addresses"])
                                pg.addresses.push_back(addr.get<uint16_t>());
                        }

                        if (pg_json.contains("reset") && pg_json["reset"].is_array())
                        {
                            for (auto& rst : pg_json["reset"])
                                pg.reset_values.push_back(rst.get<uint8_t>());
                        }

                        while (pg.reset_values.size() < pg.addresses.size())
                            pg.reset_values.push_back(0x00);

                        model.pad_groups.push_back(std::move(pg));
                    }
                }
                else if (val.contains("addresses") && val["addresses"].is_array())
                {
                    PadGroup pg;
                    pg.description = "Waste counters";
                    pg.kind = "";

                    for (auto& addr : val["addresses"])
                        pg.addresses.push_back(addr.get<uint16_t>());

                    if (val.contains("reset") && val["reset"].is_array())
                    {
                        for (auto& rst : val["reset"])
                            pg.reset_values.push_back(rst.get<uint8_t>());
                    }

                    while (pg.reset_values.size() < pg.addresses.size())
                        pg.reset_values.push_back(0x00);

                    if (!pg.addresses.empty())
                        model.pad_groups.push_back(std::move(pg));
                }

                database[key] = std::move(model);
            }
            return true;
        }
        catch (const json::exception& e)
        {
            std::cerr << "[!] JSON Parse Error: " << e.what() << std::endl;
            return false;
        }
    }

    bool UniversalGenerator::IsEmpty() const
    {
        return database.empty();
    }

    std::vector<DbPrinterModel> UniversalGenerator::GetAvailableModels() const
    {
        std::vector<DbPrinterModel> models;
        models.reserve(database.size());

        for (const auto& pair : database)
            models.push_back(pair.second);

        return models;
    }

    std::vector<unsigned char> UniversalGenerator::GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value, const std::string& wkey) const
    {
        uint8_t c = EpsonD4::CMD_EEPROM_WRITE;
        uint8_t not_c = ~c & 0xFF;
        uint8_t shift_c = ((c >> 1) & 0x7F) | ((c << 7) & 0x80);

        std::vector<unsigned char> inner;
        inner.push_back(rkey & 0xFF);
        inner.push_back((rkey >> 8) & 0xFF);
        inner.push_back(c);
        inner.push_back(not_c);
        inner.push_back(shift_c);
        inner.push_back(address & 0xFF);
        inner.push_back((address >> 8) & 0xFF);
        inner.push_back(value);
        inner.insert(inner.end(), wkey.begin(), wkey.end());

        std::vector<unsigned char> epson_cmd;
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        epson_cmd.push_back(EpsonD4::PREFIX_PIPE);
        uint16_t len = inner.size();
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        // D4 header uses Big-Endian length; inner Epson frame uses Little-Endian length
        std::vector<unsigned char> d4;
        d4.push_back(EpsonD4::SOCKET_EPSON_CTRL);
        d4.push_back(EpsonD4::SOCKET_EPSON_CTRL);
        uint16_t d4_len = epson_cmd.size() + 6;
        d4.push_back((d4_len >> 8) & 0xFF);
        d4.push_back(d4_len & 0xFF);
        d4.push_back(EpsonD4::CREDIT);
        d4.push_back(0x00);
        d4.insert(d4.end(), epson_cmd.begin(), epson_cmd.end());

        return d4;
    }

    std::vector<std::vector<unsigned char>> UniversalGenerator::GenerateSequence(const DbPrinterModel& model) const
    {
        std::vector<std::vector<unsigned char>> sequence;

        // 3 NUL bytes flush hardware parser state
        const unsigned char ejl_init[] = {
            0x00, 0x00, 0x00, 0x1B, 0x01, '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4', '.', '4', '\n',
            '@', 'E', 'J', 'L', '\n',
            '@', 'E', 'J', 'L', '\n'
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(ejl_init), std::end(ejl_init)));

        const unsigned char d4_init[] = {
            0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_init), std::end(d4_init)));

        const unsigned char d4_open[] = {
            0x00, 0x00, 0x00, 0x11, 0x01, 0x00, 0x01,
            EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_open), std::end(d4_open)));

        const unsigned char d4_credit_grant[] = {
            0x00, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x03, EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0x00, 0x01
        };
        const unsigned char d4_credit_req[] = {
            0x00, 0x00, 0x00, 0x0D, 0x01, 0x00, 0x04, EpsonD4::SOCKET_EPSON_CTRL, EpsonD4::SOCKET_EPSON_CTRL, 0xFF, 0xFF, 0x00, 0x01
        };

        for (const auto& group : model.pad_groups)
        {
            for (size_t i = 0; i < group.addresses.size(); ++i)
            {
                sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_grant), std::end(d4_credit_grant)));
                sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_req), std::end(d4_credit_req)));
                sequence.push_back(GenerateWritePacket(model.rkey, group.addresses[i], group.reset_values[i], model.wkey));
            }
        }

        return sequence;
    }

} // namespace ewr
