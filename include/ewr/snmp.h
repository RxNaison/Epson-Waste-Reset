#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The little of SNMP v1 a network run needs: one GET request, one response.
//
// A printer on the network has no IEEE 1284.4 channel to open, but its SNMP
// agent carries the same EPSON-CTRL commands: the command bytes ride as the
// trailing arcs of an OID under Epson's enterprise tree, and the reply comes
// back as the OCTET STRING that channel would have returned. A GET is the
// whole transport - no session, no credit, nothing to close.

namespace ewr {
namespace snmp {

    using Oid = std::vector<uint32_t>;

    constexpr uint16_t kPort = 161;

    constexpr unsigned char kTagOctetString = 0x04;

    // "1.3.6.1.4.1.1248..." form, for the trace.
    std::string FormatOid(const Oid& oid);

    // A complete SNMP v1 GetRequest datagram for one OID. Empty when the OID
    // cannot be encoded: fewer than two arcs, or a first pair outside what
    // BER packs into one byte.
    std::vector<unsigned char> EncodeGetRequest(const std::string& community,
                                                int32_t requestId,
                                                const Oid& oid);

    struct GetResponse
    {
        int32_t requestId = 0;
        // 0 = noError. v1 answers an OID it does not serve with 2 (noSuchName).
        int errorStatus = 0;
        // BER tag and content of the first variable binding's value.
        unsigned char valueTag = 0;
        std::vector<unsigned char> value;
    };

    // False for anything that is not a well-formed GetResponse. Lengths are
    // checked against the datagram before anything is read through them.
    bool DecodeGetResponse(const std::vector<unsigned char>& datagram, GetResponse& out);

    // One peer, datagrams in and out. Behind an interface so the gateway is
    // testable against a scripted printer.
    class IDatagramChannel
    {
    public:
        virtual ~IDatagramChannel() = default;
        virtual bool Send(const std::vector<unsigned char>& datagram) = 0;
        // One datagram, or empty when none arrived within timeoutMs.
        virtual std::vector<unsigned char> Receive(int timeoutMs) = 0;
    };

    // UDP to host:port, `host` a name or an address. nullptr with `error` set
    // when the name does not resolve or no socket can be opened; a printer
    // that is merely absent still yields a channel, and silence on it.
    std::unique_ptr<IDatagramChannel> OpenUdpChannel(const std::string& host,
                                                     uint16_t port,
                                                     std::string& error);

} // namespace snmp
} // namespace ewr
