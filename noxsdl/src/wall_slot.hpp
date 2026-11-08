// Which of a wall piece's four images is drawn, as the game picks it. A WALL variant has four:
// 0 the wall as it stands, 1 its front image (a wall in front of the eye, seen through), and 2
// and 3 the same two for a wall that can be broken (in the map's DestructableWalls, wall flag
// 8): the cracked look that tells the player it can be broken. No SDL in it, so the tests drive it.
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <cstddef>

namespace noxsdl {

/// The slot of `slots` drawn for a wall piece: its front image when `front` (and it has one), the
/// breakable one of the two when `breakable` (and it has one).
inline std::size_t wallSlot(const nox::things::WallVariant& slots, bool front, bool breakable)
{
    const std::size_t base = front && slots[1].image.index >= 0 ? 1 : 0;
    const auto& cracked = slots[base + 2].image;
    return breakable && (cracked.index >= 0 || !cracked.name.empty()) ? base + 2 : base;
}

} // namespace noxsdl
