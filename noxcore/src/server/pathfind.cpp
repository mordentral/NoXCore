// The monsters' path finder: a 256 x 256 tile grid of obstacle marks, a breadth-first
// search over it (findPath), and the roaming waypoint graph. pathfind.hpp describes the
// grid's flags and the search's rules; monsters.cpp is the main caller.
#include "noxcore/server/pathfind.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

constexpr int kSize = 256;
constexpr float kTile = 23.0f;
constexpr float kHalf = 11.5f;
constexpr float kInset = std::bit_cast<float>(0x40133333u); // (2.3)

/// The 8 steps: the 4 straight, then the diagonals.
constexpr std::array<std::pair<int, int>, 8> kSteps = {{{1, 0}, {0, -1}, {-1, 0}, {0, 1}, {1, 1}, {-1, -1}, {-1, 1}, {1, -1}}};
/// A tile's 9 sample points.
/// (at 2.3, 11.5 and 20.7 into the tile, each way)
const std::array<std::pair<float, float>, 9> kSamples = {{
    {kInset, kInset}, {kHalf, kInset}, {std::bit_cast<float>(0x41A59999u), kInset},
    {kInset, kHalf}, {kHalf, kHalf}, {std::bit_cast<float>(0x41A59999u), kHalf},
    {kInset, std::bit_cast<float>(0x41A59999u)}, {kHalf, std::bit_cast<float>(0x41A59999u)},
    {std::bit_cast<float>(0x41A59999u), std::bit_cast<float>(0x41A59999u)},
}};
/// A tile's 4 edge middles, for water.
constexpr std::array<std::pair<float, float>, 4> kEdges = {{{kHalf, 0.0f}, {kTile, kHalf}, {0.0f, kHalf}, {kHalf, kTile}}};

bool inGrid(int x, int y) { return 0 <= x && x < kSize && 0 <= y && y < kSize; }
World::PathTile& tileAt(World& w, int x, int y) { return w.pathTiles[static_cast<std::size_t>(x * kSize + y)]; }  // x-major: x * 256 + y

/// The tile the marking writes for (x, y). The game's marking has no bounds check: it writes the
/// grid's entry x * 256 + y, so a y one past either edge lands at the other end of the next or
/// last column (an original bug), and beyond the grid it writes past it. The first kept, the
/// second not written (null).
World::PathTile* markedTile(World& w, int x, int y)
{
    const std::int64_t at = static_cast<std::int64_t>(x) * kSize + y;
    return 0 <= at && at < static_cast<std::int64_t>(kSize) * kSize ? &w.pathTiles[static_cast<std::size_t>(at)] : nullptr;
}

/// A shape for the marking (an object's, a DANGEROUS one's grown by 11.5), as the game's shape
/// holds it: a circle's r squared kept (stored), a box's corners (boxCorners). Its box (the tiles
/// walked) about its position, its point test about its NewPos (hx, hy).
struct Shape {
    ShapeKind kind = ShapeKind::None;
    float x = 0;
    float y = 0;
    float radius = 0;
    float width = 0;
    float height = 0;
    float r2 = 0;
    float hx = 0;
    float hy = 0;
};

Shape shapeOf(const ServerObject& o, bool grown)
{
    using util::X87;
    Shape s{o.shape, o.x, o.y, o.radius, o.boxWidth, o.boxHeight, (X87(o.radius) * o.radius).f(), o.newX, o.newY};
    if (grown && (o.classes & oc::Dangerous)) {
        // (Its shape grown in place: the radius's sum stored, r squared from the unstored sum;
        // a box's sides by 11.5 + 11.5 each, stored.)
        if (s.kind == ShapeKind::Circle) {
            const X87 r = X87(kHalf) + o.radius;
            s.radius = r.f();
            s.r2 = (r * r).f();
        } else if (s.kind == ShapeKind::Box) {
            s.width = (X87(kHalf) + kHalf + o.boxWidth).f();
            s.height = (X87(kHalf) + kHalf + o.boxHeight).f();
        }
    }
    return s;
}

/// The shape's box (objectUnkUpdateCoords's CollideP1 / P2: each side one add, stored).
bool boundsOf(const Shape& s, float b[4])
{
    using util::X87;
    switch (s.kind) {
    case ShapeKind::Center:
        b[0] = b[2] = s.x;
        b[1] = b[3] = s.y;
        return true;
    case ShapeKind::Circle:
        b[0] = (X87(s.x) - s.radius).f();
        b[1] = (X87(s.y) - s.radius).f();
        b[2] = (X87(s.radius) + s.x).f();
        b[3] = (X87(s.radius) + s.y).f();
        return true;
    case ShapeKind::Box: {
        const BoxCorners c = boxCorners(s.width, s.height);
        b[0] = (X87(c.c1x) + s.x).f();
        b[1] = (X87(c.c0y) + s.y).f();
        b[2] = (X87(c.c2x) + s.x).f();
        b[3] = (X87(c.c3y) + s.y).f();
        return true;
    }
    default:
        return false;
    }
}

/// Whether the shape holds the point, about its NewPos (the shape's point test: in
/// the game's x87, the distance's squares and sum on the stack against the stored r squared; a box
/// by insideBoxCorners).
bool holds(const Shape& s, float px, float py)
{
    using util::X87;
    if (s.kind == ShapeKind::Circle) {
        const X87 dx = X87(s.hx) - px;
        const X87 dy = X87(s.hy) - py;
        return dx * dx + dy * dy <= s.r2;
    }
    if (s.kind == ShapeKind::Box) {
        return insideBoxCorners(boxCorners(s.width, s.height), s.hx, s.hy, px, py);
    }
    return false;
}

/// Each tile of the shape's box with a sample point inside it (rows outer), as markedTile finds
/// it. (The walk bounded to what can reach the grid: past that the game writes out of it.)
template <typename F>
void eachCoveredTile(World& w, const Shape& s, F&& f)
{
    float b[4];
    if (!boundsOf(s, b)) {
        return;
    }
    constexpr int kFar = kSize * kSize;
    const int x0 = std::max(tileOf(b[0]), -kSize);
    const int y0 = std::max(tileOf(b[1]), -kFar);
    const int x1 = std::min(tileOf(b[2]), 2 * kSize);
    const int y1 = std::min(tileOf(b[3]), 2 * kFar);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            World::PathTile* t = markedTile(w, x, y);
            if (!t) {
                continue;
            }
            // (The tile's corner stored, each sample point's sum stored.)
            const float bx = static_cast<float>(x * 23);
            const float by = static_cast<float>(y * 23);
            for (const auto& [sx, sy] : kSamples) {
                if (holds(s, (util::X87(bx) + sx).f(), (util::X87(by) + sy).f())) {
                    f(*t);
                    break;
                }
            }
        }
    }
}

/// The moving obstacles' marks: obstacles and what doesn't update 0x100 (0x200
/// unless flag 0x10), fire 0x400.
void markMoving(World& w, const ServerObject& o)
{
    const std::uint32_t c = o.classes;
    // Skipped: doors and elevators (0xC080); a non-fire that's BELOW, ALLOW_OVERLAP or
    // NO_COLLIDE (0x49); anything that updates and is neither an obstacle nor a fire (0x2008).
    if ((c & 0xC080u) || (!(c & oc::Fire) && (o.flags & 0x49u)) || (!(c & 0x2008u) && !(o.flags & ObjFlag::NoUpdate))) {
        return;
    }
    eachCoveredTile(w, shapeOf(o, true), [&](World::PathTile& t) {
        t.stamp = w.pathStamp;
        if ((c & oc::Obstacle) || (o.flags & ObjFlag::NoUpdate)) {
            t.flags |= (o.flags & 0x10u) ? 0x100 : 0x300;  // 0x200 also blocks flyers, unless flag 0x10
        } else if (c & oc::Fire) {
            t.flags |= 0x400;
        }
    });
}

/// The grid made: every object's moving marks, then the fixed ones.
void buildGrid(World& w)
{
    w.pathTiles.assign(static_cast<std::size_t>(kSize * kSize), {});
    w.pathVisit = 0;
    w.pathStamp = 0;
    for (auto& [e, o] : w.objects()) {
        if (o.gone() || o.holder != 0) {
            continue;
        }
        markMoving(w, o);
        const std::uint32_t c = o.classes;
        const auto flagAt = [&](std::uint16_t bit) {
            if (World::PathTile* t = markedTile(w, tileOf(o.x), tileOf(o.y))) {
                t->flags |= bit;
            }
        };
        if (c & oc::Door) {
            continue;
        }
        if (c & oc::Hole) {
            flagAt(0x10);
        } else if (c & oc::Transporter) {
            flagAt(0x20);
        } else if (c & oc::Elevator) {
            flagAt(4);
        } else if (c & oc::ElevatorShaft) {
            flagAt(8);
        } else if (!(o.flags & 0x49u) && (c & oc::Immobile)) {
            eachCoveredTile(w, shapeOf(o, false), [&](World::PathTile& t) {
                t.flags |= (o.flags & 0x10u) ? 1 : 3;  // 2 also blocks flyers, unless flag 0x10
            });
        }
    }
    for (const auto& wp : w.waypoints()) {
        // Roaming waypoints (link flag 0x80) mark their tile 0x40.
        if (wp.linkFlags & 0x80u) {
            if (World::PathTile* t = markedTile(w, tileOf(wp.x), tileOf(wp.y))) {
                t->flags |= 0x40;
            }
        }
    }
}

/// The moving obstacles marked afresh, at most every 15 frames, when an obstacle
/// or a fire has joined the motion since the last (a flag the exe's path finder keeps for it).
void refresh(World& w, std::uint32_t frame)
{
    if (w.pathTiles.empty()) {
        buildGrid(w);
    }
    // The marks stand until an obstacle or fire joins the motion (which clears pathMarksStand);
    // then at most every 15 frames, under a new stamp so the old marks lapse.
    if (!w.pathMarksStand && 14u < frame - w.pathMarkedAt) {
        w.pathMarkedAt = frame;
        ++w.pathStamp;
        for (auto& [e, o] : w.objects()) {
            if (!o.gone() && o.holder == 0) {
                markMoving(w, o);
            }
        }
        w.pathMarksStand = true;
    }
}

bool flying(const ServerObject& o) { return (o.flags & ObjFlag::Airborne) != 0; }
bool fireproof(const ServerObject& o) { return (o.subclasses & 0x400u) != 0; }

/// A fixed obstacle in the tile: 2 to a flyer, 1 to the rest.
bool fixedObstacle(World& w, const ServerObject& o, int x, int y)
{
    if (!inGrid(x, y)) {
        return false;
    }
    return (tileAt(w, x, y).flags & (flying(o) ? 2 : 1)) != 0;
}

/// A moving obstacle there now: marked this time; 0x200 to a flyer; 0x100;
/// fire (but to the fireproof).
bool movingObstacle(World& w, const ServerObject& o, int x, int y)
{
    if (!inGrid(x, y)) {
        return false;
    }
    const auto& t = tileAt(w, x, y);
    if (t.stamp != w.pathStamp) {
        return false;
    }
    if (flying(o)) {
        return (t.flags & 0x200) != 0;
    }
    return (t.flags & 0x100) || (!fireproof(o) && (t.flags & 0x400));
}

/// Free of obstacles (pathfind_preCheckWalls2).
bool clearTile(World& w, const ServerObject& o, int x, int y) { return !fixedObstacle(w, o, x, y) && !movingObstacle(w, o, x, y); }

/// Not water (a walker that doesn't swim (subclass 0x400) keeps off a tile with
/// water at an edge's middle).
bool dryTile(World& w, const ServerObject& o, int x, int y)
{
    if (flying(o) || fireproof(o)) {
        return true;
    }
    const auto look = [&] {
        for (const auto& [ex, ey] : kEdges) {
            if (w.floorAt(static_cast<float>(x * 23) + ex, static_cast<float>(y * 23) + ey) == 6) {
                return false;
            }
        }
        return true;
    };
    // (A map's floor is set at its load: a tile's answer, once looked at, kept with it.)
    if (!inGrid(x, y) || w.pathTiles.empty()) {
        return look();
    }
    World::PathTile& t = tileAt(w, x, y);
    if (t.water == 0) {
        t.water = look() ? 1 : 2;
    }
    return t.water == 1;
}

/// The key to a door it carries (doorGetSomeKey): none to a mechanism (lock 5)
/// or an owned door. (A quest's shared silver key: with the quests.)
bool hasKey(World& w, const ServerObject& mover, const ServerObject& door)
{
    if (door.lock == 5 || door.owner != 0 || door.lock == 0 || 4 < door.lock) {
        return false;
    }
    static constexpr std::string_view kKeys[5] = {"", "SilverKey", "GoldKey", "RubyKey", "SapphireKey"};
    for (const std::uint32_t e : mover.inventory) {
        const ServerObject* i = w.byExtent(e);
        if (i && (i->classes & oc::Key) && i->typeName == kKeys[door.lock]) {
            return true;
        }
    }
    return false;
}

/// A shut door's leaf as a wall shape (0 or 1), -1 neither.
int doorShape(const ServerObject& door)
{
    const auto [lx, ly] = doorLeaf(door.doorClosed);
    if ((0 < lx && 0 < ly) || (lx < 0 && ly < 0)) {
        return 1;
    }
    if ((lx < 0 && 0 < ly) || (0 < lx && ly < 0)) {
        return 0;
    }
    return -1;
}

/// A wall for the diagonal steps (with the mover's flags, a3): a window not to
/// flyers, an open secret wall not; a door: an unlocked or keyed one not (the key counted once a
/// step), a shut one (or a gate) by its leaf.
int cornerWall(World& w, const ServerObject& mover, int x, int y, bool& keyCheck)
{
    if (!inGrid(x, y)) {
        return -1;
    }
    const WallCell* c = w.wall(x, y);
    if (!c) {
        return -1;
    }
    const bool walker = !flying(mover);
    if (c->door == 0) {
        if ((walker || !c->window) && (!c->secret || c->secretOpen < 12)) {
            return c->direction;
        }
        return -1;
    }
    const ServerObject* door = w.byExtent(c->door);
    if (!door) {
        return -1;
    }
    if (door->lock == 0) {
        return -1;
    }
    if (keyCheck && hasKey(w, mover, *door)) {
        keyCheck = false;
        return -1;
    }
    if (door->doorDirection == door->doorClosed) {
        return doorShape(*door);
    }
    return -1;
}

/// Where a hole, transporter, elevator or shaft in the tile leads: the tile of
/// its fall, its partner, its other half.
const ServerObject* transitAt(World& w, int x, int y, std::uint16_t flags)
{
    std::uint32_t cls = 0;
    if (flags & 0x10) {
        cls = oc::Hole;
    } else if (flags & 0x20) {
        cls = oc::Transporter;
    } else if (flags & 4) {
        cls = oc::Elevator;
    } else if (flags & 8) {
        cls = oc::ElevatorShaft;
    } else {
        return nullptr;
    }
    // (at the tile's corner, x 23: of the map grid's cell there, the last
    // of the class its extent list holds, wherever in the cell it is.)
    const ServerObject* found = nullptr;
    w.unitsInCell(static_cast<float>(x) * 23.0f, static_cast<float>(y) * 23.0f, [&](const ServerObject& o) {
        if (o.classes & cls) {
            found = &o;
        }
    });
    return found;
}

bool transitTo(World& w, int x, int y, int& tx, int& ty)
{
    const std::uint16_t flags = tileAt(w, x, y).flags;
    if (!(flags & 0x3C)) {  // a hole, transporter, elevator or shaft
        return false;
    }
    const ServerObject* o = transitAt(w, x, y, flags);
    if (!o || !(o->flags & ObjFlag::Enabled)) {
        return false;
    }
    if (flags & 0x10) {
        if (!o->pit) {
            return false;
        }
        tx = static_cast<int>(o->pit->fallX) / 23;
        ty = static_cast<int>(o->pit->fallY) / 23;
        return true;
    }
    const std::uint32_t link = (flags & 0x20) ? (o->transport ? o->transport->link : 0) : o->elevatorLink;
    const ServerObject* to = link ? w.byExtent(link) : nullptr;
    // (a transporter's partner is a transporter only, as the game links them; else it leads nowhere)
    if (!to || ((flags & 0x20) && !(to->classes & oc::Transporter))) {
        return false;
    }
    tx = tileOf(to->x);
    ty = tileOf(to->y);
    return true;
}

/// What it rides at the tile: its position and code.
bool transitObject(World& w, int x, int y, float& px, float& py, std::uint32_t& code)
{
    const std::uint16_t flags = tileAt(w, x, y).flags;
    if (!(flags & 0x3C)) {
        return false;
    }
    const ServerObject* o = transitAt(w, x, y, flags);
    if (!o || !(o->flags & ObjFlag::Enabled)) {
        return false;
    }
    px = o->x;
    py = o->y;
    code = o->extent;
    return true;
}

/// Out of a wall's tile, a step along the way the mover is farther off its middle
/// (pathfind_preCheckWalls: x's offset left on the stack, y's stored).
void stepOffWall(const ServerObject& mover, int& sx, int& sy)
{
    using util::X87;
    const X87 fx = X87(mover.x) - (X87::fromDouble(sx) * kTile + kHalf);
    const float fy = (X87(mover.y) - (X87::fromDouble(sy) * kTile + kHalf).f()).f();
    if (std::abs(fy) < util::fabs(fx).d()) {
        sx += X87(0.0f) < fx ? 1 : -1;
    } else {
        sy += 0.0f < fy ? 1 : -1;
    }
}

struct Node {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    int parent = -1;
    std::uint8_t flags = 0; ///< 2: it rides on
};

/// A search's lists (its nodes, a level of the search and the next), kept on the thread from
/// search to search to save their allocations. A search under another (none does) has its own.
struct SearchLists {
    std::vector<Node> nodes;
    std::vector<int> frontier;
    std::vector<int> next;
    bool busy = false;
};

/// The thread's lists for one search, or a fresh set when they're in use.
class SearchListsHeld {
public:
    SearchListsHeld() : lists_(kept().busy ? own_ : kept())
    {
        lists_.busy = true;
        lists_.nodes.clear();
        lists_.frontier.clear();
        lists_.next.clear();
    }
    ~SearchListsHeld() { lists_.busy = false; }
    SearchListsHeld(const SearchListsHeld&) = delete;
    SearchListsHeld& operator=(const SearchListsHeld&) = delete;
    SearchLists& operator*() noexcept { return lists_; }

private:
    static SearchLists& kept()
    {
        thread_local SearchLists lists;
        return lists;
    }
    SearchLists own_;
    SearchLists& lists_;
};

/// The path from the start to `end`, the target last when it was reached.
void buildPath(World& w, const std::vector<Node>& nodes, int end, const float* target, PathSearch& out)
{
    out.points.clear();
    if (end < 0) {
        return;
    }
    const Node* n = &nodes[static_cast<std::size_t>(end)];
    float px = 0;
    float py = 0;
    std::uint32_t code = 0;
    if (target) {
        out.points.emplace_back(target[0], target[1]);
    } else if ((n->flags & 2) && transitObject(w, n->x, n->y, px, py, code)) {
        out.points.emplace_back(px, py);
        if (out.transits.size() < 8) {
            out.transits.push_back(code);
        }
    } else {
        out.points.emplace_back(static_cast<float>(n->x) * kTile + kHalf, static_cast<float>(n->y) * kTile + kHalf);
    }
    const Node* child = n;
    // Back up the parents to the start (the points are reversed at the end).
    for (int pi = n->parent; pi >= 0; pi = nodes[static_cast<std::size_t>(pi)].parent) {
        const Node& p = nodes[static_cast<std::size_t>(pi)];
        const int dx = static_cast<int>(p.x) - static_cast<int>(child->x);
        const int dy = static_cast<int>(p.y) - static_cast<int>(child->y);
        if ((p.flags & 2) && transitObject(w, p.x, p.y, px, py, code)) {
            out.points.emplace_back(px, py);
            if (out.transits.size() < 8) {
                // A transit within 69 tiles of the next tile, with a clear ray between, isn't listed.
                bool skip = false;
                if (static_cast<float>(dy * dy + dx * dx) <= 4761.0f) { // (69 tiles squared)
                    skip = w.traceRay(static_cast<float>(p.x * 23 + 11), static_cast<float>(p.y * 23 + 11), static_cast<float>(child->x * 23 + 11),
                                      static_cast<float>(child->y * 23 + 11), 1);
                }
                if (!skip) {
                    out.transits.push_back(code);
                }
            }
        } else {
            // Its point by the corner or edge of its tile nearest the next (on the stack from the
            // tile's corner, then stored).
            using util::X87;
            const X87 bx = X87(static_cast<float>(p.x)) * kTile;
            const X87 by = X87(static_cast<float>(p.y)) * kTile;
            float qx = (bx + kHalf).f();
            float qy = (by + kHalf).f();
            if (dx < 0) {
                qx = (bx + kTile - kInset).f();
            } else if (0 < dx) {
                qx = (bx + kInset).f();
            }
            if (dy < 0) {
                qy = (by + kTile - kInset).f();
            } else if (0 < dy) {
                qy = (by + kInset).f();
            }
            out.points.emplace_back(qx, qy);
        }
        child = &p;
    }
    std::reverse(out.points.begin(), out.points.end());
}

} // namespace

int tileOf(float v) { return wallCellOf(v); } // (Math_FloatToInt of the chopped product)

int wallBlocking(World& world, const ServerObject& mover, int tx, int ty, std::minstd_rand& rng)
{
    // (Its y range check is never true: a y off the grid is looked up as is.)
    if (tx < 0 || 0xFF < tx) {
        return -1;
    }
    const WallCell* c = world.wall(tx, ty);
    if (!c) {
        return -1;
    }
    if (c->door == 0) {
        if (flying(mover) && c->window) {
            return -1;
        }
        if (c->secret && ((c->secretFlags & 2) || 0xB < c->secretOpen)) {
            return -1;
        }
        return c->direction;
    }
    const ServerObject* door = world.byExtent(c->door);
    if (!door || door->doorDirection != door->doorClosed) {
        return -1;
    }
    const int shape = doorShape(*door);
    if (shape < 0) {
        return -1;
    }
    if (door->owner == 0) {
        if (door->lock != 0 && !hasKey(world, mover, *door)) {
            return shape;
        }
    // A door with an owner (a magic lock) blocks half the time, at random.
    } else if (49 < nox::util::uniform_int_distribution<int>(0, 100)(rng)) {
        return shape;
    }
    return -1;
}

PathSearch findPath(World& world, const ServerObject& mover, float toX, float toY, std::minstd_rand& rng, std::uint32_t frame, bool away,
                    const std::function<bool(int, int)>& inside, int maxSteps)
{
    // pathFind.
    PathSearch out;
    refresh(world, frame);
    const std::uint32_t visit = ++world.pathVisit;
    // away: the farthest tile from the target so far; else the nearest.
    int best = away ? 0 : 999999;
    int bestNode = -1;
    int sx = tileOf(mover.x);
    int sy = tileOf(mover.y);
    // In a wall's tile, a step out of it along the way it's farther off the middle
    // (pathfind_preCheckWalls).
    if (wallBlocking(world, mover, sx, sy, rng) != -1) {
        stepOffWall(mover, sx, sy);
    }
    // Starting in an obstacle, or outside `inside`: the search first gets it out (or in).
    bool inObstacle = !clearTile(world, mover, sx, sy);
    bool outside = inside && !inside(sx, sy);
    const int tx = tileOf(toX);
    const int ty = tileOf(toY);
    const float target[2] = {toX, toY};
    constexpr std::size_t kPool = 0x400;  // at most 1024 nodes; past that the search is exhausted (status 1)
    SearchListsHeld lists;
    std::vector<Node>& nodes = (*lists).nodes;
    nodes.reserve(kPool);
    nodes.push_back({static_cast<std::uint16_t>(sx), static_cast<std::uint16_t>(sy), -1, 0});
    if (inGrid(sx, sy)) {
        tileAt(world, sx, sy).visit = visit;
    }
    std::vector<int>& frontier = (*lists).frontier;
    frontier.push_back(0);
    std::vector<int>& next = (*lists).next; // (each level's, prepended in the exe: walked from its end)
    int steps = 0;
    const auto exhausted = [&] {
        out.status = 1;
        buildPath(world, nodes, bestNode, nullptr, out);
        return out;
    };
    // One level of the breadth-first search a pass.
    for (;;) {
        bool gotOut = false;
        bool gotIn = false;
        next.clear();
        // Whether the search may step from the node (nx, ny) to its neighbour (cx, cy) the way `dir`
        // (dx, dy) goes, when it isn't straight to the target.
        const auto mayStep = [&](int nx, int ny, int cx, int cy, int dir, int dx, int dy) {
            if (4 <= dir) {
                // A diagonal: its two corner walls must let it by, and no fixed obstacle
                // in the two tiles it cuts.
                bool keyCheck = true;
                const auto wallAt = [&](int x, int y) { return cornerWall(world, mover, x, y, keyCheck); };
                bool corner = false;
                const auto in = [](int v, std::initializer_list<int> set) { return std::find(set.begin(), set.end(), v) != set.end(); };
                // Each side cell of the diagonal must hold no wall, or one of the shapes that leave
                // that corner open.
                switch (dir) {
                case 4:
                    corner = in(wallAt(nx, ny + 1), {1, 6, 10, 9, -1}) && in(wallAt(nx + 1, ny), {1, 4, 7, 8, -1});
                    break;
                case 5:
                    corner = in(wallAt(nx - 1, ny), {1, 6, 10, 9, -1}) && in(wallAt(nx, ny - 1), {1, 4, 7, 8, -1});
                    break;
                case 6:
                    corner = in(wallAt(nx, ny + 1), {0, 5, 9, 8, -1}) && in(wallAt(nx - 1, ny), {0, 3, 10, 7, -1});
                    break;
                default:
                    corner = in(wallAt(nx + 1, ny), {0, 5, 9, 8, -1}) && in(wallAt(nx, ny - 1), {0, 3, 10, 7, -1});
                    break;
                }
                if (!corner || fixedObstacle(world, mover, nx, ny + dy) || fixedObstacle(world, mover, nx + dx, ny)) {
                    return false;
                }
            }
            if (wallBlocking(world, mover, cx, cy, rng) != -1 || !dryTile(world, mover, cx, cy)) {
                return false;
            }
            const bool clear = clearTile(world, mover, cx, cy);
            // Starting in an obstacle: it may cross obstacle tiles (but not from a non-fixed one into
            // a fixed one) till it reaches a clear tile.
            if (inObstacle) {
                if (!clear) {
                    if (!fixedObstacle(world, mover, nx, ny) && fixedObstacle(world, mover, cx, cy)) {
                        return false;
                    }
                } else {
                    gotOut = true;
                }
            } else {
                if (!clear) {
                    return false;
                }
                // Starting outside `inside`: anything till it gets in; once in, it stays in.
                if (inside) {
                    const bool in = inside(cx, cy);
                    if (outside) {
                        if (in) {
                            gotIn = true;
                        }
                    } else if (!in) {
                        return false;
                    }
                }
            }
            return true;
        };
        for (const int ni : frontier) {
            const int nx = nodes[static_cast<std::size_t>(ni)].x;
            const int ny = nodes[static_cast<std::size_t>(ni)].y;
            const int d = (ny - ty) * (ny - ty) + (nx - tx) * (nx - tx);
            if (!away) {
                if (d < best) {
                    best = d;
                    bestNode = ni;
                }
                if (!inObstacle && !outside && nx == tx && ny == ty) {
                    out.status = 0;
                    buildPath(world, nodes, ni, target, out);
                    return out;
                }
            } else if (best < d) {
                best = d;
                bestNode = ni;
            }
            // The 8 neighbours, starting from a random one.
            const int from = nox::util::uniform_int_distribution<int>(0, 7)(rng);
            for (int k = 0; k < 8; ++k) {
                const int dir = (k + from) & 7;
                const auto [dx, dy] = kSteps[static_cast<std::size_t>(dir)];
                const int cx = nx + dx;
                const int cy = ny + dy;
                if (!inGrid(cx, cy) || tileAt(world, cx, cy).visit == visit) {
                    continue;
                }
                tileAt(world, cx, cy).visit = visit;
                bool add = false;
                if (cx == tx && cy == ty) {
                    // Straight there from the node's middle: taken.
                    const float x0 = static_cast<float>(nx * 23 + 11);
                    const float y0 = static_cast<float>(ny * 23 + 11);
                    add = lineClear(world, mover, x0, y0, toX, toY) && world.traceRay(x0, y0, toX, toY, flying(mover) ? 5 : 1);  // ray flags: 5 for a flyer (through windows), else 1
                }
                if (!add && !mayStep(nx, ny, cx, cy, dir, dx, dy)) {
                    continue;
                }
                if (kPool <= nodes.size()) {
                    return exhausted(); // ("buildPath: Exhausted ...")
                }
                nodes.push_back({static_cast<std::uint16_t>(cx), static_cast<std::uint16_t>(cy), ni, 0});
                next.push_back(static_cast<int>(nodes.size()) - 1);
            }
            // A hole, transporter or elevator here: on to where it leads.
            int lx = 0;
            int ly = 0;
            if (inGrid(nx, ny) && transitTo(world, nx, ny, lx, ly)) {
                nodes[static_cast<std::size_t>(ni)].flags |= 2;  // flag 2: it leads on through a transit
                if (kPool <= nodes.size()) {
                    return exhausted();
                }
                nodes.push_back({static_cast<std::uint16_t>(lx), static_cast<std::uint16_t>(ly), ni, 0});
                next.push_back(static_cast<int>(nodes.size()) - 1);
            }
        }
        std::reverse(next.begin(), next.end());
        // Once out (or in), the next level keeps only the clear (or inside) tiles.
        if (gotOut) {
            std::erase_if(next, [&](int i) { return !clearTile(world, mover, nodes[static_cast<std::size_t>(i)].x, nodes[static_cast<std::size_t>(i)].y); });
            inObstacle = false;
        }
        if (gotIn) {
            std::erase_if(next, [&](int i) { return !inside(nodes[static_cast<std::size_t>(i)].x, nodes[static_cast<std::size_t>(i)].y); });
            outside = false;
        }
        // No tiles left, or the step limit reached: the path to the best tile found (status 2).
        if (next.empty() || (maxSteps != 0 && maxSteps <= steps)) {
            out.status = 2;
            buildPath(world, nodes, bestNode, nullptr, out);
            return out;
        }
        ++steps;
        frontier.swap(next);
    }
}

PathSearch pathTo(World& world, const ServerObject& mover, float toX, float toY, std::minstd_rand& rng, std::uint32_t frame)
{
    // genPathToPoint into the monster's 32 (31 at most).
    PathSearch s = findPath(world, mover, toX, toY, rng, frame);
    if (32 < s.points.size() && s.status == 0) {
        s.status = 1;
    }
    if (31 < s.points.size()) {
        s.points.resize(31);
    }
    return s;
}

PathSearch retreatPath(World& world, const ServerObject& mover, float fromX, float fromY, std::minstd_rand& rng, std::uint32_t frame)
{
    // generateRetreatPath, its tiles tested by `safe`.
    const auto safe = [&](int x, int y) {
        const float cx = static_cast<float>(x) * kTile + kHalf;
        const float cy = static_cast<float>(y) * kTile + kHalf;
        // (unitsGetInCircle of 100 about the tile's middle, by the grid)
        bool clear = true;
        world.unitsInCircle(cx, cy, 100.0f, [&](const ServerObject& t) {
            if (clear && isEnemy(world, mover, t)) { // (no 0x20 test)
                clear = false;
            }
        });
        return clear;
    };
    PathSearch s = findPath(world, mover, fromX, fromY, rng, frame, true, safe, 6);
    if (32 < s.points.size() && s.status == 0) {
        s.status = 1;
    }
    if (31 < s.points.size()) {
        s.points.resize(31);
    }
    return s;
}

bool lineClear(World& world, const ServerObject& mover, float x0, float y0, float x1, float y1)
{
    const float left = std::min(x0, x1);
    const float right = std::max(x0, x1);
    const float top = std::min(y0, y1);
    const float bottom = std::max(y0, y1);
    // (mapTraceObstacles: the units in the line's box, by the grid, each through
    // its line test; any across it blocks. The game's walk goes on past the first across it, doing
    // nothing more: ours stops there.)
    bool clear = true;
    world.unitsInRectUntil(left, top, right, bottom, [&](const ServerObject& t) {
        if (&t == &mover) { // (no 0x20 test)
            return false;
        }
        // Creatures block unless they're its enemies; other things only when immobile or obstacles.
        if (t.classes & (oc::Monster | oc::Player)) {
            if (isEnemy(world, mover, t)) {
                return false;
            }
        } else if (!(t.classes & (oc::Immobile | oc::Obstacle))) {
            return false;
        }
        if ((t.flags & (ObjFlag::AllowOverlap | ObjFlag::NoCollide)) || (t.classes & oc::Door)) {
            return false;
        }
        // (The game's x87: the distance's squares and sum on the stack against the shape's
        // stored r squared; a box's corners (boxCorners) each about it stored.)
        using util::X87;
        if (t.shape == ShapeKind::Circle) {
            float fx = 0;
            float fy = 0;
            if (pointOnLine(x0, y0, x1, y1, t.x, t.y, fx, fy)) {
                const X87 dx = X87(fx) - t.x;
                const X87 dy = X87(fy) - t.y;
                if (dx * dx + dy * dy <= (X87(t.radius) * t.radius).f()) {
                    clear = false;
                }
            }
        } else if (t.shape == ShapeKind::Box) {
            const BoxCorners k = boxCorners(t.boxWidth, t.boxHeight);
            const float c0x = (X87(k.c0x) + t.x).f(), c0y = (X87(k.c0y) + t.y).f();
            const float c1x = (X87(k.c1x) + t.x).f(), c1y = (X87(k.c1y) + t.y).f();
            const float c2x = (X87(k.c2x) + t.x).f(), c2y = (X87(k.c2y) + t.y).f();
            const float c3x = (X87(k.c3x) + t.x).f(), c3y = (X87(k.c3y) + t.y).f();
            if (segmentsCross(x0, y0, x1, y1, c0x, c0y, c1x, c1y) || segmentsCross(x0, y0, x1, y1, c0x, c0y, c2x, c2y) ||
                segmentsCross(x0, y0, x1, y1, c3x, c3y, c2x, c2y) || segmentsCross(x0, y0, x1, y1, c3x, c3y, c1x, c1y)) {
                clear = false;
            }
        }
        return !clear;
    });
    return clear;
}

bool waypointUsable(World& world, std::uint32_t wp, std::uint8_t flags)
{
    if (wp == 0 || world.waypoints().size() < wp) {
        return false;
    }
    const auto& w = world.waypoints()[wp - 1];
    return w.enabled && (w.linkFlags & flags) != 0;
}

std::uint32_t nearestWaypoint(World& world, float x, float y, std::uint8_t flags, bool linked)
{
    // By the map grid's waypoint lists (each in the cell it was made in), squares
    // about (x, y) growing from its own cell by 85 while under 1000, each
    // cell once (its stamp): the nearest (strictly, within 1000 at first) that it sees (ray flags
    // 1). One found, the square is made its distance; a square that finds none nearer ends it.
    // The walk takes the cells y outer, each list from its head: on, of `flags`,
    // and with `linked` one of its links so too.
    world.registerWaypoints();
    MapGrid& grid = world.grid();
    const std::uint32_t stamp = grid.newWaypointStamp();
    const auto& wps = world.waypoints();
    // (The game's x87: the square's sides stored; each distance's root taken on the stack and
    // compared there, stored when nearer.)
    using util::X87;
    float best = 1000.0f;
    std::uint32_t found = 0;
    float r = 0;
    do {
        std::uint32_t nearer = 0;
        grid.waypointCells(MapGrid::cellOf((X87(x) - r).f()), MapGrid::cellOf((X87(y) - r).f()), MapGrid::cellOf((X87(r) + x).f()),
                           MapGrid::cellOf((X87(r) + y).f()), stamp, [&](std::uint32_t id) {
            if (wps.size() < id) {
                return;
            }
            const auto& w = wps[id - 1];
            if (!w.enabled || !(w.linkFlags & flags)) {
                return;
            }
            if (linked) {
                bool any = false;
                for (const std::int32_t l : w.links) {
                    const ServerWaypoint* to = world.waypoint(l);
                    if (to && to->enabled && (to->linkFlags & flags)) {
                        any = true;
                    }
                }
                if (!any) {
                    return;
                }
            }
            const X87 dx = X87(x) - w.x;
            const X87 dy = X87(y) - w.y;
            const X87 d = util::sqrt(dx * dx + dy * dy);
            if (d < best && world.traceRay(x, y, w.x, w.y, 1)) {
                best = d.f();
                nearer = id;
            }
        });
        if (nearer == 0) {
            if (found != 0) {
                return found;
            }
            r = (X87(r) + 85.0f).f();
        } else {
            r = best;
            found = nearer;
        }
    } while (r < 1000.0f);
    return found;
}

std::uint32_t waypointFrom(World& world, const ServerObject& mover, float x, float y, std::minstd_rand& rng)
{
    // Breadth first, the 4 ways, through what it could walk, to a tile with a
    // roaming waypoint and no wall.
    if (world.pathTiles.empty()) {
        buildGrid(world);
    }
    const std::uint32_t visit = ++world.pathVisit;
    int sx = tileOf(x);
    int sy = tileOf(y);
    if (wallBlocking(world, mover, sx, sy, rng) != -1) {
        stepOffWall(mover, sx, sy);
    }
    constexpr std::array<std::pair<int, int>, 4> kWays = {{{1, 0}, {0, -1}, {-1, 0}, {0, 1}}};
    std::vector<std::pair<int, int>> frontier{{sx, sy}};
    if (inGrid(sx, sy)) {
        tileAt(world, sx, sy).visit = visit;
    }
    std::size_t used = 1;
    while (!frontier.empty()) {
        std::vector<std::pair<int, int>> next;
        for (const auto& [nx, ny] : frontier) {
            if (inGrid(nx, ny) && (tileAt(world, nx, ny).flags & 0x40) && wallBlocking(world, mover, nx, ny, rng) == -1) {  // a roaming waypoint's tile
                return nearestWaypoint(world, static_cast<float>(nx * 23 + 11), static_cast<float>(ny * 23 + 11), 0x80, true);
            }
            for (const auto& [dx, dy] : kWays) {
                const int cx = nx + dx;
                const int cy = ny + dy;
                if (!inGrid(cx, cy) || tileAt(world, cx, cy).visit == visit) {
                    continue;
                }
                tileAt(world, cx, cy).visit = visit;
                if (wallBlocking(world, mover, cx, cy, rng) == -1 && dryTile(world, mover, cx, cy) && used < 0x400) {
                    ++used;
                    next.emplace_back(cx, cy);
                }
            }
        }
        std::reverse(next.begin(), next.end());
        frontier = std::move(next);
    }
    return 0;
}

std::vector<std::uint32_t> waypointRoute(World& world, std::uint32_t from, std::uint32_t to, int most)
{
    // BuildWaypointPath: breadth first over the usable links (flag 0x80).
    std::vector<std::uint32_t> route;
    world.waypointPathStatus = 2;
    if (!waypointUsable(world, from, 0x80) || !waypointUsable(world, to, 0x80)) {
        return route;
    }
    const auto& wps = world.waypoints();
    std::vector<std::uint32_t> parent(wps.size() + 1, 0);
    std::vector<bool> seen(wps.size() + 1, false);
    seen[from] = true;
    std::vector<std::uint32_t> level{from};
    while (!level.empty()) {
        std::vector<std::uint32_t> next; // (prepended)
        for (const std::uint32_t w : level) {
            if (w == to) {
                std::vector<std::uint32_t> back;
                for (std::uint32_t c = w; c != 0 && back.size() < 256; c = parent[c]) {
                    back.push_back(c);
                }
                world.waypointPathStatus = 0;
                for (auto it = back.rbegin(); it != back.rend(); ++it) {
                    if (static_cast<int>(route.size()) == most) {
                        world.waypointPathStatus = 1; // ("Node list too long")
                        break;
                    }
                    route.push_back(*it);
                }
                return route;
            }
            for (const std::int32_t id : wps[w - 1].links) {
                const ServerWaypoint* l = world.waypoint(id);
                if (!l) {
                    continue;
                }
                const auto li = static_cast<std::uint32_t>(l - wps.data()) + 1;
                if (!seen[li] && waypointUsable(world, li, 0x80)) {
                    seen[li] = true;
                    parent[li] = w;
                    next.push_back(li);
                }
            }
        }
        std::reverse(next.begin(), next.end());
        level = std::move(next);
    }
    return route;
}

std::vector<std::uint32_t> waypointPathTo(World& world, const ServerObject& mover, float x, float y, std::minstd_rand& rng)
{
    // A waypoint from each end, then the route between them.
    const std::uint32_t a = waypointFrom(world, mover, mover.x, mover.y, rng);
    const std::uint32_t b = waypointFrom(world, mover, x, y, rng);
    if (a == 0 || b == 0 || a == b) {
        return {};
    }
    return waypointRoute(world, a, b, 16);
}

} // namespace nox::server
