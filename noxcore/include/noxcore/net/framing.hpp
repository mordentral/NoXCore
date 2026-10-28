// Splitting a datagram's payload into messages, the way the 1.2b decoders do.
//
// Messages sit back to back with no length prefix, so the receiver has to know each message's
// size to find the next one. The client decoder (CliNet_OnPacket) and the server decoder
// (SrvNet_OnPacket) each handle a fixed set of opcodes; anything else ends the datagram, and the rest
// of it is dropped. Most sizes are fixed (messages.hpp); the variable ones are computed here:
//   S->C  0xA4 UPDATE_STREAM  needs the client's alias table and thing classes (update_stream.hpp)
//         0xA8 TEXT_MESSAGE   11 + size x (1 if flags & (2|8) else 2)
//         0xA9 INFORM         per sub-op; sub 15 is 3 + strlen + 1
//         0xAA IMPORTANT      5 only when the receiver is the host (game flag 1), else 1
//         0xB9 MAP_SEND_PACKET 6 + u16 at +4
//         0xCC SEQ_IMPORTANT  4 + u8 at +3
//   C->S  0x3F PLAYER_INPUT   2 + u8 at +1
//         0xA8 TEXT_MESSAGE   11 + size x (1 if flags & 2 else 2): the server ignores flag 8
//         0xBB SERVER_CMD     7 + 2 x u8 at +4 (the 1.2b client sends 2 bytes more than its text)
//   both  families            by sub-op; TEAM 0x00 is 18 + 2 x nameLen, XFER 0x02 is 8 + u16 at +6
#pragma once

#include "noxcore/net/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace nox::net {

enum class Direction : std::uint8_t {
    ServerToClient,
    ClientToServer,
};

struct FramingOptions {
    /// The receiver is the host player (game flag 1); only changes S->C IMPORTANT.
    bool receiverIsHost = false;
};

/// Size of the message at the start of `data` (which begins with its opcode) as the receiving
/// side reads it. Returns 0 if the receiver doesn't decode that opcode or sub-op, or if the
/// message runs past the end of `data`; returns kVariableSize for S->C UPDATE_STREAM, which only
/// the stream reader can size.
std::size_t messageSize(std::span<const std::uint8_t> data, Direction dir, const FramingOptions& options = {});

/// Sizes an S->C UPDATE_STREAM at the start of the span it is given (see update_stream.hpp).
using UpdateStreamSizer = std::function<std::size_t(std::span<const std::uint8_t>)>;

struct SplitResult {
    std::vector<std::span<const std::uint8_t>> messages;
    /// Bytes after the last message that the receiver ignores (an unknown or truncated message).
    std::span<const std::uint8_t> dropped;
};

/// Splits a datagram payload (after the 2-byte transport header) into messages. Without a
/// `streamSizer` the split stops at an S->C UPDATE_STREAM.
SplitResult splitMessages(std::span<const std::uint8_t> payload, Direction dir, const FramingOptions& options = {},
                          const UpdateStreamSizer& streamSizer = {});

/// Encodes a message for `dir`, trimming the direction-dependent ones (IMPORTANT, OUTGOING_CLIENT,
/// REPORT_SECONDARY_WEAPON) to the size the receiver reads.
template <class Message>
std::vector<std::uint8_t> encode(const Message& m, Direction dir, const FramingOptions& options = {});

/// Decodes one whole message (opcode first, as split by splitMessages). Throws FormatError if
/// the opcode or sub-op doesn't match or the size is wrong.
template <class Message>
Message decode(std::span<const std::uint8_t> bytes);

/// A message's bytes as it writes them, its opcode first (not trimmed for a direction).
template <class Message>
std::vector<std::uint8_t> bytesOf(const Message& m);
/// bytesOf's bytes appended to `out`.
template <class Message>
void appendBytesOf(std::vector<std::uint8_t>& out, const Message& m);

namespace detail {
std::vector<std::uint8_t> trimForDirection(std::vector<std::uint8_t> bytes, Direction dir, const FramingOptions& options);
} // namespace detail

} // namespace nox::net

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

template <class Message>
Message nox::net::decode(std::span<const std::uint8_t> bytes)
{
    ByteReader r(bytes, opInfo(Message::kOp).name);
    if (r.u8() != static_cast<std::uint8_t>(Message::kOp)) {
        r.fail("wrong opcode");
    }
    if constexpr (requires { Message::kSub; }) {
        if (r.u8() != Message::kSub) {
            r.fail("wrong sub-op");
        }
    }
    Message m = Message::read(r);
    r.expectEnd();
    return m;
}

template <class Message>
std::vector<std::uint8_t> nox::net::encode(const Message& m, Direction dir, const FramingOptions& options)
{
    ByteWriter w;
    m.write(w);
    return detail::trimForDirection(std::move(w).take(), dir, options);
}

template <class Message>
std::vector<std::uint8_t> nox::net::bytesOf(const Message& m)
{
    ByteWriter w;
    m.write(w);
    return std::move(w).take();
}

template <class Message>
void nox::net::appendBytesOf(std::vector<std::uint8_t>& out, const Message& m)
{
    ByteWriter w(std::move(out));
    m.write(w);
    out = std::move(w).take();
}
