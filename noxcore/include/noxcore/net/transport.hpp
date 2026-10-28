// The 1.2b UDP transport ("netstr", comlib.c), without sockets.
//
// Datagram: u8 id, u8 seq, then messages back to back (framing.hpp).
//   id    the sender's stream id on the receiver (0xFF before one is assigned) | 0x80 = reliable
//   seq   reliable: the packet's sequence number (u8, per stream, from 0)
//         unreliable: the next reliable sequence the sender expects, i.e. a cumulative ack
// Once connected, whole datagrams (header included) are XORed with the key from SERVER_ACCEPT.
//
// Reliable packets (Net_Send flag 1) each travel alone as [id|0x80][seq][messages],
// are resent 2 s after sending until acked (timer runs at most once a second), and
// are only accepted in order: the receiver drops any seq other than the one it expects.
// Unreliable messages collect in the stream buffer and go out on flush (Net_FlushStream).
// A changed ack (an unreliable header, or a 0x1F message) drops every queued packet before it,
// marks the packet it names for an immediate resend, and injects "0x26 ack" into the game.
//
// Messages below 0x20 at the start of a datagram are transport control: connect
// (0x00), accept (0x01), refusals (0x02-0x04), pings (0x05-0x09), disconnects (0x0A, 0x0B)
// and explicit acks (0x1F). The transport answers pings itself and injects 0x21-0x26 into the
// game queue.
//
// Joining a server (client side):
//   lobby socket   -> 00 00 0E TRY_JOIN; <- 0x14 JOIN_OK, 0x0F PASSWORD_REQUIRED (-> 0x11),
//                     0x13 SERVER_ERROR, 0x15 JOIN_FAIL; pings 0x10 are echoed back as 0x12
//   game socket    -> reliable FF 00 00 (connect)
//                  <- reliable [80][s] 1F <our seq> 01 <u32 id> <u8 key>     (not XORed; a
//                     server heads all it sends with its own stream index, 0)
//                  -> reliable [80|id][1] 1F <s+1> 20 CLIENT_ACCEPT          (XORed from here)
#pragma once

#include "noxcore/net/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::net {

inline constexpr std::uint16_t kDefaultServerPort = 18590; ///< 0x489E (server listen)
inline constexpr std::uint32_t kGameVersion = 0x1039A;     ///< TRY_JOIN version the 1.2b server accepts
inline constexpr std::uint8_t kUnassignedId = 0xFF;
inline constexpr std::uint8_t kReliableBit = 0x80;
inline constexpr std::size_t kMaxReliablePayload = 0x400; ///< Net init's (200, 0x400)
inline constexpr std::size_t kMaxQueuedReliable = 200;
inline constexpr std::size_t kClientBufferSize = 0x800;   ///< client stream buffer
inline constexpr std::uint32_t kResendDelayMs = 2000;
inline constexpr std::uint32_t kResendTimerMs = 1000;
inline constexpr std::uint32_t kConnectTimeoutMs = 60 * 1000; ///< cliWaitServerResponse

/// XORs every byte with `key`.
void xorBytes(std::span<std::uint8_t> data, std::uint8_t key) noexcept;

/// One end of a netstr stream: sequence numbers, the reliable queue and the unreliable buffer.
class Stream {
public:
    explicit Stream(std::size_t bufferSize = kClientBufferSize)
        : bufferSize_(bufferSize)
    {
    }

    /// Header byte 0 of what we send (kUnassignedId until the peer assigns one).
    std::uint8_t id() const noexcept { return id_; }
    void setId(std::uint8_t id) noexcept { id_ = id; }
    /// Next reliable sequence we expect from the peer: header byte 1 of our unreliable datagrams.
    std::uint8_t expectedSeq() const noexcept { return expected_; }
    /// The peer's last ack of our reliable packets (0xFF until the first).
    std::uint8_t lastAck() const noexcept { return lastAck_; }
    std::uint8_t nextSeq() const noexcept { return nextSeq_; }
    std::size_t queuedReliable() const noexcept { return queue_.size(); }
    /// The map sender's count: the queued (unacknowledged) reliable packets whose first message is
    /// `op` or `op2` (walk the queue's nodes, each one Net_Send's
    /// data: sendReliableAlone's packets).
    std::size_t queuedReliableOf(std::uint8_t op, std::uint8_t op2) const noexcept
    {
        std::size_t n = 0;
        for (const auto& p : queue_) {
            n += p.bytes.size() > 2 && (p.bytes[2] == op || p.bytes[2] == op2);
        }
        return n;
    }
    std::size_t bufferedUnreliable() const noexcept { return unreliable_.size(); }

    /// Queues [id|0x80][seq][messages] for sending on the next flush; returns the seq. Messages
    /// queued before that flush join the last packet while it has room (kMaxReliablePayload).
    /// Throws FormatError if `messages` is too big or the queue is full.
    std::uint8_t queueReliable(std::span<const std::uint8_t> messages);
    /// Sends a queued reliable packet now, as Net_Send does with flags 3; empty if not queued.
    std::vector<std::uint8_t> sendReliableNow(std::uint8_t seq, std::uint32_t nowMs);
    /// Net_Send's flags 3 whole: `messages` as a packet of their own (makes a queue
    /// node each call, so the map sender's count finds them first in it), sent now; a packet still unsent
    /// goes out before it (the peer takes reliable packets only in order). The datagrams, in
    /// order. Throws FormatError like queueReliable.
    std::vector<std::vector<std::uint8_t>> sendReliableAlone(std::span<const std::uint8_t> messages, std::uint32_t nowMs);
    /// Appends to the unreliable buffer; false if it doesn't fit (Net_Send's -7).
    bool queueUnreliable(std::span<const std::uint8_t> messages);
    /// A standalone unreliable datagram [id][expected][messages].
    std::vector<std::uint8_t> unreliableDatagram(std::span<const std::uint8_t> messages) const;
    /// The unreliable buffer as a datagram, the buffer emptied (empty if it was: Net_FlushStream's
    /// unreliable part, sent only past its 2-byte header).
    std::vector<std::uint8_t> takeUnreliable();

    /// Net_FlushStream: reliable packets due to be sent, oldest first, then the unreliable buffer
    /// if it isn't empty. 1.2b sends newest first, which makes the receiver drop all but one
    /// of several pending packets (ours is a deliberate deviation).
    std::vector<std::vector<std::uint8_t>> flush(std::uint32_t nowMs);
    /// The same datagrams appended to `out`.
    void flushInto(std::uint32_t nowMs, std::vector<std::vector<std::uint8_t>>& out);
    /// The reliable packets due alone, the unreliable buffer kept (the timer's).
    std::vector<std::vector<std::uint8_t>> flushReliable(std::uint32_t nowMs);
    /// The resend timer: packets sent at least kResendDelayMs ago are due again.
    void markOverdue(std::uint32_t nowMs);

    /// Applies the peer's ack (the queue's drop and the resend mark, each mode 1). Returns true if it changed,
    /// in which case the game gets "0x26 ack".
    bool applyAck(std::uint8_t ack);
    /// Drops one queued packet (the queue's drop, mode 0).
    void forget(std::uint8_t seq);

    /// Header handling for a datagram from the peer. Returns false if it must be
    /// dropped: too short, or a reliable packet out of order. `ackChanged` is set when an
    /// unreliable header carried a new ack.
    bool acceptDatagram(std::span<const std::uint8_t> datagram, std::optional<std::uint8_t>& ackChanged);

private:
    struct Packet {
        std::uint8_t seq = 0;
        bool due = true;   ///< send on the next flush
        bool sent = false; ///< sent at least once (no more messages join it)
        std::uint32_t resendAt = 0;
        std::vector<std::uint8_t> bytes; ///< the whole datagram

        /// Sent now: no longer due, closed to more messages, resent if unacked by then.
        void markSent(std::uint32_t nowMs) noexcept
        {
            due = false;
            sent = true;
            resendAt = nowMs + kResendDelayMs;
        }
    };
    /// A new packet [id|0x80][seq][messages], taking the next seq.
    Packet newPacket(std::span<const std::uint8_t> messages);

    std::size_t bufferSize_;
    std::uint8_t id_ = kUnassignedId;
    std::uint8_t expected_ = 0;
    std::uint8_t nextSeq_ = 0;
    std::uint8_t lastAck_ = 0xFF;
    std::vector<Packet> queue_; ///< oldest first
    std::vector<std::uint8_t> unreliable_;
};

/// A client's connection to a 1.2b server: lobby join, connect handshake and the stream.
/// It owns no sockets: feed it datagrams and the time, and send what it hands back.
class ClientConnection {
public:
    enum class State : std::uint8_t {
        Idle,
        Joining,          ///< TRY_JOIN sent, waiting for the lobby answer
        PasswordRequired, ///< server wants SERVER_PASSWORD
        JoinOk,           ///< lobby accepted; call connect()
        JoinRefused,      ///< SERVER_ERROR or JOIN_FAIL; see joinError()
        Connecting,       ///< connect sent, waiting for SERVER_ACCEPT
        Connected,
        ConnectRefused,   ///< 0x02/0x03/0x04, or no answer within kConnectTimeoutMs
        Closed,           ///< the server disconnected
    };

    struct Datagram {
        bool lobby = false; ///< send from the lobby socket (else the game socket)
        std::vector<std::uint8_t> bytes;
    };

    State state() const noexcept { return state_; }
    /// SERVER_ERROR code (0x13), or -1.
    int joinError() const noexcept { return joinError_; }
    /// The id the server assigned (SERVER_ACCEPT), valid once connected.
    std::uint32_t playerId() const noexcept { return playerId_; }
    std::uint8_t xorKey() const noexcept { return key_; }
    const Stream& stream() const noexcept { return stream_; }

    /// Lobby: sends TRY_JOIN (version forced to kGameVersion).
    void tryJoin(msg::ServerTryJoin join);
    void sendPassword(const msg::ServerPassword& password);
    /// A datagram that arrived on the lobby socket from the server's address.
    void receiveLobby(std::span<const std::uint8_t> datagram);

    /// Starts the connect handshake; `accept` is sent once the server accepts.
    void connect(const msg::ClientAccept& accept, std::uint32_t nowMs);
    /// A datagram that arrived on the game socket from the server's address (still XORed).
    void receive(std::span<const std::uint8_t> datagram, std::uint32_t nowMs);

    /// Queues game messages (whole messages, opcode first).
    void sendReliable(std::span<const std::uint8_t> messages);
    bool sendUnreliable(std::span<const std::uint8_t> messages);
    /// Net_Send's flags 2: `messages` as an unreliable datagram of their own, sent now (not
    /// through the stream's buffer). Nothing while not connected.
    void sendUnreliableNow(std::span<const std::uint8_t> messages);

    /// Runs the resend timer and connect timeout, and flushes the stream.
    void poll(std::uint32_t nowMs);
    /// Sends CLIENT_CLOSE (0x0A); the server closes our stream (case 10) and removes the
    /// player. Send it only after OUTGOING_CLIENT has been acked, as the vanilla client does
    /// (HeadlessClient::quit).
    void disconnect();

    /// Datagrams to send, in order (game-socket ones already XORed).
    std::vector<Datagram> takeOutgoing();
    /// Game payloads, in order: the messages of each game datagram, and the messages the
    /// transport injects (0x21, 0x23-0x26) as their own payloads. Split them with framing.hpp.
    std::vector<std::vector<std::uint8_t>> takeReceived();

private:
    void sendGame(std::vector<std::uint8_t> bytes);
    void control(std::span<const std::uint8_t> datagram, std::uint32_t nowMs);
    void inject(std::vector<std::uint8_t> message) { received_.push_back(std::move(message)); }

    State state_ = State::Idle;
    int joinError_ = -1;
    Stream stream_;
    std::uint8_t key_ = 0;
    std::uint32_t playerId_ = 0;
    std::optional<msg::ClientAccept> pendingAccept_;
    std::uint32_t connectStart_ = 0;
    std::uint32_t lastTimer_ = 0;
    bool timerStarted_ = false;
    /// Our ping: CLIENT_PING with our clock every 2 s; the server echoes it
    /// (CLIENT_PONG) and our answer (0x09, our clock then) is what it measures our latency by.
    std::optional<std::uint32_t> pingSentAt_;
    std::uint32_t lastPingAt_ = 0;
    bool pinged_ = false;
    std::vector<Datagram> outgoing_;
    std::vector<std::vector<std::uint8_t>> received_;
};

} // namespace nox::net
