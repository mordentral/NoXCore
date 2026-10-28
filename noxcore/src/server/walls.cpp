// Breakable walls (walls.hpp): blows to a wall cell, the debris thrown, a wall's breaking, and the
// walls struck about a blow or a blast.
#include "noxcore/server/walls.hpp"

#include "noxcore/server/physics.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
using util::X87;

/// A cell's middle (x 23 + 11.5).
float middle(int c) { return static_cast<float>(c) * 23.0f + 11.5f; }

} // namespace

void Walls::debris(const std::string& type, float x, float y, float push, float fromX, float fromY)
{
    // (a material that names none throws Brick0.)
    const std::string name = type.empty() ? "Brick0" : type;
    if (!things.findThing(name)) {
        return;
    }
    ServerObject& d = world.spawn(things, name, x, y);
    d.born = frame;
    d.flags |= ObjFlag::Active;
    d.touch();
    applyForce(world, d, fromX, fromY, push);
}

bool Walls::damage(int x, int y, int amount, int type, ServerObject* by)
{
    (void)type;
    WallCell* w = world.gridWall(x, y);
    if (!w || w->broken) {
        return false;
    }
    const things::WallDef* wallDef = w->material < things.walls.size() ? &things.walls[w->material] : nullptr;
    const auto wallMaterial = static_cast<std::uint16_t>(wallDef ? wallDef->material : 0);
    if (!w->destructible) {
        if (by) { // (the two materials meeting, at what struck it)
            materialSound(world, *by, wallMaterial, by->material);
        }
        return false;
    }
    // A quest's magic wall breaks at once to a player's blow.
    int dmg = amount;
    if (world.quest() && by && w->material == world.wallMaterial("MagicWallSystemUseOnly")) {
        const ServerObject* p = by;
        // Up the owner chain to the player behind the blow (16 steps at most).
        for (int guard = 0; p && !(p->classes & oc::Player) && p->owner != 0 && guard < 16; ++guard) {
            const ServerObject* next = world.byExtent(p->owner);
            if (!next) {
                break;
            }
            p = next;
        }
        if (p && (p->classes & oc::Player)) {
            dmg = 99999999;
        }
    }
    if (w->health > dmg) {
        w->health = static_cast<std::uint8_t>(w->health - dmg);
        const things::WallDef* def = w->material < things.walls.size() ? &things.walls[w->material] : nullptr;
        if (def && !def->debris.empty()) {
            // One piece from its middle (pushed from its middle too: no push at all).
            const int i = nox::util::uniform_int_distribution<int>(0, static_cast<int>(def->debris.size()) - 1)(rng);
            const float push = util::randomFloat(rng, 10.0f, 20.0f).f(); // (randomFloat, stored)
            debris(def->debris[static_cast<std::size_t>(i)], middle(x), middle(y), push, middle(x), middle(y));
        }
        if (by) { // (its material damaged, at what struck it)
            damagedSound(world, *by, wallMaterial);
        }
        return false;
    }
    return breakWall(x, y);
}

bool Walls::breakWall(int x, int y)
{
    WallCell* w = world.gridWall(x, y);
    if (!w || !w->destructible || w->broken) {
        return false;
    }
    const float cx = middle(x);
    const float cy = middle(y);
    w->health = 0;
    // Its material's break sound (else WallDestroyed) at its middle.
    {
        const things::WallDef* def = w->material < things.walls.size() ? &things.walls[w->material] : nullptr;
        playSoundAt(world, def && !def->breakSound.empty() ? std::string_view(def->breakSound) : std::string_view("WallDestroyed"), cx, cy);
    }
    // A burst effect (0x8A) at its middle for every client.
    pointFx(world, 0x8A, cx, cy);
    if (!world.quest()) {
        const things::WallDef* def = w->material < things.walls.size() ? &things.walls[w->material] : nullptr;
        const int count = def ? static_cast<int>(def->debris.size()) : 0;
        if (count != 0) {
            // 3-6 pieces, its material's debris kinds taken in turn from a random first one.
            int i = nox::util::uniform_int_distribution<int>(0, count - 1)(rng);
            for (int n = nox::util::uniform_int_distribution<int>(3, 6)(rng); 0 < n; --n) {
                // (randomFloat's stack values added to the middle, each one store; y's drawn first)
                const float py = (util::randomFloat(rng, -2.0f, 2.0f) + cy).f();
                const float px = (util::randomFloat(rng, -2.0f, 2.0f) + cx).f();
                const float push = util::randomFloat(rng, 4.0f, 10.0f).f();
                debris(def->debris[static_cast<std::size_t>(i)], px, py, push, cx, cy);
                i = (i + 1) % count;
            }
        }
    }
    if (w->material != world.wallMaterial("MagicWallSystemUseOnly")) {
        // Map_BreakWallById: broken; MSG_DESTROY_WALL (0x3A, its index) to every player.
        w->broken = true;
        world.broadcasts().push_back(
            {{0x3A, static_cast<std::uint8_t>(w->wallId), static_cast<std::uint8_t>(w->wallId >> 8)}, true, 0});
        return true;
    }
    revertMagicWalls(world, [&](const MagicWall& m) { return m.x == x && m.y == y; });
    return true;
}

bool Walls::damageAround(int x0, int y0, int x1, int y1, float px, float py, float reach, int amount, int type, ServerObject* by)
{
    // From a creature, or a weapon held (by its holder's facing): only the cells ahead of it.
    bool facing = false;
    float ox = px;
    float oy = py;
    std::pair<float, float> way{};
    if (by) {
        const ServerObject* facer = nullptr;
        if (by->classes & (oc::Monster | oc::Player)) {
            facer = by;
        } else if ((by->classes & 0x1001000u) && by->holder != 0) {  // a weapon or wand in hand
            facer = world.byExtent(by->holder);
        }
        if (facer) {
            facing = true;
            way = directionVector(facer->direction);
            // (The exe measures from the object itself; a held weapon's place is its holder's.)
            ox = by->holder != 0 ? facer->x : by->x;
            oy = by->holder != 0 ? facer->y : by->y;
        }
    }
    std::vector<std::pair<int, int>> struck;
    const float reach2 = (X87(reach) * reach).f();
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (!world.gridWall(x, y)) {
                continue;
            }
            // (On the x87 as the exe has it: the x offset stored, the y one kept for its square
            // and stored; the square's test on the stack, then it stored.)
            const float dx = (X87(middle(x)) - ox).f();
            const X87 dy53 = X87(middle(y)) - oy;
            const float dy = dy53.f();
            const X87 d2 = dy53 * dy + X87(dx) * dx;
            if (!(d2 < X87(reach2))) {
                continue;
            }
            // Ahead of the facer: within 60 degrees of its facing (cos 60 = 0.5, a double).
            if (const float d2f = d2.f(); facing && 0 < d2f) {
                const X87 d = util::sqrt(X87(d2f)) + 0.5f;
                const float ax = (X87(dx) / d).f();
                if (!(X87::fromDouble(0.5) < X87(dy) / d * way.second + X87(ax) * way.first)) {
                    continue;
                }
            }
            RayHit hit;
            const bool clear = world.traceRay(px, py, middle(x), middle(y), Trace::Doors, &hit);
            // Struck when the ray to its middle is clear, or meets its first wall in this very cell.
            if ((clear || (hit.cellX == x && hit.cellY == y)) && struck.size() < 32) {
                struck.emplace_back(x, y);
            }
        }
    }
    for (const auto& [x, y] : struck) {
        damage(x, y, amount, type, by);
    }
    return !struck.empty();
}

} // namespace nox::server
