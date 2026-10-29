// The scene's order for the things lying on the floor (no SDL: the tests take it). The game's
// scene builder puts each drawable in view on one of four lists: the floor's decals (a BELOW
// thing drawn by StaticDraw or StaticRandomDraw, not PICKUP or VISIBLE_ENABLE, ALLOW_OVERLAP,
// NO_COLLIDE or IMMOBILE, not OWNER_VISIBLE), drawn with the floor tiles; the other BELOW things,
// drawn next, before the walls; the wall hangings; and the rest, sorted by y. So a decal is under
// every other BELOW thing wherever the two stand: a cave lift's stone ring (CaveElevatorBase)
// under its pit (CaveElevatorPit), whichever comes first in the map and whatever their y (a
// saved game writes the ring after the pit, half a unit lower: the drawables' places as saved).
#pragma once

#include "noxcore/things/object_class.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace noxsdl::scene {

/// Subtracted from a BELOW thing's sort key, so it's drawn under everything standing on the
/// floor, in its own y order.
inline constexpr float kBelow = 100000.0f;
/// Subtracted from a floor decal's sort key: under every other BELOW thing, in its own y order.
inline constexpr float kFloorDecal = 2 * kBelow;

/// Whether a type is one of the floor's decals (the scene's second list), from its thing.bin
/// facts: FLAGS (BELOW, EDIT_VISIBLE, ALLOW_OVERLAP, NO_COLLIDE, OWNER_VISIBLE), its DRAW's name
/// and its classes (with PICKUP for a type with a WEIGHT, as the client's reader gives it).
constexpr bool floorDecal(bool below, bool editorOnly, std::string_view drawName, std::uint32_t classes, bool allowOverlap, bool noCollide,
                          bool ownerVisible)
{
    namespace oc = nox::things::ObjectClass;
    return below && !editorOnly && (drawName == "StaticDraw" || drawName == "StaticRandomDraw") && !(classes & (oc::Pickup | oc::VisibleEnable)) &&
           (allowOverlap || noCollide || (classes & oc::Immobile)) && !ownerVisible;
}

/// What a drawable sorts by in the scene (a stable sort, so equal keys keep their order): a wall
/// hanging first of all, then the floor's decals, then the other BELOW things (and pressure
/// plates, `plate`), each by y, then the rest by y.
constexpr float sortKey(bool hanging, bool decal, bool below, bool plate, float y)
{
    if (hanging) {
        return std::numeric_limits<float>::lowest();
    }
    if (decal) {
        return y - kFloorDecal;
    }
    return below || plate ? y - kBelow : y;
}

} // namespace noxsdl::scene
