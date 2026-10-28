// Winsock / BSD socket glue shared by the socket classes. Internal to noxcore.
#pragma once

#include "noxcore/net/udp_socket.hpp"

#include <cstdint>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nox::net::detail {

#ifdef _WIN32
using SocketLength = int;
using RawSocket = SOCKET;
inline constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(INVALID_SOCKET);

inline int lastError()
{
    return WSAGetLastError();
}

inline void ensureStarted()
{
    static const bool started = [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::system_error(lastError(), std::system_category(), "WSAStartup");
        }
        return true;
    }();
    (void)started;
}

inline void closeSocket(std::uintptr_t h)
{
    ::closesocket(static_cast<SOCKET>(h));
}
#else
using SocketLength = socklen_t;
using RawSocket = int;
inline constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(-1);

inline int lastError()
{
    return errno;
}

inline void ensureStarted()
{
}

inline void closeSocket(std::uintptr_t h)
{
    ::close(static_cast<int>(h));
}
#endif

inline RawSocket raw(std::uintptr_t h)
{
    return static_cast<RawSocket>(h);
}

inline void setNonBlocking(std::uintptr_t h)
{
#ifdef _WIN32
    u_long nonBlocking = 1;
    if (ioctlsocket(raw(h), FIONBIO, &nonBlocking) != 0) {
        throw std::system_error(lastError(), std::system_category(), "ioctlsocket");
    }
#else
    if (fcntl(raw(h), F_SETFL, fcntl(raw(h), F_GETFL) | O_NONBLOCK) != 0) {
        throw std::system_error(lastError(), std::system_category(), "fcntl");
    }
#endif
}

inline sockaddr_in toSockaddr(const Endpoint& e)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = htonl(e.address);
    return a;
}

} // namespace nox::net::detail
