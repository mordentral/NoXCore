// The live enemy creatures a unit sees about it, nearest first: the search a mode's weapons and
// blocks share, and the toolbox's nox.nearestFoes. Nothing here knows any mode.
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace nox::server::detail {

/// The live enemy creatures within `range` of `unit` it sees (no wall between), nearest first:
/// of the 24 nearest (one past that replacing the farthest kept), the walls tested for the first
/// `tested`, each one seen handed to `take` till it answers false.
template <class Take>
void visibleFoes(World& world, const ServerObject& unit, float range, std::size_t tested, Take take)
{
    struct Cand {
        ServerObject* o;
        float d2;
    };
    Cand found[24];
    std::size_t n = 0;
    world.unitsInCircle(unit.x, unit.y, range, [&](ServerObject& o) {
        if (!o.monster || o.gone() || o.pendingDelete || (o.flags & ObjFlag::Dead) || o.health == 0 || !isEnemy(world, unit, o)) {
            return;
        }
        const float dx = o.x - unit.x;
        const float dy = o.y - unit.y;
        const Cand c{&o, dx * dx + dy * dy};
        if (n < std::size(found)) {
            found[n++] = c;
            return;
        }
        // (Full: the farthest kept gives way.)
        auto farthest = std::max_element(found, found + n, [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        if (c.d2 < farthest->d2) {
            *farthest = c;
        }
    });
    std::sort(found, found + n, [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    for (std::size_t i = 0; i < n && i < tested; ++i) {
        if (world.traceRay(unit.x, unit.y, found[i].o->x, found[i].o->y, Trace::Doors | Trace::ThroughWindows) && !take(found[i].o)) {
            return;
        }
    }
}

/// Whether `unit` sees any live enemy creature within `range` (the walls tested for the `tested`
/// nearest).
inline bool foeInSight(World& world, const ServerObject& unit, float range, std::size_t tested)
{
    bool seen = false;
    visibleFoes(world, unit, range, tested, [&](ServerObject*) {
        seen = true;
        return false;
    });
    return seen;
}

} // namespace nox::server::detail
