// The inventory grid as Inventory::room reads it (rows 0-19, each row's 4 columns), as plain data:
// the game we host reads its client's room for a pickup from its own thread (LocalHost's client
// view), the client from its Inventory.
#pragma once

#include <array>
#include <cstdint>

namespace noxsdl {

struct RoomCell {
    std::uint16_t typeId = 0;  ///< the cell's thing type (0: empty)
    std::uint16_t count = 0;   ///< the things stacked in it (0: empty)
    std::uint16_t limit = 31;  ///< a stack of its type at most (31; food 3, or 9 in a solo or quest game)
    bool stackable = false;    ///< its type stacks (not NOT_STACKABLE)
};
using RoomCells = std::array<RoomCell, 20 * 4>;

/// The cells that could take `count` more of `typeId`: empty ones, and stacks of it with room.
inline int roomIn(const RoomCells& cells, std::uint16_t typeId, int count)
{
    int n = 0;
    for (const RoomCell& c : cells) {
        if (c.count == 0) {
            ++n;
        } else if (c.stackable && c.typeId == typeId && c.count + count <= c.limit) {
            ++n;
        }
    }
    return n;
}

} // namespace noxsdl
