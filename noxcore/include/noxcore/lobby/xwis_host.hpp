// Posting a hosted game to XWIS as GAME.EXE does through WOLAPI.dll, so other players' game lists
// show it (xwis.hpp's listing reads it back):
//   - logged in (as XwisSession: CVERS, PASS, NICK, apgar, USER) with the host's WOL login,
//   - the game's channel made and joined (the host's Channel to RequestChannelCreate,
//     as WOLAPI words it): "JOINGAME #<nick>'s_game 1 32 37 3 1 1 <reserved> <key>": at least 1
//     and at most 32 users, game type 37 (Nox: the SKU's high byte), WOLAPI's 3
//     and 1, tournament 1, the reserved word (the game's place on noxworld's map, x | y << 16:
//     live hosts post (192, 212) or (192, 636)) and no key (password),
//   - its topic (WOLAPI's topic request, "TOPIC #<channel> :<prefix><topic>"): the prefix 'G' (in a
//     game) or 'g', '0' + the least users, '0' + the most ('P' for 32), '0' + the extra info's
//     length and the extra info itself, the version 0x1039A as three bytes (9A 03 01); then
//     the 69-byte server record (listing.hpp's makeWolRecord) packed seven bits a byte,
//     as a C string. Sent again whenever the record changes (players, map),
//   - staying in the channel while the game runs: XWIS lists the channel, with the address the
//     host connected from; leaving it (PART, QUIT) takes the game off the list.
//
// XwisHostSession is the conversation without sockets; XwisHost drives one over a TcpSocket.
#pragma once

#include "noxcore/lobby/xwis.hpp"
#include "noxcore/net/tcp_socket.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {

/// noxworld's map place live hosts use (192, 212).
inline constexpr std::uint32_t kXwisDefaultPlace = 192u | 212u << 16;

/// The game channel's topic for a server record: "G1P3", the version's three bytes, the record
/// packed (up to its first 0).
std::string xwisGameTopic(std::span<const std::uint8_t> record);

class XwisHostSession {
public:
    enum class State : std::uint8_t { LoggingIn, Joining, Hosting, Failed };

    XwisHostSession(std::string nick, std::string apgarPassword, std::uint32_t place = kXwisDefaultPlace);

    /// The record to post (sent as the topic once in the channel, and again when it changes).
    void setRecord(std::span<const std::uint8_t> record);
    /// Leaves: PART and QUIT.
    void quit();

    std::vector<std::string> takeOutgoing();
    void receive(std::span<const std::uint8_t> bytes);
    /// For tests: one line from the server (without its line end).
    void line(std::string_view text);

    State state() const noexcept { return state_; }
    const std::string& error() const noexcept { return error_; }
    const std::string& channel() const noexcept { return channel_; }

private:
    void sendTopic();

    std::string nick_;
    std::string channel_;
    std::uint32_t place_;
    State state_ = State::LoggingIn;
    std::string error_;
    std::string partial_;
    std::vector<std::string> outgoing_;
    std::string topic_;     ///< what's to be posted
    std::string topicSent_; ///< what was
};

/// An XwisHostSession over a socket, polled; it reconnects 30 s after the connection fails.
class XwisHost {
public:
    XwisHost(std::string nick, std::string apgarPassword, std::uint32_t nowMs, std::uint32_t place = kXwisDefaultPlace);
    ~XwisHost();
    XwisHost(const XwisHost&) = delete;
    XwisHost& operator=(const XwisHost&) = delete;

    void setRecord(std::span<const std::uint8_t> record);
    void poll(std::uint32_t nowMs);
    /// Leaves XWIS (the game off the list); polling after does nothing.
    void stop();

    bool hosting() const noexcept { return session_ && session_->state() == XwisHostSession::State::Hosting; }
    const std::string& error() const noexcept { return error_; }

private:
    void connect(std::uint32_t nowMs);
    void flush();

    std::string nick_;
    std::string password_;
    std::uint32_t place_;
    std::optional<net::TcpSocket> socket_;
    std::optional<XwisHostSession> session_;
    std::vector<std::uint8_t> record_;
    std::string send_;
    std::string error_;
    std::uint32_t retryAt_ = 0;
    bool stopped_ = false;
};

} // namespace nox::lobby
