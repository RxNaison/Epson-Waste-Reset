#include "ewr/info.h"
#include "protocol/ctrl.h"
#include "protocol/d4session.h"

#include <algorithm>

namespace ewr {

    namespace {

        // A little-endian value over `addresses`, or -1 if any byte is missing.
        long long LittleEndian(const std::vector<uint16_t>& addresses,
                               const std::vector<std::pair<uint16_t, int>>& values)
        {
            long long value = 0;
            for (size_t i = 0; i < addresses.size(); ++i)
            {
                int raw = -1;
                for (const auto& entry : values)
                {
                    if (entry.first == addresses[i])
                    {
                        raw = entry.second;
                        break;
                    }
                }
                if (raw < 0)
                    return -1;
                value |= static_cast<long long>(raw) << (8 * i);
            }
            return value;
        }

        void Add(std::vector<uint16_t>& list, const std::vector<uint16_t>& more)
        {
            for (uint16_t a : more)
            {
                if (std::find(list.begin(), list.end(), a) == list.end())
                    list.push_back(a);
            }
        }

    } // namespace

    std::string ctrl::ParseVersionReply(const std::vector<unsigned char>& raw)
    {
        const std::vector<unsigned char> payload = d4::ExtractPayload(raw);
        const std::string text(payload.begin(), payload.end());

        const size_t vi = text.find("vi:");
        if (vi == std::string::npos)
            return "";

        // "vi:" then a two-digit result and a colon, then the version.
        size_t body = vi + 3;
        if (body + 3 <= text.size() && text[body + 2] == ':')
            body += 3;

        const size_t end = text.find(';', body);
        if (end == std::string::npos)
            return "";

        std::string version = text.substr(body, end - body);
        if (version == "NA" || version == "NG")
            return "";

        // Printable only: the field ends up on a screen and in JSON.
        version.erase(std::remove_if(version.begin(), version.end(),
                                     [](char c) { return c < 0x20 || c > 0x7E; }),
                      version.end());
        return version;
    }

    PrinterInfo info::Read(IDeviceGateway& gateway, const DbPrinterModel* model,
                                log::Reporter& reporter, const ExecutorOptions& options)
    {
        PrinterInfo info;

        // Status and firmware in one session: neither needs a key.
        std::vector<std::vector<unsigned char>> queries;
        queries.push_back(ctrl::StatusQueryPacket());
        queries.push_back(ctrl::ControlPacket('v', 'i', { 0x00 }));

        const QueryRunResult run = gateway.RunQuery(d4::Handshake(), queries, options);
        if (!run.deviceFound || run.query.handshakeFailed || run.query.replies.empty()
            || run.query.replies[0].empty())
            return info;

        info.available = true;
        info.status = ctrl::ParseStatusReply(run.query.replies[0]);
        if (run.query.replies.size() > 1)
            info.firmware = ctrl::ParseVersionReply(run.query.replies[1]);

        if (!info.status.serial.empty())
        {
            info.serial = info.status.serial;
            info.serialSource = "status";
        }

        if (!model)
            return info;

        // Everything the entry names, read in one pass: range reads where the
        // printer answers them, byte by byte where it does not.
        std::vector<uint16_t> addresses;
        if (info.serial.empty())
            Add(addresses, model->serial_addresses);
        for (const auto& counter : model->report)
            Add(addresses, counter.addresses);
        Add(addresses, model->first_use);
        for (const auto& slot : model->fatal)
            Add(addresses, slot.addresses);

        // A 1-byte read address cannot name what lies above 0xFF.
        addresses.erase(std::remove_if(addresses.begin(), addresses.end(),
                                       [model](uint16_t a) {
                                           return a > model->mem_high || (model->ReadAddressLength() == 1 && a > 0xFF);
                                       }),
                        addresses.end());
        std::sort(addresses.begin(), addresses.end());

        if (addresses.empty())
            return info;

        Session reader(*model, gateway, reporter, options);
        const StateSnapshot state = reader.ReadAddresses(addresses);
        if (!state.available)
            return info;

        if (info.serial.empty() && !model->serial_addresses.empty())
        {
            std::string serial;
            bool whole = true;
            for (uint16_t a : model->serial_addresses)
            {
                const long long c = LittleEndian({ a }, state.values);
                if (c < 0)
                {
                    whole = false;
                    break;
                }
                if (c >= 0x20 && c <= 0x7E)
                    serial += static_cast<char>(c);
            }

            if (whole && !serial.empty())
            {
                info.serial = serial;
                info.serialSource = "eeprom";
            }
        }

        for (const auto& counter : model->report)
            info.counters.push_back({ counter.key, LittleEndian(counter.addresses, state.values) });

        if (!model->first_use.empty())
            info.firstUse = LittleEndian(model->first_use, state.values);

        for (const auto& slot : model->fatal)
        {
            const long long code = LittleEndian(slot.addresses, state.values);
            info.fatal.push_back({ slot.kind, slot.slot, code < 0 ? -1 : static_cast<int>(code) });
        }

        return info;
    }

} // namespace ewr
