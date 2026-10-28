// Breakable walls: blows and area damage taking their health, debris, their breaking.
//
//   - A blow to a wall cell (damageToMap): a wall there (Map_GetWallAtGrid: an
//     even cell's, not broken, not a door's) that isn't breakable only sounds its material
//     against the striker's; a breakable one loses the damage (a quest's magic wall: everything, to a
//     player's blow), throwing one of its material's debris (Brick0 when it names none) from its
//     middle at 10-20; at 0 it breaks.
//   - Breaking (wallPreDestroy): its health 0, MSG_FX 0x8A and its
//     material's break sound (else WallDestroyed), 3-6 debris (not in a quest) pushed off at
//     4-10; a map wall marked broken (its flag 0x20) and MSG_DESTROY_WALL (its index) to every
//     player; a Wall spell's wall put back as it was.
//   - The walls about a blow (mapDamageToWalls): the cells of a box, their middles
//     within `reach` of the point (from a creature or a held weapon: within 60 degrees of the
//     facing, from its own place), the ray from the point to the middle clear or ending in that
//     cell (flags 1); at most 32, each struck. By the area damage (Missiles::damageAt: the
//     box about it) and the melee blows (Fight::attack: the box of its reach).
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <cstdint>
#include <random>

namespace nox::server {

struct Walls {
    World& world;
    const things::ThingDb& things;
    std::minstd_rand& rng;
    std::uint32_t frame;

    /// damageToMap; whether it broke.
    bool damage(int x, int y, int amount, int type, ServerObject* by);
    /// wallPreDestroy; whether there was a breakable wall to break.
    bool breakWall(int x, int y);
    /// mapDamageToWalls over cells (x0, y0)-(x1, y1) about (px, py); whether any
    /// was struck.
    bool damageAround(int x0, int y0, int x1, int y1, float px, float py, float reach, int amount, int type, ServerObject* by);

private:
    void debris(const std::string& type, float x, float y, float push, float fromX, float fromY);
};

} // namespace nox::server
