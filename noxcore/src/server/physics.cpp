// The motion: each frame's moving things pushed apart by bodies, walls and doors in five
// sub-steps, their velocities and positions integrated, doors swung, and the touches found
// collected as Contacts for collide.cpp. physics.hpp describes the steps and the queue.
#include "noxcore/server/physics.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"

#include "noxcore/server/enchant.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/wall_shapes.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <tuple>
#include <vector>

namespace nox::server {

namespace {

using util::X87;

constexpr float kStep = 0.2f;   // (five of them)
constexpr float kStill = 0.01f;
constexpr float kWallPush = 100;
constexpr float kBodyPush = 40;
constexpr float kGlance = 0.7f;
constexpr float kSqrtHalf = 0.70710677f; // (the turned frame's)
constexpr float kInvCell = kInvWallCell;
// The exe's shorter root half, 0.7071 (a box's sides, the walls' diagonals); its half, the box
// corners', is world.hpp's kShapeBoxHalf.
constexpr float kRoot = kShortRootHalf;
constexpr float kDoorPushDamp = 0.25f;

/// updateUnits' test: beyond 0.01 either way, against the double 0.01 (not the float).
bool astir(float v) { return static_cast<double>(v) <= -0.01 || 0.01 <= static_cast<double>(v); }

/// A door's edge from its hinge at each angle, half length (sincosTable16: the
/// exe's own, quirks and all).
constexpr std::pair<int, int> kDoorEdge[256] = {
#include "door_edge_table.inc"
};

/// A box against the walls (the box wall step's table, one entry a wall shape 0-10): each wall
/// as its two lines in the turned frame, `across` at u = the cell's middle running along v,
/// `along` at v = the cell's middle running along u; a line is skipped while the box's centre is
/// in a quarter of the cell its mask names (1: the low u and v half, 2: high u, low v, 4: high
/// u and v, 8: low u, high v). From its start (the cell's low end plus `offset`) it runs
/// `length`. The half lines that start at the middle run a whole diagonal, half a cell past the
/// cell (the exe's table: an original bug, kept).
struct BoxWallLine {
    bool present;
    std::uint8_t mask;
    float offset;
    float length;
};
constexpr float kCellHalfDiag = std::bit_cast<float>(0x41821B8Fu); ///< 23 x 0.7071, 16.26: half a cell's diagonal
constexpr float kCellDiag = std::bit_cast<float>(0x42021B8Fu);     ///< 32.53: a cell's diagonal
constexpr float kH = kCellHalfDiag;
constexpr float kF = kCellDiag;
constexpr std::array<std::array<BoxWallLine, 2>, 11> kBoxWalls = {{
    {{{true, 0, 0, kF}, {false, 0, 0, 0}}},
    {{{false, 0, 0, 0}, {true, 0, 0, kF}}},
    {{{true, 0, 0, kF}, {true, 0, 0, kF}}},
    {{{true, 0, 0, kF}, {true, 6, 0, kH}}},
    {{{true, 0xC, 0, kH}, {true, 0, 0, kF}}},
    {{{true, 0, 0, kF}, {true, 9, kH, kF}}},
    {{{true, 3, kH, kF}, {true, 0, 0, kF}}},
    {{{true, 8, 0, kH}, {true, 2, 0, kH}}},
    {{{true, 4, 0, kH}, {true, 1, kH, kF}}},
    {{{true, 2, kH, kF}, {true, 8, kH, kF}}},
    {{{true, 1, kH, kF}, {true, 4, 0, kH}}},
}};

struct Box {
    float left, top, right, bottom;
};

/// A box's corners about its centre: a rectangle turned 45 degrees, `width` along
/// (1, 1) and `height` along (-1, 1); top, left, right, bottom: the shape set-up's
/// (server::boxCorners, world.hpp).
std::array<std::pair<float, float>, 4> boxCorners(const ServerObject& o)
{
    const BoxCorners c = server::boxCorners(o.boxWidth, o.boxHeight);
    return {{{c.c0x, c.c0y}, {c.c1x, c.c1y}, {c.c2x, c.c2y}, {c.c3x, c.c3y}}};
}

/// Its collision box (CollideP1 / P2, objectUnkUpdateCoords) about its frame's position: a box's
/// from its corners (the left's x, the top's y, the right's x, the bottom's y).
std::optional<Box> boxOf(const ServerObject& o)
{
    switch (o.shape) {
    case ShapeKind::Center:
        return Box{o.x, o.y, o.x, o.y};
    case ShapeKind::Circle:
        return Box{(X87(o.x) - o.radius).f(), (X87(o.y) - o.radius).f(), (X87(o.radius) + o.x).f(), (X87(o.radius) + o.y).f()};
    case ShapeKind::Box: {
        const auto c = boxCorners(o);
        return Box{(X87(c[1].first) + o.x).f(), (X87(c[0].second) + o.y).f(), (X87(c[2].first) + o.x).f(), (X87(c[3].second) + o.y).f()};
    }
    default:
        return std::nullopt;
    }
}

/// How far a circle at (px, py) is into a box (see boxDepth), as the exe leaves it on the stack.
X87 boxDepth53(const ServerObject& box, float px, float py, float radius, float& nx, float& ny, std::minstd_rand& rng);

/// Whether two objects push each other at all, for the kinds there are so far.
bool bodiesMeet(const World& world, const ServerObject& a, const ServerObject& b)
{
    const std::uint32_t ca = a.classes;
    const std::uint32_t cb = b.classes;
    const std::uint32_t fa = a.flags;
    const std::uint32_t fb = b.flags;
    // A telekinetic hand never meets a pentagram.
    if ((a.collideFn == NameId::PentagramCollide && b.typeName == NameId::TelekinesisHand) ||
        (b.collideFn == NameId::PentagramCollide && a.typeName == NameId::TelekinesisHand)) {
        return false;
    }
    // A door never meets a BELOW or ALLOW_OVERLAP thing (flags 9).
    if (((ca & 0x80) && (fb & 9)) || ((cb & 0x80) && (fa & 9))) {
        return false;
    }
    if (&a == &b || !a.collides || !b.collides || (fa & 0x60) || (fb & 0x60) || a.pendingDelete || b.pendingDelete) { // (0x20: ours' pendingDelete)
        return false;
    }
    // A flying monster and a transporter, hole or elevator: only one its path rides
    // (the monster's transits, the path finder's).
    const auto rides = [](const ServerObject& m, const ServerObject& t) {
        return m.monster && std::ranges::find(m.monster->transits, t.extent) != m.monster->transits.end();
    };
    if ((ca & 2) && (fa & 0x4000) && (cb & 0xCC00)) {
        return rides(a, b);
    }
    if ((cb & 2) && (fb & 0x4000) && (ca & 0xCC00)) {
        return rides(b, a);
    }
    // Fire always meets black powder.
    if (((ca & 0x2000) && b.typeName == NameId::BlackPowder) || ((cb & 0x2000) && a.typeName == NameId::BlackPowder)) {
        return true;
    }
    // Else they meet, unless: both ALLOW_OVERLAP; a door and an ALLOW_OVERLAP thing; something
    // BELOW (or flag 0x10) and something AIRBORNE or FALLING, when neither is a trigger; two doors,
    // two elevators or two shafts; or a NO_COLLIDE_OWNER thing (0x400) and its own side.
    const bool trigger = a.typeName == NameId::Trigger || b.typeName == NameId::Trigger;
    return !((fa & 8) && (fb & 8)) && !((ca & 0x80) && (fb & 8)) && !((cb & 0x80) && (fa & 8)) &&
           (trigger || (!((fa & 0x11) && (fb & 0x24000)) && !((fb & 0x11) && (fa & 0x24000)))) &&
           !((ca & 0x80) && (cb & 0x80)) && !((ca & 0x4000) && (cb & 0x4000)) &&
           !((ca & 0x8000) && (cb & 0x8000)) &&
           !((fa & 0x400) && sameSide(world, a, b)); // (NO_COLLIDE_OWNER: unitsHaveSameTeam)
}

struct Mover {
    World& world;
    std::vector<ServerObject*>& moving;
    std::minstd_rand& rng;
    std::uint32_t frame;
    std::vector<Contact>* contacts = nullptr;
    /// The contacts' pairs, either way round, in order (the world's list, emptied for the motion:
    /// World::contactPairs).
    std::vector<std::uint64_t>* paired = nullptr;

    static std::uint64_t pairKey(std::uint32_t a, std::uint32_t b)
    {
        return a < b ? (std::uint64_t{a} << 32) | b : (std::uint64_t{b} << 32) | a;
    }
    /// The pair noted; false when it was already.
    bool pair(std::uint32_t a, std::uint32_t b)
    {
        const std::uint64_t key = pairKey(a, b);
        const auto at = std::lower_bound(paired->begin(), paired->end(), key);
        if (at != paired->end() && *at == key) {
            return false;
        }
        paired->insert(at, key);
        return true;
    }

    /// collSysAddCollision: a pair once, either way round (each new one goes
    /// first in its list).
    void addContact(std::uint32_t a, std::uint32_t b, float nx, float ny)
    {
        if (!contacts || !pair(a, b)) {
            return;
        }
        contacts->push_back(Contact{a, b, nx, ny});
    }

    /// unitHasCollideOrUpdateFn: it joins the queue the steps are running over (once), if it can move at all.
    void join(ServerObject& o)
    {
        if (queueMotion(world, o)) {
            moving.push_back(&o);
        }
    }

    /// One at rest that a body touched wakes and joins.
    void wake(ServerObject& o)
    {
        if (o.flags & ObjFlag::Resting) {
            join(o);
            o.flags &= ~ObjFlag::Resting;
        }
    }

    /// A push on `o` this step (its push plus each part over its mass, one store each).
    static void addPush(ServerObject& o, float px, float py)
    {
        o.pushX = (X87(px) / o.mass + o.pushX).f();
        o.pushY = (X87(py) / o.mass + o.pushY).f();
    }

    /// Two circles (collisionCheckCircleCircle): `self` takes the push. On the x87 as the exe
    /// has it: the offsets, the distance, the overlap, the normal and the velocities' difference
    /// stored; 40 x the overlap kept on the stack, and the glancing pull's x side from the slide
    /// on the stack, its y side from the slide stored.
    void circles(ServerObject& self, ServerObject& other)
    {
        float dx = (X87(other.newX) - self.newX).f();
        float dy = (X87(other.newY) - self.newY).f();
        if (dx == 0 && dy == 0) { // one on the other: a nudge any of four ways
            constexpr std::array<std::pair<float, float>, 4> kNudge = {{{0.1f, 0.0f}, {-0.1f, 0.0f}, {0.0f, 0.1f}, {0.0f, -0.1f}}};
            std::tie(dx, dy) = kNudge[nox::util::uniform_int_distribution<int>(0, 3)(rng)];
        }
        const X87 dist53 = util::sqrt(X87(dy) * dy + X87(dx) * dx);
        const float dist = dist53 == X87(0.0f) ? 0.01f : dist53.f();
        const X87 overlap53 = X87(self.radius) + other.radius - dist;
        const float overlap = overlap53.f();
        if (!(X87(0.0f) < overlap53)) {
            return;
        }
        // Players, triggers and fires (0x2204) through a wall from each other don't meet.
        if ((self.classes & 0x2204) && (other.classes & 0x2204) &&
            !world.traceRay(self.x, self.y, other.x, other.y, 0)) {
            return;
        }
        addContact(other.extent, self.extent, dx, dy);
        const bool push = !(self.flags & ObjFlag::AllowOverlap) && !(other.flags & ObjFlag::AllowOverlap);
        // A creature isn't pushed by a NO_PUSH_CHARACTERS thing.
        if ((!(self.classes & 6) || !(other.flags & ObjFlag::NoPushCharacters)) && push) {
            const float nx = (X87(dx) / dist).f();
            const float ny = (X87(dy) / dist).f();
            const float dvx = (X87(self.vx) - other.vx).f();
            const float dvy = (X87(self.vy) - other.vy).f();
            const float lighter = other.mass <= self.mass ? other.mass : self.mass;
            const X87 f = X87(kBodyPush) * overlap;
            float px = (-(f * nx)).f();
            float py = (-(f * ny)).f();
            if (!(self.classes & 6) || !(other.classes & 6)) { // not two creatures: a glancing pull
                const X87 across = -X87(ny);
                const X87 slide53 = across * dvx + X87(dvy) * nx;
                const float slide = slide53.f();
                px = (X87(px) - slide53 * across * lighter * kGlance).f();
                py = (X87(py) - X87(slide) * lighter * nx * kGlance).f();
            }
            addPush(self, px, py);
        }
        wake(self);
        wake(other);
    }

    /// A circle and a box: the circle takes the push, or with `boxTakes` the box
    /// does, the other way.
    void circleBox(ServerObject& circle, ServerObject& box, bool boxTakes)
    {
        float nx = 0;
        float ny = 0;
        const float depth = boxDepth53(box, circle.newX, circle.newY, circle.radius, nx, ny, rng).f(); // (FST: its sign is the stack's)
        if (!(0 <= depth)) {
            return;
        }
        if ((circle.classes & 0x2204) && (box.classes & 0x2204) && !world.traceRay(circle.x, circle.y, box.x, box.y, 0)) {
            return;
        }
        addContact(circle.extent, box.extent, nx, ny);
        const bool push = !(circle.flags & ObjFlag::AllowOverlap) && !(box.flags & ObjFlag::AllowOverlap);
        if ((boxTakes || !(circle.classes & 6) || !(box.flags & ObjFlag::NoPushCharacters)) && push) {
            // (On the x87: 40 x the depth and the velocities' differences kept on the stack; the
            // slide stored, then times the lighter mass kept.)
            const X87 f = X87(kBodyPush) * depth;
            float px = (X87(nx) * f).f();
            float py = (X87(ny) * f).f();
            const float across = -ny;
            const X87 dvx = X87(circle.vx) - box.vx;
            const X87 dvy = X87(circle.vy) - box.vy;
            const float slide = (X87(across) * dvx + dvy * nx).f();
            const float lighter = box.mass <= circle.mass ? box.mass : circle.mass;
            const X87 ls = X87(lighter) * slide;
            px = (X87(px) - ls * across * kGlance).f();
            py = (X87(py) - ls * nx * kGlance).f();
            if (boxTakes) {
                addPush(box, -px, -py);
            } else {
                addPush(circle, px, py);
            }
        }
        wake(circle);
        wake(box);
    }

    /// A circle and a door's leaf (Obj_CollideDoorWithCircle): with `solid` the circle is pushed off it;
    /// without, the door (pushed on its own step) swings away from it unless locked.
    void doorCircle(ServerObject& door, ServerObject& o, bool solid)
    {
        const int at = (door.doorAngle + 0xA0) & 0xFF;
        const auto [ex, ey] = kDoorEdge[at];
        const float x0 = door.newX;
        const float y0 = door.newY;
        const float x1 = (X87::fromDouble(ex * 2) + x0).f();
        const float y1 = (X87::fromDouble(ey * 2) + y0).f();
        // The nearest point of the leaf (32 its length; the exe's helper, on the x87: the leaf's
        // x run stored, its y run kept for the dot product and stored for the offset, the dot
        // product kept, the x offset stored before adding).
        const X87 px = X87(o.newX) - x0;
        const X87 py = X87(o.newY) - y0;
        const float runX = (X87(x1) - x0).f();
        const X87 runY53 = X87(y1) - y0;
        const float runY = runY53.f();
        const X87 t = runY53 * py + X87(runX) * px;
        const X87 len2 = X87(32.0f) * 32.0f;
        const float offX = (t * runX / len2).f();
        float cx = (X87(offX) + x0).f();
        float cy = (t * runY / len2 + y0).f();
        cx = std::clamp(cx, std::min(x0, x1), std::max(x0, x1));
        cy = std::clamp(cy, std::min(y0, y1), std::max(y0, y1));
        // The distance: x's offset kept, y's stored (and squared as kept x stored).
        const X87 dx53 = X87(o.newX) - cx;
        const X87 dy53 = X87(o.newY) - cy;
        const float dy = dy53.f();
        const X87 dist53 = util::sqrt(dy53 * dy + dx53 * dx53);
        const float dist = dist53 == X87(0.0f) ? 0.1f : dist53.f();
        if (!(dist < o.radius)) {
            return;
        }
        const float nx = (dx53 / dist).f();
        const float ny = (X87(dy) / dist).f();
        addContact(o.extent, door.extent, nx, ny);
        door.doorTouched = frame;
        if (solid) {
            leafPush(o, (X87(o.radius) - dist).f(), nx, ny);
        }
        if (o.flags & ObjFlag::Resting) {
            if (!(o.flags & ObjFlag::AllowOverlap)) {
                join(o);
            }
            o.flags &= ~ObjFlag::Resting;
        }
        join(door);
        doorReached(door, o, solid, (X87(o.radius) - dist).f());
    }

    /// A door leaf's push on `o`, `depth` into it along (nx, ny) (Obj_CollideDoorWithCircle's and
    /// Obj_CollideDoorWithBox's): 100 x the depth plus a damping term against its speed into
    /// the leaf, all on the x87 (the speed into it stored), its parts stored once each.
    static void leafPush(ServerObject& o, float depth, float nx, float ny)
    {
        const float inward = (-(X87(ny) * o.vy) + -(X87(nx) * o.vx)).f();
        const X87 f = util::sqrt(X87(o.mass) * kWallPush * 4.0f) * inward * kDoorPushDamp + X87(kWallPush) * depth;
        addPush(o, (f * nx).f(), (f * ny).f());
    }

    /// A box's rectangle in the exe's turned frame (u = (x + y) / sqrt 2, v = (y - x) / sqrt 2),
    /// about its NewPos: left, top, right, bottom.
    static std::pair<float, float> turnedCentre(const ServerObject& o)
    {
        return {((X87(o.newY) + o.newX) * kSqrtHalf).f(), ((X87(o.newY) - o.newX) * kSqrtHalf).f()};
    }
    static std::array<float, 4> turnedRect(const ServerObject& o)
    {
        const auto [u, v] = turnedCentre(o);
        const float w = o.boxWidth * 0.5f; // (exact)
        const float h = o.boxHeight * 0.5f;
        return {(X87(u) - w).f(), (X87(v) - h).f(), (X87(w) + u).f(), (X87(h) + v).f()};
    }

    /// Where segment `s` (x0, y0, x1, y1) meets a horizontal edge at `y` from x0 to x1,
    /// or a vertical one at `x` from y0 to y1; `back`: the
    /// segment's parameter may run below 0.
    /// (On the x87: the differences kept on the stack, t and k stored.)
    static bool meetsRow(const std::array<float, 4>& s, float x0, float x1, float y, bool back, float& ox, float& oy)
    {
        const X87 d = X87(s[1]) - s[3];
        if (d == X87(0.0f)) {
            return false;
        }
        const float t = ((X87(y) - s[3]) / d).f();
        if (!((0 <= t || back) && t <= 1.0f)) {
            return false;
        }
        const X87 across = X87(x0) - x1;
        if (across == X87(0.0f)) {
            return false;
        }
        const float k = (((X87(1.0f) - t) * s[2] + X87(t) * s[0] - x1) / across).f();
        if (!(0 <= k && k <= 1.0f)) {
            return false;
        }
        ox = ((X87(1.0f) - k) * x1 + X87(k) * x0).f();
        oy = y;
        return true;
    }
    static bool meetsColumn(const std::array<float, 4>& s, float x, float y0, float y1, bool back, float& ox, float& oy)
    {
        const X87 d = X87(s[0]) - s[2];
        if (d == X87(0.0f)) {
            return false;
        }
        const float t = ((X87(x) - s[2]) / d).f();
        if (!((0 <= t || back) && t <= 1.0f)) {
            return false;
        }
        const X87 down = X87(y0) - y1;
        if (down == X87(0.0f)) {
            return false;
        }
        const float k = (((X87(1.0f) - t) * s[3] + X87(t) * s[1] - y1) / down).f();
        if (!(0 <= k && k <= 1.0f)) {
            return false;
        }
        ox = x;
        oy = ((X87(1.0f) - k) * y1 + X87(k) * y0).f();
        return true;
    }
    /// Up to `most` points where segment `s` meets rectangle `r`'s edges, top, right, bottom,
    /// left.
    static int meetsRect(const std::array<float, 4>& s, const std::array<float, 4>& r, int most, bool back, std::array<float, 4>& out)
    {
        int found = 0;
        for (int edge = 0; edge < 4 && found < most; ++edge) {
            float& ox = out[static_cast<std::size_t>(found * 2)];
            float& oy = out[static_cast<std::size_t>(found * 2 + 1)];
            const bool met = edge == 0   ? meetsRow(s, r[0], r[2], r[1], back, ox, oy)
                             : edge == 1 ? meetsColumn(s, r[2], r[1], r[3], back, ox, oy)
                             : edge == 2 ? meetsRow(s, r[0], r[2], r[3], back, ox, oy)
                                         : meetsColumn(s, r[0], r[1], r[3], back, ox, oy);
            if (met) {
                ++found;
            }
        }
        return found;
    }

    /// The reach of a door touched by `o` (Obj_CollideDoorWithCircle's and Obj_CollideDoorWithBox's tail): a team's shut gate
    /// holds the other teams ("GateLockedMechanism", a second apart); else, not solid, unlocked
    /// and under no other's magic lock, it swings away by `depth` x `o`'s mass on `o`'s side of
    /// it (the force and the side on the x87, unstored).
    void doorReached(ServerObject& door, ServerObject& o, bool solid, float depth)
    {
        const bool teamShut = door.team != 0 && door.doorDirection == door.doorClosed && !(o.team != 0 && o.team == door.team);
        if (!teamShut) {
            if (!solid && door.lock == 0 && (door.owner == 0 || door.owner == o.extent)) {
                const auto [ex, ey] = kDoorEdge[(door.doorAngle + 0xA0) & 0xFF];
                const X87 side = X87::fromDouble(ex) * (X87(o.newY) - door.newY) - X87::fromDouble(ey) * (X87(o.newX) - door.newX);
                const X87 force = X87(depth) * o.mass;
                door.doorSwing = (side <= X87(0.0f) ? X87(door.doorSwing) + force : X87(door.doorSwing) - force).f();
                door.doorSwinging = true;
                // (unitAddToUpdatable: swung only, it updates
                // till it's shut; a locked, solid or held one touched stays off the list.)
                world.addUpdatable(door);
            }
        } else if (door.timer < frame) {
            door.timer = frame + 30;
            if (o.player) {
                constexpr std::string_view id = "objcoll.c:GateLockedMechanism";
                // SrvNet_SendInformString's message: 0xA9 0x0F, sign 0, the string id, a NUL.
                world.broadcasts().push_back({informStringBytes(id), true, o.extent});
            }
        }
    }

    /// A box and a door's leaf (Obj_CollideDoorWithBox), worked in the turned frame:
    /// the leaf's segment there (its 32 along the direction table from the door's turned
    /// position, as the exe has it) meeting the box's rectangle; the box pushed out (`solid`) or
    /// the door swung.
    void doorBox(ServerObject& door, ServerObject& o, bool solid)
    {
        // (On the x87: the door's u kept on the stack for the leaf's far end, the rest stored.)
        const X87 du53 = (X87(door.newY) + door.newX) * kSqrtHalf;
        const float du = du53.f();
        const float dv = ((X87(door.newY) - door.newX) * kSqrtHalf).f();
        const auto [ou, ov] = turnedCentre(o);
        const std::array<float, 4> box = turnedRect(o);
        int dir = door.doorAngle + 0x80;  // the leaf points half round from its angle
        if (0xFF < dir) {
            dir = door.doorAngle - 0x80;
        }
        const auto [lx, ly] = directionVector(dir);
        const std::array<float, 4> leaf = {du, dv, (X87(lx) * 32.0f + du53).f(), (X87(ly) * 32.0f + dv).f()};
        const std::array<float, 4> span = {std::min(leaf[0], leaf[2]), std::min(leaf[1], leaf[3]), std::max(leaf[0], leaf[2]),
                                           std::max(leaf[1], leaf[3])};
        if (!(span[0] <= box[2] && span[1] <= box[3] && box[0] <= span[2] && box[1] <= span[3])) {
            return;
        }
        // Where the leaf is in the box: wholly, its middle; once across an edge,
        // half way to the end inside; twice, between the two.
        float hu = 0;
        float hv = 0;
        if (box[0] <= span[0] && span[2] <= box[2] && box[1] <= span[1] && span[3] <= box[3]) {
            hu = ((X87(leaf[2]) + leaf[0]) * 0.5f).f();
            hv = ((X87(leaf[3]) + leaf[1]) * 0.5f).f();
        } else {
            std::array<float, 4> at{};
            const int n = meetsRect(leaf, box, 2, false, at);
            if (n == 1) {
                const bool startIn = box[0] <= leaf[0] && leaf[0] <= box[2] && box[1] <= leaf[1] && leaf[1] <= box[3];
                hu = ((X87(at[0]) + (startIn ? leaf[0] : leaf[2])) * 0.5f).f();
                hv = ((X87(at[1]) + (startIn ? leaf[1] : leaf[3])) * 0.5f).f();
            } else if (n == 2) {
                hu = ((X87(at[2]) + at[0]) * 0.5f).f();
                hv = ((X87(at[3]) + at[1]) * 0.5f).f();
            } else {
                return;
            }
        }
        // (The u offset kept on the stack, the v one stored; the length's test on the stack.)
        const X87 nu53 = X87(ou) - hu;
        const X87 nv53 = X87(ov) - hv;
        const float nvStored = nv53.f();
        const X87 len53 = util::sqrt(nv53 * nvStored + nu53 * nu53);
        if (len53 == X87(0.0f)) {
            return;
        }
        const float len = len53.f();
        const float nu = (nu53 / len).f();
        const float nv = (X87(nvStored) / len).f();
        // From the box's centre through that point to its edge: how far in the leaf is.
        std::array<float, 4> edge{};
        if (meetsRect({ou, ov, hu, hv}, box, 1, true, edge) != 1) {
            return;
        }
        const X87 eu = X87(edge[0]) - ou;
        const X87 ev = X87(edge[1]) - ov;
        const X87 toEdge = util::sqrt(ev * ev + eu * eu); // (kept on the stack)
        if (toEdge == X87(0.0f)) {
            return;
        }
        const float nx = ((X87(nu) - nv) * kSqrtHalf).f(); // (back to the world's frame)
        const float ny = ((X87(nv) + nu) * kSqrtHalf).f();
        const X87 depth53 = toEdge - len;
        if (!(X87(0.0f) < depth53)) {
            return;
        }
        const float depth = depth53.f();
        addContact(o.extent, door.extent, nx, ny);
        door.doorTouched = frame;
        if (solid) {
            leafPush(o, depth, nx, ny);
        }
        if (o.flags & ObjFlag::Resting) {
            if (!(o.flags & ObjFlag::AllowOverlap)) {
                join(o);
            }
            o.flags &= ~ObjFlag::Resting;
        }
        join(door);
        doorReached(door, o, solid, depth);
    }

    /// Two boxes, in the turned frame: `self` takes the push, out along the axis
    /// they overlap least on, 40 x it.
    void boxes(ServerObject& self, ServerObject& other)
    {
        const std::array<float, 4> a = turnedRect(self);
        const std::array<float, 4> b = turnedRect(other);
        if (!(a[0] <= b[2] && a[1] <= b[3] && b[0] <= a[2] && b[1] <= a[3])) {
            return;
        }
        addContact(other.extent, self.extent, (X87(other.newX) - self.newX).f(), (X87(other.newY) - self.newY).f());
        const bool push = !(self.flags & ObjFlag::AllowOverlap) && !(other.flags & ObjFlag::AllowOverlap);
        if ((!(self.classes & 6) || !(other.flags & ObjFlag::NoPushCharacters)) && push) {  // classes 6: Monster | Player
            const float across = (X87(std::min(a[2], b[2])) - std::max(a[0], b[0])).f();
            const float down = (X87(std::min(a[3], b[3])) - std::max(a[1], b[1])).f();
            // (Which way: their turned centres, as computed for the rectangles.)
            const auto [su, sv] = turnedCentre(self);
            const auto [ou, ov] = turnedCentre(other);
            float pu = 0;
            float pv = 0;
            if (down <= across) {
                pv = sv < ov ? -down : down;
            } else {
                pu = su < ou ? -across : across;
            }
            // Back from the turned frame to the world's, 40 x the pushes kept on the stack.
            const X87 fu = X87(pu) * kBodyPush;
            const X87 fv = X87(pv) * kBodyPush;
            const float px = ((fu - fv) * kSqrtHalf).f();
            const float py = ((fu + fv) * kSqrtHalf).f();
            addPush(self, px, py);
        }
        wake(self);
        wake(other);
    }

    /// noxcpp's substeps: the walls' test at a place of our choosing that only measures (how far
    /// the circle is into the walls there, the deepest), moving and noting nothing.
    struct WallProbe {
        float x = 0;
        float y = 0;
        float deepest = -1;
    };

    /// A circle against the point of a wall diagonal nearest it: pushed out, its
    /// inward velocity taken off. (With a probe: only how far in it is, at the probe's place.)
    bool fromWall(ServerObject& o, float cx, float cy, WallProbe* probe = nullptr)
    {
        // On the x87 as the exe has it: the x offset kept on the stack, the y one stored (and
        // squared as kept times stored); the y normal's kept value in the speed into the wall; 100
        // x the depth kept.
        const X87 dx53 = X87(probe ? probe->x : o.newX) - cx;
        const X87 dy53 = X87(probe ? probe->y : o.newY) - cy;
        const float dy = dy53.f();
        const X87 dist53 = util::sqrt(dy53 * dy + dx53 * dx53);
        const float dist = dist53 == X87(0.0f) ? 0.01f : dist53.f();
        if (!(dist < o.radius)) {
            return false;
        }
        if (probe) {
            probe->deepest = std::max(probe->deepest, o.radius - dist);
            return true;
        }
        const float nx = (dx53 / dist).f();
        const X87 ny53 = X87(dy) / dist;
        const float ny = ny53.f();
        const X87 inward53 = -ny53 * o.vy + -X87(nx) * o.vx;
        const float inward = inward53.f();
        if (X87(0.0f) < inward53 && o.typeName != NameId::GameBall) {
            o.vx = (X87(inward) * nx + o.vx).f();
            o.vy = (X87(inward) * ny + o.vy).f();
        }
        const X87 f = (X87(o.radius) - dist) * kWallPush;
        addPush(o, (f * nx).f(), (f * ny).f());
        addContact(o.extent, kWallContact, nx, ny);
        return true;
    }

    /// A circle against a cell's wall. (With a probe: measured at the probe's place, nothing
    /// changed.)
    bool wallCell(ServerObject& o, int cx, int cy, WallProbe* probe = nullptr)
    {
        const float px = probe ? probe->x : o.newX;
        const float py = probe ? probe->y : o.newY;
        using namespace wall_shapes;
        // Windows count, but a small circle in the air goes through one.
        const std::uint8_t mode = !(o.flags & ObjFlag::Airborne) || 9.0f < o.radius ? 0x40 : 0;
        const int shape = world.wallShape(cx, cy, mode);
        if (shape < 0 || shape > 10) {
            return false;
        }
        const float ox = static_cast<float>(cx * 23);
        const float oy = static_cast<float>(cy * 23);
        const auto open = [&](int x, int y) { return world.wallShape(x, y, mode) < 0; };
        const auto& [back, slash] = kShapes[shape];
        const std::uint8_t next = kNeighbours[shape];
        bool roundStart = false; // (carried from the \ to the /, as the exe has it)
        bool roundEnd = false;
        bool touched = false;
        // Where along a diagonal it's nearest; past an end only where that end is rounded. (The
        // offset stays on the x87 to the nearest point's store.)
        const auto clampTo = [&](X87 t, const Segment& s) -> std::optional<X87> {
            if (t < X87(s.from)) {
                t = X87(s.from);
                if (!roundStart) {
                    return std::nullopt;
                }
            }
            if (X87(s.to) < t) {
                t = X87(s.to);
                if (!roundEnd) {
                    return std::nullopt;
                }
            }
            return t;
        };
        // An end is rounded (pushes round it) only where no wall carries on from it.
        if (back.present) {
            if ((next & 2) && open(cx - 1, cy - 1)) {
                roundStart = true;
            }
            if ((next & 4) && open(cx + 1, cy + 1)) {
                roundEnd = true;
            }
            // Shapes 7-10 are two half diagonals meeting at the centre: the ends there are rounded.
            if (shape == 7 || shape == 10) {
                roundEnd = true;
            } else if (shape == 8 || shape == 9) {
                roundStart = true;
            }
            // A player notes the wall (what a charge hits).
            // Its offset along the back diagonal (where it's nearest; the exe's 0.7071 twice).
            const X87 t = ((X87(px) - ox) + (X87(py) - oy)) * kRoot * kRoot;
            const auto at = clampTo(t, back);
            if (at && o.player && !probe) {
                o.player->lastWallX = cx;
                o.player->lastWallY = cy;
            }
            if (at && fromWall(o, (*at + ox).f(), (*at + oy).f(), probe)) {
                touched = true;
            }
        }
        if (slash.present) {
            if ((next & 8) && open(cx - 1, cy + 1)) {
                roundStart = true;
            }
            if ((next & 1) && open(cx + 1, cy - 1)) {
                roundEnd = true;
            }
            if (shape == 7 || shape == 8) {
                roundStart = true;
            } else if (shape == 9 || shape == 10) {
                roundEnd = true;
            }
            const X87 t = X87(kCell) - ((X87(py) - oy) * kRoot - (X87(px) - (X87(ox) + kCell)) * kRoot) * kRoot;
            const auto at = clampTo(t, slash);
            if (at && o.player && !probe) {
                o.player->lastWallX = cx;
                o.player->lastWallY = cy;
            }
            if (at && fromWall(o, (*at + ox).f(), ((X87(kCell) - *at) + oy).f(), probe)) {
                touched = true;
            }
        }
        return touched;
    }

    /// A creature against a shut secret wall that opens to the touch: it opens.
    void touchWall(const ServerObject& o, int cx, int cy)
    {
        WallCell* c = world.gridWall(cx, cy);
        if (c && c->secret && (o.classes & 6) && c->secretState == 1 && (c->secretFlags & 2)) {  // classes 6: Monster | Player
            c->secretState = 4;
            c->secretOpen = 0;
            // Its material's OPEN sound, else SecretWallOpen (wallFindOpenSound), at
            // the cell's middle (x * 23 + 11.5, y * 23 + 11.5: audCreate).
            const things::WallDef* def = world.thingDb && c->material < world.thingDb->walls.size() ? &world.thingDb->walls[c->material] : nullptr;
            const std::string_view name = def && !def->openSound.empty() ? std::string_view(def->openSound) : std::string_view("SecretWallOpen");
            playSoundAt(world, name, static_cast<float>(cx * 23) + 11.5f, static_cast<float>(cy * 23) + 11.5f);
        }
    }

    /// A box against one wall line (the box wall step's two line tests; `a` 0: an `across` line
    /// at u = from[0], 1: an `along` line at v = from[1]), all in the turned frame: `at` its
    /// NewPos, `prev` its PrevPos, `rect` its rectangle about `at`. Past the line since PrevPos,
    /// it's put back 2 short of it (NewPos with it). Overlapping the line, it takes 100 x its
    /// depth plus a damping term against its speed across, times the share of its side the line
    /// covers, its speed into the line's normal taken off and a 0.7 drag along it. The normal is
    /// always the line's positive one, its depth being never negative there, so from the low
    /// side the speed away from the wall is what's taken off (an original bug, kept). On the x87
    /// as the exe has it.
    bool boxWallLine(ServerObject& o, std::array<float, 2>& at, const std::array<float, 2>& prev, std::array<float, 4>& rect,
                     const std::array<float, 2>& from, float length, int a)
    {
        const int b = 1 - a;
        const auto ia = static_cast<std::size_t>(a);
        const auto ib = static_cast<std::size_t>(b);
        // The line's run against the rectangle's side (the line's end kept on the stack).
        const float start = from[ib] < rect[ib] ? rect[ib] : from[ib];
        const X87 end53 = X87(length) + from[ib];
        const float end = rect[ib + 2] < end53 ? rect[ib + 2] : end53.f();
        if (end53 < X87(start) || !(from[ib] <= end)) {
            return false;
        }
        X87 d53 = X87(at[ia]) - from[ia];
        const X87 p53 = X87(prev[ia]) - from[ia];
        const float p = p53.f();
        if (!(X87(0.0f) <= p53 * d53)) {
            at[ia] = (p < 0 ? X87(from[ia]) - 2.0f : X87(from[ia]) + 2.0f).f();
            o.newX = ((X87(at[0]) - at[1]) * kSqrtHalf).f();
            o.newY = ((X87(at[1]) + at[0]) * kSqrtHalf).f();
            const X87 half = X87(a == 0 ? o.boxWidth : o.boxHeight) * 0.5f;
            rect[ia] = (X87(at[ia]) - half).f();
            rect[ia + 2] = (half + at[ia]).f();
            d53 = X87(at[ia]) - from[ia];
        }
        if (from[ia] < rect[ia] || !(from[ia] <= rect[ia + 2])) {
            return false;
        }
        const float share = ((X87(end) - start) / (X87(rect[ib + 2]) - rect[ib])).f();
        const float across = a == 0 ? ((X87(o.vx) + o.vy) * kSqrtHalf).f() : ((X87(o.vy) - o.vx) * kSqrtHalf).f();
        float depth = 0;
        X87 spring53;
        if (!(X87(0.0f) <= d53)) {
            depth = (X87(rect[ia + 2]) - from[ia]).f();
            spring53 = -(X87(kWallPush) * depth);
        } else {
            depth = (X87(from[ia]) - rect[ia]).f();
            spring53 = X87(kWallPush) * depth;
        }
        const float spring = spring53.f();
        const X87 f = (util::sqrt(X87(o.mass) * kWallPush * 4.0f) * -X87(across) * 0.5f + spring) * share;
        float px = 0;
        float py = 0;
        float nx = 0;
        float ny = 0;
        const bool low = !(0.0f <= depth);
        if (a == 0) {
            px = py = (f * kSqrtHalf).f();
            nx = ny = low ? -kSqrtHalf : kSqrtHalf;
        } else {
            px = (f * -kSqrtHalf).f();
            py = (f * kSqrtHalf).f();
            nx = low ? kSqrtHalf : -kSqrtHalf;
            ny = -nx;
        }
        const X87 into = X87(nx) * o.vx + X87(ny) * o.vy;
        if (!(X87(0.0f) <= into)) {
            o.vx = (X87(o.vx) - into * nx).f();
            o.vy = (X87(o.vy) - into * ny).f();
        }
        const float tx = -ny;
        const float slide = (X87(tx) * o.vx + X87(nx) * o.vy).f();
        px = (X87(px) - X87(o.mass) * slide * tx * kGlance).f();
        py = (X87(py) - X87(o.mass) * slide * nx * kGlance).f();
        addPush(o, px, py);
        addContact(o.extent, kWallContact, px, py); // (the push for its normal)
        return true;
    }

    /// A box against a cell's wall (the box wall step's cell test): the wall's shape as a box
    /// meets it (windows count), its two lines (kBoxWalls) in turn, the second seeing what the
    /// first did to NewPos and the rectangle.
    bool boxWallCell(ServerObject& o, int cx, int cy)
    {
        const int shape = world.wallShape(cx, cy, 0x40);
        if (shape < 0 || shape > 10) {
            return false;
        }
        const float fx = static_cast<float>(cx * 23);
        const float fy = static_cast<float>(cy * 23);
        std::array<float, 2> at{};
        std::tie(at[0], at[1]) = turnedCentre(o);
        const std::array<float, 2> prev = {((X87(o.lastY) + o.lastX) * kSqrtHalf).f(), ((X87(o.lastY) - o.lastX) * kSqrtHalf).f()};
        // (The half width kept on the stack, the half height stored.)
        const X87 halfW = X87(o.boxWidth) * 0.5f;
        const float halfH = (X87(o.boxHeight) * 0.5f).f();
        std::array<float, 4> rect = {(X87(at[0]) - halfW).f(), (X87(at[1]) - halfH).f(), (halfW + at[0]).f(), (X87(halfH) + at[1]).f()};
        const float cu = ((X87(fy) + fx) * kSqrtHalf).f();
        const float cv = ((X87(fy) - fx) * kSqrtHalf).f();
        // Which quarter of the cell its centre is in (the u offset stored, the v one kept).
        const bool lowU = !((X87(at[0]) - cu).f() > kCellHalfDiag);
        const bool highV = X87(0.0f) < X87(at[1]) - cv;
        const std::uint8_t quarter = lowU ? (highV ? 8 : 1) : (highV ? 4 : 2);
        const auto& [across, along] = kBoxWalls[static_cast<std::size_t>(shape)];
        bool touched = false;
        if (across.present && !(across.mask & quarter)) {
            const std::array<float, 2> from = {(X87(cu) + kCellHalfDiag).f(), ((X87(cv) - kCellHalfDiag) + across.offset).f()};
            touched = boxWallLine(o, at, prev, rect, from, across.length, 0);
        }
        if (along.present && !(along.mask & quarter)) {
            const std::array<float, 2> from = {(X87(cu) + along.offset).f(), cv};
            touched = boxWallLine(o, at, prev, rect, from, along.length, 1) || touched;
        }
        return touched;
    }

    /// Fists and meteors pass elevators and shafts by (the exe's test, by type).
    static bool passesElevators(const ServerObject& o)
    {
        return o.typeName == NameId::SmallFist || o.typeName == NameId::MediumFist || o.typeName == NameId::LargeFist || o.typeName == NameId::Meteor;
    }

    static void raiseTo(ServerObject& o, float z)
    {
        o.raise(z); // (unitRaise)
    }

    /// The elevator's contact (with its a3 set): within 10 of its height and over it, `o` stands on it
    /// (ON_OBJECT, its height + 4, not falling); lower, it's a box in the way.
    void elevatorContact(ServerObject& elevator, ServerObject& o)
    {
        if (passesElevators(o)) {
            return;
        }
        const auto level = static_cast<float>(elevator.elevatorLevel);
        if (10.0f < std::fabs((X87(o.z) - level).f())) { // (the difference stored for the float fabs)
            if (o.z < level) {
                if (o.shape == ShapeKind::Circle) {
                    circleBox(o, elevator, false);
                } else if (o.shape == ShapeKind::Box) {
                    boxes(o, elevator);
                }
            }
        } else if (insideBox(elevator, elevator.newX, elevator.newY, o.newX, o.newY)) {
            o.flags = (o.flags & ~ObjFlag::InHole) | ObjFlag::OnObject;
            raiseTo(o, level + 4.0f);
            o.zVel = 0;
        }
    }

    /// What fits its opening and is over it stands on its elevator's top (64 below
    /// the elevator's height) when within 10 of it; else, with the elevator 10 or more below
    /// the floor, it falls in (IN_HOLE: drawn to the shaft, out at the elevator).
    void shaftContact(ServerObject& shaft, ServerObject& o)
    {
        const ServerObject* elevator = shaft.elevatorLink ? world.byExtent(shaft.elevatorLink) : nullptr;
        if (!elevator || passesElevators(o)) {
            return;
        }
        if (o.shape == ShapeKind::Box) {
            if (shaft.boxWidth < o.boxWidth || shaft.boxHeight < o.boxHeight) {
                return;
            }
        } else if (o.shape == ShapeKind::Circle) {
            const float across = o.radius + o.radius;
            if (shaft.boxWidth < across || shaft.boxHeight < across) {
                return;
            }
        }
        if (!insideBox(shaft, shaft.newX, shaft.newY, o.newX, o.newY)) {
            return;
        }
        const auto top = static_cast<float>(elevator->elevatorLevel - 0x40);
        if (std::fabs((X87(o.z) - top).f()) <= 10.0f) {
            o.flags = (o.flags & ~ObjFlag::InHole) | ObjFlag::OnObject;
            raiseTo(o, top + 4.0f);
            o.zVel = 0;
            return;
        }
        if (top <= -10.0f) {
            o.flags |= ObjFlag::InHole;
            o.homeX = shaft.newX;
            o.homeY = shaft.newY;
            o.fallToX = elevator->newX;
            o.fallToY = elevator->newY;
        }
    }

    /// A mover's pushes this step.
    void gather(ServerObject& o)
    {
        o.pushX = 0;
        o.pushY = 0;
        if ((o.flags & 0x60) || o.pendingDelete) { // (0x20: ours' pendingDelete)
            return;
        }
        // Its collision box as the grid has it (CollideP1 / P2: about its NewPos when it was last
        // linked, each step it moves; a shapeless thing's never set).
        const MapGrid::Rect* const linkedBox = world.grid().box(o);
        const MapGrid::Rect box = linkedBox ? *linkedBox : World::collideBoxAt(o, o.newX, o.newY);
        // The bodies whose boxes meet its own (getUnitsInRect), in the
        // grid's order.
        world.unitsInRect(box.left, box.top, box.right, box.bottom, [&](ServerObject& other) {
            if (&other == &o || !bodiesMeet(world, o, other)) {
                return;
            }
            // (the grid's body first.) An elevator holds up what's on it, only as the
            // body met; a shaft holds or swallows what's over it, either way round.
            if (o.classes & 0x4000u) {  // an elevator
                return;
            }
            if (other.classes & 0x4000u) {
                elevatorContact(other, o);
                return;
            }
            if (o.classes & 0x8000u) {  // an elevator shaft
                shaftContact(o, other);
                return;
            }
            if (other.classes & 0x8000u) {
                shaftContact(other, o);
                return;
            }
            const bool circle = o.shape == ShapeKind::Circle;
            if (!(o.classes & things::ObjectClass::Door)) {
                if (circle && (other.classes & things::ObjectClass::Door)) {
                    doorCircle(other, o, true);
                } else if (circle && other.shape == ShapeKind::Circle) {
                    circles(o, other);
                } else if (circle && other.shape == ShapeKind::Box) {
                    circleBox(o, other, false);
                } else if (o.shape == ShapeKind::Box && (other.classes & things::ObjectClass::Door)) {
                    doorBox(other, o, true);
                } else if (o.shape == ShapeKind::Box && other.shape == ShapeKind::Circle) {
                    circleBox(other, o, true);
                } else if (o.shape == ShapeKind::Box && other.shape == ShapeKind::Box) {
                    boxes(o, other);
                }
            } else if (other.shape == ShapeKind::Circle) {
                doorCircle(o, other, false);
            } else if (other.shape == ShapeKind::Box) {
                doorBox(o, other, false);
            }
        });
        // The walls in its cells (Math_FloatToInt of each edge x 1/23: stored, then truncated), a
        // circle's or a box's (not IMMOBILE), row by row.
        const bool circle = o.shape == ShapeKind::Circle;
        if (!(o.flags & ObjFlag::AllowOverlap) && (circle || o.shape == ShapeKind::Box) && !(o.classes & 0x400000u)) {
            const int x0 = wallCellOf(box.left);
            const int y0 = wallCellOf(box.top);
            const int x1 = wallCellOf(box.right);
            const int y1 = wallCellOf(box.bottom);
            if (!circle && boxWalls(o, x0, y0, x1, y1)) {
                return;
            }
            for (int cy = y0; cy <= y1; ++cy) {
                for (int cx = x0; cx <= x1; ++cx) {
                    if (circle ? wallCell(o, cx, cy) : boxWallCell(o, cx, cy)) {
                        touchWall(o, cx, cy);
                    }
                }
            }
        }
    }

    /// noxcpp's (not the exe's): the box wall step's cells, rows outer as the exe's loop takes
    /// them, but only those the walls' map holds (a cell without a wall does nothing): found
    /// column by column (a search a column, not one a cell), then run in the exe's order. False
    /// (nothing done) for a box over more cells holding walls than it keeps room for.
    bool boxWalls(ServerObject& o, int x0, int y0, int x1, int y1)
    {
        std::array<std::pair<int, int>, 64> found{}; // (y, x)
        std::size_t n = 0;
        const auto& walls = world.walls();
        for (int cx = x0; cx <= x1; ++cx) {
            for (auto it = walls.lower_bound({cx, y0}); it != walls.end() && it->first.first == cx && it->first.second <= y1; ++it) {
                if (n == found.size()) {
                    return false;
                }
                found[n++] = {it->first.second, cx};
            }
        }
        std::sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const auto [cy, cx] = found[i];
            if (boxWallCell(o, cx, cy)) {
                touchWall(o, cx, cy);
            }
        }
        return true;
    }

    /// Its velocity and position this step (updateObjectsVelocity).
    void integrate(ServerObject& o)
    {
        // NO_UPDATE, or a monster waiting under the host's cursor (DEPENDENCY_UNDER_CURSOR on its
        // stack): no push, no velocity.
        if ((o.flags & ObjFlag::NoUpdate) || (o.monster && Monsters::has(o, Dependency::UnderCursor))) {
            o.pushX = o.pushY = 0;
            o.vx = o.vy = 0;
            return;
        }
        // On the x87 as the exe has it (chop mode, 53 bits): the x push stays on the stack, the y
        // push is stored to a float first, and so is the y side's (push - drag x v). Held, frozen
        // or charmed, its own force counts for nothing: the push alone.
        using util::X87;
        const bool forceOff = enchanted(o, Enchant::Held) || enchanted(o, Enchant::Freeze) || enchanted(o, Enchant::Charming);
        const X87 ax = forceOff ? X87(o.pushX) : X87(o.pushX) + o.forceX;
        const float ay = forceOff ? o.pushY : (X87(o.pushY) + o.forceY).f();
        const X87 sx = ax - X87(o.drag) * o.vx;
        const float sy = (X87(ay) - X87(o.vy) * o.drag).f();
        o.vx = (sx * kStep + o.vx).f();
        o.vy = (X87(sy) * kStep + o.vy).f();
        const float tx = (X87(kStep) * o.vx + o.newX).f();
        const float ty = (X87(kStep) * o.vy + o.newY).f();
        std::uint8_t flags = static_cast<std::uint8_t>(((o.flags >> 12) & Trace::ThroughWindows) | Trace::Doors);  // AIRBORNE (0x4000) >> 12 is ThroughWindows (4)
        if (o.typeName.has(NameTrait::FlameCleanse)) { // (its name ends with "FlameCleanse")
            flags |= Trace::SightOnly; // (the cleansing flames, by the exe's list: only sight-blocking walls stop them)
        }
        if (world.traceRay(o.newX, o.newY, tx, ty, flags)) {
            o.newX = tx;
            o.newY = ty;
        }
        // On a harming floor (FLOR 6; not AIRBORNE, with health): a touch of the floor (the exe's
        // 6 in place of an object), once a frame like any pair: 2 of lava (collide.cpp).
        if (!(o.flags & ObjFlag::Airborne) && (o.maxHealth != 0 || o.player) && world.floorAt(o.newX, o.newY) == 6) {
            addContact(o.extent, kFloorContact, 0, 0);
        }
        // Moved more than 0.01 from where it set out: synced, and relinked in the grid at
        // its NewPos (objectUnkUpdateCoords, moveUpdateSpecial).
        if (X87(kStill) < util::fabs(X87(o.newX) - o.x) || X87(kStill) < util::fabs(X87(o.newY) - o.y)) {
            o.touch();
            world.gridRelink(o, o.newX, o.newY);
        }
    }
};

/// How far a circle at (px, py) is into a box, and which way out: past one of
/// the box's sides, straight out of it; past two, out of their corner; inside, away from
/// the centre (the depth then being the distance from it). Negative: not touching. On the x87 as
/// the exe has it: each side's test stored (times its 0.7071); the depth left on the stack; a
/// corner's y offset kept for its square, the distance stored; inside, the normal from the
/// distance kept.
X87 boxDepth53(const ServerObject& box, float px, float py, float radius, float& nx, float& ny, std::minstd_rand& rng)
{
    const auto c = boxCorners(box);
    const auto at = [&](int i) { return std::pair{(X87(c[i].first) + box.newX).f(), (X87(c[i].second) + box.newY).f()}; };
    const auto [tx, ty] = at(0);
    const auto [lx, ly] = at(1);
    const auto [rx, ry] = at(2);
    const auto [bx, by] = at(3);
    const float upRight = ((X87(ty) - tx + px - py) * kRoot).f();  // > 0: past the top-right side
    const float downLeft = ((X87(ly) - lx + px - py) * kRoot).f(); // < 0: past the bottom-left one
    const float downRight = ((X87(ry) + rx - px - py) * kRoot).f(); // < 0: past the bottom-right one
    const float upLeft = ((X87(ty) + tx - px - py) * kRoot).f();   // > 0: past the top-left one
    // Bits: 1 past the top-right side, 2 the bottom-left, 4 the top-left, 8 the bottom-right;
    // two of them: past their corner.
    int side = 0;
    if (0 < upRight) {
        side = 1;
    } else if (downLeft < 0) {
        side = 2;
    }
    if (downRight < 0) {
        side |= 8;
    } else if (0 < upLeft) {
        side |= 4;
    }
    const auto fromCorner = [&](float cx, float cy) {
        const float dx = (X87(px) - cx).f();
        const X87 dy53 = X87(py) - cy;
        const float dy = dy53.f();
        const X87 d53 = util::sqrt(dy53 * dy + X87(dx) * dx);
        const float d = d53 == X87(0.0f) ? 0.1f : d53.f();
        const X87 depth = X87(radius) - d;
        if (X87(0.0f) <= depth) {
            nx = (X87(dx) / d).f();
            ny = (X87(dy) / d).f();
        }
        return depth;
    };
    switch (side) {
    case 0: { // inside
        float dx = (X87(px) - box.newX).f();
        float dy = (X87(py) - box.newY).f();
        if (dx == 0 && dy == 0) {
            constexpr std::array<std::pair<float, float>, 4> kNudge = {{{0.1f, 0.0f}, {-0.1f, 0.0f}, {0.0f, 0.1f}, {0.0f, -0.1f}}};
            std::tie(dx, dy) = kNudge[nox::util::uniform_int_distribution<int>(0, 3)(rng)];
        }
        X87 d = util::sqrt(X87(dy) * dy + X87(dx) * dx);
        if (d == X87(0.0f)) {
            d = X87(0.1f);
        }
        nx = (X87(dx) / d).f();
        ny = (X87(dy) / d).f();
        return d;
    }
    case 1:
        nx = kRoot;
        ny = -kRoot;
        return X87(radius) - upRight;
    case 2:
        nx = -kRoot;
        ny = kRoot;
        return X87(radius) - -downLeft;
    case 4:
        nx = -kRoot;
        ny = -kRoot;
        return X87(radius) - upLeft;
    case 8:
        nx = kRoot;
        ny = kRoot;
        return X87(radius) - -downRight;
    case 5:
        return fromCorner(tx, ty);
    case 6:
        return fromCorner(lx, ly);
    case 9:
        return fromCorner(rx, ry);
    case 10:
        return fromCorner(bx, by);
    default:
        return X87(-1.0f);
    }
}

} // namespace

float boxDepth(const ServerObject& box, float px, float py, float radius, float& nx, float& ny, std::minstd_rand& rng)
{
    return boxDepth53(box, px, py, radius, nx, ny, rng).f(); // (the exe's callers store it)
}

std::pair<int, int> sinCos16(int angle)
{
    return kDoorEdge[angle & 0xFF];
}

std::optional<std::array<float, 4>> collisionBox(const ServerObject& o)
{
    const auto b = boxOf(o);
    if (!b) {
        return std::nullopt;
    }
    return std::array<float, 4>{b->left, b->top, b->right, b->bottom};
}

int directionOf(float x, float y)
{
    // (atan2 + 2 pi) x 128 / pi, rounded (the exe's math): FPATAN's angle on the x87 at 53
    // bits (a double's atan2 here), the rest on the stack, stored, then truncated.
    const X87 a = X87::fromDouble(std::atan2(static_cast<double>(y), static_cast<double>(x)));
    return util::floatToInt((a + 6.2831855f) * 40.743664f + 0.5f) & 0xFF;
}

std::pair<float, float> directionVector(int direction)
{
    // (The exe's own floats: computing them comes out an ulp off here and there.)
    // 256 (cos, sin) pairs, as float bit patterns.
    static constexpr std::uint32_t kTable[512] = {
    0x3F800000u, 0x00000000u, 0x3F7FEC43u, 0x3CC90AB1u, 0x3F7FB10Fu, 0x3D48FB2Eu, 0x3F7F4E6Du, 0x3D96A904u,
    0x3F7EC46Du, 0x3DC8BD36u, 0x3F7E1323u, 0x3DFAB273u, 0x3F7D3AACu, 0x3E164083u, 0x3F7C3B28u, 0x3E2F10A2u,
    0x3F7B14BEu, 0x3E47C5C2u, 0x3F79C79Du, 0x3E605C13u, 0x3F7853F8u, 0x3E78CFCCu, 0x3F76BA07u, 0x3E888E93u,
    0x3F74FA0Bu, 0x3E94A032u, 0x3F731447u, 0x3EA09AE5u, 0x3F710908u, 0x3EAC7CD4u, 0x3F6ED89Eu, 0x3EB8442Au,
    0x3F6C835Eu, 0x3EC3EF15u, 0x3F6A09A7u, 0x3ECF7BCAu, 0x3F676BD8u, 0x3EDAE880u, 0x3F64AA59u, 0x3EE63375u,
    0x3F61C597u, 0x3EF15AEAu, 0x3F5EBE05u, 0x3EFC5D27u, 0x3F5B941Au, 0x3F039C3Du, 0x3F584853u, 0x3F08F59Bu,
    0x3F54DB31u, 0x3F0E39DAu, 0x3F514D3Du, 0x3F13682Au, 0x3F4D9F02u, 0x3F187FC0u, 0x3F49D112u, 0x3F1D7FD1u,
    0x3F45E403u, 0x3F226799u, 0x3F41D870u, 0x3F273656u, 0x3F3DAEF9u, 0x3F2BEB4Au, 0x3F396842u, 0x3F3085BBu,
    0x3F3504F3u, 0x3F3504F3u, 0x3F3085BBu, 0x3F396842u, 0x3F2BEB4Au, 0x3F3DAEF9u, 0x3F273656u, 0x3F41D870u,
    0x3F226799u, 0x3F45E403u, 0x3F1D7FD1u, 0x3F49D112u, 0x3F187FC0u, 0x3F4D9F02u, 0x3F13682Au, 0x3F514D3Du,
    0x3F0E39DAu, 0x3F54DB31u, 0x3F08F59Bu, 0x3F584853u, 0x3F039C3Du, 0x3F5B941Au, 0x3EFC5D27u, 0x3F5EBE05u,
    0x3EF15AEAu, 0x3F61C597u, 0x3EE63375u, 0x3F64AA59u, 0x3EDAE880u, 0x3F676BD8u, 0x3ECF7BCAu, 0x3F6A09A7u,
    0x3EC3EF15u, 0x3F6C835Eu, 0x3EB8442Au, 0x3F6ED89Eu, 0x3EAC7CD4u, 0x3F710908u, 0x3EA09AE5u, 0x3F731447u,
    0x3E94A032u, 0x3F74FA0Bu, 0x3E888E93u, 0x3F76BA07u, 0x3E78CFCCu, 0x3F7853F8u, 0x3E605C13u, 0x3F79C79Du,
    0x3E47C5C2u, 0x3F7B14BEu, 0x3E2F10A2u, 0x3F7C3B28u, 0x3E164083u, 0x3F7D3AACu, 0x3DFAB273u, 0x3F7E1323u,
    0x3DC8BD36u, 0x3F7EC46Du, 0x3D96A904u, 0x3F7F4E6Du, 0x3D48FB2Eu, 0x3F7FB10Fu, 0x3CC90AB1u, 0x3F7FEC43u,
    0x00000000u, 0x3F800000u, 0xBCC90AB1u, 0x3F7FEC43u, 0xBD48FB2Eu, 0x3F7FB10Fu, 0xBD96A904u, 0x3F7F4E6Du,
    0xBDC8BD36u, 0x3F7EC46Du, 0xBDFAB273u, 0x3F7E1323u, 0xBE164083u, 0x3F7D3AACu, 0xBE2F10A2u, 0x3F7C3B28u,
    0xBE47C5C2u, 0x3F7B14BEu, 0xBE605C13u, 0x3F79C79Du, 0xBE78CFCCu, 0x3F7853F8u, 0xBE888E93u, 0x3F76BA07u,
    0xBE94A032u, 0x3F74FA0Bu, 0xBEA09AE5u, 0x3F731447u, 0xBEAC7CD4u, 0x3F710908u, 0xBEB8442Au, 0x3F6ED89Eu,
    0xBEC3EF15u, 0x3F6C835Eu, 0xBECF7BCAu, 0x3F6A09A7u, 0xBEDAE880u, 0x3F676BD8u, 0xBEE63375u, 0x3F64AA59u,
    0xBEF15AEAu, 0x3F61C597u, 0xBEFC5D27u, 0x3F5EBE05u, 0xBF039C3Du, 0x3F5B941Au, 0xBF08F59Bu, 0x3F584853u,
    0xBF0E39DAu, 0x3F54DB31u, 0xBF13682Au, 0x3F514D3Du, 0xBF187FC0u, 0x3F4D9F02u, 0xBF1D7FD1u, 0x3F49D112u,
    0xBF226799u, 0x3F45E403u, 0xBF273656u, 0x3F41D870u, 0xBF2BEB4Au, 0x3F3DAEF9u, 0xBF3085BBu, 0x3F396842u,
    0xBF3504F3u, 0x3F3504F3u, 0xBF396842u, 0x3F3085BBu, 0xBF3DAEF9u, 0x3F2BEB4Au, 0xBF41D870u, 0x3F273656u,
    0xBF45E403u, 0x3F226799u, 0xBF49D112u, 0x3F1D7FD1u, 0xBF4D9F02u, 0x3F187FC0u, 0xBF514D3Du, 0x3F13682Au,
    0xBF54DB31u, 0x3F0E39DAu, 0xBF584853u, 0x3F08F59Bu, 0xBF5B941Au, 0x3F039C3Du, 0xBF5EBE05u, 0x3EFC5D27u,
    0xBF61C597u, 0x3EF15AEAu, 0xBF64AA59u, 0x3EE63375u, 0xBF676BD8u, 0x3EDAE880u, 0xBF6A09A7u, 0x3ECF7BCAu,
    0xBF6C835Eu, 0x3EC3EF15u, 0xBF6ED89Eu, 0x3EB8442Au, 0xBF710908u, 0x3EAC7CD4u, 0xBF731447u, 0x3EA09AE5u,
    0xBF74FA0Bu, 0x3E94A032u, 0xBF76BA07u, 0x3E888E93u, 0xBF7853F8u, 0x3E78CFCCu, 0xBF79C79Du, 0x3E605C13u,
    0xBF7B14BEu, 0x3E47C5C2u, 0xBF7C3B28u, 0x3E2F10A2u, 0xBF7D3AACu, 0x3E164083u, 0xBF7E1323u, 0x3DFAB273u,
    0xBF7EC46Du, 0x3DC8BD36u, 0xBF7F4E6Du, 0x3D96A904u, 0xBF7FB10Fu, 0x3D48FB2Eu, 0xBF7FEC43u, 0x3CC90AB1u,
    0xBF800000u, 0x00000000u, 0xBF7FEC43u, 0xBCC90AB1u, 0xBF7FB10Fu, 0xBD48FB2Eu, 0xBF7F4E6Du, 0xBD96A904u,
    0xBF7EC46Du, 0xBDC8BD36u, 0xBF7E1323u, 0xBDFAB273u, 0xBF7D3AACu, 0xBE164083u, 0xBF7C3B28u, 0xBE2F10A2u,
    0xBF7B14BEu, 0xBE47C5C2u, 0xBF79C79Du, 0xBE605C13u, 0xBF7853F8u, 0xBE78CFCCu, 0xBF76BA07u, 0xBE888E93u,
    0xBF74FA0Bu, 0xBE94A032u, 0xBF731447u, 0xBEA09AE5u, 0xBF710908u, 0xBEAC7CD4u, 0xBF6ED89Eu, 0xBEB8442Au,
    0xBF6C835Eu, 0xBEC3EF15u, 0xBF6A09A7u, 0xBECF7BCAu, 0xBF676BD8u, 0xBEDAE880u, 0xBF64AA59u, 0xBEE63375u,
    0xBF61C597u, 0xBEF15AEAu, 0xBF5EBE05u, 0xBEFC5D27u, 0xBF5B941Au, 0xBF039C3Du, 0xBF584853u, 0xBF08F59Bu,
    0xBF54DB31u, 0xBF0E39DAu, 0xBF514D3Du, 0xBF13682Au, 0xBF4D9F02u, 0xBF187FC0u, 0xBF49D112u, 0xBF1D7FD1u,
    0xBF45E403u, 0xBF226799u, 0xBF41D870u, 0xBF273656u, 0xBF3DAEF9u, 0xBF2BEB4Au, 0xBF396842u, 0xBF3085BBu,
    0xBF3504F3u, 0xBF3504F3u, 0xBF3085BBu, 0xBF396842u, 0xBF2BEB4Au, 0xBF3DAEF9u, 0xBF273656u, 0xBF41D870u,
    0xBF226799u, 0xBF45E403u, 0xBF1D7FD1u, 0xBF49D112u, 0xBF187FC0u, 0xBF4D9F02u, 0xBF13682Au, 0xBF514D3Du,
    0xBF0E39DAu, 0xBF54DB31u, 0xBF08F59Bu, 0xBF584853u, 0xBF039C3Du, 0xBF5B941Au, 0xBEFC5D27u, 0xBF5EBE05u,
    0xBEF15AEAu, 0xBF61C597u, 0xBEE63375u, 0xBF64AA59u, 0xBEDAE880u, 0xBF676BD8u, 0xBECF7BCAu, 0xBF6A09A7u,
    0xBEC3EF15u, 0xBF6C835Eu, 0xBEB8442Au, 0xBF6ED89Eu, 0xBEAC7CD4u, 0xBF710908u, 0xBEA09AE5u, 0xBF731447u,
    0xBE94A032u, 0xBF74FA0Bu, 0xBE888E93u, 0xBF76BA07u, 0xBE78CFCCu, 0xBF7853F8u, 0xBE605C13u, 0xBF79C79Du,
    0xBE47C5C2u, 0xBF7B14BEu, 0xBE2F10A2u, 0xBF7C3B28u, 0xBE164083u, 0xBF7D3AACu, 0xBDFAB273u, 0xBF7E1323u,
    0xBDC8BD36u, 0xBF7EC46Du, 0xBD96A904u, 0xBF7F4E6Du, 0xBD48FB2Eu, 0xBF7FB10Fu, 0xBCC90AB1u, 0xBF7FEC43u,
    0x00000000u, 0xBF800000u, 0x3CC90AB1u, 0xBF7FEC43u, 0x3D48FB2Eu, 0xBF7FB10Fu, 0x3D96A904u, 0xBF7F4E6Du,
    0x3DC8BD36u, 0xBF7EC46Du, 0x3DFAB273u, 0xBF7E1323u, 0x3E164083u, 0xBF7D3AACu, 0x3E2F10A2u, 0xBF7C3B28u,
    0x3E47C5C2u, 0xBF7B14BEu, 0x3E605C13u, 0xBF79C79Du, 0x3E78CFCCu, 0xBF7853F8u, 0x3E888E93u, 0xBF76BA07u,
    0x3E94A032u, 0xBF74FA0Bu, 0x3EA09AE5u, 0xBF731447u, 0x3EAC7CD4u, 0xBF710908u, 0x3EB8442Au, 0xBF6ED89Eu,
    0x3EC3EF15u, 0xBF6C835Eu, 0x3ECF7BCAu, 0xBF6A09A7u, 0x3EDAE880u, 0xBF676BD8u, 0x3EE63375u, 0xBF64AA59u,
    0x3EF15AEAu, 0xBF61C597u, 0x3EFC5D27u, 0xBF5EBE05u, 0x3F039C3Du, 0xBF5B941Au, 0x3F08F59Bu, 0xBF584853u,
    0x3F0E39DAu, 0xBF54DB31u, 0x3F13682Au, 0xBF514D3Du, 0x3F187FC0u, 0xBF4D9F02u, 0x3F1D7FD1u, 0xBF49D112u,
    0x3F226799u, 0xBF45E403u, 0x3F273656u, 0xBF41D870u, 0x3F2BEB4Au, 0xBF3DAEF9u, 0x3F3085BBu, 0xBF396842u,
    0x3F3504F3u, 0xBF3504F3u, 0x3F396842u, 0xBF3085BBu, 0x3F3DAEF9u, 0xBF2BEB4Au, 0x3F41D870u, 0xBF273656u,
    0x3F45E403u, 0xBF226799u, 0x3F49D112u, 0xBF1D7FD1u, 0x3F4D9F02u, 0xBF187FC0u, 0x3F514D3Du, 0xBF13682Au,
    0x3F54DB31u, 0xBF0E39DAu, 0x3F584853u, 0xBF08F59Bu, 0x3F5B941Au, 0xBF039C3Du, 0x3F5EBE05u, 0xBEFC5D27u,
    0x3F61C597u, 0xBEF15AEAu, 0x3F64AA59u, 0xBEE63375u, 0x3F676BD8u, 0xBEDAE880u, 0x3F6A09A7u, 0xBECF7BCAu,
    0x3F6C835Eu, 0xBEC3EF15u, 0x3F6ED89Eu, 0xBEB8442Au, 0x3F710908u, 0xBEAC7CD4u, 0x3F731447u, 0xBEA09AE5u,
    0x3F74FA0Bu, 0xBE94A032u, 0x3F76BA07u, 0xBE888E93u, 0x3F7853F8u, 0xBE78CFCCu, 0x3F79C79Du, 0xBE605C13u,
    0x3F7B14BEu, 0xBE47C5C2u, 0x3F7C3B28u, 0xBE2F10A2u, 0x3F7D3AACu, 0xBE164083u, 0x3F7E1323u, 0xBDFAB273u,
    0x3F7EC46Du, 0xBDC8BD36u, 0x3F7F4E6Du, 0xBD96A904u, 0x3F7FB10Fu, 0xBD48FB2Eu, 0x3F7FEC43u, 0xBCC90AB1u,
    };
    const int i = (direction & 0xFF) * 2;
    return {std::bit_cast<float>(kTable[i]), std::bit_cast<float>(kTable[i + 1])};
}

void applyForce(World& world, ServerObject& o, float x, float y, float force)
{
    if ((o.flags & 0x8068u) || (o.classes & 0x400000u)) {  // DEAD, DESTROYED, NO_COLLIDE, ALLOW_OVERLAP; or IMMOBILE
        return;
    }
    // On the x87 as the exe has it: the offsets and the distance stored (the y offset squared as
    // kept times stored), 10 x the force stored, over the mass kept.
    const float dx = (X87(o.x) - x).f();
    const X87 dy53 = X87(o.y) - y;
    const float dy = dy53.f();
    const float d = (util::sqrt(dy53 * dy + X87(dx) * dx) + 0.1f).f();
    const X87 f = X87((X87(force) * 10.0f).f()) / o.mass;
    o.forceX = (X87(dx) * f / d + o.forceX).f();
    o.forceY = (X87(dy) * f / d + o.forceY).f();
    if (!(o.classes & things::ObjectClass::Missile)) {
        queueMotion(world, o);
    }
}

bool queueMotion(World& world, ServerObject& o)
{
    if (o.inMotion || o.pendingDelete || !joinsMotion(o)) {
        return false;
    }
    if (o.classes & 0x2008u) { // (an obstacle or a fire)
        world.pathMarksStand = false;
    }
    o.inMotion = true;
    o.motionTicket = ++world.motionTicket;
    world.motionQueue.emplace_back(o.extent, o.motionTicket);
    return true;
}

void settleOrQueue(World& world, ServerObject& o)
{
    // (The moves stored, then their fabs.)
    if (astir(o.forceX) || astir(o.forceY) || astir(o.vx) || astir(o.vy) || astir((X87(o.newX) - o.x).f()) ||
        astir((X87(o.newY) - o.y).f()) || astir(o.z) || astir(o.zVel)) {
        queueMotion(world, o);
        o.flags &= ~ObjFlag::Resting;
        return;
    }
    o.forceX = o.forceY = 0;
    o.vx = o.vy = 0;
    o.raise(0.0f); // (unitRaise)
    o.newX = o.x;
    o.newY = o.y;
    o.zVel = 0;
    o.flags |= ObjFlag::Resting;
}

void queueNewObjects(World& world)
{
    // (No walk is under way: the grid's given-back entries are free again.)
    world.grid().recycle();
    for (const std::uint32_t e : world.takeNew()) {
        const auto it = world.objects().find(e);
        if (it == world.objects().end()) {
            continue; // (deleted and freed meanwhile)
        }
        ServerObject& o = it->second;
        o.fresh = false;
        if (o.holder != 0 || o.gone() || o.pendingDelete) {
            continue; // (in an inventory since: never on the map)
        }
        // A missile at its list's head; anything else with an UPDATE or a velocity, not a door,
        // at the updatable list's (unitAddToUpdatable); then the grid;
        // then one with a COLLIDE joins the motion (unitHasCollideOrUpdateFn
        // asks the rest).
        if (o.classes & things::ObjectClass::Missile) {
            world.grid().addMissile(o);
        } else if ((o.updates || o.vx != 0 || o.vy != 0) && !(o.classes & things::ObjectClass::Door)) {
            world.addUpdatable(o);
        }
        world.gridLink(o, o.x, o.y);
        if (o.collides && !(o.classes & things::ObjectClass::Missile)) {
            queueMotion(world, o);
        }
    }
}

bool joinsMotion(const ServerObject& o)
{
    if (o.gone() || o.holder != 0 || !(o.updates || (o.collides && !(o.flags & ObjFlag::NoCollide)))) {
        return false;
    }
    if (o.alwaysMoves) {
        return true; // (put on the updatable list by hand)
    }
    if (o.classes & things::ObjectClass::Missile) {
        return false; // (missiles fly on their own: missiles.hpp)
    }
    // IMMOBILE or ALLOW_OVERLAP: only doors and elevators (0xE080), the fist, the undead killer
    // and the spikes.
    if ((o.classes & 0x400000u) || (o.flags & ObjFlag::AllowOverlap)) {
        return (o.classes & 0xE080u) != 0 || o.collideFn == NameId::FistCollide || o.collideFn == NameId::UndeadKillerCollide ||
               o.typeName == NameId::Spike || o.typeName == NameId::PeriodicSpike;
    }
    return true;
}

void updateDoor(World& world, ServerObject& door, std::uint32_t frame)
{
    // updateDoor: leaving its shut angle it sounds opening, coming back to it
    // shutting: a gate (subclass 4) by its material (WOOD 8: 0xF5 / 0xF6, else 0xF1 / 0xF3),
    // else subclass 1 0xF7 / 0xF8, subclass 0x1000 0x3F6 / 0x3F7, any other 0xED / 0xEF.
    const auto sound = [&](bool opening) {
        std::uint32_t id = 0;
        if (door.subclasses & 4u) {
            id = (door.material & 8u) ? (opening ? 0xF5u : 0xF6u) : (opening ? 0xF1u : 0xF3u);
        } else if (door.subclasses & 1u) {
            id = opening ? 0xF7u : 0xF8u;
        } else if (door.subclasses & 0x1000u) {
            id = opening ? 0x3F6u : 0x3F7u;
        } else {
            id = opening ? 0xEDu : 0xEFu;
        }
        playSound(world, id, door);
    };
    if (door.doorPrevious == door.doorClosed) {
        if (door.doorDirection != door.doorClosed) {
            sound(true);
        }
    } else if (door.doorDirection == door.doorClosed) {
        sound(false);
        world.removeUpdatable(door); // (unitRemoveFromUpdatable: shut, it updates no more)
    }
    if (door.doorDirection != door.doorPrevious) {
        door.touch(); // (unitNeedSync: clients get its angle)
        door.doorPrevious = door.doorDirection;
    }
    // Half a second untouched and enabled, it swings back shut by 2 a frame, and joins
    // the motion (unitHasCollideOrUpdateFn).
    if ((door.flags & ObjFlag::Enabled) && 15u < frame - door.doorTouched && door.doorDirection != door.doorClosed) {
        // Back towards shut the shorter way round (32 directions).
        int off = door.doorDirection - door.doorClosed;
        if (off < 0) {
            off += 32;
        }
        door.doorAngle = static_cast<std::int16_t>((door.doorAngle + (off < 16 ? -2 : 2) + 256) % 256);
        door.doorSwinging = true;
        queueMotion(world, door);
    }
}

bool insideBox(const ServerObject& box, float bx, float by, float px, float py)
{
    // The shape's point test's box arm, on its corners (world.hpp).
    return insideBoxCorners(server::boxCorners(box.boxWidth, box.boxHeight), bx, by, px, py);
}

bool substepPressesBody(World& world, const ServerObject& o)
{
    // Only a circle the frame's motion would push off what it meets; a missile's touch ends it in
    // the frame instead, and an ALLOW_OVERLAP one is never pushed.
    if (o.shape != ShapeKind::Circle || (o.flags & ObjFlag::AllowOverlap) || (o.classes & things::ObjectClass::Missile)) {
        return false;
    }
    const MapGrid::Rect box = World::collideBoxAt(o, o.x, o.y);
    std::minstd_rand rng; // (boxDepth's way out from a box's dead centre: unused here)
    bool presses = false;
    world.unitsInRect(box.left, box.top, box.right, box.bottom, [&](ServerObject& other) {
        if (presses || &other == &o || !bodiesMeet(world, o, other)) {
            return;
        }
        // What the motion pushes it off, as gather and circles / circleBox decide: not an elevator,
        // a shaft or a door (the substep's wall ray has the closed doors), not ALLOW_OVERLAP, not a
        // NO_PUSH_CHARACTERS one against a creature, not a creature through a wall from it.
        if ((other.classes & (0x4000u | 0x8000u | things::ObjectClass::Door)) || (other.flags & ObjFlag::AllowOverlap)) {
            return;
        }
        if ((o.classes & 6) && (other.flags & ObjFlag::NoPushCharacters)) {
            return;
        }
        if ((o.classes & 0x2204) && (other.classes & 0x2204) && !world.traceRay(o.x, o.y, other.x, other.y, 0)) {
            return;
        }
        // How far into it now (both where this substep has them) and at the frame's start.
        float now = 0;
        float before = 0;
        if (other.shape == ShapeKind::Circle) {
            const float reach = o.radius + other.radius;
            now = reach - std::hypot(other.x - o.x, other.y - o.y);
            before = reach - std::hypot(other.frameStartX - o.frameStartX, other.frameStartY - o.frameStartY);
        } else if (other.shape == ShapeKind::Box) {
            // (boxDepth reads the box at its NewPos: where the last frame left it, as the substeps
            // don't carry boxes. Inside it, the depth is the distance from its centre instead:
            // left to the frame.)
            if (insideBox(other, other.newX, other.newY, o.x, o.y)) {
                return;
            }
            float nx = 0;
            float ny = 0;
            now = boxDepth(other, o.x, o.y, o.radius, nx, ny, rng);
            before = boxDepth(other, o.frameStartX, o.frameStartY, o.radius, nx, ny, rng);
        } else {
            return;
        }
        presses = 0 < now && before + 0.01f < now;
    });
    return presses;
}

bool substepPressesWall(World& world, ServerObject& o)
{
    // Only a circle the motion pushes off walls (it joins the motion; gather's test: not
    // ALLOW_OVERLAP, not class 0x400000). What doesn't join it moves by its velocity alone.
    if (o.shape != ShapeKind::Circle || (o.flags & ObjFlag::AllowOverlap) || (o.classes & 0x400000u) || !joinsMotion(o)) {
        return false;
    }
    std::vector<ServerObject*> none;
    std::minstd_rand rng;
    Mover m{world, none, rng, 0};
    // How far into the walls in its cells at a place (the deepest of them), as the motion finds it.
    const auto depthAt = [&](float x, float y) {
        Mover::WallProbe probe{x, y};
        const MapGrid::Rect box = World::collideBoxAt(o, x, y);
        const int x0 = static_cast<int>(box.left * kInvCell);
        const int y0 = static_cast<int>(box.top * kInvCell);
        const int x1 = static_cast<int>(box.right * kInvCell);
        const int y1 = static_cast<int>(box.bottom * kInvCell);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                m.wallCell(o, cx, cy, &probe);
            }
        }
        return probe.deepest;
    };
    const float now = depthAt(o.x, o.y);
    return 0 < now && depthAt(o.frameStartX, o.frameStartY) + 0.01f < now;
}

X87 edgeDistance53(const ServerObject& a, const ServerObject& b)
{
    // calcDistance works it all on the x87 and hands back the stack's value (53 bits): most of its
    // callers compare that at once (edgeDistance53); the others store it (edgeDistance).
    const X87 dx = X87(a.x) - b.x;
    const X87 dy = X87(a.y) - b.y;
    X87 d = util::sqrt(dy * dy + dx * dx);
    const auto less = [&](const ServerObject& o) {
        if (o.shape == ShapeKind::Circle) {
            d -= X87(o.radius);
        } else if (o.shape == ShapeKind::Box) {
            d -= X87(std::max(o.boxWidth * 0.5f, o.boxHeight * 0.5f)); // (exact halves)
        }
    };
    less(a);
    less(b);
    return d < X87(0.01f) ? X87(0.01f) : d;
}

float edgeDistance(const ServerObject& a, const ServerObject& b) { return edgeDistance53(a, b).f(); }

void moveObjects(World& world, std::minstd_rand& rng, std::uint32_t frame, std::vector<Contact>* contacts)
{
    // The motion queue, in the order its members joined; one gone,
    // deleted or taken into an inventory since leaves it. (Taken whole: what joins during the
    // steps goes into an empty queue, which the end clears. The lists are the world's, kept.)
    std::vector<ServerObject*>& moving = world.motionMoving;
    moving.clear();
    std::vector<std::pair<std::uint32_t, std::uint64_t>>& taken = world.motionTaken;
    taken.clear();
    taken.swap(world.motionQueue);
    for (const auto& [extent, ticket] : taken) {
        const auto it = world.objects().find(extent);
        if (it == world.objects().end()) {
            continue;
        }
        ServerObject& o = it->second;
        if (!o.inMotion || o.motionTicket != ticket) {
            continue; // (left it and joined again since: its later place counts)
        }
        if (o.gone() || o.pendingDelete || o.holder != 0) {
            o.inMotion = false;
        } else {
            moving.push_back(&o);
        }
    }
    Mover m{world, moving, rng, frame, contacts};
    world.contactPairs.clear();
    m.paired = &world.contactPairs;
    const std::size_t before = contacts ? contacts->size() : 0;
    // The pairs already handed in count as found.
    if (contacts) {
        for (const Contact& c : *contacts) {
            m.pair(c.a, c.b);
        }
    }
    // Five sub-steps a frame: every mover's pushes gathered, then all integrated, then the doors
    // pushed this step turned.
    for (int step = 0; step < 5; ++step) {
        for (std::size_t i = 0; i < moving.size(); ++i) { // (those woken join as it goes)
            m.gather(*moving[i]);
        }
        for (ServerObject* o : moving) {
            m.integrate(*o);
        }
        // The doors pushed this step turn, at most 4 of 256 a step and no further than 12
        // directions either way from shut.
        for (ServerObject* d : moving) {
            if (!d->doorSwinging) {
                continue;
            }
            int turn = util::floatToInt(X87(d->doorSwing) + d->doorSwing + 0.5f); // (on the x87, stored, truncated)
            turn = std::min(std::abs(turn), 4);
            if (d->doorSwing < -kStill) {
                int limit = d->doorClosed - 12;  // (shut - 12, wrapped round the 32 directions)
                if (limit < 0) {
                    limit = d->doorClosed + 20;
                }
                if (d->doorDirection != limit) {
                    d->doorAngle = static_cast<std::int16_t>(d->doorAngle - turn);
                    if (d->doorAngle < 0) {
                        d->doorAngle = static_cast<std::int16_t>(d->doorAngle + 256);
                    }
                }
            } else if (kStill < d->doorSwing && d->doorDirection != ((d->doorClosed + 12) & 31)) {
                d->doorAngle = static_cast<std::int16_t>(d->doorAngle + turn);
                if (d->doorAngle > 255) {
                    d->doorAngle = static_cast<std::int16_t>(d->doorAngle - 256);
                }
            }
            d->doorDirection = static_cast<std::uint8_t>(((d->doorAngle * 32) >> 8) & 31);  // angle 0-255 to direction 0-31
            d->doorSwinging = false;
            d->doorSwing = 0;
        }
    }
    // The queue emptied, each but a door back on the updatable list
    // (unitAddToUpdatable), in the queue's order; what joined during the steps
    // went with it.
    for (ServerObject* o : moving) {
        o->inMotion = false;
        if (!(o->classes & things::ObjectClass::Door)) {
            world.addUpdatable(*o);
        }
    }
    world.motionQueue.clear();
    taken.clear();
    moving.clear();
    if (contacts) { // (the newest first, ahead of any it was handed)
        std::reverse(contacts->begin() + static_cast<std::ptrdiff_t>(before), contacts->end());
        std::rotate(contacts->begin(), contacts->begin() + static_cast<std::ptrdiff_t>(before), contacts->end());
    }
    // (The frame's end for each, updateUnits's last pass over the updatable list: Server::updateWorld.)
}
} // namespace nox::server
