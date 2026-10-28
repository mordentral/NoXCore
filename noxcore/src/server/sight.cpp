// Line-of-sight tests: whether one unit can see another (blindness, invisibility),
// whether bodies or walls stand between them, and the spells' target searches.
// Used by the spells, the monsters' AI and anything that needs "can a reach b".
#include "noxcore/server/sight.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

// A line segment from (x0, y0) to (x1, y1).
struct Segment {
    float x0, y0, x1, y1;
};

/// Whether two segments cross: their boxes meet, then the sides each way.
bool crosses(const Segment& a, const Segment& b)
{
    const float aLeft = std::min(a.x0, a.x1);
    const float aRight = a.x1 <= a.x0 ? a.x0 : a.x1;
    const float aTop = std::min(a.y0, a.y1);
    const float aBottom = a.y1 <= a.y0 ? a.y0 : a.y1;
    const float bLeft = std::min(b.x0, b.x1);
    const float bRight = b.x1 <= b.x0 ? b.x0 : b.x1;
    const float bTop = std::min(b.y0, b.y1);
    const float bBottom = b.y1 <= b.y0 ? b.y0 : b.y1;
    if (!(bLeft <= aRight && aLeft <= bRight && bTop <= aBottom && aTop <= bBottom)) {
        return false;
    }
    // Both horizontal: the boxes meeting is enough.
    if (a.y0 == a.y1 && b.y0 == b.y1) {
        return true;
    }
    // Where each meets the other's line, as fractions s / d along b and t / d along a;
    // both must fall inside their segment (b just touching a's line at its start doesn't count).
    // (The game's x87: each difference stored; s's first product takes oy before its store, at
    // 53 bits; each product pair's difference on the stack, then stored.)
    using util::X87;
    const float adx = (X87(a.x1) - a.x0).f();
    const float ady = (X87(a.y1) - a.y0).f();
    const float bdx = (X87(b.x1) - b.x0).f();
    const float bdy = (X87(b.y1) - b.y0).f();
    const float ox = (X87(b.x0) - a.x0).f();
    const X87 oyRaw = X87(b.y0) - a.y0;
    const float oy = oyRaw.f();
    const float s = (oyRaw * adx - X87(ox) * ady).f();
    const float d = (X87(bdx) * ady - X87(bdy) * adx).f();
    if (s == 0 || d == 0 || (s < 0 && 0 < d) || (0 < s && d < 0) || std::fabs(d) < std::fabs(s)) {
        return false;
    }
    const float t = (X87(oy) * bdx - X87(ox) * bdy).f();
    if ((t < 0 && 0 < d) || (0 < t && d < 0)) {
        return false;
    }
    return std::fabs(t) <= std::fabs(d);
}

/// The foot of the perpendicular from (px, py) to the line, if it falls on the segment
/// (mathPointOnTheLine).
bool footOn(const Segment& s, float px, float py, float& fx, float& fy)
{
    // (The game's x87: dx stays on the stack throughout and dy is stored, the products taking
    // the 53-bit dy or its float as the exe's; len2 and `along` stored; fx's quotient takes the
    // unstored `along`, fy's the stored one.)
    using util::X87;
    const X87 dx = X87(s.x1) - s.x0;
    const X87 dyRaw = X87(s.y1) - s.y0;
    const float dy = dyRaw.f();
    const float len2 = (dyRaw * dy + dx * dx).f();
    const X87 alongRaw = (X87(py) - s.y0) * dy + (X87(px) - s.x0) * dx;
    const float along = alongRaw.f();
    fx = (alongRaw * dx / len2 + s.x0).f();
    const float qy = (X87(along) * dy / len2).f();
    fy = (X87(qy) + s.y0).f();
    const float left = s.x1 <= s.x0 ? s.x1 : s.x0;
    const float right = s.x1 <= s.x0 ? s.x0 : s.x1;
    const float top = s.y1 <= s.y0 ? s.y1 : s.y0;
    const float bottom = s.y1 <= s.y0 ? s.y0 : s.y1;
    return left <= fx && fx <= right && top <= fy && fy <= bottom;
}

/// A door's leaf from its hinge at each of its 32 directions.
constexpr std::array<std::pair<int, int>, 32> kLeaf = {{
    {-23, -23}, {-18, -27}, {-12, -30}, {-6, -31}, {0, -32}, {6, -31}, {12, -30}, {18, -27},
    {23, -23}, {27, -18}, {30, -12}, {31, -6}, {32, 0}, {31, 6}, {30, 12}, {27, 18},
    {23, 23}, {18, 27}, {12, 30}, {6, 31}, {0, 32}, {-6, 31}, {-12, 30}, {-18, 27},
    {-23, 23}, {-27, 18}, {-30, 12}, {-31, 6}, {-32, 0}, {-31, -6}, {-30, -12}, {-27, -18},
}};

/// Whether `o` stands across the line.
bool inTheWay(const ServerObject& o, const Segment& line)
{
    // A door blocks with its leaf (from an eighth of its length behind the hinge to its tip);
    // a gate (subclass 4, see-through bars) never blocks.
    if (o.classes & oc::Door) {
        if (o.subclasses & 4u) {
            return false;
        }
        // (The game's x87: an eighth of the leaf exact; each end stored, the tip's sum of three on
        // the stack.)
        using util::X87;
        const auto [lx, ly] = kLeaf[o.doorDirection & 31];
        const X87 ex = X87::fromDouble(lx) * 0.125f;
        const float ey = (X87::fromDouble(ly) * 0.125f).f();
        const float x0 = (X87(o.x) - ex).f();
        const float y0 = (X87(o.y) - ey).f();
        return crosses(line, {x0, y0, (X87::fromDouble(lx) + x0 + ex).f(), (X87::fromDouble(ly) + y0 + ey).f()});
    }
    // A circle blocks when the segment passes within its radius of the centre (its shape's
    // r squared, stored; the distance's squares and sum on the stack).
    if (o.shape == ShapeKind::Circle) {
        using util::X87;
        float fx = 0;
        float fy = 0;
        if (!footOn(line, o.x, o.y, fx, fy)) {
            return false;
        }
        const X87 dx = X87(fx) - o.x;
        const X87 dy = X87(fy) - o.y;
        return dx * dx + dy * dy <= (X87(o.radius) * o.radius).f();
    }
    if (o.shape == ShapeKind::Box) {
        // Its corners (its shape's, boxCorners: top, left, right, bottom), each about it stored.
        using util::X87;
        const BoxCorners k = boxCorners(o.boxWidth, o.boxHeight);
        const float c0x = (X87(k.c0x) + o.x).f(), c0y = (X87(k.c0y) + o.y).f();
        const float c1x = (X87(k.c1x) + o.x).f(), c1y = (X87(k.c1y) + o.y).f();
        const float c2x = (X87(k.c2x) + o.x).f(), c2y = (X87(k.c2y) + o.y).f();
        const float c3x = (X87(k.c3x) + o.x).f(), c3y = (X87(k.c3y) + o.y).f();
        return crosses(line, {c0x, c0y, c1x, c1y}) || crosses(line, {c0x, c0y, c2x, c2y}) || crosses(line, {c3x, c3y, c2x, c2y}) ||
               crosses(line, {c3x, c3y, c1x, c1y});
    }
    return false;
}

} // namespace

bool pointOnLine(float x0, float y0, float x1, float y1, float px, float py, float& fx, float& fy)
{
    return footOn({x0, y0, x1, y1}, px, py, fx, fy);
}

bool segmentsCross(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1)
{
    return crosses({ax0, ay0, ax1, ay1}, {bx0, by0, bx1, by1});
}

std::pair<int, int> doorLeaf(int direction)
{
    return kLeaf[static_cast<std::size_t>(direction & 31)];
}

bool canSee(const World& world, const ServerObject& viewer, const ServerObject& seen, int flags)
{
    if (enchanted(viewer, Enchant::Blinded)) {
        return false;
    }
    // An invisible one is seen only with infravision (in a quest only Hecubah and the
    // Necromancer see it), by a pixie whose owner has infravision, or while it moves fast.
    if (!(flags & 1) && enchanted(seen, Enchant::Invisible)) {
        bool sees = world.quest() ? viewer.typeName == NameId::Hecubah || viewer.typeName == NameId::Necromancer
                                  : enchanted(viewer, Enchant::Infravision);
        if (!sees && viewer.typeName == NameId::Pixie && viewer.owner != 0) {
            const auto owner = world.objects().find(viewer.owner);
            sees = owner != world.objects().end() && enchanted(owner->second, Enchant::Infravision);
        }
        if (!sees && std::fabs(seen.vx) <= 6.0f && std::fabs(seen.vy) <= 6.0f) {
            return false;
        }
    }
    // A monster of subclass 0x2000 doesn't see a warrior using Tread Lightly (ability 4).
    if ((viewer.classes & oc::Monster) && (viewer.subclasses & 0x2000u) && abilityActive(world, seen, 4)) {
        return false;
    }
    return true;
}

bool bodiesClear(const World& world, const ServerObject& from, const ServerObject& to)
{
    Segment line{from.x, from.y, to.x, to.y};
    // A gate is aimed at the middle of its leaf instead of its hinge.
    if ((to.classes & oc::Door) && (to.subclasses & 4u)) { // (a door's middle)
        const auto [lx, ly] = kLeaf[to.doorDirection & 31];
        line.x1 = (util::X87::fromDouble(lx / 2) + to.x).f();
        line.y1 = (util::X87::fromDouble(ly / 2) + to.y).f();
    }
    // (mapCheck) The units in the line's box (getUnitsInRect, by the grid; a search
    // three deep finds none), each through any across it blocks. (The game's walk goes on past
    // the first across it, doing nothing more: ours stops there.)
    bool clear = true;
    const float left = line.x1 <= line.x0 ? line.x1 : line.x0;
    const float right = line.x1 <= line.x0 ? line.x0 : line.x1;
    const float top = line.y1 <= line.y0 ? line.y1 : line.y0;
    const float bottom = line.y1 <= line.y0 ? line.y0 : line.y1;
    const_cast<World&>(world).unitsInRectUntil(left, top, right, bottom, [&](const ServerObject& o) {
        if (&o != &from && &o != &to && (o.flags & 0x10000u) && inTheWay(o, line)) { // (no 0x20 test)
            clear = false;
        }
        return !clear;
    });
    return clear && world.traceRay(line.x0, line.y0, line.x1, line.y1, Trace::Doors | Trace::NearWindows);
}

bool canInteract(const World& world, const ServerObject& a, const ServerObject& b, int flags)
{
    return canSee(world, a, b, flags) && bodiesClear(world, a, b);
}

bool sameSide(const World& world, const ServerObject& a, const ServerObject& b)
{
    World& w = const_cast<World&>(world);
    const ServerObject* x = &a;
    // Each of a and its owners (at most 16 up the chain) against each of b and its owners.
    for (int i = 0; x && i < 16; ++i) {
        const ServerObject* y = &b;
        for (int j = 0; y && j < 16; ++j) {
            if ((x->team != 0 && x->team == y->team) || x == y) {
                return true;
            }
            y = y->owner ? w.byExtent(y->owner) : nullptr;
        }
        x = x->owner ? w.byExtent(x->owner) : nullptr;
    }
    return false;
}

ServerObject* nearestCreature(World& world, ServerObject& from, float range)
{
    // with {sides 0xF, interact 1, enemy 0, classes 6, flags any,
    // not 0x8000}: the facing sides (all) always pass.
    // (The game's x87: range squared stored; each distance squared summed on the stack, compared
    // there, stored when nearer.)
    using util::X87;
    ServerObject* best = nullptr;
    float bestD2 = (X87(range) * range).f();
    world.unitsInCircle(from.x, from.y, range, [&](ServerObject& o) { // (unitsGetInCircle, by the grid)
        if (o.pendingDelete || o.holder != 0 || &o == &from || sameSide(world, o, from) || (o.player && (o.player->statusFlags & 1u)) ||
            !(o.classes & (oc::Monster | oc::Player)) || o.flags == 0 || (o.flags & ObjFlag::Dead) || !canInteract(world, from, o)) {
            return;
        }
        const X87 dx = X87(o.x) - from.x;
        const X87 dy = X87(o.y) - from.y;
        const X87 d2 = dx * dx + dy * dy;
        if (d2 < bestD2) {
            bestD2 = d2.f();
            best = &o;
        }
    });
    return best;
}

ServerObject* searchTarget(World& world, const float* point, ServerObject* searcher, std::uint32_t spellFlags, float range, int mode,
                           ServerObject* cursorOwner)
{
    // A hostile spell (flag 0x20) takes the enemy under the caster's cursor first.
    if (cursorOwner && cursorOwner->player && (spellFlags & 0x20u) && cursorOwner->player->cursorObject != 0) {
        ServerObject* under = world.byExtent(cursorOwner->player->cursorObject);
        if (under && isEnemy(world, *cursorOwner, *under) && (mode == 1 || (mode == 0 && searcher != under))) {
            return under;
        }
    }
    float x = 0;
    float y = 0;
    // The search centre: the point when the searcher has a clear ray to it, else the searcher.
    if (!point) {
        if (!searcher) {
            return nullptr;
        }
        x = searcher->x;
        y = searcher->y;
    } else if (!searcher) {
        x = point[0];
        y = point[1];
    } else if (world.traceRay(searcher->x, searcher->y, point[0], point[1], Trace::Doors | Trace::ThroughWindows)) {
        x = point[0];
        y = point[1];
    } else {
        x = searcher->x;
        y = searcher->y;
    }
    // Its player (findParentChainPlayer).
    ServerObject* parent = searcher;
    for (int guard = 0; parent && !(parent->classes & oc::Player) && parent->owner != 0 && guard < 16; ++guard) {
        ServerObject* next = world.byExtent(parent->owner);
        if (!next) {
            break;
        }
        parent = next;
    }
    ServerObject* best = nullptr;
    // (The game's x87: range squared and the square's sides stored; each distance squared summed
    // on the stack, then stored.)
    using util::X87;
    float bestD2 = 1e8f; // (0x4CBEBC20)
    const float range2 = (X87(range) * range).f();
    world.unitsInRect((X87(x) - range).f(), (X87(y) - range).f(), (X87(x) + range).f(), (X87(y) + range).f(),
                      [&](ServerObject& o) { // (getUnitsInRect, by the grid)
        if (o.pendingDelete) { // (0x8020: ours' 0x20)
            return;
        }
        // Mode 0 leaves the searcher itself out; mode 1 may pick it (a heal on oneself).
        if (mode == 0 && searcher == &o) {
            return;
        }
        if (!(o.classes & (oc::Monster | oc::Player | oc::MonsterGenerator))) {
            return;
        }
        if ((o.classes & oc::Monster) && (o.subclasses & 0x8000u)) {  // NO_SPELL_TARGET
            return;
        }
        if ((o.classes & oc::Player) && o.player && (o.player->statusFlags & 1u)) {  // observing
            return;
        }
        if (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
            return;
        }
        if ((o.classes & oc::Monster) && (o.subclasses & 0x4000u)) {  // NO_TARGET
            return;
        }
        if ((spellFlags & 0x20u) && !(searcher && isEnemy(world, *searcher, o))) {
            return;
        }
        const X87 dx = X87(x) - o.x;
        const X87 dy = X87(y) - o.y;
        const float d2 = (dx * dx + dy * dy).f();
        if (range2 < d2) {
            return;
        }
        // Both the searcher and its player must see it with nothing in the way.
        if (searcher && !canInteract(world, *searcher, o)) {
            return;
        }
        if (parent && !canInteract(world, *parent, o)) {
            return;
        }
        if (d2 < bestD2) {
            best = &o;
            bestD2 = d2;
        }
    });
    return best;
}

} // namespace nox::server
