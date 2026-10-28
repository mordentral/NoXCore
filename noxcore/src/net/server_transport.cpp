// The server side of the netstr transport: the lobby (discover, join checks, password and
// ping tests), the connect handshake, and one Stream per connected client. Like transport.cpp
// it owns no sockets; see server_transport.hpp.
#include "noxcore/net/server_transport.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/net/framing.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cstring>
#include <exception>

namespace nox::net {

namespace {

/// The server's own (listening) stream index: the header byte of everything it sends a client.
constexpr std::uint8_t kServerStreamIndex = 0;

using util::putU32;
using util::u32At;

// A lobby datagram: header 00 <seq>, then `body`.
std::vector<std::uint8_t> lobbyReply(std::uint8_t seq, std::initializer_list<std::uint8_t> body)
{
    std::vector<std::uint8_t> d = {0, seq};
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// A UTF-16LE string from a fixed field, up to its first NUL.
using util::utf16Until;

// Equal without the case of A-Z (the join's names and serials).
template <class A, class B>
bool sameFolded(const A& a, const B& b)
{
    const auto fold = [](auto c) { return (c >= 'A' && c <= 'Z') ? static_cast<decltype(c)>(c + 0x20) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](auto x, auto y) { return fold(x) == fold(y); });
}

} // namespace

void ServerTransport::refuse(const Endpoint& to, std::uint8_t seq, std::uint8_t code)
{
    sendTo(to, lobbyReply(seq, {static_cast<std::uint8_t>(Op::ServerError), code}));
}

ServerTransport::ServerTransport(Options options)
    : options_(std::move(options))
    , rng_(options_.seed)
{
}

std::optional<Endpoint> ServerTransport::endpointOf(std::uint8_t peer) const
{
    const auto it = peers_.find(peer);
    return it != peers_.end() ? std::optional(it->second.at) : std::nullopt;
}

// The connected peer whose address is `at` (its stream id in *id), or null.
ServerTransport::Peer* ServerTransport::peerAt(const Endpoint& at, std::uint8_t* id)
{
    for (auto& [n, p] : peers_) {
        if (p.at == at) {
            if (id) {
                *id = n;
            }
            return &p;
        }
    }
    return nullptr;
}

void ServerTransport::receive(const Endpoint& from, std::span<const std::uint8_t> datagram, std::uint32_t nowMs)
{
    lastPoll_ = nowMs;
    if (datagram.size() < 3) {
        return;
    }
    std::uint8_t id = 0;
    if (Peer* p = peerAt(from, &id)) {
        // A client with a stream: its datagrams are XORed with its key.
        std::vector<std::uint8_t>& d = received_;
        d.assign(datagram.begin(), datagram.end());
        xorBytes(d, p->key);
        if (d[0] == (kUnassignedId | kReliableBit) && d[2] == static_cast<std::uint8_t>(Op::ServerConnect)) {
            // A connect again from an address with a stream (a stock client resends its connect
            // once the accept's 1F names its own seq): the receive takes header FF to the connection
            // handler, whose case 0 answers 00 <its seq> 02 when full, else 04 (that address has a
            // stream), sent XORed with the address's key; nothing with the match over.
            if (matchOver_) {
                return;
            }
            const auto code = static_cast<int>(peers_.size()) >= options_.maxPlayers ? Op::Code2 : Op::Code4;
            sendPeer(*p, {0, d[1], static_cast<std::uint8_t>(code)});
            return;
        }
        if ((d[0] & 0x7F) != id) {
            return; // (the slot must match too)
        }
        std::optional<std::uint8_t> ack;
        if (!p->stream.acceptDatagram(d, ack)) {
            return;
        }
        if (p->awaitingAccept && ((d[0] & kReliableBit) != 0 || ack)) {
            // A reliable packet in order (its seq counted first) or a header with a
            // new ack (applied first, the game told of it after) goes to the accept check while the
            // stream awaits its CLIENT_ACCEPT; what it takes isn't read further.
            if (takeAccept(id, *p, d) && ack) {
                events_.push_back({Event::Kind::Payload, id, {static_cast<std::uint8_t>(Op::Code38), *ack}});
            }
            return;
        }
        if (ack) {
            events_.push_back({Event::Kind::Payload, id, {static_cast<std::uint8_t>(Op::Code38), *ack}});
        }
        // Ops below 0x20 are the transport's own control messages; 0x20 and up are the game's.
        if (d[2] < 0x20) {
            control(id, *p, d);
        } else {
            events_.push_back({Event::Kind::Payload, id, {d.begin() + 2, d.end()}});
        }
        return;
    }
    // No stream for this address: only a connect or a lobby message is taken.
    const std::uint8_t op = datagram[2];
    if (datagram[0] == (kUnassignedId | kReliableBit) && op == static_cast<std::uint8_t>(Op::ServerConnect)) {
        if (!matchOver_) { // (the connection handler's case 0: Game_HasFlags(1) and (8), nothing)
            connect(from, datagram, nowMs);
        }
        return;
    }
    // The receive takes DISCOVER (0x0C) and 0x0E-0x14 by the op alone, whatever the header's bytes
    // (the exe's own later pings carry stack bytes there, which its client echoes).
    if (op == static_cast<std::uint8_t>(Op::ServerDiscover) || (op >= 0x0E && op <= 0x14) ||
        (datagram[0] == 0 && op == lobby::kExtendedOp)) {
        lobby(from, datagram, nowMs);
    }
    // Anything else from an address without a stream is dropped.
}

void ServerTransport::admit(const Endpoint& from, std::uint8_t seq, std::uint32_t nowMs)
{
    // The connection handler's end of a join's checks: with a ping limit and a free test
    // slot (32), the ping test (SERVER_PING 0x10 {u8 the ping's
    // number, u32 the time}); else JOIN_OK (0x14).
    if ((access_.minPing || access_.maxPing) && pingTests_.size() < 32) {
        PingTest t;
        t.at = from;
        t.seq = seq;
        pingTests_.push_back(t);
        sendPing(pingTests_.back(), nowMs);
        return;
    }
    sendTo(from, lobbyReply(seq, {static_cast<std::uint8_t>(Op::ServerJoinOk)}));
}

void ServerTransport::sendPing(PingTest& t, std::uint32_t nowMs)
{
    t.sentAt = nowMs;
    std::vector<std::uint8_t> d = lobbyReply(t.seq, {static_cast<std::uint8_t>(Op::ServerPing), t.index});
    putU32(d, nowMs);
    sendTo(t.at, std::move(d));
}

void ServerTransport::pollPingTests(std::uint32_t nowMs)
{
    // Each poll: a ping unanswered for 2 s is lost (-1); more than 3 lost, refused
    // (13 01: the refusal with 1); else the next. Ten done: more than 3 lost refused (13 01); the
    // answered ones' average under the lowest allowed refused (13 00), over the highest (13 01);
    // then JOIN_OK (0x14), sent even after a refusal (an original bug: the client's
    // already been refused). Those two build their own header, 00 00, not the TRY_JOIN's seq.
    constexpr int kLostAllowed = 3;
    for (auto it = pingTests_.begin(); it != pingTests_.end();) {
        PingTest& t = *it;
        if (t.index < 10) {
            if (static_cast<std::int32_t>(nowMs - t.sentAt) > 2000) {
                t.rtt[t.index] = -1;
                if (kLostAllowed < ++t.lost) {
                    refuse(t.at, 0, JoinError::PingTooHigh);
                    it = pingTests_.erase(it);
                    continue;
                }
                ++t.index;
                sendPing(t, nowMs);
            }
            ++it;
            continue;
        }
        if (kLostAllowed < t.lost) {
            refuse(t.at, 0, JoinError::PingTooHigh);
        } else {
            int sum = 0;
            int n = 0;
            // Average only the answered pings; lost ones were marked -1.
            for (const int r : t.rtt) {
                if (0 < r) {
                    sum += r;
                    ++n;
                }
            }
            const int average = n != 0 ? sum / n : 0; // (the exe divides by none: an original bug)
            if (access_.minPing && average < *access_.minPing) {
                refuse(t.at, 0, JoinError::PingTooLow);
            }
            if (access_.maxPing && *access_.maxPing < average) {
                refuse(t.at, 0, JoinError::PingTooHigh);
            }
        }
        sendTo(t.at, lobbyReply(0, {static_cast<std::uint8_t>(Op::ServerJoinOk)}));
        it = pingTests_.erase(it);
    }
}

void ServerTransport::lobby(const Endpoint& from, std::span<const std::uint8_t> d, std::uint32_t nowMs)
{
    const std::uint8_t seq = d[1];
    const auto op = static_cast<Op>(d[2]);
    if (op == Op::ServerDiscover && d.size() >= 2 + msg::ServerDiscover::kSize) {
        // server_makeServerInfoPacket: the info, the discover's token echoed.
        msg::ServerInfo info = info_;
        // The discover's token is the u32 5 bytes into its body.
        info.token = u32At(d, 3 + 5);
        info.players = static_cast<std::uint8_t>(peers_.size());
        ByteWriter w;
        w.u8(0);
        w.u8(0);
        info.write(w);
        sendTo(from, w.data());
        return;
    }
    if (d[2] == lobby::kExtendedOp) {
        if (const auto token = lobby::decodeExtendedQuery(d); token && extended_) {
            if (typesHash_ != 0 || minClientLevel_ != 0 || lobby::kClientLevel != 0) {
                lobby::ExtendedServer server = *extended_;
                server.typesHash = typesHash_;
                server.typesPacks = typesPacks_;
                server.minClientLevel = minClientLevel_;
                server.clientLevel = lobby::kClientLevel;
                sendTo(from, lobby::encodeExtendedReply(*token, server));
            } else {
                sendTo(from, lobby::encodeExtendedReply(*token, *extended_));
            }
            // (noxcpp's: a client that asks from the address it then connects from is noxsdl,
            // and gets the identified clients' extras: ServerTransport::identified)
            std::erase_if(identifying_, [nowMs](const auto& kv) { return nowMs - kv.second > 60'000; });
            std::erase_if(identifyingFeatures_, [this](const auto& kv) { return !identifying_.contains(kv.first); });
            std::erase_if(identifyingLevels_, [this](const auto& kv) { return !identifying_.contains(kv.first); });
            identifying_[from] = nowMs;
            if (const std::uint32_t features = lobby::extendedQueryClientFeatures(d).value_or(0); features != 0) {
                identifyingFeatures_[from] = features;
            } else {
                identifyingFeatures_.erase(from);
            }
            // (a version 4 query too short for its level: level 0)
            const std::uint16_t level = lobby::extendedQueryClientLevel(d).value_or(0);
            if (level != 0) {
                identifyingLevels_[from] = level;
            } else {
                identifyingLevels_.erase(from);
            }
            if (typesHash_ != 0 || minClientLevel_ != 0) { // (kept only by a host with a gate: nothing else asks)
                std::erase_if(identifyingAsked_, [nowMs](const auto& kv) { return nowMs - kv.second.at > kAskedKeepMs; });
                // A game's query (version 2 on: it names features, types or a level; one cut short
                // names none, level 0) is kept; a browser's bare one (version 1's) says nothing of
                // a game, so it's no record.
                if (lobby::extendedQueryVersion(d).value_or(1) >= 2) {
                    identifyingAsked_[from] = {lobby::extendedQueryTypesHash(d).value_or(0), level, nowMs};
                } else {
                    identifyingAsked_.erase(from);
                }
            }
        }
        return;
    }
    if (op == Op::ServerTryJoin) {
        tryJoin(from, seq, d, nowMs);
        return;
    }
    if (op == Op::ServerPong && d.size() >= 8) {
        // The connection handler's case 0x12: the test's ping answered (by its address, its number): its time
        // kept, the next sent; the tenth, the poll decides.
        for (PingTest& t : pingTests_) {
            if (t.at == from && t.index < 10 && d[3] == t.index) {
                t.rtt[t.index] = static_cast<int>(nowMs - u32At(d, 4));
                ++t.index;
                if (t.index < 10) {
                    sendPing(t, nowMs);
                }
                break;
            }
        }
        return;
    }
    if (op == Op::ServerPassword && d.size() >= 4 + 18) {
        // A wide string, compared with the server's: up to 9 UTF-16LE characters from +4,
        // NUL-terminated.
        if (utf16Until(d.subspan(4, 18)) != password()) {
            refuse(from, seq, JoinError::WrongPassword);
            return;
        }
        admit(from, seq, nowMs);
    }
}

void ServerTransport::tryJoin(const Endpoint& from, std::uint8_t seq, std::span<const std::uint8_t> d, std::uint32_t nowMs)
{
    msg::ServerTryJoin join;
    try {
        ByteReader r(d.subspan(3), "TRY_JOIN");
        join = msg::ServerTryJoin::read(r);
    } catch (const std::exception&) {
        return;
    }
    if (join.version != kGameVersion) {
        refuse(from, seq, JoinError::WrongVersion);
        return;
    }
    // noxcpp's: a host whose content packs add object types lets in only a game with the same ones
    // (types travel by number: any other game reads their objects wrongly). The join comes from
    // the client's lobby socket, its query from its game socket: by the address here, by the
    // address and port at the connect.
    if (!sameTypes(from, false, nowMs)) {
        refuse(from, seq, JoinError::WrongVersion);
        return;
    }
    // noxcpp's: a host with a lowest client level lets a NoX Core game below it no further (one
    // that never asked, a stock game, is let in as before).
    if (!levelLetIn(from, false, nowMs)) {
        refuse(from, seq, JoinError::WrongVersion);
        return;
    }
    if (static_cast<int>(peers_.size()) >= options_.maxPlayers) {
        refuse(from, seq, JoinError::Full);
        return;
    }
    // Closed (the access settings' flag 0x10): only those on the allowed list, by name (list
    // 4); else the banned list: an entry with a serial by it, else by
    // the name (5).
    // The name arrives as UTF-16LE bytes; keep it up to the first NUL.
    const std::u16string name = utf16Until(join.playerName);
    const std::string serial(reinterpret_cast<const char*>(join.serial.data()),
                             strnlen(reinterpret_cast<const char*>(join.serial.data()), join.serial.size()));
    // (The names and the serials both compared without the case of A-Z: sameFolded.)
    if (access_.closed) {
        if (std::ranges::none_of(access_.allowed, [&](const std::u16string& a) { return sameFolded(a, name); })) {
            refuse(from, seq, JoinError::NotAllowed);
            return;
        }
    } else {
        const bool banned = std::ranges::any_of(access_.banned, [&](const Access::Ban& b) {
            const bool still = b.untilMs == 0 || static_cast<std::int32_t>(b.untilMs - nowMs) > 0;
            return still && (b.serial.empty() ? sameFolded(b.name, name) : sameFolded(b.serial, serial));
        });
        if (banned) {
            refuse(from, seq, JoinError::Banned);
            return;
        }
    }
    // One bit per class, indexed by the low 5 bits of the join's class byte.
    const std::uint8_t restricted = access_.bannedClasses ? access_.bannedClasses : options_.bannedClasses;
    if (restricted && (restricted & (1u << (join.playerClass & 0x1F)))) {
        refuse(from, seq, JoinError::ClassNotAllowed);
        return;
    }
    // A private game (the access settings' flag 0x20) asks for its password, an empty one too.
    if (access_.privateGame || !password().empty()) {
        sendTo(from, lobbyReply(seq, {static_cast<std::uint8_t>(Op::PasswordRequired)}));
        return;
    }
    admit(from, seq, nowMs);
}

bool ServerTransport::sameTypes(const Endpoint& from, bool exact, std::uint32_t nowMs)
{
    if (typesHash_ == 0) {
        return true;
    }
    // A query from there (at that port too when `exact`) naming the same types, within kAskedKeepMs.
    const auto there = [&](const Endpoint& at) { return at == from || (!exact && at.address == from.address); };
    bool same = false;
    for (const auto& [at, asked] : identifyingAsked_) {
        same = same || (there(at) && nowMs - asked.at <= kAskedKeepMs && asked.hash == typesHash_);
    }
    // (asked at all: a NoX Core game with other types or none, or an older one; else a stock game)
    const bool asked = std::ranges::any_of(identifyingAsked_, [&](const auto& kv) { return there(kv.first); }) ||
                       std::ranges::any_of(identifying_, [&](const auto& kv) { return there(kv.first); });
    if (!same) {
        logRefusal(from, nowMs,
                   from.toString() + " refused: this game's content packs (" + typesPacks_ + ") add object types, and " +
                       (asked ? "it has other ones added, or none (or is a NoX Core from before added types)" : "it's a stock game (it didn't say which types it has)") +
                       ": only NoX Core games with the same packs can join");
    }
    return same;
}

bool ServerTransport::levelLetIn(const Endpoint& from, bool exact, std::uint32_t nowMs)
{
    if (minClientLevel_ == 0) {
        return true;
    }
    // The games' queries from there, within kAskedKeepMs at that port (`exact`: the connect, over
    // a password prompt too), else within a minute at that address (the join, just after its
    // query: an older one there may be another game's behind the same router): none, a stock
    // game (let in); else one naming the lowest level or above.
    const auto there = [&](const Endpoint& at) { return at == from || (!exact && at.address == from.address); };
    const std::uint32_t keepMs = exact ? kAskedKeepMs : kAskedJoinMs;
    bool asked = false;
    bool letIn = false;
    std::uint16_t highest = 0;
    for (const auto& [at, a] : identifyingAsked_) {
        if (there(at) && nowMs - a.at <= keepMs) {
            asked = true;
            highest = std::max(highest, a.level);
            letIn = letIn || a.level >= minClientLevel_;
        }
    }
    if (asked && !letIn) {
        logRefusal(from, nowMs,
                   from.toString() + " refused: its NoX Core is at level " + std::to_string(highest) + ", and this game lets in level " +
                       std::to_string(minClientLevel_) + " and above (" + std::string(lobby::kClientTooOldNotice) + ")");
    }
    return !asked || letIn;
}

void ServerTransport::logRefusal(const Endpoint& from, std::uint32_t nowMs, const std::string& line)
{
    if (!log_) {
        return;
    }
    const auto last = refusalLogged_.find(from.address);
    if (last == refusalLogged_.end() || nowMs - last->second > 60'000) {
        std::erase_if(refusalLogged_, [nowMs](const auto& kv) { return nowMs - kv.second > 60'000; });
        refusalLogged_[from.address] = nowMs;
        log_(line);
    }
}

void ServerTransport::connect(const Endpoint& from, std::span<const std::uint8_t> d, std::uint32_t nowMs)
{
    const std::uint8_t clientSeq = d[1];
    if (static_cast<int>(peers_.size()) >= options_.maxPlayers) {
        sendTo(from, {0, clientSeq, static_cast<std::uint8_t>(Op::Code2)}); // (the handler's case 0: 00 <seq> 02)
        return;
    }
    if (!sameTypes(from, true, nowMs) || !levelLetIn(from, true, nowMs)) {
        sendTo(from, {0, clientSeq, static_cast<std::uint8_t>(Op::Code2)}); // (noxcpp's: not the same object types, or too low a level; as when full)
        return;
    }
    identifyingAsked_.erase(from); // (taken by this connect)
    // The lowest free stream id (0 is the server's own).
    std::uint8_t id = 1;
    while (peers_.contains(id)) {
        ++id;
    }
    Peer& p = peers_[id];
    p.at = from;
    p.connectedAt = frame_; // (state 1 and its frame: the game tick's sweep, closeUnaccepted)
    // What the server sends heads with its own stream index, not the client's (the handler sets
    // the new stream's header byte to the listening stream's, 0): the client looks
    // that index up among its own streams and drops what doesn't match.
    p.stream.setId(kServerStreamIndex);
    p.identified = identifying_.erase(from) != 0;
    if (const auto f = identifyingFeatures_.find(from); f != identifyingFeatures_.end()) {
        p.clientFeatures = p.identified ? f->second : 0u;
        identifyingFeatures_.erase(f);
    }
    if (const auto l = identifyingLevels_.find(from); l != identifyingLevels_.end()) {
        p.clientLevel = p.identified ? l->second : std::uint16_t{0};
        identifyingLevels_.erase(l);
    }
    std::optional<std::uint8_t> ack;
    p.stream.acceptDatagram(d, ack); // the connect is its reliable seq 0: expect 1 next
    p.key = static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(1, 255)(rng_));
    // [80][seq] 1F <its connect's seq> 01 <u32 id> <u8 key>, before the key applies (the handler's
    // end: the ack byte is the connect's own seq).
    std::vector<std::uint8_t> accept = {static_cast<std::uint8_t>(Op::Accepted), clientSeq,
                                        static_cast<std::uint8_t>(Op::ServerAccept)};
    putU32(accept, id);
    accept.push_back(p.key);
    const std::uint8_t seq = p.stream.queueReliable(accept);
    sendTo(from, p.stream.sendReliableNow(seq, nowMs));
    events_.push_back({Event::Kind::Connected, id, {}});
}

bool ServerTransport::takeAccept(std::uint8_t id, Peer& p, std::span<const std::uint8_t> d)
{
    // The accept check (the accept's seq kept on the stream, 0: every packet past it). More streams than the
    // player limit - 1 by now: this one closed (Net_CloseStream), the game told nothing.
    if (static_cast<int>(peers_.size()) > options_.maxPlayers) {
        close(id);
        return false;
    }
    // "[80|id][seq] 1F <ack> 20 ..": state 2, the game given the CLIENT_ACCEPT from its 0x20 (the
    // 1F before it not read: the accept stays unacked till a later header acks it).
    if (d.size() > 4 && d[4] == static_cast<std::uint8_t>(Op::ClientAccept)) {
        p.awaitingAccept = false;
        events_.push_back({Event::Kind::Payload, id, {d.begin() + 4, d.end()}});
    }
    return true;
}

void ServerTransport::holdFlush(std::uint8_t peer, bool hold)
{
    if (const auto it = peers_.find(peer); it != peers_.end()) {
        it->second.held = hold;
    }
}

void ServerTransport::control(std::uint8_t id, Peer& p, std::span<const std::uint8_t> d)
{
    // The connection handler, server side.
    std::size_t off = 2;
    while (off < d.size() && d[off] < 0x20) { // (the game's messages follow the control ones)
        const auto op = static_cast<Op>(d[off]);
        const std::size_t size = opInfo(op).layoutSize;
        if (size == kVariableSize || off + size > d.size()) {
            return;
        }
        const auto body = d.subspan(off + 1);
        switch (op) {
        case Op::ClientPing: { // case 6: tell the game, keep its time, echo CLIENT_PONG
            events_.push_back({Event::Kind::Payload, id, {static_cast<std::uint8_t>(Op::Code37)}});
            if (id < latency_.size()) {
                latency_[id].pingedAt = u32At(body, 0);
            }
            std::vector<std::uint8_t> reply = {kServerStreamIndex, p.stream.expectedSeq(),
                                               static_cast<std::uint8_t>(Op::ClientPong)};
            putU32(reply, u32At(body, 0));
            sendPeer(p, std::move(reply));
            return;
        }
        case Op::Code9: { // case 9: the client's clock as our echo came back
            // Its round trip by its own clock: the 0x09's time less its CLIENT_PING's; under 1 ms or
            // over 999 dropped; five in a row averaged (integer division) into the stream's latency.
            if (id >= latency_.size()) {
                return;
            }
            LatencyRecord& r = latency_[id];
            const auto rtt = static_cast<std::int32_t>(u32At(body, 0) - r.pingedAt);
            if (rtt < 1 || 999 < rtt) {
                return;
            }
            r.samples[static_cast<std::size_t>(r.next)] = rtt;
            r.next = (r.next + 1) % 5;
            if (r.next == 0) {
                int sum = 0;
                for (const int s : r.samples) {
                    sum += s;
                }
                r.average = sum / 5;
            }
            return;
        }
        case Op::ClientClose: // case 10: 0x22 to the game, the stream goes (Net_CloseStream: SERVER_CLOSE too)
            if (p.awaitingAccept) {
                return; // (still awaiting its accept: ignored)
            }
            events_.push_back({Event::Kind::Closed, id, {static_cast<std::uint8_t>(Op::ClientCloseAck)}});
            close(id);
            return;
        case Op::Accepted: // case 0x1F: an explicit ack
            if (p.stream.applyAck(body[0])) {
                events_.push_back({Event::Kind::Payload, id, {static_cast<std::uint8_t>(Op::Code38), body[0]}});
            }
            break;
        default:
            return;
        }
        off += size;
    }
    // (Game messages after the control ones in the same datagram.)
    if (off < d.size()) {
        events_.push_back({Event::Kind::Payload, id, {d.begin() + static_cast<std::ptrdiff_t>(off), d.end()}});
    }
}

bool ServerTransport::sendReliable(std::uint8_t peer, std::span<const std::uint8_t> messages)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return false;
    }
    try {
        it->second.stream.queueReliable(messages);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool ServerTransport::sendReliableNow(std::uint8_t peer, std::span<const std::uint8_t> messages, std::uint32_t nowMs)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return false;
    }
    try {
        // (A packet of its own, as Net_Send makes one each call: what's queued for
        // the frame's flush doesn't join it, and goes out first to keep the order.)
        for (auto& d : it->second.stream.sendReliableAlone(messages, nowMs)) {
            sendPeer(it->second, std::move(d));
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool ServerTransport::sendUnreliable(std::uint8_t peer, std::span<const std::uint8_t> messages)
{
    const auto it = peers_.find(peer);
    return it != peers_.end() && it->second.stream.queueUnreliable(messages);
}

void ServerTransport::flushUnreliable(std::uint8_t peer)
{
    if (const auto it = peers_.find(peer); it != peers_.end()) {
        sendPeer(it->second, it->second.stream.takeUnreliable());
    }
}

void ServerTransport::close(std::uint8_t peer)
{
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return;
    }
    // Net_CloseStream: Net_FlushStream(.., 1) first (the reliable packets due, then what
    // the unreliable buffer holds), then SERVER_CLOSE (0x0B) through the buffer and flushed again.
    for (auto& d : it->second.stream.flush(lastPoll_)) {
        sendPeer(it->second, std::move(d));
    }
    const std::uint8_t closeMsg[] = {static_cast<std::uint8_t>(Op::ServerClose)};
    sendPeer(it->second, it->second.stream.unreliableDatagram(closeMsg));
    peers_.erase(it);
}

void ServerTransport::closeAll()
{
    // Net_CloseStream of the host's own stream: every client stream closed as close() does one.
    std::vector<std::uint8_t> ids;
    for (const auto& [id, p] : peers_) {
        ids.push_back(id);
    }
    for (const std::uint8_t id : ids) {
        close(id);
    }
}

void ServerTransport::closeUnaccepted()
{
    // (the game tick, before the packets are read) Every stream still in state 1 (its accept sent,
    // no CLIENT_ACCEPT yet) whose connect's frame + 300 is behind the frame now: Net_CloseStream.
    std::vector<std::uint8_t> stale;
    for (const auto& [id, p] : peers_) {
        if (p.awaitingAccept && p.connectedAt + kAcceptFrames < frame_) {
            stale.push_back(id);
        }
    }
    for (const std::uint8_t id : stale) {
        close(id);
    }
}

void ServerTransport::sendPeer(Peer& p, std::vector<std::uint8_t> bytes)
{
    if (bytes.empty()) {
        return;
    }
    xorBytes(bytes, p.key);
    outgoing_.push_back({p.at, std::move(bytes)});
}

void ServerTransport::poll(std::uint32_t nowMs)
{
    lastPoll_ = nowMs;
    pollPingTests(nowMs);
    for (auto& [id, p] : peers_) {
        bool timer = false;
        // The resend timer, run at most once per kResendTimerMs per peer.
        if (!p.timerStarted) {
            p.timerStarted = true;
            p.lastTimer = nowMs;
        } else if (static_cast<std::int32_t>(nowMs - p.lastTimer) > static_cast<std::int32_t>(kResendTimerMs)) {
            p.stream.markOverdue(nowMs);
            p.lastTimer = nowMs;
            timer = true;
        }
        if (p.held) {
            // (holdFlush: only the timer's resends.)
            if (timer) {
                for (auto& d : p.stream.flushReliable(nowMs)) {
                    sendPeer(p, std::move(d));
                }
            }
            continue;
        }
        flushed_.clear();
        p.stream.flushInto(nowMs, flushed_);
        for (auto& d : flushed_) {
            sendPeer(p, std::move(d));
        }
        flushed_.clear();
    }
}

} // namespace nox::net
