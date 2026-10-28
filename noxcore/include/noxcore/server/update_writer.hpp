// The server's side of MSG_UPDATE_STREAM (0xA4; net/update_stream.hpp has the format and the
// client's reader). GAME.EXE's server queues a record per object a player is sent this frame
// (SrvNet_SendPlayerObj, SrvNet_SendComplexObj, SrvNet_SendSimpleObj) and packs them:
//  the header from the player's own record: its position rounded down to even
//  then the record nearest the last position written: the first within 127 each way
//             in queue order, written as signed byte offsets; if none, the window grows by 127
//             (up to 0xFE0) and the first in it is written absolute after a 0 byte
//  an entry: the client's alias for it, or 0xFF net code type; the position; for a
//             complex object (0x30) or a player (0xC3) the animation byte (| 0x80 with a frame
//             byte after), and a player's state byte
//  then 00 00 00
// An alias is the client's (it names them with MSG_NEW_ALIAS): probes from the net
// code's low byte for a slot holding that net code and type that hasn't expired.
#pragma once

#include "noxcore/net/update_stream.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace nox::server {

struct UpdateRecord {
    enum class Kind : std::uint8_t { Simple = 0x2F, Complex = 0x30, Player = 0xC3 };
    Kind kind = Kind::Simple;
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t anim = 0;     ///< complex and player: a player's direction << 4
    std::uint8_t frame = 0xFF; ///< 0xFF: none
    std::uint8_t state = 0;    ///< player: its animation
};

/// An object's facing as the update's animation byte: its 8-way direction (xferIndexedDirection: dx,
/// dy each -1..1) as dy * 3 + dx + 4, less one past the middle, in the top nibble
/// (SrvNet_SendPlayerObj, SrvNet_SendComplexObj).
std::uint8_t directionNibble(std::uint16_t direction);

/// the client's alias for (netCode, typeId) at `frame`, or 0xFF.
std::uint8_t serverAlias(const net::AliasTable& aliases, std::uint16_t netCode, std::uint16_t typeId, std::uint32_t frame);

/// The whole MSG_UPDATE_STREAM, opcode first; at most `budget` bytes (entries that don't fit are
/// left out). `self` must be a player record. `written`, when given, says of each of `others`
/// whether it went in (a record left out wasn't taken: the caller keeps it owed).
std::vector<std::uint8_t> writeUpdateStream(const UpdateRecord& self, std::span<const UpdateRecord> others,
                                            const net::AliasTable& aliases, std::uint32_t frame,
                                            std::size_t budget = 1000, std::vector<bool>* written = nullptr);
/// The same stream appended to `out` (the budget counted from where it starts); `written` set as
/// above, `used` the writer's own scratch (both the caller's, kept to save their allocations).
void appendUpdateStream(std::vector<std::uint8_t>& out, const UpdateRecord& self, std::span<const UpdateRecord> others,
                        const net::AliasTable& aliases, std::uint32_t frame, std::size_t budget, std::vector<bool>& written,
                        std::vector<bool>& used);

} // namespace nox::server
