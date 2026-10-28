// A player's XWIS login, checked: connects to xwis.net, logs in with the nick and the encoded
// password, and says whether the server took it; on the same connection it lists the Nox games
// (as the game's one WOL session does), since XWIS refuses another login from this address for
// a few seconds. Polled, without blocking, from the menus.
#pragma once

#include "noxcore/lobby/xwis.hpp"
#include "noxcore/net/tcp_socket.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nox::lobby {

class XwisLogin {
public:
    enum class State : std::uint8_t { Pending, Succeeded, Failed };

    /// Starts it; `apgarPassword` is already encoded (apgar()).
    XwisLogin(std::string nick, std::string apgarPassword, std::uint32_t nowMs);

    /// Moves it on: sends, receives, times out after 20 seconds.
    void poll(std::uint32_t nowMs);

    State state() const { return state_; }
    /// Why it failed: the server's reply line, or what went wrong with the connection.
    const std::string& error() const { return error_; }
    /// Whether it failed because the server answered with an error (a wrong password, a nick in
    /// use), not because it couldn't be reached.
    bool refused() const { return refused_; }
    /// The Nox games XWIS listed, once it succeeded.
    const std::vector<GameListing>& games() const { return games_; }

private:
    void fail(std::string why, bool refused);

    std::optional<net::TcpSocket> socket_;
    std::optional<XwisSession> session_;
    std::string send_;
    std::uint32_t startedAt_ = 0;
    /// Logged in and QUIT sent: waiting (up to 3 seconds) for the server to close, since it
    /// refuses another login from this address while the connection lasts (the game list's).
    std::uint32_t quitAt_ = 0;
    State state_ = State::Pending;
    std::string error_;
    bool refused_ = false;
    std::vector<GameListing> games_;
};

} // namespace nox::lobby
