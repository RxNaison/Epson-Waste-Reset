#pragma once
// Sockets first: winsock2.h must come before anything that brings windows.h.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>
#include <thread>
#include "ewr/replay.h"
#include "ewr/gates.h"
#include "ewr/keysearch.h"
#include "protocol/ctrl.h"
#include "ewr/format.h"
#include "ewr/database.h"
#include "protocol/executor.h"
#include "ewr/status.h"
#include "ewr/deviceid.h"
#include "ewr/updater.h"
#include "protocol/end4.h"
#include "protocol/jig.h"

#include "protocol/d4session.h"
#include "ewr/log.h"
#include "ewr/session.h"
#include "ewr/maintenance.h"
#include "protocol/remote_mode.h"
#include "ewr/restore.h"
#include "ewr/backup.h"
#include "transport/usb_backend.h"
#include "transport/usb_driver.h"
#include "ewr/run_lock.h"
#include "ewr/json.h"
#include "ewr/ewr_c.h"
#include "ewr/version.h"
#include "ewr/discovery.h"
#include "ewr/snmp.h"
#include "ewr/network.h"
#include "../cli/color.h"
#include "../cli/console.h"

namespace fs = std::filesystem;

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(cond)) {                                                                  \
            ++g_failures;                                                               \
            std::cout << "  [FAIL] " << #cond << "  (" << __FILE__ << ":" << __LINE__   \
                      << ")" << std::endl;                                              \
        }                                                                               \
    } while (0)

namespace legacy {

    inline std::vector<unsigned char> GenerateWritePacket(uint16_t rkey, uint16_t address, uint8_t value, const std::string& wkey)
    {
        uint8_t c = 0x42;
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
        epson_cmd.push_back(0x7C);
        epson_cmd.push_back(0x7C);
        uint16_t len = inner.size();
        epson_cmd.push_back(len & 0xFF);
        epson_cmd.push_back((len >> 8) & 0xFF);
        epson_cmd.insert(epson_cmd.end(), inner.begin(), inner.end());

        std::vector<unsigned char> d4;
        d4.push_back(0x02);
        d4.push_back(0x02);
        uint16_t d4_len = epson_cmd.size() + 6;
        d4.push_back((d4_len >> 8) & 0xFF);
        d4.push_back(d4_len & 0xFF);
        d4.push_back(0x00);
        d4.push_back(0x00);
        d4.insert(d4.end(), epson_cmd.begin(), epson_cmd.end());
        return d4;
    }

    inline std::vector<std::vector<unsigned char>> GenerateSequence(const ewr::DbPrinterModel& model)
    {
        std::vector<std::vector<unsigned char>> sequence;

        const unsigned char ejl_init[] = {
            0x00, 0x00, 0x00, 0x1B, 0x01, '@', 'E', 'J', 'L', ' ', '1', '2', '8', '4', '.', '4', '\n',
            '@', 'E', 'J', 'L', '\n', '@', 'E', 'J', 'L', '\n'
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(ejl_init), std::end(ejl_init)));

        const unsigned char d4_init[] = { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_init), std::end(d4_init)));

        const unsigned char d4_open[] = {
            0x00, 0x00, 0x00, 0x11, 0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        sequence.push_back(std::vector<unsigned char>(std::begin(d4_open), std::end(d4_open)));

        const unsigned char d4_credit_grant[] = { 0x00, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x03, 0x02, 0x02, 0x00, 0x01 };
        const unsigned char d4_credit_req[]   = { 0x00, 0x00, 0x00, 0x0D, 0x01, 0x00, 0x04, 0x02, 0x02, 0xFF, 0xFF, 0x00, 0x01 };

        auto addrs = model.GetAllAddresses();
        auto resets = model.GetAllResetValues();

        for (size_t i = 0; i < addrs.size(); ++i)
        {
            if (addrs[i] > model.mem_high)
                continue; // mirrors the generator's EEPROM bounds clamp

            sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_grant), std::end(d4_credit_grant)));
            sequence.push_back(std::vector<unsigned char>(std::begin(d4_credit_req), std::end(d4_credit_req)));
            sequence.push_back(GenerateWritePacket(model.rkey, addrs[i], resets[i], model.wkey));
        }
        return sequence;
    }

} // namespace legacy

inline ewr::DbPrinterModel MakeTestModel()
{
    ewr::DbPrinterModel m;
    m.name = "TestPrinter";
    m.rkey = 0x0008;
    m.wkey = "Arkanoid";
    ewr::PadGroup pg;
    pg.description = "Main Pad Counter";
    pg.kind = "main";
    pg.addresses = { 0x0018, 0x0019 };
    pg.reset_values = { 0x00, 0x00 };
    m.pad_groups.push_back(pg);
    return m;
}

class FakeTransport final : public ewr::ITransport
{
public:
    std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> replyFor;
    std::vector<std::vector<unsigned char>> sent;
    std::vector<int> drainTimeouts;
    bool failSend = false;
    // Fails only the sends this says to, so a transport can break mid-session
    // and recover - which is what the close-and-recover paths need.
    std::function<bool(const std::vector<unsigned char>&)> failSendIf;

    bool Send(const std::vector<unsigned char>& packet) override
    {
        sent.push_back(packet);
        if (failSend || (failSendIf && failSendIf(packet)))
            return false;
        pending_ = replyFor ? replyFor(packet) : std::vector<unsigned char>{};
        return true;
    }

    std::vector<unsigned char> Drain(int timeoutMs) override
    {
        drainTimeouts.push_back(timeoutMs);
        auto reply = pending_;
        pending_.clear();
        return reply;
    }

private:
    std::vector<unsigned char> pending_;
};

inline std::vector<unsigned char> OkAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'O', 'K', ';', 0x0c };
}

inline std::vector<unsigned char> NgAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'N', 'G', ';', 0x0c };
}

// Byte-for-byte the reply captured from a real R220 locked by an empty
// cartridge.
inline std::vector<unsigned char> NaAck()
{
    return { 0x02, 0x02, 0x00, 0x10, 0x00, 0x01, 0x7c, 0x7c, ':', '4', '2', ':', 'N', 'A', ';', 0x0c };
}

inline std::vector<unsigned char> HandshakeAck()
{
    return { 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 0x83, 0x00, 0x02, 0x02 };
}

inline std::vector<unsigned char> OpenChannelAck()
{
    return { 0x00, 0x00, 0x00, 0x0c, 0x01, 0x00, 0x81, 0x00, 0x02, 0x02, 0x00, 0x01 };
}

inline ewr::ExecutorOptions FastOptions()
{
    ewr::ExecutorOptions options;
    options.interPacketDelayMs = 0;
    options.retryDelayMs = 0;
    // The executor tests below assert the credit re-send on write retry.
    options.resendCreditOnRetry = true;
    return options;
}

inline std::ofstream NullLog()
{
    return std::ofstream();
}

// ---------------------------------------------------------------------------
// Status / read-back fixtures
// ---------------------------------------------------------------------------

// GetSocketID can hand back a socket other than the well-known 2, and the
// session frames its replies with whatever it negotiated.
inline std::vector<unsigned char> WrapD4DataOnSocket(const std::vector<unsigned char>& payload,
                                                     unsigned char socket)
{
    std::vector<unsigned char> pkt;
    const uint16_t len = static_cast<uint16_t>(payload.size() + 6);
    pkt.push_back(socket);
    pkt.push_back(socket);
    pkt.push_back((len >> 8) & 0xFF);
    pkt.push_back(len & 0xFF);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

inline std::vector<unsigned char> WrapD4Data(const std::vector<unsigned char>& payload)
{
    return WrapD4DataOnSocket(payload, ewr::d4::SOCKET_EPSON_CTRL);
}

// '@BDC ST2', a 2-byte little-endian body length, then the field block.
// `declaredLength` overrides that length so a report can announce more than it
// delivers, the way a reply cut short by the transport does.
inline std::vector<unsigned char> WrapSt2Body(const std::vector<unsigned char>& body,
                                              unsigned char socket = ewr::d4::SOCKET_EPSON_CTRL,
                                              int declaredLength = -1)
{
    const size_t declared = (declaredLength >= 0) ? static_cast<size_t>(declaredLength) : body.size();

    std::vector<unsigned char> payload;
    const std::string prefix = "@BDC ST2\r\n";
    payload.insert(payload.end(), prefix.begin(), prefix.end());
    payload.push_back(static_cast<unsigned char>(declared & 0xFF));
    payload.push_back(static_cast<unsigned char>((declared >> 8) & 0xFF));
    payload.insert(payload.end(), body.begin(), body.end());

    return WrapD4DataOnSocket(payload, socket);
}

// A synthetic but structurally faithful '@BDC ST2' status reply:
// state ERROR, error INK OUT, maintenance box 90%, Black 42% + Yellow missing,
// serial X7A9000123.
inline std::vector<unsigned char> MakeSt2Reply()
{
    std::vector<unsigned char> body;
    body.insert(body.end(), { 0x01, 0x01, 0x00 });                               // state: ERROR
    body.insert(body.end(), { 0x02, 0x01, 0x05 });                               // error: INK OUT
    body.insert(body.end(), { 0x0D, 0x01, 90 });                                 // maintenance box: 90
    body.insert(body.end(), { 0x0F, 0x07, 0x03, 0x00, 0x00, 42, 0x00, 0x03, 110 }); // inks: Black 42, Yellow missing

    const std::string serial = "X7A9000123";
    body.push_back(0x40);
    body.push_back(static_cast<unsigned char>(serial.size()));
    body.insert(body.end(), serial.begin(), serial.end());

    return WrapSt2Body(body);
}

inline std::vector<unsigned char> MakeEepromReadReply(uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nrw:41:";
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

// R220-style reply: the firmware echoes the address, then the value.
inline std::vector<unsigned char> MakeEepromReadReplyEE(uint8_t addr, uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nEE:";
    s += hex[(addr >> 4) & 0x0F];
    s += hex[addr & 0x0F];
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

// ---------------------------------------------------------------------------
// ewr::Session facade (session.h): blocker policy and the reset lifecycle
// ---------------------------------------------------------------------------

// Scripted device gateway: drives the whole Session lifecycle with no USB.
struct FakeGateway : ewr::IDeviceGateway
{
    ewr::QueryRunResult queryResult;
    ewr::ResetRunResult resetResult;
    int queryCalls = 0;
    int resetCalls = 0;
    std::vector<std::vector<unsigned char>> lastSequence;
    ewr::ExecutorOptions lastResetOptions;
    ewr::ExecutorOptions lastQueryOptions;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions& options) override
    {
        queryCalls++;
        lastQueryOptions = options;
        return queryResult;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>& sequence,
                                 const ewr::ExecutorOptions& options) override
    {
        resetCalls++;
        lastSequence = sequence;
        lastResetOptions = options;
        return resetResult;
    }
};

inline ewr::DbPrinterModel MakeSessionModel()
{
    ewr::DbPrinterModel model;
    model.name = "TestJet 100";
    model.rkey = 0x3B10;
    model.wkey = "McLaren";
    model.rlen = 1;
    model.wlen = 1;

    ewr::PadGroup group;
    group.description = "Waste counter";
    group.addresses = { 0x0C, 0x0D };
    group.reset_values = { 0x00, 0x00 };
    model.pad_groups.push_back(group);

    return model;
}

// A gateway whose printer answers with INK OUT (a foreign lock) and whose
// counters already read back as the reset values.
inline FakeGateway MakeSessionGateway()
{
    FakeGateway gw;
    gw.queryResult.deviceFound = true;
    gw.queryResult.query.success = true;
    gw.queryResult.query.replies = {
        MakeSt2Reply(), // state ERROR, INK OUT (0x05)
        MakeEepromReadReplyEE(0x0C, 0x00),
        MakeEepromReadReplyEE(0x0D, 0x00),
    };
    gw.resetResult.deviceFound = true;
    gw.resetResult.exec.success = true;
    return gw;
}

// ---- IEEE 1284.4 session core ----

// Returns queued chunks one Drain() at a time, so tests can split one
// packet across reads or coalesce several packets into one read.
class ChunkedTransport final : public ewr::ITransport
{
public:
    std::vector<std::vector<unsigned char>> chunks;
    size_t next = 0;

    bool Send(const std::vector<unsigned char>&) override { return true; }

    std::vector<unsigned char> Drain(int) override
    {
        if (next >= chunks.size())
            return {};
        return chunks[next++];
    }
};

inline std::vector<unsigned char> D4Frame(unsigned char psid, unsigned char ssid,
                                          const std::vector<unsigned char>& payload,
                                          unsigned char credit = 0x00, unsigned char control = 0x00)
{
    std::vector<unsigned char> raw;
    const size_t total = payload.size() + 6;
    raw.push_back(psid);
    raw.push_back(ssid);
    raw.push_back(static_cast<unsigned char>((total >> 8) & 0xFF));
    raw.push_back(static_cast<unsigned char>(total & 0xFF));
    raw.push_back(credit);
    raw.push_back(control);
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

// Answers the D4 session choreography like a real R220: Init, GetSocketID,
// OpenChannel (MTU to printer configurable, MTU to host 256, no initial
// credit), credit grants of 1, close/exit. Data packets go to dataReplyFor.
inline std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)>
D4SessionReplier(unsigned char socket,
                 std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> dataReplyFor,
                 bool answerGetSocketId = true,
                 unsigned char mtuHi = 0x00, unsigned char mtuLo = 0x40)
{
    return [socket, dataReplyFor, answerGetSocketId, mtuHi, mtuLo](
               const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        if (sent.size() < 7)
            return {};

        // EJL enter blob: transaction header followed by the '@EJL' magic.
        if (sent[0] == 0x00 && sent[1] == 0x00 && sent[6] == '@')
            return D4Frame(0x00, 0x00, { 0xC5, 0x00 }, 0x01);

        if (sent[0] == 0x00 && sent[1] == 0x00)
        {
            switch (sent[6])
            {
                case 0x00: return D4Frame(0x00, 0x00, { 0x80, 0x00, 0x10 }, 0x01);
                case 0x09:
                    if (!answerGetSocketId)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x89, 0x00, socket }, 0x01);
                case 0x01:
                    return D4Frame(0x00, 0x00,
                                   { 0x81, 0x00, socket, socket, mtuHi, mtuLo, 0x01, 0x00, 0x00, 0x00 }, 0x01);
                case 0x03: return D4Frame(0x00, 0x00, { 0x83, 0x00, socket, socket }, 0x01);
                case 0x04: return D4Frame(0x00, 0x00, { 0x84, 0x00, socket, socket, 0x00, 0x01 }, 0x01);
                case 0x02: return D4Frame(0x00, 0x00, { 0x82, 0x00, socket, socket }, 0x01);
                case 0x08: return D4Frame(0x00, 0x00, { 0x88, 0x00 }, 0x01);
                default:   return {};
            }
        }

        return dataReplyFor ? dataReplyFor(sent) : std::vector<unsigned char>{};
    };
}

// A D4 printer that keeps real credit books on both sides (issue #39). A reply
// goes out only against credit the host granted, a data packet is accepted
// only against credit the printer granted, and Init resets both - so a reply
// that vanishes on the way, the way one does when another process reads the
// shared usbprint handle first, leaves the books as wrong as on hardware.
// Like an R220, Init does not drop a reply still waiting for credit: the next
// session that grants some gets it.
class CreditBookPrinter
{
public:
    // A reply left over from an earlier session, waiting for credit.
    void HoldOver(std::vector<unsigned char> reply) { held_.push_back(std::move(reply)); }

    std::function<std::vector<unsigned char>(const std::vector<unsigned char>&)> answer;
    // 1-based counts of data replies / credit-request replies that the
    // printer sends but the host never receives.
    std::vector<int> lostDataReplies;
    std::vector<int> lostCreditReplies;
    // After this many data replies the printer never answers data again,
    // not even on a fresh session. 0 = never.
    int silentAfter = 0;
    // Sessions after the first whose GetSocketID and OpenChannel go
    // unanswered: the L365's control service busy while its D4 core still
    // answers Init and Exit.
    int sessionsWithSilentService = 0;

    int inits = 0;
    int dataReplies = 0;
    int creditReplies = 0;

    std::vector<unsigned char> operator()(const std::vector<unsigned char>& sent)
    {
        if (sent.size() < 7)
            return {};

        if (sent[0] == 0x00 && sent[1] == 0x00 && sent[6] == '@')
            return D4Frame(0x00, 0x00, { 0xC5, 0x00 }, 0x01);

        if (sent[0] == 0x00 && sent[1] == 0x00)
        {
            switch (sent[6])
            {
                case 0x00:
                    ++inits;
                    printerCredit_ = 0;
                    hostCredit_ = 0;
                    serviceSilent_ = (inits > 1) && (sessionsWithSilentService-- > 0);
                    return D4Frame(0x00, 0x00, { 0x80, 0x00, 0x10 }, 0x01);
                case 0x09:
                    if (serviceSilent_)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x89, 0x00, 0x02 }, 0x01);
                case 0x01:
                    if (serviceSilent_)
                        return {};
                    return D4Frame(0x00, 0x00, { 0x81, 0x00, 0x02, 0x02, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00 }, 0x01);
                case 0x03:
                {
                    printerCredit_ += (sent.size() >= 11) ? ((sent[9] << 8) | sent[10]) : 0;
                    std::vector<unsigned char> out = D4Frame(0x00, 0x00, { 0x83, 0x00, 0x02, 0x02 }, 0x01);
                    SendHeld(out);
                    return out;
                }
                case 0x04:
                {
                    // The host asks with a maximum outstanding credit of one.
                    const unsigned char granted = (hostCredit_ < 1) ? 1 : 0;
                    hostCredit_ += granted;
                    ++creditReplies;
                    if (Contains(lostCreditReplies, creditReplies))
                        return {};
                    return D4Frame(0x00, 0x00, { 0x84, 0x00, 0x02, 0x02, 0x00, granted }, 0x01);
                }
                case 0x02: return D4Frame(0x00, 0x00, { 0x82, 0x00, 0x02, 0x02 }, 0x01);
                case 0x08: return D4Frame(0x00, 0x00, { 0x88, 0x00 }, 0x01);
                default:   return {};
            }
        }

        if (hostCredit_ <= 0)
            return {};
        --hostCredit_;

        if (silentAfter > 0 && dataReplies >= silentAfter)
            return {};

        held_.push_back(answer(sent));
        std::vector<unsigned char> out;
        SendHeld(out);
        return out;
    }

private:
    static bool Contains(const std::vector<int>& list, int n)
    {
        return std::find(list.begin(), list.end(), n) != list.end();
    }

    void SendHeld(std::vector<unsigned char>& out)
    {
        while (!held_.empty() && printerCredit_ > 0)
        {
            --printerCredit_;
            ++dataReplies;
            if (!Contains(lostDataReplies, dataReplies))
                out.insert(out.end(), held_.front().begin(), held_.front().end());
            held_.erase(held_.begin());
        }
    }

    bool serviceSilent_ = false;
    int printerCredit_ = 0; // granted by the host, spent on replies
    int hostCredit_ = 0;    // granted to the host, spent on its data
    std::vector<std::vector<unsigned char>> held_;
};

// Reads of 0x10..0x15 on a 1-byte-address model, each answered with its own
// address echoed and value 0x40 + address.
inline std::vector<std::vector<unsigned char>> LostReplyQueries()
{
    std::vector<std::vector<unsigned char>> queries;
    for (uint8_t addr = 0x10; addr <= 0x15; ++addr)
        queries.push_back(ewr::ctrl::ReadPacket(0x0008, addr, 1));
    return queries;
}

inline std::vector<unsigned char> EchoReadReply(const std::vector<unsigned char>& sent)
{
    const uint8_t addr = sent.back();
    return MakeEepromReadReplyEE(addr, static_cast<uint8_t>(0x40 + addr));
}

inline ewr::ExecutorOptions LostReplyOptions()
{
    ewr::ExecutorOptions options = FastOptions();
    options.useSessionLayer = true;
    options.handshakeDrainTimeoutMs = 40;
    options.writeAckTimeoutMs = 20;
    options.sessionRestartBackoffMs = 1; // the wait itself is not what is under test
    return options;
}

inline int CountAnsweredReads(const ewr::QuerySessionResult& result)
{
    int answered = 0;
    for (size_t i = 0; i < result.replies.size(); ++i)
    {
        uint8_t value = 0;
        const int addr = 0x10 + static_cast<int>(i);
        if (ewr::ctrl::ParseReadReply(result.replies[i], value, addr) && value == 0x40 + addr)
            ++answered;
    }
    return answered;
}

// Answers like the SX110 did: D4 replies until the packet-mode exit, then
// plain "JGPG" replies with the bytes WIC read.
inline FakeTransport MakeJigPrinter(std::map<int, uint8_t> memory, bool takesJigMode = true)
{
    FakeTransport t;
    t.replyFor = [memory, takesJigMode](const std::vector<unsigned char>& sent) -> std::vector<unsigned char> {
        if (sent.size() == 27 && sent[5] == '@')                                  // EJL enter or leave
            return { 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0xc5, 0x00 };
        if (sent == std::vector<unsigned char>({ 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x10 }))
            return { 0x00, 0x00, 0x00, 0x09, 0x01, 0x00, 0x80, 0x00, 0x10 };     // D4 Init
        const int address = ewr::jig::ReadPacketAddress(sent);
        if (address >= 0 && takesJigMode)
        {
            auto it = memory.find(address);
            return { 'J', 'G', 'P', 'G', 0x01, 0x00, it == memory.end() ? uint8_t(0) : it->second };
        }
        return {};
    };
    return t;
}

// L3260-style reply: models with rlen == 2 echo a 2-byte address.
inline std::vector<unsigned char> MakeEepromReadReplyEE16(uint16_t addr, uint8_t value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s = "@BDC PS\r\nEE:";
    s += hex[(addr >> 12) & 0x0F];
    s += hex[(addr >> 8) & 0x0F];
    s += hex[(addr >> 4) & 0x0F];
    s += hex[addr & 0x0F];
    s += hex[(value >> 4) & 0x0F];
    s += hex[value & 0x0F];
    s += ';';
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

inline bool EndsWithKey(const std::vector<unsigned char>& packet, const std::string& key)
{
    if (packet.size() < key.size())
        return false;

    const size_t offset = packet.size() - key.size();
    for (size_t i = 0; i < key.size(); ++i)
    {
        if (packet[offset + i] != static_cast<unsigned char>(key[i]))
            return false;
    }

    return true;
}

// The SX110's layout: two little-endian counters, the reset clearing six bytes.
inline ewr::DbPrinterModel MakeJigModel()
{
    ewr::DbPrinterModel model;
    model.name = "SX110";
    model.jig = true;
    model.mem_high = 0x2FF;

    ewr::PadGroup group;
    group.addresses = { 0x214, 0x215, 0x216, 0x217, 0x218, 0x219 };
    group.reset_values = { 0, 0, 0, 0, 0, 0 };

    ewr::CounterSpec first;
    first.description = "Waste Counter 1";
    first.bytes = { { 0x214 }, { 0x215, 0xFF, 256 } };
    first.max_value = 7300;

    ewr::CounterSpec second;
    second.description = "Waste Counter 2";
    second.bytes = { { 0x216 }, { 0x217, 0xFF, 256 } };
    second.max_value = 38900;

    group.counters = { first, second };
    model.pad_groups = { group };
    return model;
}

// A status over D4, then the JIG read: what the SX110 answered WIC.
struct JigGateway : ewr::IDeviceGateway
{
    std::vector<ewr::ExecutorOptions> queryOptions;
    std::vector<std::vector<std::vector<unsigned char>>> queries;
    int resets = 0;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>& sent,
                                 const ewr::ExecutorOptions& options) override
    {
        queryOptions.push_back(options);
        queries.push_back(sent);

        ewr::QueryRunResult run;
        run.deviceFound = true;
        run.query.success = true;

        if (!options.jigMode)
        {
            run.query.replies = { MakeSt2Reply() };
            return run;
        }

        const std::map<int, uint8_t> memory = { { 0x214, 0xe0 }, { 0x215, 0x01 }, { 0x216, 0x00 }, { 0x217, 0x94 } };
        for (const auto& packet : sent)
        {
            const auto it = memory.find(ewr::jig::ReadPacketAddress(packet));
            run.query.replies.push_back({ 'J', 'G', 'P', 'G', 0x01, 0x00, it == memory.end() ? uint8_t(0) : it->second });
        }
        return run;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        resets++;
        return {};
    }
};

// Answers each read by its key, the way a printer does, and ends the session
// at the first read it ignores - which is what the real query session does.
struct KeyGateway : ewr::IDeviceGateway
{
    std::function<std::vector<unsigned char>(uint16_t, int)> answer;
    bool deviceFound = true;
    bool handshakeFailed = false;
    int sessions = 0;
    int resets = 0;
    int nonReads = 0;
    std::vector<uint16_t> asked;
    ewr::ExecutorOptions lastOptions;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>& queries,
                                 const ewr::ExecutorOptions& options) override
    {
        sessions++;
        lastOptions = options;

        ewr::QueryRunResult run;
        run.deviceFound = deviceFound;
        if (!deviceFound)
            return run;

        if (handshakeFailed)
        {
            run.query.handshakeFailed = true;
            run.query.error = "no handshake";
            return run;
        }

        run.query.handshakeConfirmed = true;
        for (const auto& q : queries)
        {
            // D4 header (6), '||', length (2), key (2), action 0x41 + 0xBE.
            if (q.size() < 16 || q[12] != 0x41 || q[13] != 0xBE)
                nonReads++;

            const uint16_t rkey = static_cast<uint16_t>(q[10] | (q[11] << 8));
            asked.push_back(rkey);
            run.query.packetsSent++;

            std::vector<unsigned char> reply = answer(rkey, q[8] - 5);
            if (reply.empty())
            {
                run.query.error = "The printer stopped answering";
                run.query.replies.resize(queries.size());
                return run;
            }

            run.query.replies.push_back(std::move(reply));
        }

        run.query.success = true;
        return run;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        resets++;
        return {};
    }
};

// The session model with a cartridge ink map: two colors, one counter
// byte each, plus a close op that must NOT run on the ink path (ink
// persistence comes from the power cycle, never from a commit).
inline ewr::DbPrinterModel MakeInkSessionModel()
{
    ewr::DbPrinterModel model = MakeSessionModel();

    ewr::InkGroup black;
    black.color = "black";
    black.addresses = { 0x84 };
    black.reset_values = { 0x00 };
    model.ink_groups.push_back(black);

    ewr::InkGroup lightmagenta;
    lightmagenta.color = "lightmagenta";
    lightmagenta.addresses = { 0x98 };
    lightmagenta.reset_values = { 0x00 };
    model.ink_groups.push_back(lightmagenta);

    ewr::CloseOp close;
    close.address = 0x0C;
    close.and_mask = 0xFF;
    close.or_mask = 0x80;
    model.close_ops.push_back(close);

    return model;
}

// A gateway whose printer answers INK OUT and whose ink counter bytes
// already read back zeroed (the post-write mirror state).
inline FakeGateway MakeInkSessionGateway()
{
    FakeGateway gw;
    gw.queryResult.deviceFound = true;
    gw.queryResult.query.success = true;
    gw.queryResult.query.replies = {
        MakeSt2Reply(), // state ERROR, INK OUT (0x05)
        MakeEepromReadReplyEE(0x84, 0x00),
        MakeEepromReadReplyEE(0x98, 0x00),
    };
    gw.resetResult.deviceFound = true;
    gw.resetResult.exec.success = true;
    return gw;
}

// A stand-in for what an ESC/P Remote capable printer answers a factory write.
// No END4 envelope: the '||' verdict arrives bare.
inline std::vector<unsigned char> BareOkAck()
{
    const std::string ack = "||:42:OK;";
    return std::vector<unsigned char>(ack.begin(), ack.end());
}

// Wrap a factory-control inner payload in the 10-byte END4 reply framing a real
// printer returns, so FakeTransport can hand it back byte-shaped.
inline std::vector<unsigned char> End4Wrap(const std::vector<unsigned char>& inner)
{
    std::vector<unsigned char> reply = {
        'E', 'N', 'D', '4', 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x02, 0x00
    };
    reply.insert(reply.end(), inner.begin(), inner.end());
    reply[9] = static_cast<unsigned char>(reply.size() & 0xFF);
    return reply;
}

// ---------------------------------------------------------------------------
// Composite backend (usb_composite.cpp)
// ---------------------------------------------------------------------------

// Describe(), QueryDeviceId() and AttemptsPerCandidate() all echo the ordinal
// they were handed, so a mistranslated ordinal fails loudly instead of
// looking like a pass.
class FakeBackend final : public ewr::usb::Backend
{
public:
    FakeBackend(std::string name, std::vector<ewr::usb::Candidate> candidates, std::string initError = "")
        : name_(std::move(name)), candidates_(std::move(candidates)), initError_(std::move(initError))
    {
        for (std::size_t i = 0; i < candidates_.size(); ++i)
            candidates_[i].ordinal = i;
    }

    const char* PlatformName() const override { return name_.c_str(); }
    const std::string& InitError() const override { return initError_; }

    std::vector<ewr::usb::Candidate> Enumerate() override
    {
        ++enumerations;
        return candidates_;
    }

    std::string Describe(std::size_t ordinal) const override
    {
        return name_ + " candidate " + std::to_string(ordinal);
    }

    ewr::ITransport* Open(std::size_t ordinal) override
    {
        opened.push_back(ordinal);
        return openSucceeds ? &transport : nullptr;
    }

    void Close() override { ++closes; }

    bool SoftReset() override
    {
        ++softResets;
        return softResetSucceeds;
    }

    int AttemptsPerCandidate(std::size_t ordinal) const override
    {
        return attemptsBase + static_cast<int>(ordinal);
    }

    std::string QueryDeviceId(std::size_t ordinal) override
    {
        return name_ + " id " + std::to_string(ordinal);
    }

    std::string DescribeOpenFailure(bool) override { return name_ + " could not open."; }

    FakeTransport transport;
    std::vector<std::size_t> opened;
    int closes = 0;
    int softResets = 0;
    int enumerations = 0;
    int attemptsBase = 1;
    bool openSucceeds = true;
    bool softResetSucceeds = true;

private:
    std::string name_;
    std::vector<ewr::usb::Candidate> candidates_;
    std::string initError_;
};

inline ewr::usb::Candidate MakeCandidate(const std::string& className, int interfaceNumber,
                                       const std::string& pid = "1187")
{
    ewr::usb::Candidate cand;
    cand.className = className;
    cand.interfaceNumber = interfaceNumber;
    cand.path = className + "#if_" + std::to_string(interfaceNumber);
    cand.pid = pid;
    return cand;
}

inline ewr::usb::BackendMember Member(std::unique_ptr<ewr::usb::Backend> backend, std::string tag)
{
    ewr::usb::BackendMember member;
    member.backend = std::move(backend);
    member.tag = std::move(tag);
    return member;
}

// The exact 57 bytes topdeckg's ET-2800 returned to the END4 packet-mode
// flush in issue #16.
inline std::vector<unsigned char> Http500Reply()
{
    const std::string body = "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    return std::vector<unsigned char>(body.begin(), body.end());
}

// ---------------------------------------------------------------------------
// Address discovery (discover.cpp)
// ---------------------------------------------------------------------------

inline ewr::discovery::EepromSnapshot Snap(std::initializer_list<std::pair<uint16_t, int>> v)
{
    return ewr::discovery::EepromSnapshot(v);
}

// ---- SNMP (the --ip network transport) ----------------------------------

namespace snmp_test {

    inline void AppendTlv(std::vector<unsigned char>& out, unsigned char tag, const std::vector<unsigned char>& content)
    {
        out.push_back(tag);
        if (content.size() < 0x80)
        {
            out.push_back(static_cast<unsigned char>(content.size()));
        }
        else
        {
            out.push_back(0x82);
            out.push_back(static_cast<unsigned char>(content.size() >> 8));
            out.push_back(static_cast<unsigned char>(content.size() & 0xFF));
        }
        out.insert(out.end(), content.begin(), content.end());
    }

    // Shortest BER form of a non-negative INTEGER.
    inline std::vector<unsigned char> IntegerContent(int value)
    {
        std::vector<unsigned char> bytes;
        do
        {
            bytes.insert(bytes.begin(), static_cast<unsigned char>(value & 0xFF));
            value >>= 8;
        } while (value > 0);

        if (bytes[0] & 0x80)
            bytes.insert(bytes.begin(), 0x00);
        return bytes;
    }

    // A GetRequest read back the way an agent reads it: its ID and its one
    // OID. False for anything else, or for a community other than "public".
    inline bool ParseRequest(const std::vector<unsigned char>& datagram, int& id, ewr::snmp::Oid& oid)
    {
        std::size_t pos = 0;

        // One TLV of `tag` at pos: stepped into, or over with its content kept.
        auto tlv = [&](unsigned char tag, bool enter, std::vector<unsigned char>* content) -> bool
        {
            if (pos + 2 > datagram.size() || datagram[pos] != tag)
                return false;

            std::size_t length = datagram[pos + 1];
            pos += 2;
            if (length & 0x80)
            {
                const std::size_t count = length & 0x7F;
                length = 0;
                for (std::size_t i = 0; i < count && pos < datagram.size(); ++i)
                    length = (length << 8) | datagram[pos++];
            }

            if (length > datagram.size() - pos)
                return false;
            if (content)
                content->assign(datagram.begin() + static_cast<long>(pos),
                                datagram.begin() + static_cast<long>(pos + length));
            if (!enter)
                pos += length;
            return true;
        };

        std::vector<unsigned char> community, idBytes, oidBytes;
        if (!tlv(0x30, true, nullptr) || !tlv(0x02, false, nullptr) || !tlv(0x04, false, &community)
            || std::string(community.begin(), community.end()) != "public"
            || !tlv(0xA0, true, nullptr) || !tlv(0x02, false, &idBytes)
            || !tlv(0x02, false, nullptr) || !tlv(0x02, false, nullptr)
            || !tlv(0x30, true, nullptr) || !tlv(0x30, true, nullptr) || !tlv(0x06, false, &oidBytes)
            || idBytes.empty() || oidBytes.empty())
            return false;

        id = 0;
        for (unsigned char b : idBytes)
            id = (id << 8) | b;

        oid = { oidBytes[0] / 40u, oidBytes[0] % 40u };
        uint32_t arc = 0;
        for (std::size_t i = 1; i < oidBytes.size(); ++i)
        {
            arc = (arc << 7) | (oidBytes[i] & 0x7Fu);
            if ((oidBytes[i] & 0x80) == 0)
            {
                oid.push_back(arc);
                arc = 0;
            }
        }
        return true;
    }

    // A GetResponse as an agent would send it. The OID is not echoed
    // faithfully - the decoder only steps over it.
    inline std::vector<unsigned char> MakeResponse(int requestId, int errorStatus, unsigned char valueTag,
                                            const std::vector<unsigned char>& value)
    {
        std::vector<unsigned char> binding;
        AppendTlv(binding, 0x06, { 0x2B, 0x06, 0x01 });
        AppendTlv(binding, valueTag, value);

        std::vector<unsigned char> list;
        AppendTlv(list, 0x30, binding);

        std::vector<unsigned char> pdu;
        AppendTlv(pdu, 0x02, IntegerContent(requestId));
        AppendTlv(pdu, 0x02, IntegerContent(errorStatus));
        AppendTlv(pdu, 0x02, { 0x00 });
        AppendTlv(pdu, 0x30, list);

        std::vector<unsigned char> message;
        AppendTlv(message, 0x02, { 0x00 });
        AppendTlv(message, 0x04, { 'p', 'u', 'b', 'l', 'i', 'c' });
        AppendTlv(message, 0xA2, pdu);

        std::vector<unsigned char> datagram;
        AppendTlv(datagram, 0x30, message);
        return datagram;
    }

    // A printer that serves a fixed set of OIDs. `dropFirst` loses that many
    // requests before it starts answering; `silent` never answers.
    struct ScriptedPrinter final : ewr::snmp::IDatagramChannel
    {
        std::map<std::string, std::vector<unsigned char>> served;
        std::vector<std::vector<unsigned char>> pending;
        std::vector<std::string> requested; // OIDs, one per datagram sent
        int dropFirst = 0;
        bool silent = false;
        bool refuseSend = false; // the local stack will not send at all

        bool Send(const std::vector<unsigned char>& datagram) override
        {
            if (refuseSend)
                return false;

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid) || std::find(known.begin(), known.end(), oid) == known.end())
            {
                requested.push_back("?");
                return true;
            }

            const std::string name = ewr::snmp::FormatOid(oid);
            requested.push_back(name);

            if (silent)
                return true;
            if (dropFirst > 0)
            {
                --dropFirst;
                return true;
            }

            const auto hit = served.find(name);
            if (hit == served.end())
                pending.push_back(MakeResponse(id, 2, 0x05, {}));
            else
                pending.push_back(MakeResponse(id, 0, 0x04, hit->second));
            return true;
        }

        std::vector<unsigned char> Receive(int) override
        {
            if (pending.empty())
                return {};

            std::vector<unsigned char> next = pending.front();
            pending.erase(pending.begin());
            return next;
        }

        void Serve(const ewr::snmp::Oid& oid, const std::vector<unsigned char>& value)
        {
            known.push_back(oid);
            served[ewr::snmp::FormatOid(oid)] = value;
        }

        // Recognised, but answered with noSuchName.
        void Know(const ewr::snmp::Oid& oid) { known.push_back(oid); }

        std::vector<ewr::snmp::Oid> known;
    };

    inline std::vector<unsigned char> Bytes(const std::string& text)
    {
        return std::vector<unsigned char>(text.begin(), text.end());
    }

    // The EPSON-CTRL command inside a generated query packet.
    inline std::vector<unsigned char> CommandOf(const std::vector<unsigned char>& packet)
    {
        std::vector<unsigned char> command;
        ewr::d4::ExtractDataPayload(packet, command);
        return command;
    }

    inline ewr::DbPrinterModel TwoByteModel()
    {
        ewr::DbPrinterModel model;
        model.name = "NetTest";
        model.rkey = 0x0797; // 151, 7
        model.wkey = "Maribaya";

        ewr::PadGroup group;
        group.addresses = { 0x30, 0x1FE };
        group.reset_values = { 0x00, 0x00 };
        model.pad_groups.push_back(group);
        return model;
    }

} // namespace snmp_test

namespace snmp_test {

    // A printer with an EEPROM: reads answer from it, writes with
    // `acceptedKey` change it, writes with any other keyword come back
    // ':42:NG;', and `refuse` makes every write come back ':42:NA;'.
    // `locked` is firmware that shuts the EEPROM over the network: status
    // still answers, every read and write is refused with a bare block.
    struct EepromPrinter final : ewr::snmp::IDatagramChannel
    {
        ewr::DbPrinterModel model;
        std::map<uint16_t, uint8_t> eeprom;
        std::string acceptedKey;
        bool refuse = false;
        bool locked = false;
        int writesSeen = 0;
        std::vector<std::vector<unsigned char>> pending;

        bool Send(const std::vector<unsigned char>& datagram) override
        {
            static const char* hex = "0123456789ABCDEF";

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid))
                return true;

            if (oid == ewr::snmp::StatusOid())
            {
                pending.push_back(MakeResponse(id, 0, 0x04,
                    Bytes("@BDC ST2\r\n\x03" + std::string(1, '\0') + "\x01\x01\x04")));
                return true;
            }

            for (auto& cell : eeprom)
            {
                if (oid == ewr::snmp::ControlOid(CommandOf(
                               ewr::ctrl::ReadPacket(model.rkey, cell.first))))
                {
                    if (locked)
                    {
                        pending.push_back(MakeResponse(id, 0, 0x04, Bytes("||:41:NA;\f")));
                        return true;
                    }

                    std::string read = "@BDC PS\r\nEE:";
                    read += hex[(cell.first >> 12) & 0xF]; read += hex[(cell.first >> 8) & 0xF];
                    read += hex[(cell.first >> 4) & 0xF];  read += hex[cell.first & 0xF];
                    read += hex[(cell.second >> 4) & 0xF]; read += hex[cell.second & 0xF];
                    read += ";\f";
                    pending.push_back(MakeResponse(id, 0, 0x04, Bytes(read)));
                    return true;
                }

                for (const std::string& key : { model.wkey, model.wkey1 })
                {
                    if (key.empty())
                        continue;

                    // Any byte: a restore writes back what a reset cleared.
                    for (int value = 0; value < 256; ++value)
                    {
                        if (oid != ewr::snmp::ControlOid(CommandOf(ewr::ctrl::WritePacket(
                                       model.rkey, cell.first, static_cast<uint8_t>(value), key))))
                            continue;

                        ++writesSeen;
                        std::string verdict = "OK";
                        if (refuse || locked)
                            verdict = "NA";
                        else if (key != acceptedKey)
                            verdict = "NG";
                        else
                            cell.second = static_cast<uint8_t>(value);

                        pending.push_back(MakeResponse(id, 0, 0x04, Bytes("@BDC PS\r\n||:42:" + verdict + ";\f")));
                        return true;
                    }
                }
            }

            return true;
        }

        std::vector<unsigned char> Receive(int) override
        {
            if (pending.empty())
                return {};

            std::vector<unsigned char> next = pending.front();
            pending.erase(pending.begin());
            return next;
        }
    };

    inline std::unique_ptr<EepromPrinter> FullPads()
    {
        auto printer = std::make_unique<EepromPrinter>();
        printer->model = TwoByteModel();
        printer->model.wkey1 = "Nbsjcbzb";
        printer->acceptedKey = printer->model.wkey;
        printer->eeprom[0x30] = 0xCA;
        printer->eeprom[0x1FE] = 0x18;
        return printer;
    }

} // namespace snmp_test

namespace snmp_test {

    // A network of agents answering a broadcast: each answers every request
    // it hears, so two rounds to two addresses bring four answers apiece.
    struct ScriptedNetwork final : ewr::snmp::IBroadcastChannel
    {
        struct Agent
        {
            std::string address;
            std::string deviceId; // empty: answers noSuchName
            bool wrongId = false;
            bool malformed = false;
        };

        std::vector<Agent> agents;
        std::vector<std::string> sentTo;
        std::vector<std::pair<std::string, std::vector<unsigned char>>> pending;
        bool refuseSend = false; // macOS without the Local Network permission

        bool SendTo(const std::string& address, uint16_t port, const std::vector<unsigned char>& datagram) override
        {
            if (refuseSend)
                return false;

            sentTo.push_back(address + ":" + std::to_string(port));

            int id = 0;
            ewr::snmp::Oid oid;
            if (!ParseRequest(datagram, id, oid) || oid != ewr::snmp::DeviceIdOid())
                return true;

            for (const Agent& agent : agents)
            {
                if (agent.malformed)
                    pending.push_back({ agent.address, { 0x30, 0x03, 0x02 } });
                else if (agent.deviceId.empty())
                    pending.push_back({ agent.address, MakeResponse(id, 2, 0x05, {}) });
                else
                    pending.push_back({ agent.address, MakeResponse(agent.wrongId ? id + 1 : id, 0, 0x04,
                                                                    Bytes(agent.deviceId)) });
            }
            return true;
        }

        bool ReceiveFrom(int timeoutMs, std::string& from, std::vector<unsigned char>& datagram) override
        {
            if (pending.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
                return false;
            }

            from = pending.front().first;
            datagram = pending.front().second;
            pending.erase(pending.begin());
            return true;
        }
    };

} // namespace snmp_test

// ---------------------------------------------------------------------------
// Backups (backup.h) and the backup step of the reset lifecycle
// ---------------------------------------------------------------------------

inline ewr::Backup MakeTestBackup()
{
    ewr::Backup backup;
    backup.model = "TestJet 100";
    backup.target = "waste";
    backup.created = "2026-10-25T09:30:00Z";
    backup.ewrVersion = "1.6.0";
    backup.transport = "usb";
    backup.deviceId = "MFG:EPSON;MDL:TestJet 100;";
    backup.serial = "X7A9000123";
    backup.addressLength = 1;
    backup.bytes = { { 0x0C, 0x12 }, { 0x0D, 0x34 }, { 0x2B, -1 } };
    return backup;
}

// ---------------------------------------------------------------------------
// Restore (restore.h): the plan, and Session::Restore over the lifecycle
// ---------------------------------------------------------------------------

inline bool SequenceHas(const std::vector<std::vector<unsigned char>>& sequence,
                        const std::vector<unsigned char>& packet)
{
    return std::find(sequence.begin(), sequence.end(), packet) != sequence.end();
}

inline size_t CountWritePackets(const std::vector<std::vector<unsigned char>>& sequence)
{
    // '||', a 2-byte length, the 2-byte read key, then the action code.
    size_t writes = 0;
    for (const auto& packet : sequence)
    {
        const std::string text(packet.begin(), packet.end());
        const size_t pipes = text.find("||");
        if (pipes != std::string::npos && pipes + 6 < packet.size() && packet[pipes + 6] == 0x42)
            writes++;
    }
    return writes;
}

inline std::vector<unsigned char> MakeRangeReply(const std::string& text)
{
    const std::string s = "@BDC PS\r\n" + text;
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

// A printer in memory: every read is answered from `eeprom`, the way the
// R220 answers (address echo). Range reads are answered, refused, or
// answered wrongly, as the test asks.
struct EepromGateway : ewr::IDeviceGateway
{
    enum class Ranges { Answer, Refuse, Lie };

    std::map<uint16_t, uint8_t> eeprom;
    Ranges ranges = Ranges::Answer;
    int sessions = 0;
    int rangePackets = 0;
    int singlePackets = 0;

    static std::string Hex(uint32_t value, int digits)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string s;
        for (int i = digits - 1; i >= 0; --i)
            s += hex[(value >> (i * 4)) & 0x0F];
        return s;
    }

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>& queries,
                                 const ewr::ExecutorOptions&) override
    {
        sessions++;

        ewr::QueryRunResult run;
        run.deviceFound = true;
        run.query.success = true;

        for (const auto& packet : queries)
        {
            // D4 header (6), '||', length (2), key (2), action, 2 check bytes.
            const bool factory = packet.size() > 15 && packet[6] == '|' && packet[7] == '|';
            if (!factory)
            {
                run.query.replies.push_back(MakeSt2Reply());
                continue;
            }

            const uint8_t action = packet[12];
            const uint16_t address = packet[15];
            if (action == 0x41)
            {
                singlePackets++;
                run.query.replies.push_back(MakeEepromReadReplyEE(static_cast<uint8_t>(address), eeprom[address]));
            }
            else if (action == 0x51)
            {
                rangePackets++;
                const uint8_t count = packet[16];
                if (ranges == Ranges::Refuse)
                {
                    run.query.replies.push_back(MakeRangeReply("||:51:NA;"));
                    continue;
                }

                std::string text = "EE:" + Hex(address, 2);
                for (uint8_t k = 0; k < count; ++k)
                    text += Hex(ranges == Ranges::Lie ? 0xFF : eeprom[static_cast<uint16_t>(address + k)], 2);
                run.query.replies.push_back(MakeRangeReply(text + ";"));
            }
        }

        return run;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        return {};
    }
};

// ---------------------------------------------------------------------------
// Maintenance: remote-mode jobs, the gate, the routes, send-once and watching
// ---------------------------------------------------------------------------

inline std::vector<unsigned char> MakeSt2State(int state, int error = -1)
{
    std::vector<unsigned char> body;
    body.insert(body.end(), { 0x01, 0x01, static_cast<unsigned char>(state) });
    if (error >= 0)
        body.insert(body.end(), { 0x02, 0x01, static_cast<unsigned char>(error) });
    return WrapSt2Body(body);
}

// A printer whose status moves through a script, one entry per poll, and
// that records every command and print job it is sent.
struct MaintenanceGateway : ewr::IDeviceGateway
{
    std::vector<std::vector<unsigned char>> statuses; // one per status query, the last repeats
    size_t nextStatus = 0;
    std::map<uint16_t, uint8_t> eeprom;
    std::vector<std::vector<unsigned char>> commands;
    std::vector<std::vector<unsigned char>> jobs;
    std::vector<unsigned char> commandReply;
    bool commandSupported = true;

    ewr::QueryRunResult RunQuery(const std::vector<std::vector<unsigned char>>&,
                                 const std::vector<std::vector<unsigned char>>& queries,
                                 const ewr::ExecutorOptions&) override
    {
        ewr::QueryRunResult run;
        run.deviceFound = true;
        run.query.success = true;

        for (const auto& packet : queries)
        {
            const bool factory = packet.size() > 15 && packet[6] == '|' && packet[7] == '|';
            if (!factory)
            {
                const size_t i = std::min(nextStatus, statuses.size() - 1);
                nextStatus++;
                run.query.replies.push_back(statuses[i]);
                continue;
            }
            const uint8_t address = packet[15];
            run.query.replies.push_back(MakeEepromReadReplyEE(address, eeprom[address]));
        }
        return run;
    }

    ewr::ResetRunResult RunReset(const std::vector<std::vector<unsigned char>>&,
                                 const ewr::ExecutorOptions&) override
    {
        return {};
    }

    ewr::CommandRunResult RunCommand(const std::vector<unsigned char>& command,
                                     const ewr::ExecutorOptions&) override
    {
        ewr::CommandRunResult out;
        out.supported = commandSupported;
        out.deviceFound = true;
        out.sent = commandSupported;
        if (commandSupported)
            commands.push_back(command);
        out.reply = commandReply;
        return out;
    }

    ewr::PrintDataResult SendPrintData(const std::vector<unsigned char>& data,
                                       const ewr::ExecutorOptions&) override
    {
        ewr::PrintDataResult out;
        out.supported = true;
        out.deviceFound = true;
        out.sent = true;
        jobs.push_back(data);
        return out;
    }
};

inline std::vector<unsigned char> MakeCommandReply(const std::string& text)
{
    const std::string s = "@BDC PS\r\n" + text;
    return WrapD4Data(std::vector<unsigned char>(s.begin(), s.end()));
}

namespace raw_port_test {

#ifdef _WIN32
    using Socket = SOCKET;
    const Socket kNone = INVALID_SOCKET;
    inline void CloseIt(Socket s) { closesocket(s); }
#else
    using Socket = int;
    const Socket kNone = -1;
    inline void CloseIt(Socket s) { close(s); }
#endif

} // namespace raw_port_test
