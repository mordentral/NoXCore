// Finding Nox games to join, from every place they're advertised, merged into one list:
//   XWIS      the games 1.2b hosts post to xwis.net (xwis.hpp)
//   LAN       a MSG_SERVER_DISCOVER broadcast to port 18590 on the local network
//   list      servers named directly ("host" or "host:port"), e.g. from a servers file
// Every server found is pinged with MSG_SERVER_DISCOVER (header 00 00, a token the server
// echoes); the MSG_SERVER_INFO reply gives the round trip and fresh details (full name, current
// players). Entries are merged by address and port. OpenNox's discovery does the same (its HTTP
// lobby could be another source).
//
// The game list's Network (LAN play) mode asks no XWIS at all (setXwisListing(false)): as the
// game's refreshServerList, which in network mode only broadcasts the LAN query.
//
// Polled, like the rest of the networking: call poll() often.
#pragma once

#include "noxcore/lobby/extended.hpp"
#include "noxcore/lobby/listing.hpp"
#include "noxcore/lobby/xwis.hpp"
#include "noxcore/net/tcp_socket.hpp"
#include "noxcore/net/udp_socket.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nox::lobby {

enum SourceBits : unsigned {
    kFromXwis = 1,
    kFromLan = 2,
    kFromList = 4,
};

struct ServerEntry {
    GameListing game;
    unsigned sources = 0;             ///< SourceBits
    std::optional<std::uint32_t> pingMs; ///< round trip of the discover ping, if it answered
    bool answered = false;            ///< the server replied to a ping (details are fresh)
    /// A noxcpp server: it answered the extended query sent beside the ping (extended.hpp).
    std::optional<ExtendedServer> extended;
};

class ServerBrowser {
public:
    struct Options {
        bool xwis = true;
        bool lan = true;
        std::vector<std::string> servers; ///< pinged directly
        std::string xwisNick;             ///< empty: xwisNick() with a random suffix
        /// The nick's password, apgar-encoded (a player's own login); empty: the nick stands in
        /// (a throwaway listing login).
        std::string xwisPassword;
        /// Where XWIS is (default xwis.net:4000; tests: a loopback fake).
        std::optional<net::Endpoint> xwisServer;
        /// Where the LAN query goes (default the broadcast to the game port, 18590; tests: a
        /// loopback fake). Every reply to it counts as found on the LAN.
        std::optional<net::Endpoint> lanQueryTo;
    };

    explicit ServerBrowser(Options options);
    /// Whether searches list XWIS (on by default; Options::xwis off overrides it). Off for the
    /// game list's Network (LAN play): refresh() neither logs in to XWIS nor takes the games it
    /// is given, so only the LAN's replies and the listed servers show. Turned off, a login
    /// still under way is dropped.
    void setXwisListing(bool on);
    bool listsXwis() const { return options_.xwis && xwisListing_; }
    bool listsLan() const { return options_.lan; }
    bool hasServerList() const { return !options_.servers.empty(); }
    /// Lists XWIS through this login from the next refresh on (a player who logged in: XWIS
    /// won't take a throwaway nick from the same address right after a registered one).
    void setXwisLogin(std::string nick, std::string apgarPassword)
    {
        options_.xwisNick = std::move(nick);
        options_.xwisPassword = std::move(apgarPassword);
    }

    /// Starts a new search (forgets the last one's results). With `xwisGames`, XWIS's games as
    /// a login just listed them, instead of logging in again (XWIS refuses a second login from
    /// the same address within a few seconds); ignored while XWIS isn't listed.
    void refresh(std::uint32_t nowMs, const std::vector<GameListing>* xwisGames = nullptr);
    /// Pumps the sockets. Never throws; network trouble ends up in the log.
    void poll(std::uint32_t nowMs);

    /// Still waiting on XWIS or on pings.
    bool searching() const;
    /// The servers found so far.
    const std::vector<ServerEntry>& servers() const { return entries_; }
    /// Progress and problems since the last call, for showing or logging.
    std::vector<std::string> takeLog() { return std::exchange(log_, {}); }

private:
    struct Ping {
        net::Endpoint to;
        std::uint32_t sentAt = 0;
        int tries = 0;
        bool broadcast = false;
    };

    void ping(const net::Endpoint& to, std::uint32_t nowMs, bool broadcast = false);
    void pollXwis(std::uint32_t nowMs);
    void pollUdp(std::uint32_t nowMs);
    ServerEntry& upsert(std::uint32_t address, std::uint16_t port);
    void closeXwis(const std::string& why);

    Options options_;
    bool xwisListing_ = true;
    std::minstd_rand rng_;
    std::optional<net::UdpSocket> udp_;
    std::optional<net::TcpSocket> xwisSocket_;
    std::optional<XwisSession> xwis_;
    std::uint32_t xwisStartedAt_ = 0;
    std::string xwisSend_;
    std::map<std::uint32_t, Ping> pings_; ///< by token
    std::set<std::uint32_t> queried_;     ///< tokens of this search's extended queries
    std::vector<std::pair<net::Endpoint, bool>> queued_; ///< pings to send (broadcast?) on the next poll
    std::vector<ServerEntry> entries_;
    std::vector<std::string> log_;
};

} // namespace nox::lobby
