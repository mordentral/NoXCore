// A minimal non-blocking IPv4 UDP socket (Winsock or BSD sockets).
#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nox::net {

struct Endpoint {
    std::uint32_t address = 0; ///< IPv4, host byte order
    std::uint16_t port = 0;

    /// Resolves "host" or "a.b.c.d"; nullopt if it can't.
    static std::optional<Endpoint> resolve(std::string_view host, std::uint16_t port);
    std::string toString() const;

    bool operator==(const Endpoint&) const = default;
    auto operator<=>(const Endpoint&) const = default;
};

class UdpSocket {
public:
    /// Binds to `port` on all interfaces (0 = any free port), or on the loopback alone (a local
    /// test's host: nothing outside this computer reaches it). Throws std::system_error.
    static UdpSocket open(std::uint16_t port = 0, bool loopbackOnly = false);

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket();

    std::uint16_t localPort() const;
    /// Allows sending to broadcast addresses (LAN discovery). Throws std::system_error.
    void enableBroadcast();
    /// Throws std::system_error if the send fails outright.
    void sendTo(const Endpoint& to, std::span<const std::uint8_t> data);

    struct Received {
        Endpoint from;
        std::size_t size = 0;
    };
    /// One datagram into `buffer`, or nullopt if none is waiting.
    std::optional<Received> receive(std::span<std::uint8_t> buffer);

private:
    explicit UdpSocket(std::uintptr_t handle)
        : handle_(handle)
    {
    }
    void close() noexcept;

    std::uintptr_t handle_;
};

} // namespace nox::net
