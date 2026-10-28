// Non-blocking UDP socket and address resolution over Winsock or BSD sockets.
#include "noxcore/net/udp_socket.hpp"

#include "socket_platform.hpp"

#include <system_error>
#include <utility>

namespace nox::net {

using namespace detail;

namespace {

#ifdef _WIN32
bool wouldBlock(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET; // ICMP port unreachable surfaces as a reset
}
#else
bool wouldBlock(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED;
}
#endif

} // namespace

std::optional<Endpoint> Endpoint::resolve(std::string_view host, std::uint16_t port)
{
    ensureStarted();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(std::string(host).c_str(), nullptr, &hints, &result) != 0 || !result) {
        return std::nullopt;
    }
    Endpoint e;
    e.address = ntohl(reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    e.port = port;
    freeaddrinfo(result);
    return e;
}

std::string Endpoint::toString() const
{
    return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 0xFF) + "." +
           std::to_string((address >> 8) & 0xFF) + "." + std::to_string(address & 0xFF) + ":" + std::to_string(port);
}

UdpSocket UdpSocket::open(std::uint16_t port, bool loopbackOnly)
{
    ensureStarted();
    const auto s = static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s == kInvalid) {
        throw std::system_error(lastError(), std::system_category(), "socket");
    }
    UdpSocket sock(s);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
#ifdef _WIN32
    if (port != 0) {
        // (Linux already refuses a port another socket has on any of the same addresses.)
        const int on = 1;
        if (::setsockopt(raw(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
            throw std::system_error(lastError(), std::system_category(), "setsockopt(SO_EXCLUSIVEADDRUSE)");
        }
    }
#endif
    if (::bind(raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        throw std::system_error(lastError(), std::system_category(), "bind");
    }
    setNonBlocking(s);
    return sock;
}

UdpSocket UdpSocket::openHosting(std::uint16_t port, bool loopbackOnly)
{
    for (std::uint32_t p = port;; ++p) {
        try {
            return open(static_cast<std::uint16_t>(p), loopbackOnly);
        } catch (const std::system_error& e) {
            if (port == 0 || p >= 0xFFFF || !addressInUse(e.code())) {
                throw;
            }
        }
    }
}

bool UdpSocket::addressInUse(const std::error_code& error) noexcept
{
    if (error.category() != std::system_category()) {
        return false;
    }
#ifdef _WIN32
    // (WSAEACCES: held by a socket bound with SO_EXCLUSIVEADDRUSE)
    return error.value() == WSAEADDRINUSE || error.value() == WSAEACCES;
#else
    return error.value() == EADDRINUSE;
#endif
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalid))
{
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalid);
    }
    return *this;
}

UdpSocket::~UdpSocket()
{
    close();
}

void UdpSocket::close() noexcept
{
    if (handle_ != kInvalid) {
        closeSocket(handle_);
        handle_ = kInvalid;
    }
}

std::uint16_t UdpSocket::localPort() const
{
    sockaddr_in a{};
    SocketLength len = sizeof(a);
    if (::getsockname(raw(handle_), reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        return 0;
    }
    return ntohs(a.sin_port);
}

void UdpSocket::enableBroadcast()
{
    const int on = 1;
    if (::setsockopt(raw(handle_), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
        throw std::system_error(lastError(), std::system_category(), "setsockopt(SO_BROADCAST)");
    }
}

void UdpSocket::sendTo(const Endpoint& to, std::span<const std::uint8_t> data)
{
    const sockaddr_in a = toSockaddr(to);
    const auto sent = ::sendto(raw(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
                               0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    if (sent < 0 && !wouldBlock(lastError())) {
        throw std::system_error(lastError(), std::system_category(), "sendto");
    }
}

std::optional<UdpSocket::Received> UdpSocket::receive(std::span<std::uint8_t> buffer)
{
    for (;;) {
        sockaddr_in a{};
        SocketLength len = sizeof(a);
        const auto got = ::recvfrom(raw(handle_), reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&a), &len);
        if (got >= 0) {
            return Received{{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)}, static_cast<std::size_t>(got)};
        }
        const int err = lastError();
#ifdef _WIN32
        if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
            continue; // a previous send bounced, or an oversized datagram: skip it
        }
#endif
        if (wouldBlock(err)) {
            return std::nullopt;
        }
        throw std::system_error(err, std::system_category(), "recvfrom");
    }
}

} // namespace nox::net
