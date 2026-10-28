// Message sizing and datagram splitting; the size rules are listed in framing.hpp.
#include "noxcore/net/framing.hpp"

#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace nox::net {

namespace {

// (u16At: the caller has checked that both bytes exist.)
using util::u16At;

/// `size` if it fits in `data`, else 0.
std::size_t fits(std::span<const std::uint8_t> data, std::size_t size)
{
    return size <= data.size() ? size : 0;
}

// Size of a message from one of the sub-op families (opcode, then a sub-op byte that picks the
// layout). Fixed sizes come from the sub-op table; the three variable ones are worked out here.
// Returns 0 when the message is unknown or runs past the end of `data`.
std::size_t subOpSize(std::span<const std::uint8_t> data, Direction dir)
{
    if (data.size() < 2) {
        return 0;
    }
    const Op op = static_cast<Op>(data[0]);
    const std::uint8_t sub = data[1];
    const SubOpInfo* info = subOpInfo(op, sub);
    if (!info) {
        return 0; // both decoders give up on unknown sub-ops
    }
    const std::size_t size = dir == Direction::ServerToClient ? info->serverToClient : info->clientToServer;
    if (size != kVariableSize) {
        return fits(data, size);
    }
    switch (op) {
    case Op::Inform: // sub 15: u8 isSign, then a NUL-terminated string id
        if (data.size() > 3) {
            const auto* nul = static_cast<const std::uint8_t*>(std::memchr(data.data() + 3, 0, data.size() - 3));
            if (nul) {
                return static_cast<std::size_t>(nul - data.data()) + 1;
            }
        }
        return 0;
    case Op::TeamMsg: // sub 0: u8 nameLen at +15, UTF-16 name at +18
        return data.size() > 15 ? fits(data, 18 + 2 * std::size_t{data[15]}) : 0;
    case Op::XferMsg: // sub 2: u16 size at +6, data at +8
        return data.size() >= 8 ? fits(data, 8 + std::size_t{u16At(data, 6)}) : 0;
    default:
        return 0;
    }
}

} // namespace

std::size_t messageSize(std::span<const std::uint8_t> data, Direction dir, const FramingOptions& options)
{
    if (data.empty()) {
        return 0;
    }
    const Op op = static_cast<Op>(data[0]);
    const OpInfo& info = opInfo(op);
    const bool toClient = dir == Direction::ServerToClient;
    const std::size_t size = toClient ? info.serverToClient : info.clientToServer;
    if (size == 0) {
        return 0; // this side's decoder has no case for the opcode
    }
    // S->C IMPORTANT carries its 4-byte payload only to the host; everyone else reads the opcode alone.
    if (toClient && op == Op::Important) {
        return fits(data, options.receiverIsHost ? msg::Important::kSize : 1);
    }
    if (size != kVariableSize) {
        return fits(data, size);
    }
    switch (op) {
    case Op::UpdateStream:
        return kVariableSize;
    case Op::TextMessage: {
        if (data.size() < 9) {
            return 0;
        }
        // Flag 2 (and, on the client only, flag 8) marks single-byte text; otherwise it is
        // UTF-16. The character count is the u8 at +8, and 11 bytes of header come first.
        const std::uint8_t flags = data[3];
        const std::size_t perChar = (flags & (toClient ? 0x0A : 0x02)) ? 1 : 2;
        return fits(data, 11 + perChar * data[8]);
    }
    case Op::MapSendPacket:
        return data.size() >= 6 ? fits(data, 6 + std::size_t{u16At(data, 4)}) : 0;
    case Op::NoxcppSubstep: // (noxcpp's: k, m, count, u16 frame, then 6 bytes an object)
        return data.size() >= 6 ? fits(data, 6 + 6 * std::size_t{data[3]}) : 0;
    case Op::NoxcppMapVote: // (noxcpp's: sub, u16 the rest's length)
    case Op::NoxcppModeF4:
    case Op::NoxcppModeF5:
    case Op::NoxcppEdit:
    case Op::NoxcppCoop:
    case Op::NoxcppModeUi:
        return data.size() >= 4 ? fits(data, 4 + std::size_t{u16At(data, 2)}) : 0;
    case Op::SeqImportant:
        return data.size() >= 4 ? fits(data, 4 + std::size_t{data[3]}) : 0;
    case Op::PlayerInput:
        return data.size() >= 2 ? fits(data, 2 + std::size_t{data[1]}) : 0;
    case Op::ServerCmd: // 5-byte header (text length at +4), UTF-16 text, and the 2 extra bytes 1.2b sends
        return data.size() >= 5 ? fits(data, 7 + 2 * std::size_t{data[4]}) : 0;
    default:
        return subOpSize(data, dir);
    }
}

SplitResult splitMessages(std::span<const std::uint8_t> payload, Direction dir, const FramingOptions& options,
                          const UpdateStreamSizer& streamSizer)
{
    SplitResult out;
    std::size_t offset = 0;
    // Peel messages off the front until the payload runs out or one can't be sized; like the
    // exe, everything from an unknown or truncated message on is dropped.
    while (offset < payload.size()) {
        const auto rest = payload.subspan(offset);
        std::size_t size = messageSize(rest, dir, options);
        if (size == kVariableSize) {
            size = streamSizer ? streamSizer(rest) : 0;
        }
        if (size == 0 || size > rest.size()) {
            break;
        }
        out.messages.push_back(rest.first(size));
        offset += size;
    }
    out.dropped = payload.subspan(offset);
    return out;
}

namespace detail {

// Cuts an encoded message down to the size the receiving side reads for its opcode, so a
// message written with every field still goes out at the length the other end expects.
// Variable-size and unknown messages are left as they are.
std::vector<std::uint8_t> trimForDirection(std::vector<std::uint8_t> bytes, Direction dir, const FramingOptions& options)
{
    if (bytes.empty()) {
        return bytes;
    }
    const Op op = static_cast<Op>(bytes[0]);
    const OpInfo& info = opInfo(op);
    std::size_t size = dir == Direction::ServerToClient ? info.serverToClient : info.clientToServer;
    if (dir == Direction::ServerToClient && op == Op::Important) {
        size = options.receiverIsHost ? msg::Important::kSize : 1;
    }
    if (size != 0 && size != kVariableSize && size < bytes.size()) {
        bytes.resize(size);
    }
    return bytes;
}

} // namespace detail

} // namespace nox::net
