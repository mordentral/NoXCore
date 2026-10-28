// XWIS: the community revival of Westwood Online (xwis.net:4000), where 1.2b hosts still post
// their games. The protocol is IRC-like text over TCP.
//
// Listing games needs no account: log in with a throwaway nick whose password is the nick
// itself (XWIS makes the account on the spot), then `LIST -1 37` (37 = Nox) answers one
// `326` line per game and `323` at the end. A game's topic is "G1P3", the game version (three
// bytes, 0x1039A) and the host's server-info record packed seven bits per byte with the top bit
// set, so it stays printable (GAME.EXE's packer is the record is parseWolRecord's).
// OpenNox's lobby lists XWIS the same way.
//
// XwisSession is the conversation without sockets: queue its lines to the server, feed it what
// comes back (ServerBrowser drives it over a TcpSocket).
#pragma once

#include "noxcore/lobby/listing.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::lobby {

inline constexpr std::string_view kXwisHost = "xwis.net";
inline constexpr std::uint16_t kXwisPort = 4000;

/// `data` as 7-bit groups, least significant bit first, each byte's top bit set.
std::vector<std::uint8_t> packSevenBit(std::span<const std::uint8_t> data);
/// The inverse: the first `size` bytes from packed text.
std::vector<std::uint8_t> unpackSevenBit(std::span<const std::uint8_t> packed, std::size_t size);

/// A game's topic ("G1P3" + version + packed record, maybe after a leading ':') as a listing
/// (the address isn't in it).
std::optional<GameListing> parseXwisTopic(std::string_view topic);

/// A `326` reply line from `LIST -1 37`:
///   ":server 326 nick #channel users 0 37 1 <flags> <host IPv4 as a big-endian number> 128 :<topic>"
std::optional<GameListing> parseXwisGameLine(std::string_view line);

/// A throwaway XWIS nick for listing: "NoXList" and two hex digits (logins are at most 9
/// characters).
std::string xwisNick(std::uint32_t random);

/// WOLAPI's password encoding (in WOLAPI.dll), what the login's `apgar` line and the
/// saved logins carry: for each of the first 8 characters c (pairing c[i] with c[n - i], n the
/// length up to 8, c[n] the terminator), c even gives c ^ pair, odd (c << 1) & pair; the 8
/// results (zeros past n) each pick from "a-zA-Z0-9./" by their low 6 bits.
std::string apgar(std::string_view password);

class XwisSession {
public:
    enum class State : std::uint8_t { LoggingIn, Listing, Done, Failed };
    /// List: log in, list the Nox games, quit. Login: log in and quit (a player's login checked).
    enum class Purpose : std::uint8_t { List, Login };

    /// Queues the login (the WOL client handshake OpenNox uses: CVERS, PASS, NICK, apgar with
    /// the password, USER). Without a password the nick stands in for it, as a throwaway listing
    /// login (XWIS makes the account on the spot); `apgarPassword` is already encoded.
    explicit XwisSession(std::string nick, std::string apgarPassword = {}, Purpose purpose = Purpose::List);

    /// Lines to send, without line ends.
    std::vector<std::string> takeOutgoing();
    /// Bytes from the server; complete lines are handled as they arrive.
    void receive(std::span<const std::uint8_t> bytes);

    State state() const { return state_; }
    const std::string& error() const { return error_; }
    const std::vector<GameListing>& games() const { return games_; }
    const std::string& nick() const { return nick_; }

private:
    void line(std::string_view text);

    std::string nick_;
    Purpose purpose_ = Purpose::List;
    State state_ = State::LoggingIn;
    std::string error_;
    std::string partial_;
    std::vector<std::string> outgoing_;
    std::vector<GameListing> games_;
};

} // namespace nox::lobby
