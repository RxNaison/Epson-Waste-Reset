#include "ewr/snmp.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ewr {
namespace snmp {

    namespace {

        constexpr unsigned char kTagInteger = 0x02;
        constexpr unsigned char kTagNull = 0x05;
        constexpr unsigned char kTagOid = 0x06;
        constexpr unsigned char kTagSequence = 0x30;
        constexpr unsigned char kTagGetRequest = 0xA0;
        constexpr unsigned char kTagGetResponse = 0xA2;

        void AppendLength(std::vector<unsigned char>& out, std::size_t length)
        {
            if (length < 0x80)
            {
                out.push_back(static_cast<unsigned char>(length));
            }
            else if (length <= 0xFF)
            {
                out.push_back(0x81);
                out.push_back(static_cast<unsigned char>(length));
            }
            else
            {
                out.push_back(0x82);
                out.push_back(static_cast<unsigned char>((length >> 8) & 0xFF));
                out.push_back(static_cast<unsigned char>(length & 0xFF));
            }
        }

        void AppendTlv(std::vector<unsigned char>& out, unsigned char tag,
                       const std::vector<unsigned char>& content)
        {
            out.push_back(tag);
            AppendLength(out, content.size());
            out.insert(out.end(), content.begin(), content.end());
        }

        // Shortest two's-complement form, as BER requires.
        std::vector<unsigned char> IntegerContent(int32_t value)
        {
            std::vector<unsigned char> bytes;
            for (int shift = 24; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<unsigned char>((static_cast<uint32_t>(value) >> shift) & 0xFF));

            while (bytes.size() > 1)
            {
                const bool redundantZero = bytes[0] == 0x00 && (bytes[1] & 0x80) == 0;
                const bool redundantOnes = bytes[0] == 0xFF && (bytes[1] & 0x80) != 0;
                if (!redundantZero && !redundantOnes)
                    break;
                bytes.erase(bytes.begin());
            }

            return bytes;
        }

        // Base 128, most significant group first, high bit on all but the last.
        void AppendArc(std::vector<unsigned char>& out, uint32_t arc)
        {
            unsigned char groups[5];
            int count = 0;
            do
            {
                groups[count++] = static_cast<unsigned char>(arc & 0x7F);
                arc >>= 7;
            } while (arc != 0);

            while (count > 0)
            {
                --count;
                out.push_back(static_cast<unsigned char>(groups[count] | (count > 0 ? 0x80 : 0x00)));
            }
        }

        // A cursor over one BER level. Every read is bounded by `end`, which a
        // nested element narrows to its own content.
        struct Reader
        {
            const unsigned char* data = nullptr;
            std::size_t pos = 0;
            std::size_t end = 0;

            bool Next(unsigned char& tag, std::size_t& start, std::size_t& length)
            {
                if (pos + 2 > end)
                    return false;

                tag = data[pos++];
                const unsigned char first = data[pos++];

                if ((first & 0x80) == 0)
                {
                    length = first;
                }
                else
                {
                    const std::size_t count = first & 0x7F;
                    if (count == 0 || count > 2 || pos + count > end)
                        return false;

                    length = 0;
                    for (std::size_t i = 0; i < count; ++i)
                        length = (length << 8) | data[pos++];
                }

                if (length > end - pos)
                    return false;

                start = pos;
                pos += length;
                return true;
            }

            bool NextInteger(int32_t& value)
            {
                unsigned char tag = 0;
                std::size_t start = 0;
                std::size_t length = 0;
                if (!Next(tag, start, length) || tag != kTagInteger || length == 0 || length > 4)
                    return false;

                uint32_t raw = (data[start] & 0x80) ? 0xFFFFFFFFu : 0u;
                for (std::size_t i = 0; i < length; ++i)
                    raw = (raw << 8) | data[start + i];

                value = static_cast<int32_t>(raw);
                return true;
            }

            bool Enter(unsigned char wantedTag, Reader& inner)
            {
                unsigned char tag = 0;
                std::size_t start = 0;
                std::size_t length = 0;
                if (!Next(tag, start, length) || tag != wantedTag)
                    return false;

                inner.data = data;
                inner.pos = start;
                inner.end = start + length;
                return true;
            }
        };

    } // namespace

    std::string FormatOid(const Oid& oid)
    {
        std::string text;
        for (std::size_t i = 0; i < oid.size(); ++i)
        {
            if (i > 0)
                text += '.';
            text += std::to_string(oid[i]);
        }
        return text;
    }

    std::vector<unsigned char> EncodeGetRequest(const std::string& community,
                                                int32_t requestId,
                                                const Oid& oid)
    {
        if (oid.size() < 2 || oid[0] > 2 || oid[1] > 39)
            return {};

        std::vector<unsigned char> oidContent;
        oidContent.push_back(static_cast<unsigned char>(oid[0] * 40 + oid[1]));
        for (std::size_t i = 2; i < oid.size(); ++i)
            AppendArc(oidContent, oid[i]);

        std::vector<unsigned char> binding;
        AppendTlv(binding, kTagOid, oidContent);
        AppendTlv(binding, kTagNull, {});

        std::vector<unsigned char> bindingList;
        AppendTlv(bindingList, kTagSequence, binding);

        std::vector<unsigned char> pdu;
        AppendTlv(pdu, kTagInteger, IntegerContent(requestId));
        AppendTlv(pdu, kTagInteger, IntegerContent(0)); // error-status
        AppendTlv(pdu, kTagInteger, IntegerContent(0)); // error-index
        AppendTlv(pdu, kTagSequence, bindingList);

        std::vector<unsigned char> message;
        AppendTlv(message, kTagInteger, IntegerContent(0)); // version: SNMPv1
        AppendTlv(message, kTagOctetString,
                  std::vector<unsigned char>(community.begin(), community.end()));
        AppendTlv(message, kTagGetRequest, pdu);

        std::vector<unsigned char> datagram;
        AppendTlv(datagram, kTagSequence, message);
        return datagram;
    }

    bool DecodeGetResponse(const std::vector<unsigned char>& datagram, GetResponse& out)
    {
        out = GetResponse{};

        Reader top{ datagram.data(), 0, datagram.size() };
        Reader message;
        if (!top.Enter(kTagSequence, message))
            return false;

        int32_t version = 0;
        if (!message.NextInteger(version))
            return false;

        unsigned char tag = 0;
        std::size_t start = 0;
        std::size_t length = 0;
        if (!message.Next(tag, start, length) || tag != kTagOctetString) // community
            return false;

        Reader pdu;
        if (!message.Enter(kTagGetResponse, pdu))
            return false;

        int32_t errorStatus = 0;
        int32_t errorIndex = 0;
        if (!pdu.NextInteger(out.requestId) || !pdu.NextInteger(errorStatus) || !pdu.NextInteger(errorIndex))
            return false;
        out.errorStatus = errorStatus;

        Reader bindingList;
        Reader binding;
        if (!pdu.Enter(kTagSequence, bindingList) || !bindingList.Enter(kTagSequence, binding))
            return false;

        if (!binding.Next(tag, start, length) || tag != kTagOid)
            return false;

        if (!binding.Next(tag, start, length))
            return false;

        out.valueTag = tag;
        out.value.assign(datagram.begin() + static_cast<std::ptrdiff_t>(start),
                         datagram.begin() + static_cast<std::ptrdiff_t>(start + length));
        return true;
    }

    // ------------------------------------------------------------------
    //  UDP
    // ------------------------------------------------------------------

    namespace {

#ifdef _WIN32
        using SocketHandle = SOCKET;
        const SocketHandle kNoSocket = INVALID_SOCKET;
        void CloseSocket(SocketHandle s) { closesocket(s); }
#else
        using SocketHandle = int;
        const SocketHandle kNoSocket = -1;
        void CloseSocket(SocketHandle s) { close(s); }
#endif

        // A send this machine refuses can fail with EPIPE even on UDP - macOS
        // does it to a terminal without the Local Network permission - and
        // the default for that is a signal that ends the process before the
        // run can say why. Off per call on Linux, per socket on macOS.
#if defined(MSG_NOSIGNAL)
        constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        constexpr int kSendFlags = 0;
#endif

        // The largest datagram UDP can carry; a status report is a few hundred
        // bytes, but nothing here should truncate one that is not.
        constexpr std::size_t kMaxDatagram = 65535;

        // Winsock counts its users: each channel starts it once and stops it
        // once, in its destructor or on its way out of a failed open.
        bool StartSockets(std::string& error)
        {
#ifdef _WIN32
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            {
                error = "Winsock could not be started.";
                return false;
            }
#else
            (void)error;
#endif
            return true;
        }

        void StopSockets()
        {
#ifdef _WIN32
            WSACleanup();
#endif
        }

        bool WaitReadable(SocketHandle socket, int timeoutMs)
        {
            if (timeoutMs < 0)
                timeoutMs = 0;

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket, &readable);

            timeval wait;
            wait.tv_sec = timeoutMs / 1000;
            wait.tv_usec = (timeoutMs % 1000) * 1000;

            // The first argument is ignored by Winsock and must be the
            // highest descriptor plus one everywhere else.
            return select(static_cast<int>(socket) + 1, &readable, nullptr, nullptr, &wait) > 0;
        }

        class UdpChannel final : public IDatagramChannel
        {
        public:
            explicit UdpChannel(SocketHandle socket) : m_socket(socket) {}

            ~UdpChannel() override
            {
                CloseSocket(m_socket);
                StopSockets();
            }

            UdpChannel(const UdpChannel&) = delete;
            UdpChannel& operator=(const UdpChannel&) = delete;

            bool Send(const std::vector<unsigned char>& datagram) override
            {
                const auto sent = send(m_socket, reinterpret_cast<const char*>(datagram.data()),
                                       static_cast<int>(datagram.size()), kSendFlags);
                return sent == static_cast<decltype(sent)>(datagram.size());
            }

            std::vector<unsigned char> Receive(int timeoutMs) override
            {
                if (!WaitReadable(m_socket, timeoutMs))
                    return {};

                std::vector<unsigned char> buffer(kMaxDatagram);
                const auto received = recv(m_socket, reinterpret_cast<char*>(buffer.data()),
                                           static_cast<int>(buffer.size()), 0);

                // A connected UDP socket reports an ICMP "port unreachable"
                // here as an error. To the caller that is the same thing as
                // silence: nothing is listening.
                if (received <= 0)
                    return {};

                buffer.resize(static_cast<std::size_t>(received));
                return buffer;
            }

        private:
            SocketHandle m_socket;
        };

        class BroadcastChannel final : public IBroadcastChannel
        {
        public:
            explicit BroadcastChannel(SocketHandle socket) : m_socket(socket) {}

            ~BroadcastChannel() override
            {
                CloseSocket(m_socket);
                StopSockets();
            }

            BroadcastChannel(const BroadcastChannel&) = delete;
            BroadcastChannel& operator=(const BroadcastChannel&) = delete;

            bool SendTo(const std::string& address, uint16_t port,
                        const std::vector<unsigned char>& datagram) override
            {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(port);
                if (inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1)
                    return false;

                const auto sent = sendto(m_socket, reinterpret_cast<const char*>(datagram.data()),
                                         static_cast<int>(datagram.size()), kSendFlags,
                                         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
                return sent == static_cast<decltype(sent)>(datagram.size());
            }

            bool ReceiveFrom(int timeoutMs, std::string& from, std::vector<unsigned char>& datagram) override
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                for (;;)
                {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()).count();
                    if (left <= 0 || !WaitReadable(m_socket, static_cast<int>(left)))
                        return false;

                    std::vector<unsigned char> buffer(kMaxDatagram);
                    sockaddr_in peer{};
                    socklen_t peerSize = sizeof(peer);
                    const auto received = recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()),
                                                   static_cast<int>(buffer.size()), 0,
                                                   reinterpret_cast<sockaddr*>(&peer), &peerSize);

                    // Windows hands an earlier send's ICMP "unreachable" to
                    // the next receive as an error; the search goes on past it.
                    if (received <= 0)
                        continue;

                    buffer.resize(static_cast<std::size_t>(received));
                    datagram = std::move(buffer);
                    from = FormatIpv4(ntohl(peer.sin_addr.s_addr));
                    return true;
                }
            }

        private:
            SocketHandle m_socket;
        };

    } // namespace

    std::unique_ptr<IDatagramChannel> OpenUdpChannel(const std::string& host,
                                                     uint16_t port,
                                                     std::string& error)
    {
        if (!StartSockets(error))
            return nullptr;

        auto fail = [&error](const std::string& message) -> std::unique_ptr<IDatagramChannel>
        {
            error = message;
            StopSockets();
            return nullptr;
        };

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* found = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &found) != 0 || !found)
            return fail("\"" + host + "\" is not an address or a host name that resolves.");

        // Connected, so the socket only ever hands back datagrams from the
        // printer it was pointed at.
        SocketHandle socketHandle = kNoSocket;
        for (addrinfo* entry = found; entry; entry = entry->ai_next)
        {
            socketHandle = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socketHandle == kNoSocket)
                continue;

#if defined(SO_NOSIGPIPE)
            int noSigpipe = 1;
            setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif

            if (connect(socketHandle, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0)
                break;

            CloseSocket(socketHandle);
            socketHandle = kNoSocket;
        }

        freeaddrinfo(found);

        if (socketHandle == kNoSocket)
            return fail("No UDP socket could be opened towards " + host + " (no route to it?).");

        return std::make_unique<UdpChannel>(socketHandle);
    }

    std::unique_ptr<IBroadcastChannel> OpenBroadcastChannel(std::string& error)
    {
        if (!StartSockets(error))
            return nullptr;

        const SocketHandle socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == kNoSocket)
        {
            error = "No UDP socket could be opened for the network search.";
            StopSockets();
            return nullptr;
        }

        const int on = 1;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);

        if (setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) != 0
            || bind(socketHandle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
        {
            error = "This computer would not let EWR broadcast on the network.";
            CloseSocket(socketHandle);
            StopSockets();
            return nullptr;
        }

#if defined(SO_NOSIGPIPE)
        setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

        return std::make_unique<BroadcastChannel>(socketHandle);
    }

    uint32_t DirectedBroadcast(uint32_t address, int prefixLength)
    {
        if (prefixLength < 1 || prefixLength > 30)
            return 0;

        return address | (0xFFFFFFFFu >> prefixLength);
    }

    std::string FormatIpv4(uint32_t address)
    {
        return std::to_string((address >> 24) & 0xFF) + "." + std::to_string((address >> 16) & 0xFF) + "."
             + std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF);
    }

    std::vector<std::string> LocalBroadcastAddresses()
    {
        std::vector<std::string> addresses = { "255.255.255.255" };
        auto add = [&addresses](uint32_t broadcast)
        {
            if (broadcast == 0)
                return;

            const std::string text = FormatIpv4(broadcast);
            if (std::find(addresses.begin(), addresses.end(), text) == addresses.end())
                addresses.push_back(text);
        };

#ifdef _WIN32
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
        {
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_INET,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }

        if (result == NO_ERROR)
        {
            for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
                 adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;

                for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                {
                    if (unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;

                    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                    add(DirectedBroadcast(ntohl(ipv4->sin_addr.s_addr), unicast->OnLinkPrefixLength));
                }
            }
        }
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0)
        {
            for (ifaddrs* entry = list; entry; entry = entry->ifa_next)
            {
                if (!entry->ifa_addr || !entry->ifa_netmask || entry->ifa_addr->sa_family != AF_INET)
                    continue;
                if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
                    continue;

                const uint32_t address = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr);
                const uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask)->sin_addr.s_addr);

                int prefix = 0;
                for (uint32_t bits = mask; bits & 0x80000000u; bits <<= 1)
                    ++prefix;

                add(DirectedBroadcast(address, prefix));
            }

            freeifaddrs(list);
        }
#endif

        return addresses;
    }

} // namespace snmp
} // namespace ewr
