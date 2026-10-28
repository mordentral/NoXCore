#include "noxcore/net/transport.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/net/framing.hpp"
#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <utility>

// The netstr stream and the client side of the connection; the protocol is described in
// transport.hpp.
namespace nox::net {

namespace {

// Little-endian reads and writes of the u32 fields in control messages.
using util::putU32;
using util::u32At;

/// `a` is at or after `b`, allowing for wraparound.
bool reached(std::uint32_t a, std::uint32_t b)
{
    return static_cast<std::int32_t>(a - b) >= 0;
}

/// mode 1: is packet `seq` acknowledged by `ack`? Near the wrap (ack < 0x20 or > 0xE0)
/// the comparison is signed, so 0xF0 counts as before 0x05.
bool acknowledged(std::uint8_t seq, std::uint8_t ack)
{
    if (ack < 0x20 || ack > 0xE0) {
        return static_cast<std::int8_t>(seq) < static_cast<std::int8_t>(ack);
    }
    return seq < ack;
}

} // namespace

void xorBytes(std::span<std::uint8_t> data, std::uint8_t key) noexcept
{
    for (auto& b : data) {
        b ^= key;
    }
}

// ---------------------------------------------------------------------------------- Stream

std::uint8_t Stream::queueReliable(std::span<const std::uint8_t> messages)
{
    if (messages.size() > kMaxReliablePayload) {
        throw FormatError("reliable packet too big");
    }
    // Messages queued before the next flush share a packet while it has room: the exe's game
    // sends a player's reliable messages through its netlist, a frame's worth in one Net_Send
    // (netImportant / netlist_clientSendWrap), not one packet each.
    // The 2 bytes subtracted are the packet's own [id][seq] header.
    if (!queue_.empty() && !queue_.back().sent && queue_.back().bytes.size() - 2 + messages.size() <= kMaxReliablePayload) {
        queue_.back().bytes.insert(queue_.back().bytes.end(), messages.begin(), messages.end());
        return queue_.back().seq;
    }
    if (queue_.size() >= kMaxQueuedReliable) {
        throw FormatError("reliable queue full");
    }
    queue_.push_back(newPacket(messages));
    return queue_.back().seq;
}

Stream::Packet Stream::newPacket(std::span<const std::uint8_t> messages)
{
    Packet p;
    p.seq = nextSeq_++;
    p.bytes.reserve(messages.size() + 2);
    p.bytes.push_back(static_cast<std::uint8_t>(id_ | kReliableBit));
    p.bytes.push_back(p.seq);
    p.bytes.insert(p.bytes.end(), messages.begin(), messages.end());
    return p;
}

std::vector<std::uint8_t> Stream::sendReliableNow(std::uint8_t seq, std::uint32_t nowMs)
{
    for (auto& p : queue_) {
        if (p.seq == seq) {
            p.markSent(nowMs);
            return p.bytes;
        }
    }
    return {};
}

std::vector<std::vector<std::uint8_t>> Stream::sendReliableAlone(std::span<const std::uint8_t> messages, std::uint32_t nowMs)
{
    if (messages.size() > kMaxReliablePayload) {
        throw FormatError("reliable packet too big");
    }
    if (queue_.size() >= kMaxQueuedReliable) {
        throw FormatError("reliable queue full");
    }
    std::vector<std::vector<std::uint8_t>> out;
    // Anything still unsent goes first: the peer only takes reliable packets in order, so the new
    // packet must not overtake them.
    for (auto& p : queue_) {
        if (!p.sent) {
            p.markSent(nowMs);
            out.push_back(p.bytes);
        }
    }
    Packet p = newPacket(messages);
    p.markSent(nowMs);
    out.push_back(p.bytes);
    queue_.push_back(std::move(p));
    return out;
}

bool Stream::queueUnreliable(std::span<const std::uint8_t> messages)
{
    // Net_Send refuses when buffer end < write pointer + len + 1.
    if (unreliable_.size() + messages.size() >= bufferSize_) {
        return false;
    }
    unreliable_.insert(unreliable_.end(), messages.begin(), messages.end());
    return true;
}

std::vector<std::uint8_t> Stream::unreliableDatagram(std::span<const std::uint8_t> messages) const
{
    std::vector<std::uint8_t> d;
    d.reserve(messages.size() + 2);
    d.push_back(id_);
    d.push_back(expected_);
    d.insert(d.end(), messages.begin(), messages.end());
    return d;
}

std::vector<std::uint8_t> Stream::takeUnreliable()
{
    if (unreliable_.empty()) {
        return {};
    }
    std::vector<std::uint8_t> d = unreliableDatagram(unreliable_);
    unreliable_.clear();
    return d;
}

std::vector<std::vector<std::uint8_t>> Stream::flushReliable(std::uint32_t nowMs)
{
    std::vector<std::vector<std::uint8_t>> out;
    for (auto& p : queue_) {
        if (p.due) {
            p.markSent(nowMs);
            out.push_back(p.bytes);
        }
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> Stream::flush(std::uint32_t nowMs)
{
    std::vector<std::vector<std::uint8_t>> out;
    flushInto(nowMs, out);
    return out;
}

void Stream::flushInto(std::uint32_t nowMs, std::vector<std::vector<std::uint8_t>>& out)
{
    for (auto& p : queue_) {
        if (p.due) {
            p.markSent(nowMs);
            out.push_back(p.bytes);
        }
    }
    if (auto u = takeUnreliable(); !u.empty()) {
        out.push_back(std::move(u));
    }
}

void Stream::markOverdue(std::uint32_t nowMs)
{
    for (auto& p : queue_) {
        if (reached(nowMs, p.resendAt)) {
            p.due = true;
        }
    }
}

bool Stream::applyAck(std::uint8_t ack)
{
    // The same ack again changes nothing (the peer repeats it in every unreliable header).
    if (ack == lastAck_) {
        return false;
    }
    for (auto& p : queue_) {
        if (p.seq == ack) {
            p.due = true; // the peer is waiting for this one: resend it at once
        }
    }
    std::erase_if(queue_, [ack](const Packet& p) { return acknowledged(p.seq, ack); });
    lastAck_ = ack;
    return true;
}

void Stream::forget(std::uint8_t seq)
{
    std::erase_if(queue_, [seq](const Packet& p) { return p.seq == seq; });
}

bool Stream::acceptDatagram(std::span<const std::uint8_t> datagram, std::optional<std::uint8_t>& ackChanged)
{
    ackChanged.reset();
    if (datagram.size() <= 2) {
        return false;
    }
    // Byte 1 is the sender's seq on a reliable packet, and its cumulative ack on an unreliable one.
    const std::uint8_t seq = datagram[1];
    if ((datagram[0] & kReliableBit) == 0) {
        if (applyAck(seq)) {
            ackChanged = seq;
        }
        return true;
    }
    if (seq != expected_) {
        return false; // duplicate or out of order: 1.2b drops it and waits for the resend
    }
    ++expected_;
    return true;
}

// ------------------------------------------------------------------------ ClientConnection

void ClientConnection::tryJoin(msg::ServerTryJoin join)
{
    join.version = kGameVersion;
    Datagram d{true, {0, 0}};
    const auto msg = encode(join, Direction::ClientToServer);
    d.bytes.insert(d.bytes.end(), msg.begin(), msg.end());
    outgoing_.push_back(std::move(d));
    state_ = State::Joining;
    joinError_ = -1;
}

void ClientConnection::sendPassword(const msg::ServerPassword& password)
{
    Datagram d{true, {0, 0}};
    const auto msg = encode(password, Direction::ClientToServer);
    d.bytes.insert(d.bytes.end(), msg.begin(), msg.end());
    outgoing_.push_back(std::move(d));
    state_ = State::Joining;
}

void ClientConnection::receiveLobby(std::span<const std::uint8_t> datagram)
{
    if (datagram.size() < 3) {
        return;
    }
    const bool waiting = state_ == State::Joining || state_ == State::PasswordRequired;
    switch (static_cast<Op>(datagram[2])) {
    case Op::PasswordRequired:
        if (waiting) {
            state_ = State::PasswordRequired;
        }
        break;
    case Op::ServerPing: // echo it back as SERVER_PONG
        if (waiting && datagram.size() >= 8) {
            Datagram d{true, {datagram.begin(), datagram.begin() + 8}};
            d.bytes[2] = static_cast<std::uint8_t>(Op::ServerPong);
            outgoing_.push_back(std::move(d));
        }
        break;
    case Op::ServerError:
        if (waiting && datagram.size() >= 4) {
            joinError_ = datagram[3];
            state_ = State::JoinRefused;
        }
        break;
    case Op::ServerJoinOk:
        if (waiting) {
            state_ = State::JoinOk;
        }
        break;
    case Op::ServerJoinFail:
        if (waiting) {
            state_ = State::JoinRefused;
        }
        break;
    default:
        break;
    }
}

void ClientConnection::connect(const msg::ClientAccept& accept, std::uint32_t nowMs)
{
    stream_ = Stream(kClientBufferSize);
    key_ = 0;
    playerId_ = 0;
    pendingAccept_ = accept;
    const std::uint8_t connect[] = {static_cast<std::uint8_t>(Op::ServerConnect)};
    const std::uint8_t seq = stream_.queueReliable(connect);
    sendGame(stream_.sendReliableNow(seq, nowMs));
    state_ = State::Connecting;
    connectStart_ = nowMs;
}

void ClientConnection::receive(std::span<const std::uint8_t> datagram, std::uint32_t nowMs)
{
    std::vector<std::uint8_t> d(datagram.begin(), datagram.end());
    // Once connected everything from the server is XORed with the key, header included.
    if (key_ != 0) {
        xorBytes(d, key_);
    }
    std::optional<std::uint8_t> ack;
    if (!stream_.acceptDatagram(d, ack)) {
        return;
    }
    // A new ack reaches the game as "0x26 ack", as 1.2b injects it.
    if (ack) {
        inject({static_cast<std::uint8_t>(Op::Code38), *ack});
    }
    // Opcodes below 0x20 are transport control; the rest is game messages for the caller.
    if (d[2] < 0x20) {
        control(d, nowMs);
    } else {
        received_.emplace_back(d.begin() + 2, d.end());
    }
}

void ClientConnection::control(std::span<const std::uint8_t> d, std::uint32_t nowMs)
{
    // client side. Handlers that answer end the datagram, as there.
    std::size_t off = 2;
    bool accepted = false;
    // Walk the control messages; layoutSize includes the opcode byte.
    while (off < d.size()) {
        const auto op = static_cast<Op>(d[off]);
        const std::size_t size = opInfo(op).layoutSize;
        if (size == kVariableSize || off + size > d.size()) {
            break;
        }
        const auto body = d.subspan(off + 1);
        switch (op) {
        case Op::ServerAccept:
            // SERVER_ACCEPT: u32 our id (its low byte becomes our stream id), u8 the XOR key.
            playerId_ = u32At(body, 0);
            stream_.setId(static_cast<std::uint8_t>(playerId_));
            key_ = body[4];
            if (state_ == State::Connecting) {
                accepted = true;
            }
            break;
        case Op::Code2:
        case Op::Code3:
        case Op::Code4:
            if (state_ == State::Connecting) {
                joinError_ = static_cast<int>(op);
                state_ = State::ConnectRefused;
            }
            break;
        case Op::Code5: { // pre-join ping: answer 0x07 with the same value
            std::vector<std::uint8_t> reply = {stream_.id(), 0, static_cast<std::uint8_t>(Op::Code7)};
            putU32(reply, u32At(body, 0));
            sendGame(std::move(reply));
            return;
        }
        case Op::ClientPing: { // answer CLIENT_PONG and tell the game
            inject({static_cast<std::uint8_t>(Op::Code37)});
            std::vector<std::uint8_t> reply = {stream_.id(), stream_.expectedSeq(),
                                               static_cast<std::uint8_t>(Op::ClientPong)};
            putU32(reply, u32At(body, 0));
            sendGame(std::move(reply));
            return;
        }
        case Op::ClientPong: { // the server's echo of our ping (case 8)
            if (!pingSentAt_ || u32At(body, 0) != *pingSentAt_) {
                return;
            }
            // Tell the game the round trip (MSG_PING), and answer 0x09 with our clock: the server
            // takes the difference as our latency (case 9, averaged over five).
            std::vector<std::uint8_t> ms = {static_cast<std::uint8_t>(Op::Ping)};
            putU32(ms, nowMs - *pingSentAt_);
            inject(std::move(ms));
            pingSentAt_.reset();
            std::vector<std::uint8_t> answer = {static_cast<std::uint8_t>(Op::Code9)};
            putU32(answer, nowMs);
            sendGame(stream_.unreliableDatagram(answer));
            return;
        }
        case Op::ServerClose:
            inject({static_cast<std::uint8_t>(Op::ServerCloseAck)});
            state_ = State::Closed;
            return;
        case Op::Accepted:
            if (stream_.applyAck(body[0])) {
                inject({static_cast<std::uint8_t>(Op::Code38), body[0]});
            }
            break;
        default: // 0x07 / 0x09 answer pings we don't send that way; anything else ends the datagram
            return;
        }
        off += size;
    }
    if (accepted && pendingAccept_) {
        // nox SERVER_ACCEPT proves the connect arrived. (Its 0x1F carries the connect's
        // own seq, which the ack rule would treat as "resend it"; we just drop the packet.)
        stream_.forget(0);
        state_ = State::Connected;
        std::vector<std::uint8_t> msgs = {static_cast<std::uint8_t>(Op::Accepted), stream_.expectedSeq()};
        const auto accept = encode(*pendingAccept_, Direction::ClientToServer);
        msgs.insert(msgs.end(), accept.begin(), accept.end());
        pendingAccept_.reset();
        const std::uint8_t seq = stream_.queueReliable(msgs);
        sendGame(stream_.sendReliableNow(seq, nowMs));
    }
}

void ClientConnection::sendReliable(std::span<const std::uint8_t> messages)
{
    stream_.queueReliable(messages);
}

bool ClientConnection::sendUnreliable(std::span<const std::uint8_t> messages)
{
    return stream_.queueUnreliable(messages);
}

void ClientConnection::sendUnreliableNow(std::span<const std::uint8_t> messages)
{
    if (state_ == State::Connected) {
        sendGame(stream_.unreliableDatagram(messages));
    }
}

void ClientConnection::poll(std::uint32_t nowMs)
{
    // No SERVER_ACCEPT within the timeout counts as a refusal.
    if (state_ == State::Connecting && reached(nowMs, connectStart_ + kConnectTimeoutMs)) {
        state_ = State::ConnectRefused;
        return;
    }
    if (state_ != State::Connecting && state_ != State::Connected) {
        return;
    }
    if (!timerStarted_) {
        timerStarted_ = true;
        lastTimer_ = nowMs;
    // The resend check runs at most once per kResendTimerMs, as the exe's timer does.
    } else if (static_cast<std::int32_t>(nowMs - lastTimer_) > static_cast<std::int32_t>(kResendTimerMs)) {
        stream_.markOverdue(nowMs);
        lastTimer_ = nowMs;
    }
    // Our ping, every 2 s (over 1999 ms since the last): the server times its round
    // trip by our clock (cases 6 / 9) and reports it to everyone as our latency
    // (MSG_REPORT_ALL_LATENCY). Without it the server has no latency for us: 0.
    if (state_ == State::Connected && (!pinged_ || nowMs - lastPingAt_ > 1999)) {
        pinged_ = true;
        lastPingAt_ = nowMs;
        pingSentAt_ = nowMs;
        std::vector<std::uint8_t> ping = {static_cast<std::uint8_t>(Op::ClientPing)};
        putU32(ping, nowMs);
        sendGame(stream_.unreliableDatagram(ping));
    }
    for (auto& d : stream_.flush(nowMs)) {
        sendGame(std::move(d));
    }
}

void ClientConnection::disconnect()
{
    if (state_ != State::Connected && state_ != State::Connecting) {
        return;
    }
    const std::uint8_t close[] = {static_cast<std::uint8_t>(Op::ClientClose)};
    sendGame(stream_.unreliableDatagram(close));
    state_ = State::Closed;
}

void ClientConnection::sendGame(std::vector<std::uint8_t> bytes)
{
    if (bytes.empty()) {
        return;
    }
    if (key_ != 0) {
        xorBytes(bytes, key_);
    }
    outgoing_.push_back({false, std::move(bytes)});
}

std::vector<ClientConnection::Datagram> ClientConnection::takeOutgoing()
{
    return std::exchange(outgoing_, {});
}

std::vector<std::vector<std::uint8_t>> ClientConnection::takeReceived()
{
    return std::exchange(received_, {});
}

} // namespace nox::net
