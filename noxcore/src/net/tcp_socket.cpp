// Non-blocking TCP socket over Winsock or BSD sockets (the lobby and chat connections).
#include "noxcore/net/tcp_socket.hpp"

#include "socket_platform.hpp"

#include <system_error>
#include <utility>

namespace nox::net {

using namespace detail;

namespace {

#ifdef _WIN32
bool inProgress(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
bool wouldBlock(int err)
{
    return err == WSAEWOULDBLOCK;
}
#else
bool inProgress(int err)
{
    return err == EINPROGRESS || err == EWOULDBLOCK;
}
bool wouldBlock(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK;
}
#endif

} // namespace

TcpSocket TcpSocket::connect(const Endpoint& to)
{
    ensureStarted();
    const auto s = static_cast<std::uintptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalid) {
        throw std::system_error(lastError(), std::system_category(), "socket");
    }
    TcpSocket sock(s);
    setNonBlocking(s);
    const sockaddr_in a = toSockaddr(to);
    if (::connect(raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0) {
        sock.state_ = State::Connected;
    } else if (!inProgress(lastError())) {
        sock.state_ = State::Failed;
    }
    return sock;
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalid)), state_(other.state_)
{
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalid);
        state_ = other.state_;
    }
    return *this;
}

TcpSocket::~TcpSocket()
{
    close();
}

void TcpSocket::close() noexcept
{
    if (handle_ != kInvalid) {
        closeSocket(handle_);
        handle_ = kInvalid;
    }
}

TcpSocket::State TcpSocket::state()
{
    if (state_ != State::Connecting) {
        return state_;
    }
    // A pending connect is done once the socket is writable (or, on Windows, in the except set).
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(raw(handle_), &writable);
    FD_SET(raw(handle_), &failed);
    timeval now{0, 0};
    if (::select(static_cast<int>(raw(handle_)) + 1, nullptr, &writable, &failed, &now) > 0) {
        int error = 0;
        SocketLength len = sizeof(error);
        ::getsockopt(raw(handle_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
        state_ = (error == 0 && !FD_ISSET(raw(handle_), &failed)) ? State::Connected : State::Failed;
    }
    return state_;
}

std::size_t TcpSocket::send(std::span<const std::uint8_t> data)
{
    if (state() != State::Connected || data.empty()) {
        return 0;
    }
    const auto sent = ::send(raw(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
    if (sent < 0) {
        if (!wouldBlock(lastError())) {
            state_ = State::Failed;
        }
        return 0;
    }
    return static_cast<std::size_t>(sent);
}

std::optional<std::size_t> TcpSocket::receive(std::span<std::uint8_t> buffer)
{
    if (state() != State::Connected) {
        return std::nullopt;
    }
    const auto got = ::recv(raw(handle_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if (got > 0) {
        return static_cast<std::size_t>(got);
    }
    if (got == 0) {
        state_ = State::Closed;
    } else if (!wouldBlock(lastError())) {
        state_ = State::Failed;
    }
    return std::nullopt;
}

} // namespace nox::net
