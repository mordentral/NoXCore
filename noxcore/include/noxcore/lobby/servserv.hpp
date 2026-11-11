// How the game finds its XWIS chat server: before any chat login, WOLAPI.dll asks Westwood
// Online's server server ("servserv", servserv.westwood.com:4005, which XWIS answers) where to
// go, and the game connects to the chat server it names. XwisConnection does the same for every
// XWIS connection NoX Core opens (the game list's listing login, the WOL chat login, a player's
// login check, a hosted game's post). By decision (2026-11-11) it goes, in turn:
//   1. servserv at xwis.net:4005 (where the community's patched WOLAPI.DLL, Nox Reloaded's, asks)
//      and the chat server it names;
//   2. the chat server xwis.net:4000 directly;
//   3. servserv at servserv.westwood.com:4005 (WOLAPI 1.20's) and the chat server it names
//      (skipped when it has the address step 1 asked: the same answer, or none).
// A step fails when its name can't be looked up, its connection is refused or isn't open in 30 s,
// or (a servserv) it doesn't answer in 30 s, closes before 607 or names no chat server for the
// language; the next step then starts at once.
//
// The query, as WOLAPI sends it on connecting (one line each, CR LF):
//   verchk 32512 65556                    WOLAPI's own product code (0x7F00) and file version (1.20)
//   verchk 9472 65540                     the game's SKU (0x2500: Nox, English) and version (0x10004)
//   lobcount 9472
//   whereto <nick> <password> 9472 65540 <serial>
//   QUIT
// The nick and the apgar-encoded password are the login's; the serial is the CD key from the
// registry, empty when there is none (as here: NoX Core never sends one).
//
// The replies, as XWIS sent them (2026-11-10):
//   : 610 u 1                                               (lobcount's answer)
//   : 605 u :xwis.net 4000 '0:XWIS' -8 36.1083 -115.0582    a chat server
//   : 607                                                   the end of the list
// A 605 line is "<host> <port> '<name>' <time zone> <latitude> <longitude>" after the target's
// ':'; the name is the language codes it serves (comma-separated) and its description, after a
// ':'. Other servers come as 608 (game results), 609 (ladder), 611 (time), 612 (mangler), 614 and
// 615, and 606 offers patches; none of them matter here. Nothing is used before 607.
//
// The game lists the chat servers in the order they came, keeps those whose name starts with a
// digit and whose codes include its language (the SKU's low four bits: 0 for English), and
// offers them by description with the first one picked; it then connects to that server's host
// and port with a 30-second timeout and doesn't try another. XwisConnection takes that first one.
// The game gives WOLAPI 30 seconds for both connections (its connection deadline: the time a
// connection may take to open). XwisConnection waits the same 30 s (kServservTimeoutMs) for a
// server server's connection and its answer, and for a chat connection to open while another step
// is left. Each login on the chat server has kXwisChatTimeoutMs from the chat connection's start
// to its welcome. Every step's outcome is logged ("XWIS: xwis.net:4005 gave chat server ...",
// "XWIS: servserv failed (...), using xwis.net:4000", "XWIS: failed (...), asking servserv ...").
#pragma once

#include "noxcore/net/tcp_socket.hpp"
#include "noxcore/net/udp_socket.hpp" // (net::Endpoint)

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::lobby {

inline constexpr std::string_view kServservHost = "servserv.westwood.com";
inline constexpr std::uint16_t kServservPort = 4005;
/// The first server server asked (on kServservPort), as the community's patched WOLAPI.DLL has it;
/// kServservHost (WOLAPI 1.20's) is asked last, after xwis.net:4000 itself.
inline constexpr std::string_view kServservCommunityHost = "xwis.net";
/// How long each server server may take to answer before the next is asked (the game's 30 s).
inline constexpr std::uint32_t kServservTimeoutMs = 30000;
/// How long a login on the chat server may take, from the chat connection's start to the end of
/// its welcome (the game's 30 s): the WOL chat, a player's login check, the game list's listing
/// login and a hosted game's post.
inline constexpr std::uint32_t kXwisChatTimeoutMs = 30000;

/// The game's WOL SKU: Nox (37) in the high byte, the language (0, English) in the low bits.
inline constexpr std::uint32_t kWolSku = 0x2500;
/// The game version the game reports to WOLAPI.
inline constexpr std::uint32_t kWolGameVersion = 0x10004;
/// WOLAPI.dll's own product code and its file version (1.20, as GOG's copy is), for servserv's first verchk.
inline constexpr std::uint32_t kWolapiSku = 0x7F00;
inline constexpr std::uint32_t kWolapiVersion = 0x10014;

/// The query's lines (without line ends): two verchks, lobcount, whereto, QUIT.
std::vector<std::string> servservRequest(std::string_view nick, std::string_view password);

/// A chat server servserv named.
struct ChatServer {
    std::string host;
    std::uint16_t port = 0;
    std::string name; ///< "0:XWIS": the language codes it serves, then its description
};

/// A 605 line's chat server; none for any other line or one without a host and a usable port.
std::optional<ChatServer> parseServservChatServer(std::string_view line);

/// Whether the game offers `server` for `language` (the SKU's low four bits): its name starts
/// with a digit and the comma-separated numbers before its ':' include the language.
bool servesLanguage(const ChatServer& server, unsigned language);

/// The servserv conversation without sockets: feed it the replies.
class ServservSession {
public:
    enum class State : std::uint8_t { Waiting, Done };

    void receive(std::span<const std::uint8_t> bytes);
    /// For tests: one line from the server (without its line end).
    void line(std::string_view text);

    /// Done once the list's end (607) came.
    State state() const noexcept { return state_; }
    /// The chat servers (605) in the order they came.
    const std::vector<ChatServer>& chatServers() const noexcept { return chatServers_; }
    /// The one the game picks first: the first chat server for `language`; none before 607.
    const ChatServer* pick(unsigned language = kWolSku & 0xF) const;

private:
    State state_ = State::Waiting;
    std::string partial_;
    std::vector<ChatServer> chatServers_;
};

/// Where an XWIS connection goes: the real places by default, loopback fakes in tests.
struct XwisRoute {
    /// The server servers to ask: the first before the fallback, the rest after it; none:
    /// kServservCommunityHost then kServservHost (on kServservPort), each looked up at its turn.
    std::vector<net::Endpoint> servservs;
    /// The chat server tried directly when the first servserv gives none; none: xwis.net:4000.
    std::optional<net::Endpoint> fallback;
    std::uint32_t servservTimeoutMs = kServservTimeoutMs;
    /// Straight to the fallback, servserv not asked.
    bool direct = false;
};

/// A TCP connection to the XWIS chat server a server server names (or the fallback), polled and
/// never waiting: Connecting while the server servers are asked and the chat server's connection
/// opens.
class XwisConnection {
public:
    /// Starts asking servserv for the chat server, with the login's nick and its apgar-encoded
    /// password (empty: the nick stands in, as the throwaway listing login has it).
    XwisConnection(std::string_view nick, std::string_view password, std::uint32_t nowMs, XwisRoute route = {});

    /// Moves the servserv query on; once it's answered (or given up), starts the chat server's
    /// connection. True on the call that started it (a login's own timeout counts from then).
    bool poll(std::uint32_t nowMs);
    /// Still asking servserv (the chat connection not started).
    bool asking() const noexcept { return !socket_ && !failed_; }
    /// Connecting until the chat server's connection is open; Failed when it can't be.
    net::TcpSocket::State state();
    /// As TcpSocket's; nothing before the chat connection is open.
    std::size_t send(std::span<const std::uint8_t> data);
    std::optional<std::size_t> receive(std::span<std::uint8_t> buffer);

    /// The chat server ("xwis.net:4000"), once chosen.
    const std::string& chatServer() const noexcept { return chatServer_; }
    /// Why it failed: the chat server couldn't be looked up or connected to.
    std::string error();
    /// The route taken, as "XWIS: ..." lines, since the last call.
    std::vector<std::string> takeLog() { return std::exchange(log_, {}); }

private:
    /// A server server to ask: by name (looked up at its turn) or a given address.
    struct Servserv {
        std::string name; ///< for the log: "servserv.westwood.com:4005", or the test's address
        std::optional<net::Endpoint> at;
    };

    /// Asks the next server server that can be looked up and reached (not one at an address
    /// already tried): the first before the fallback, the rest after it. When none is left, the
    /// fallback if it's untried, else the connection fails.
    void askNext(std::uint32_t nowMs);
    /// The way being tried gave no chat connection (`why`): the next way.
    void moveOn(const std::string& why, std::uint32_t nowMs);
    /// Another way is left after the one being tried.
    bool moreToTry() const;
    /// Why each way tried failed, "; " between.
    std::string failures() const;
    void useServserv(const ChatServer& server, std::uint32_t nowMs);
    void useFallback(const std::string& why, std::uint32_t nowMs);
    void connectChat(const net::Endpoint& to, const std::string& name, std::uint32_t nowMs);

    XwisRoute route_;
    std::string nick_;
    std::string password_;
    std::vector<Servserv> servservs_;
    std::size_t next_ = 0;                ///< the next of servservs_ to ask
    std::vector<net::Endpoint> tried_;    ///< the addresses asked so far
    std::vector<std::string> failures_;   ///< why each gave none, for the fallback's log line
    bool fallbackTried_ = false;          ///< xwis.net:4000 (or the test's) tried
    std::uint32_t startedAt_ = 0;         ///< when the one being asked was
    std::uint32_t chatStartedAt_ = 0;     ///< when the chat connection started opening
    std::string servservName_;            ///< the one being asked
    std::optional<net::TcpSocket> servserv_;
    ServservSession session_;
    std::string request_;
    std::optional<net::TcpSocket> socket_;
    std::string chatServer_;
    std::string error_;
    bool failed_ = false;
    std::vector<std::string> log_;
};

} // namespace nox::lobby
