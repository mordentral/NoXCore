// MSG_UPDATE_STREAM (0xA4, S->C): positions and animation of the objects the client can see.
//
// Layout, from CliNet_ReadUpdateStreamHeader and CliNet_ReadUpdateStreamEntry
// (CliNet_ReadUpdateStreamEntry):
//   header  id; u16 x, y (absolute, also the camera); u8 anim; [u8 frame if anim & 0x80]; u8 state
//   entries until 00 00 00 or the end of the datagram:
//           [u8 0 = absolute position]; id; i8 dx, dy (from the previous object) or i16 x, y;
//           complex objects (class 0x200000) then have u8 anim, [u8 frame if anim & 0x80] and,
//           for players (class 4), u8 state
//   id      u8 alias 0..254, or 0xFF, u16 netCode, u16 typeId. For the long form the client picks
//           an alias itself and tells the server with MSG_NEW_ALIAS (0xA5).
// Anim bits 4-6 are the direction (values above 3 shift up by 1 on the client); without a state
// byte, the low nibble of anim is the animation state.
//
// Entries need the thing class of the object, so only the client can size the message. An entry
// outside 0..6000, or whose drawable can't be created, ends the stream early; like 1.2b, the
// bytes after the position are then read as the next message.
#pragma once

#include "noxcore/net/messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace nox::net {

/// The client's update-stream aliases (g_cliUpdateAliases), 8 bytes per slot.
class AliasTable {
public:
    struct Slot {
        std::uint16_t netCode = 0;
        std::uint16_t typeId = 0;
        std::uint32_t expireFrame = 0; ///< reusable once the frame passes it; 0xFFFFFFFF = never

        bool operator==(const Slot&) const = default;
    };

    static constexpr std::uint8_t kNone = 0xFF;

    /// cliGenerateAlias: probes slots 1..254 starting at netCode & 0xFF for one
    /// that already holds this object or has expired. kNone if every slot is taken.
    std::uint8_t allocate(std::uint16_t netCode, std::uint16_t typeId, std::uint32_t frame) const;

    void set(std::uint8_t alias, const Slot& slot) { slots_[alias] = slot; }
    const Slot& operator[](std::uint8_t alias) const { return slots_[alias]; }
    void clear() { slots_ = {}; }

private:
    std::array<Slot, 256> slots_{};
};

struct StreamObject {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    std::uint8_t alias = AliasTable::kNone; ///< the alias used or assigned; kNone if the table was full
    bool longForm = false;                  ///< sent as 0xFF netCode typeId
    bool absolute = false;                  ///< entries: position sent absolute (header: always)
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool hasAnim = false; ///< header, and complex objects
    std::uint8_t anim = 0;
    std::optional<std::uint8_t> frame;
    std::optional<std::uint8_t> state; ///< header, and player objects

    bool operator==(const StreamObject&) const = default;
};

struct UpdateStream {
    StreamObject header; ///< netCode 0 and typeId 0 = camera position only
    std::vector<StreamObject> entries;
    /// MSG_NEW_ALIAS replies the client queues, in order (expire 0xFFFFFFFF for the header,
    /// frame + 60 for entries).
    std::vector<msg::NewAlias> newAliases;
    /// Bytes consumed, including the opcode. The next message starts there.
    std::size_t size = 0;
    /// An entry was rejected (position outside 0..6000 or no drawable); `size` stops after its
    /// position, as in 1.2b.
    bool aborted = false;
};

/// Thing class bits of the drawable the client uses for (netCode, typeId): the existing
/// drawable's class if it has one, else the thing type's. nullopt if it can't create one.
using DrawableClassLookup = std::function<std::optional<std::uint32_t>(std::uint16_t netCode, std::uint16_t typeId)>;

/// Reads an UPDATE_STREAM from `data` (starting at the opcode, running to the end of the
/// datagram), assigning aliases in `aliases` as the client does. Throws FormatError if the
/// message is truncated.
UpdateStream readUpdateStream(std::span<const std::uint8_t> data, AliasTable& aliases, std::uint32_t frame,
                              const DrawableClassLookup& classOf);

} // namespace nox::net
