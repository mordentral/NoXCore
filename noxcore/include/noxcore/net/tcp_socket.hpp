// A minimal non-blocking IPv4 TCP client socket (Winsock or BSD sockets), polled like UdpSocket:
// for text protocols such as XWIS's IRC-style lobby.
#pragma once

#include "noxcore/net/udp_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace nox::net {

class TcpSocket {
public:
    enum class State : std::uint8_t { Connecting, Connected, Failed, Closed };

    /// Starts connecting without waiting. Throws std::system_error if it can't even start.
    static TcpSocket connect(const Endpoint& to);

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    ~TcpSocket();

    /// Where the connection stands; checks a pending connect.
    State state();
    /// Sends what the socket takes now; returns how many bytes that was (0 if it would block).
    /// Marks the socket Failed on an error.
    std::size_t send(std::span<const std::uint8_t> data);
    /// Bytes received into `buffer`, or nullopt if none are waiting. After the peer closes (or
    /// an error) the state is Closed (Failed) and this returns nullopt.
    std::optional<std::size_t> receive(std::span<std::uint8_t> buffer);

private:
    explicit TcpSocket(std::uintptr_t handle)
        : handle_(handle)
    {
    }
    void close() noexcept;

    std::uintptr_t handle_;
    State state_ = State::Connecting;
};

} // namespace nox::net
