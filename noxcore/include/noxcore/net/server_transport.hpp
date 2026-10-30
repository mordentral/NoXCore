// The server's end of the 1.2b transport (transport.hpp has the wire format), without sockets:
// one port for the lobby and every client, as GAME.EXE's.
//
//   lobby   00 00 0C DISCOVER        -> 00 00 0D SERVER_INFO (the token echoed)
//           00 00 F0 "NXSD" query    -> the extended reply (lobby/extended.hpp; noxsdl's)
//           00 00 0E TRY_JOIN        -> 14 JOIN_OK, 0F PASSWORD_REQUIRED, 13 SERVER_ERROR <code>
//           00 00 11 SERVER_PASSWORD -> 14, or 13 06 (wrong)
//   connect FF 00 00 (reliable)      -> [80][0] 1F <its seq> 01 <u32 n> <u8 key>, not XORed;
//                                       00 <its seq> 02 when full, 04 (XORed) when that address
//                                       already has a stream; nothing with the match over
//   accepted, till its CLIENT_ACCEPT, a reliable packet in
//           order or a header with a new ack is taken only as "1F <ack> 20 ..", else dropped
//   Everything the server sends a client heads with its own stream index, 0 (| 0x80 reliable).
//   stream  XORed with the key; control below 0x20 (pings 06 / 08 / 09, close 0A, ack 1F),
//           game messages from 0x20 on.
// Anything else from an address that has no stream is dropped.
#pragma once

#include "noxcore/lobby/extended.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/net/transport.hpp"
#include "noxcore/net/udp_socket.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nox::net {

/// SERVER_ERROR codes (the lobby switch's case 0x0E).
namespace JoinError {
inline constexpr std::uint8_t PingTooLow = 0;  ///< (the ping test's)
inline constexpr std::uint8_t PingTooHigh = 1; ///< (or too many pings lost)
inline constexpr std::uint8_t NotAllowed = 4;  ///< a closed game, not on its allowed list
inline constexpr std::uint8_t Banned = 5;
inline constexpr std::uint8_t WrongPassword = 6;
inline constexpr std::uint8_t ClassNotAllowed = 7;
inline constexpr std::uint8_t Full = 0x0B;
inline constexpr std::uint8_t DuplicateSerial = 0x0C;
inline constexpr std::uint8_t WrongVersion = 0x0D;
} // namespace JoinError

class ServerTransport {
public:
    struct Options {
        int maxPlayers = 32;          ///< connections allowed (the exe's player limit - 1)
        std::string password;         ///< empty: none
        std::uint8_t bannedClasses = 0; ///< 1 warrior, 2 wizard, 4 conjurer (TRY_JOIN +0x36's bit)
        std::uint32_t seed = 1;       ///< the XOR keys
    };
    struct Datagram {
        Endpoint to;
        std::vector<std::uint8_t> bytes;
    };
    struct Event {
        enum class Kind : std::uint8_t {
            Connected, ///< a client's stream is up (its CLIENT_ACCEPT comes as a payload)
            Payload,   ///< game messages from a client (framing.hpp splits them)
            Closed,    ///< the client closed its stream (CLIENT_CLOSE)
        };
        Kind kind = Kind::Payload;
        std::uint8_t peer = 0;
        std::vector<std::uint8_t> bytes;
    };

    explicit ServerTransport(Options options);

    /// The host's Access settings the lobby's join checks use (its case 0x0E, the game settings'
    /// access part and its lists): closed, only those on the allowed list (by name), else none on
    /// the banned list (by an entry's serial when it has one, else its name; one with a time only
    /// till then); the classes refused (1 warrior, 2 wizard, 4 conjurer; 0: Options'); a private
    /// game's password asked for; the ping limits, a ping test first.
    struct Access {
        bool closed = false;
        std::vector<std::u16string> allowed;
        struct Ban {
            std::u16string name;
            std::string serial;
            std::uint32_t untilMs = 0; ///< 0: for good
        };
        std::vector<Ban> banned;
        std::uint8_t bannedClasses = 0;
        bool privateGame = false;
        std::u16string password;
        std::optional<std::uint16_t> minPing;
        std::optional<std::uint16_t> maxPing;
    };
    void setAccess(Access access) { access_ = std::move(access); }
    const Access& access() const noexcept { return access_; }
    /// The player limit (Game_SetMaxPlayers), from now on.
    void setMaxPlayers(int n) { options_.maxPlayers = n; }

    /// What a DISCOVER is answered with (its token is set per reply), and the extended reply.
    void setServerInfo(const msg::ServerInfo& info) { info_ = info; }
    void setExtended(const lobby::ExtendedServer& server) { extended_ = server; }
    /// The object types this host's content packs add (content::NewThingTypes: their hash, 0 for
    /// none, and the packs' ids): the extended reply says so, and a join is let in only from a game
    /// that asked the extended query with the same hash (a stock game, an older NoX Core or one
    /// with other packs is refused with WrongVersion: their games would read the objects wrongly).
    void setNewThingTypes(std::uint64_t hash, std::string packs)
    {
        typesHash_ = hash;
        typesPacks_ = std::move(packs);
    }
    /// The lowest client level let in (lobby::kClientLevel; 0: any, as before): the extended reply
    /// says so, and a join from a NoX Core game whose query named a lower level is refused with
    /// WrongVersion. A game that never asked (a stock one) is let in as before.
    void setMinClientLevel(std::uint16_t level) noexcept { minClientLevel_ = level; }
    /// noxcpp (a mode's "clientKeys"): this host's client key realm (0: none, as before). The
    /// extended reply names it (version 5), and a query naming it with a client's secret gives that
    /// client its key (lobby::clientKeyOf: clientKey); the secret itself isn't kept.
    void setClientKeyRealm(std::uint64_t realm) noexcept { clientKeyRealm_ = realm; }
    /// Where the lobby's refusals say why (the host's log); none: not said.
    void setLog(std::function<void(const std::string&)> log) { log_ = std::move(log); }

    /// A datagram that arrived on the server's port.
    void receive(const Endpoint& from, std::span<const std::uint8_t> datagram, std::uint32_t nowMs);
    /// Resend timers and flushing.
    void poll(std::uint32_t nowMs);
    /// The game frame now (gameFrame__), for the connects' times: the host sets it each frame.
    void setFrame(std::uint32_t frame) noexcept { frame_ = frame; }
    /// The game tick's sweep of the streams still awaiting their CLIENT_ACCEPT: one connected over
    /// kAcceptFrames frames ago is closed (Net_CloseStream, the game told nothing). A client that
    /// went before it accepted (or a stray connect) holds no player slot for good.
    void closeUnaccepted();
    static constexpr std::uint32_t kAcceptFrames = 300;

    bool sendReliable(std::uint8_t peer, std::span<const std::uint8_t> messages);
    /// Net_Send's flag 3 (reliable, flushed): a reliable packet of its own, out at once (after any
    /// still unsent: Stream::sendReliableAlone).
    bool sendReliableNow(std::uint8_t peer, std::span<const std::uint8_t> messages, std::uint32_t nowMs);
    bool sendUnreliable(std::uint8_t peer, std::span<const std::uint8_t> messages);
    /// Net_FlushStream(peer, 1)'s unreliable part: what the peer's unreliable buffer holds goes out
    /// now as its own datagram (not with the frame's update).
    void flushUnreliable(std::uint8_t peer);
    /// Closes a client's stream: what's due flushed, then SERVER_CLOSE (Net_CloseStream).
    void close(std::uint8_t peer);
    /// Closes every client's stream as close() does (Net_CloseStream of the host's stream: the
    /// server's end session).
    void closeAll();
    /// The match over on the server (game flags 1 and 8): a connect, new or again, gets no answer
    /// (the lobby switch's case 0 returns first), so a client waits on its resends till the next map.
    void setMatchOver(bool over) noexcept { matchOver_ = over; }
    /// A WOL game's player between its `rate` frames (SrvNet_UpdateAllClients skips its
    /// Net_FlushStream(.., 0)): its stream not flushed by poll, but for the resend timer's reliable
    /// packets (the timer's resend, not the unreliable buffer), till it's let go.
    void holdFlush(std::uint8_t peer, bool hold);

    std::vector<Datagram> takeOutgoing() { return std::exchange(outgoing_, {}); }
    /// The same into `into` (what it held dropped; its room given to the datagrams still to come).
    void takeOutgoing(std::vector<Datagram>& into)
    {
        into.clear();
        into.swap(outgoing_);
    }
    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }
    /// The same into `into` (what it held dropped; its room given to the events still to come).
    void takeEvents(std::vector<Event>& into)
    {
        into.clear();
        into.swap(events_);
    }

    std::size_t peers() const noexcept { return peers_.size(); }
    /// noxcpp's: a peer that sent the extended query (lobby/extended.hpp) from its address before
    /// connecting: noxsdl, which takes noxcpp's own messages (MSG_NOXCPP_SUBSTEP). A stock client
    /// never asks, so it's never sent one.
    bool identified(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        return it != peers_.end() && it->second.identified;
    }
    /// What an identified peer's query said it takes (lobby::ClientFeature); 0: nothing more, or
    /// not identified.
    std::uint32_t clientFeatures(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        return it != peers_.end() && it->second.identified ? it->second.clientFeatures : 0u;
    }
    /// The client level an identified peer's query named (lobby::kClientLevel); 0: level 0, or
    /// not identified. A peer let in past setMinClientLevel may still say 0 here (its query lost,
    /// or its identification swept during a long password prompt: it's sent a stock client's
    /// stream), so a feature keyed by level asks this, never assumes the lowest level.
    std::uint16_t clientLevel(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        return it != peers_.end() && it->second.identified ? it->second.clientLevel : std::uint16_t{0};
    }
    /// (setClientKeyRealm) The key a peer's client proved for this host's realm (its query's
    /// secret, before its connect); empty: none (a stock client, an older one, no realm).
    const std::string& clientKey(std::uint8_t peer) const
    {
        static const std::string kNone;
        const auto it = peers_.find(peer);
        return it != peers_.end() && it->second.identified ? it->second.clientKey : kNone;
    }
    /// A peer's unacknowledged reliable packets, all of them (0: no peer).
    std::size_t queuedReliable(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? 0 : it->second.stream.queuedReliable();
    }
    /// A peer's unacknowledged reliable packets whose first message is `op` or `op2` (0: no peer).
    std::size_t queuedReliableOf(std::uint8_t peer, std::uint8_t op, std::uint8_t op2) const
    {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? 0 : it->second.stream.queuedReliableOf(op, op2);
    }
    /// A peer's stream: the sequence number our next reliable packet takes, and the peer's last
    /// acknowledgement (the next it expects; 0xFF before its first); nullopt: no peer.
    struct ReliableSeqs {
        std::uint8_t next = 0;
        std::uint8_t ack = 0xFF;
    };
    std::optional<ReliableSeqs> reliableSeqs(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return std::nullopt;
        }
        return ReliableSeqs{it->second.stream.nextSeq(), it->second.stream.lastAck()};
    }
    /// A peer's reliable packets out and not yet acknowledged: the sequence numbers from its last
    /// acknowledgement to our next, or the queue's packets if more (some acknowledged across the
    /// wrap and kept, OB-001); 0: no peer.
    std::size_t reliableOutstanding(std::uint8_t peer) const
    {
        const auto it = peers_.find(peer);
        if (it == peers_.end()) {
            return 0;
        }
        const Stream& s = it->second.stream;
        return std::max<std::size_t>(s.queuedReliable(), static_cast<std::uint8_t>(s.nextSeq() - s.lastAck()));
    }
    std::optional<Endpoint> endpointOf(std::uint8_t peer) const;
    /// A stream's latency (ms): the average of the client's last five round trips (cases 6 / 9:
    /// its CLIENT_PING's clock and its 0x09's, 1-999 ms taken), set at each fifth; 0 before the
    /// first five. Kept by stream id, never reset (OB-225): a joiner taking a stream left by
    /// another starts with that one's.
    int latency(std::uint8_t peer) const { return peer < latency_.size() ? latency_[peer].average : 0; }

private:
    /// A stream's latency record (the connection handler's, by stream id).
    struct LatencyRecord {
        int next = 0;                 ///< the sample written next (0-4)
        std::uint32_t pingedAt = 0;   ///< the last CLIENT_PING's time (the client's clock)
        std::array<int, 5> samples{};
        int average = 0;
    };
    std::array<LatencyRecord, 128> latency_{}; ///< (streams 0-0x7F)
    struct Peer {
        Endpoint at;
        bool identified = false; ///< (noxcpp's: it asked the extended query from here first)
        std::uint32_t clientFeatures = 0; ///< (what that query said it takes: lobby::ClientFeature)
        std::uint16_t clientLevel = 0;    ///< (and its client level: lobby::kClientLevel)
        std::string clientKey;            ///< (and its client key: setClientKeyRealm)
        Stream stream{0x800};
        std::uint8_t key = 0;
        bool timerStarted = false;
        std::uint32_t lastTimer = 0;
        /// Accepted, its CLIENT_ACCEPT not come yet.
        bool awaitingAccept = true;
        std::uint32_t connectedAt = 0; ///< the game frame it connected (closeUnaccepted)
        bool held = false; ///< (holdFlush)
    };
    /// For a stream awaiting its CLIENT_ACCEPT (its reliable packet in order, or a
    /// header with a new ack): the streams past the player limit, this one closed; else a packet
    /// "1F <ack> 20 .." is its CLIENT_ACCEPT (state 2, the game given it from the 0x20); anything
    /// else dropped. False: the stream's gone.
    bool takeAccept(std::uint8_t id, Peer& p, std::span<const std::uint8_t> d);
    bool matchOver_ = false;

    void lobby(const Endpoint& from, std::span<const std::uint8_t> d, std::uint32_t nowMs);
    /// TRY_JOIN's checks (lobby's): version, room, the allowed or banned lists, the class, then
    /// the password or admit.
    void tryJoin(const Endpoint& from, std::uint8_t seq, std::span<const std::uint8_t> d, std::uint32_t nowMs);
    /// Whether a join or connect from `from` may go on for the added object types
    /// (setNewThingTypes): always without them; else only after an extended query naming the same
    /// ones from that address (and port, `exact`). A refusal is logged with why.
    bool sameTypes(const Endpoint& from, bool exact, std::uint32_t nowMs);
    /// Whether a join or connect from `from` may go on for the lowest client level
    /// (setMinClientLevel): always without one, and for a game that never asked; else only when a
    /// query from that address (and port, `exact`) named that level or above. A refusal is logged
    /// with why.
    bool levelLetIn(const Endpoint& from, bool exact, std::uint32_t nowMs);
    /// A refusal's line in the host's log: an address's at most once a minute (a refused game
    /// asks again and again).
    void logRefusal(const Endpoint& from, std::uint32_t nowMs, const std::string& line);
    /// SERVER_ERROR with `code` (a JoinError), in a lobby datagram of `seq`.
    void refuse(const Endpoint& to, std::uint8_t seq, std::uint8_t code);
    /// A join past its checks: the ping test, or JOIN_OK.
    void admit(const Endpoint& from, std::uint8_t seq, std::uint32_t nowMs);
    /// The password asked for: a private game's (Access), else the Options'.
    std::u16string password() const
    {
        if (access_.privateGame) {
            return access_.password;
        }
        return {options_.password.begin(), options_.password.end()};
    }
    struct PingTest {
        Endpoint at;
        std::uint8_t seq = 0;
        std::uint8_t index = 0; ///< the ping out (0-9; 10 done)
        int lost = 0;
        std::array<int, 10> rtt{};
        std::uint32_t sentAt = 0;
    };
    void sendPing(PingTest& t, std::uint32_t nowMs);
    void pollPingTests(std::uint32_t nowMs);
    std::vector<PingTest> pingTests_;
    Access access_;
    void connect(const Endpoint& from, std::span<const std::uint8_t> d, std::uint32_t nowMs);
    void control(std::uint8_t id, Peer& p, std::span<const std::uint8_t> d);
    void sendTo(const Endpoint& to, std::vector<std::uint8_t> bytes) { outgoing_.push_back({to, std::move(bytes)}); }
    void sendPeer(Peer& p, std::vector<std::uint8_t> bytes);
    Peer* peerAt(const Endpoint& at, std::uint8_t* id = nullptr);

    Options options_;
    msg::ServerInfo info_{};
    std::optional<lobby::ExtendedServer> extended_;
    std::map<std::uint8_t, Peer> peers_; ///< by stream id (1..)
    std::map<Endpoint, std::uint32_t> identifying_; ///< extended queries' addresses, when (ms)
    std::map<Endpoint, std::uint32_t> identifyingFeatures_; ///< those queries' client features (lobby::ClientFeature), when any
    std::map<Endpoint, std::uint16_t> identifyingLevels_;   ///< and their client levels, when above 0
    std::map<Endpoint, std::string> identifyingKeys_;       ///< and their client keys, when they proved one
    /// Queries' added object types' hashes and client levels, and when (ms), for the join's gates
    /// (kept only by a host with added types or a lowest level): kept apart from identifying_ for
    /// kAskedKeepMs, over a password prompt too, till the connect from there takes it.
    struct Asked {
        std::uint64_t hash = 0;
        std::uint16_t level = 0;
        std::uint32_t at = 0;
    };
    std::map<Endpoint, Asked> identifyingAsked_;
    static constexpr std::uint32_t kAskedKeepMs = 600'000;
    static constexpr std::uint32_t kAskedJoinMs = 60'000; ///< (the level gate's at a join, by address alone)
    std::map<std::uint32_t, std::uint32_t> refusalLogged_; ///< (an address's last refusal line, ms: one a minute)
    std::uint64_t typesHash_ = 0; ///< setNewThingTypes'
    std::string typesPacks_;
    std::uint16_t minClientLevel_ = 0; ///< setMinClientLevel's
    std::uint64_t clientKeyRealm_ = 0; ///< setClientKeyRealm's
    std::function<void(const std::string&)> log_;
    std::vector<Datagram> outgoing_;
    std::vector<Event> events_;
    std::vector<std::uint8_t> received_; ///< (receive's: a datagram as unXORed, kept to save its allocations)
    std::vector<std::vector<std::uint8_t>> flushed_; ///< (poll's: a stream's flush, kept likewise)
    std::mt19937 rng_;
    std::uint32_t lastPoll_ = 0; ///< the last receive's or poll's time (ms): close's resend times
    std::uint32_t frame_ = 0;    ///< setFrame's
};

} // namespace nox::net
