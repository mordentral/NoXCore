// NavGrid: the headless client's walk grid built from a map (walls, windows, solid and harmful
// objects, floor coverage) and its path search over it, rides included. See navigation.hpp.
#include "noxcore/client/navigation.hpp"

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <string>

namespace nox::client {

namespace {

struct Cell {
    int x = 0;
    int y = 0;
};

// A world coordinate's cell, clamped to the grid.
int cellOf(int world)
{
    return std::clamp(world / NavGrid::kCellSize, 0, NavGrid::kCells - 1);
}

// The world point at a cell's centre.
std::pair<int, int> centre(int cx, int cy)
{
    return {cx * NavGrid::kCellSize + NavGrid::kCellSize / 2, cy * NavGrid::kCellSize + NavGrid::kCellSize / 2};
}

} // namespace

NavGrid NavGrid::fromMap(const map::MapFile& map)
{
    NavGrid g;
    g.blocked_.assign(static_cast<std::size_t>(kCells) * kCells, 0);
    for (const auto& w : map::decodeSection<map::WallMap>(map).walls) {
        g.setBlocked(w.x, w.y, true);
    }
    // Windows: walls still, but sight goes through.
    try {
        for (const auto& w : map::decodeSection<map::WindowWalls>(map).walls) {
            if (w.x >= 0 && w.y >= 0 && w.x < kCells && w.y < kCells) {
                g.blocked_[static_cast<std::size_t>(w.y) * kCells + static_cast<std::size_t>(w.x)] |= kWindow;
            }
        }
    } catch (const std::exception&) {
        // (no window list: none)
    }
    return g;
}

bool NavGrid::blocked(int cx, int cy) const noexcept
{
    if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells || blocked_.empty()) {
        return true;
    }
    const std::uint8_t b = blocked_[static_cast<std::size_t>(cy) * kCells + cx];
    return wallBits(b) || (b & (kObstacle | kVoid)) != 0;
}

bool NavGrid::wall(int cx, int cy) const noexcept
{
    if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells || blocked_.empty()) {
        return true;
    }
    const std::uint8_t b = blocked_[static_cast<std::size_t>(cy) * kCells + cx];
    return wallBits(b) && !((b & kWindow) && !(b & kRaised)); // (a magic wall raised there blocks it)
}

void NavGrid::setLiveWalls(const std::vector<std::pair<int, int>>& raised, const std::vector<std::pair<int, int>>& gone)
{
    if (blocked_.empty()) {
        return;
    }
    // Drop the last call's marks first.
    for (auto& c : blocked_) {
        c = static_cast<std::uint8_t>(c & ~(kRaised | kGone));
    }
    const auto mark = [&](const std::pair<int, int>& cell, std::uint8_t bit) {
        if (cell.first >= 0 && cell.second >= 0 && cell.first < kCells && cell.second < kCells) {
            blocked_[static_cast<std::size_t>(cell.second) * kCells + cell.first] |= bit;
        }
    };
    for (const auto& c : raised) {
        mark(c, kRaised);
    }
    for (const auto& c : gone) {
        mark(c, kGone);
    }
}

namespace {

// Sets or clears one flag bit of a cell, making the grid on first use; cells off the grid are ignored.
void setBit(std::vector<std::uint8_t>& cells, int cx, int cy, std::uint8_t bit, bool value)
{
    if (cells.empty()) {
        cells.assign(static_cast<std::size_t>(NavGrid::kCells) * NavGrid::kCells, 0);
    }
    if (cx >= 0 && cy >= 0 && cx < NavGrid::kCells && cy < NavGrid::kCells) {
        auto& c = cells[static_cast<std::size_t>(cy) * NavGrid::kCells + cx];
        c = static_cast<std::uint8_t>(value ? (c | bit) : (c & ~bit));
    }
}

} // namespace

void NavGrid::setBlocked(int cx, int cy, bool value)
{
    setBit(blocked_, cx, cy, kWall, value);
}

void NavGrid::setObstacle(int cx, int cy, bool value)
{
    setBit(blocked_, cx, cy, kObstacle, value);
}

void NavGrid::setHazard(int cx, int cy, bool value)
{
    setBit(blocked_, cx, cy, kHazard, value);
}

void NavGrid::setWindow(int cx, int cy, bool value)
{
    setBit(blocked_, cx, cy, kWindow, value);
}

void NavGrid::addObstacles(const map::MapFile& map, const things::ThingDb& things)
{
    namespace oc = things::ObjectClass;
    // A cell is out when its centre is within the extent itself, and the one under the object's
    // centre always is. Growing extents by a player's radius closes gaps people walk through at
    // this grid's 23 units (a ring of mana obelisks 40 apart); what's left of an object's edge
    // the walk's stuck-and-sidestep deals with.
    constexpr float kMargin = 0;
    const auto toc = map::decodeSection<map::ObjectTOC>(map);
    const auto objects = map::decodeSection<map::ObjectData>(map);
    // The object TOC gives each record's type id its thing name.
    std::map<std::uint16_t, const things::ThingDef*> types;
    for (const auto& e : toc.entries) {
        types[e.id] = things.findThing(e.name);
    }
    std::size_t inside = 0; // the previous object's inventory: not on the ground
    for (const auto& rec : objects.records) {
        if (inside > 0) {
            --inside;
            continue;
        }
        map::ObjectXfer x;
        try {
            x = map::ObjectXfer::parse(rec.data);
        } catch (const std::exception&) {
            continue;
        }
        inside = x.header.inventoryCount();
        const things::ThingDef* t = types[rec.tocId];
        if (!t) {
            continue;
        }
        const std::uint32_t classes = things::thingClasses(*t);
        const std::string_view flags = t->attribute("FLAGS").value_or("");
        const std::string_view collide = t->attribute("COLLIDE").value_or("");
        const auto flagged = [&](std::string_view f) { return flags.find(f) != std::string_view::npos; };
        if (collide.starts_with("NoCollide") || flagged("NO_COLLIDE") || (classes & oc::Door)) {
            continue; // nothing to bump into or be hurt by (doors open)
        }
        if (classes & (oc::Transporter | oc::Elevator | oc::ElevatorShaft)) {
            continue; // stood on to ride them (links: setLinks)
        }
        // DANGEROUS things hurt on touch (DamageCollide: flames, lava fountains, spikes, pits,
        // poison gas): a hazard, with a player's radius round it. Solid ones (not overlapped:
        // ALLOW_OVERLAP, lying BELOW, a HOLE) are in the way too.
        const bool dangerous = (classes & oc::Dangerous) != 0;
        const bool overlapped = flagged("ALLOW_OVERLAP") || flagged("BELOW") || (classes & oc::Hole);
        const bool solid = (classes & (oc::Immobile | oc::Obstacle)) && !overlapped;
        if (!dangerous && !solid) {
            continue;
        }
        const auto extent = t->lastAttribute("EXTENT");
        if (!extent) {
            continue;
        }
        std::istringstream in{std::string(*extent)}; // "CIRCLE 14" or "BOX 20 20"
        std::string shape;
        float a = 0;
        float b = 0;
        in >> shape >> a;
        const bool circle = in && shape == "CIRCLE";
        const bool box = shape == "BOX" && static_cast<bool>(in >> b);
        if (!circle && !box) {
            continue;
        }
        // Calls `set` for every cell whose centre is inside the extent grown by `margin` (scanning the
        // extent's bounding box of cells), then for the cell under the object's centre.
        const auto mark = [&](float margin, auto set) {
            const float reach = (circle ? a : std::max(a, b) / 2) + margin;
            const int c0x = cellOf(static_cast<int>(x.header.x - reach));
            const int c1x = cellOf(static_cast<int>(x.header.x + reach));
            const int c0y = cellOf(static_cast<int>(x.header.y - reach));
            const int c1y = cellOf(static_cast<int>(x.header.y + reach));
            for (int cy = c0y; cy <= c1y; ++cy) {
                for (int cx = c0x; cx <= c1x; ++cx) {
                    const auto [px, py] = centre(cx, cy);
                    const float dx = static_cast<float>(px) - x.header.x;
                    const float dy = static_cast<float>(py) - x.header.y;
                    const bool covered = circle ? std::hypot(dx, dy) <= a + margin
                                                : std::abs(dx) <= a / 2 + margin && std::abs(dy) <= b / 2 + margin;
                    if (covered) {
                        set(cx, cy);
                    }
                }
            }
            set(cellOf(static_cast<int>(x.header.x)), cellOf(static_cast<int>(x.header.y)));
        };
        if (solid) {
            mark(kMargin, [&](int cx, int cy) { setObstacle(cx, cy, true); });
        }
        if (dangerous) {
            // Touching it is enough: its extent plus a player's radius.
            mark(10, [&](int cx, int cy) { setBit(blocked_, cx, cy, kHazard, true); });
        }
    }
}

bool NavGrid::hazard(int cx, int cy) const noexcept
{
    if (cx < 0 || cy < 0 || cx >= kCells || cy >= kCells || blocked_.empty()) {
        return false;
    }
    return (blocked_[static_cast<std::size_t>(cy) * kCells + cx] & kHazard) != 0;
}

void NavGrid::addFloor(const map::MapFile& map)
{
    if (!map.find(map::FloorMap::kName)) {
        return;
    }
    const auto floor = map::decodeSection<map::FloorMap>(map);
    std::vector<std::uint8_t> covered(static_cast<std::size_t>(kCells) * kCells, 0);
    std::vector<std::uint8_t> lava(covered.size(), 0);
    // A tile is a 46-unit diamond; pair (x, y) holds the odd one centred at (46x + 35, 46y + 12)
    // and the even one at (46x + 12, 46y + 35) (their images' top-left corners as the client
    // draws them, tileDrawImpl, plus half a tile).
    // FLOR entry 6 is lava: a unit with health standing on it, not jumping (object flag 0x4000),
    // burns (updateObjectsVelocity checks tileNFromPoint == 6).
    constexpr std::uint8_t kLavaTile = 6;
    // Marks the cells whose centres lie in the diamond round tile centre (tx, ty): |dx| + |dy| <= 24.
    const auto cover = [&](int tx, int ty, const map::FloorTile& tile) {
        for (int cy = cellOf(ty - 23); cy <= cellOf(ty + 23); ++cy) {
            for (int cx = cellOf(tx - 23); cx <= cellOf(tx + 23); ++cx) {
                const auto [px, py] = centre(cx, cy);
                if (std::abs(px - tx) + std::abs(py - ty) <= 24) {
                    covered[static_cast<std::size_t>(cy) * kCells + cx] = 1;
                    if (tile.image == kLavaTile) {
                        lava[static_cast<std::size_t>(cy) * kCells + cx] = 1;
                    }
                }
            }
        }
    };
    for (const auto& pair : floor.readPairs()) { // (as the game reads a pair stored twice)
        if (pair.odd) {
            cover(pair.x * 46 + 35, pair.y * 46 + 12, *pair.odd);
        }
        if (pair.even) {
            cover(pair.x * 46 + 12, pair.y * 46 + 35, *pair.even);
        }
    }
    // Cells no tile reached are void; covered lava cells are hazards.
    for (int cy = 0; cy < kCells; ++cy) {
        for (int cx = 0; cx < kCells; ++cx) {
            const std::size_t i = static_cast<std::size_t>(cy) * kCells + cx;
            if (!covered[i]) {
                setBit(blocked_, cx, cy, kVoid, true);
            } else if (lava[i]) {
                setBit(blocked_, cx, cy, kHazard, true);
            }
        }
    }
}

bool NavGrid::clearLine(std::pair<int, int> a, std::pair<int, int> b, bool wallsOnly, bool noHazards, const CellSet* avoid) const
{
    const double dx = b.first - a.first;
    const double dy = b.second - a.second;
    // Samples the line every 4 world units (well under a cell) and checks each sample's cell.
    const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy) / 4.0)));
    int lastX = cellOf(a.first);
    int lastY = cellOf(a.second);
    const auto stops = [&](int cx, int cy) {
        return (wallsOnly ? wall(cx, cy) : blocked(cx, cy)) || (noHazards && hazard(cx, cy)) ||
               (avoid && !wallsOnly && avoid->contains({cx, cy}));
    };
    for (int i = 0; i <= steps; ++i) {
        const int cx = cellOf(static_cast<int>(a.first + dx * i / steps));
        const int cy = cellOf(static_cast<int>(a.second + dy * i / steps));
        if (stops(cx, cy) && !(i == 0 && !wall(cx, cy))) {
            return false; // (a walk may start in an object's margin: we stand against it)
        }
        // Moving diagonally between cells: both side cells must be free, as for path steps.
        if (cx != lastX && cy != lastY && (stops(cx, lastY) || stops(lastX, cy))) {
            return false;
        }
        lastX = cx;
        lastY = cy;
    }
    return true;
}

std::vector<std::pair<int, int>> NavGrid::findPath(std::pair<int, int> from, std::pair<int, int> to, const CellSet* avoid,
                                                   bool* complete) const
{
    if (complete) {
        *complete = true;
    }
    if (blocked_.empty()) {
        return {};
    }
    if (clearLine(from, to, false, true, avoid)) {
        return {to}; // straight there, touching nothing that hurts
    }
    // (cells the walker found it couldn't pass: as if blocked)
    const auto stop = [&](int cx, int cy) { return blocked(cx, cy) || (avoid && avoid->contains({cx, cy})); };
    // Start and goal: the nearest free cells (we may be standing against a wall cell).
    const auto nearestFree = [&](int cx, int cy) -> Cell {
        // Rings of growing size round the cell, up to 3 cells out.
        for (int r = 0; r < 4; ++r) {
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (!stop(cx + dx, cy + dy)) {
                        return {cx + dx, cy + dy};
                    }
                }
            }
        }
        return {cx, cy};
    };
    const Cell start = nearestFree(cellOf(from.first), cellOf(from.second));
    const Cell goal = nearestFree(cellOf(to.first), cellOf(to.second));

    // A* over the cells: the cost so far, the cell each was reached from, and the ride taken into it.
    const auto index = [](int x, int y) { return static_cast<std::size_t>(y) * kCells + x; };
    std::vector<float> cost(static_cast<std::size_t>(kCells) * kCells, std::numeric_limits<float>::infinity());
    std::vector<std::int32_t> parent(cost.size(), -1);
    std::vector<const Link*> viaLink(cost.size(), nullptr); // reached by riding this
    using Item = std::pair<float, std::int32_t>; // estimated total, cell index
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    // (A ride can put us nearer than walking distance says: with links the estimate is 0, a
    // plain shortest-path search.)
    const float estimateScale = links_.empty() ? 1.0f : 0.0f;
    const auto heuristic = [&](int x, int y) {
        const int dx = std::abs(x - goal.x);
        const int dy = std::abs(y - goal.y);
        // Octile distance: a diagonal step costs sqrt 2, so the extra over max(dx, dy) is 0.414 per diagonal.
        return estimateScale * static_cast<float>(std::max(dx, dy) + 0.41421356 * std::min(dx, dy));
    };
    cost[index(start.x, start.y)] = 0;
    open.push({heuristic(start.x, start.y), static_cast<std::int32_t>(index(start.x, start.y))});
    bool found = false;
    // (For a goal there's no way to: the reached cell nearest it, by the crow's flight.)
    const auto fromGoal = [&](int x, int y) {
        const int dx = std::abs(x - goal.x);
        const int dy = std::abs(y - goal.y);
        return static_cast<float>(std::max(dx, dy) + 0.41421356 * std::min(dx, dy));
    };
    std::int32_t nearest = static_cast<std::int32_t>(index(start.x, start.y));
    float nearestD = fromGoal(start.x, start.y);
    while (!open.empty()) {
        const auto [estimate, at] = open.top();
        open.pop();
        // `at` is a cell index, y * kCells + x.
        const int x = at % kCells;
        const int y = at / kCells;
        if (x == goal.x && y == goal.y) {
            found = true;
            break;
        }
        if (estimate > cost[at] + heuristic(x, y) + 1e-3f) {
            continue; // stale entry
        }
        if (complete && fromGoal(x, y) < nearestD) {
            nearest = at;
            nearestD = fromGoal(x, y);
        }
        // The eight neighbouring cells.
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dy == 0) || stop(x + dx, y + dy)) {
                    continue;
                }
                if (dx != 0 && dy != 0 && (stop(x + dx, y) || stop(x, y + dy))) {
                    continue; // no corner cutting
                }
                // Hazards cost dear: round them if there's a way, across (jumping) if not. A
                // diagonal step brushes the corners of the two cells beside it.
                const bool diagonal = dx != 0 && dy != 0;
                const bool burns = hazard(x + dx, y + dy) || (diagonal && (hazard(x + dx, y) || hazard(x, y + dy)));
                const float step = (diagonal ? 1.41421356f : 1.0f) + (burns ? kHazardCost : 0.0f);
                const std::size_t next = index(x + dx, y + dy);
                if (cost[at] + step < cost[next]) {
                    cost[next] = cost[at] + step;
                    parent[next] = at;
                    viaLink[next] = nullptr;
                    open.push({cost[next] + heuristic(x + dx, y + dy), static_cast<std::int32_t>(next)});
                }
            }
        }
        // Rides from this cell.
        for (const auto& link : links_) {
            if (cellOf(link.from.first) != x || cellOf(link.from.second) != y) {
                continue;
            }
            const int tx = cellOf(link.to.first);
            const int ty = cellOf(link.to.second);
            if (tx < 0 || ty < 0 || tx >= kCells || ty >= kCells) {
                continue;
            }
            const std::size_t next = index(tx, ty);
            if (cost[at] + link.cost < cost[next]) {
                cost[next] = cost[at] + link.cost;
                parent[next] = at;
                viaLink[next] = &link;
                open.push({cost[next] + heuristic(tx, ty), static_cast<std::int32_t>(next)});
            }
        }
    }
    // Not reached: with `complete`, settle for the reached cell nearest the goal.
    std::int32_t end = static_cast<std::int32_t>(index(goal.x, goal.y));
    if (!found) {
        if (!complete || nearest == static_cast<std::int32_t>(index(start.x, start.y))) {
            if (complete) {
                *complete = false;
            }
            return {}; // (no way, or none nearer than here)
        }
        *complete = false;
        end = nearest;
    }

    // The cells back from the goal; a ride's pad and exit as their own points.
    struct Step {
        std::pair<int, int> point;
        bool rideEnd = false; ///< reached by a ride from the point before (the pad)
    };
    std::vector<Step> cells;
    const Link* rodeFrom = nullptr; // the cell before this one in the walk is that ride's pad
    for (std::int32_t at = end; at != -1; at = parent[at]) {
        const Link* link = viaLink[static_cast<std::size_t>(at)];
        if (link) {
            cells.push_back({link->to, true});
        } else if (rodeFrom) {
            cells.push_back({rodeFrom->from, false}); // the pad itself, not its cell's centre
        } else {
            cells.push_back({centre(at % kCells, at / kCells), false});
        }
        rodeFrom = link;
    }
    std::reverse(cells.begin(), cells.end());
    if (found && !cells.back().rideEnd) {
        cells.back().point = to;
    }

    // Straighten: from each point, jump to the farthest point still in a clear line, but never
    // past a ride's pad: the walk goes onto it, then the path goes on from where it puts us.
    std::vector<std::pair<int, int>> path;
    std::pair<int, int> at = from;
    std::size_t i = 0;
    while (i < cells.size()) {
        if (cells[i].rideEnd) {
            path.push_back(cells[i].point); // the ride's exit
            at = cells[i].point;
            ++i;
            continue;
        }
        std::size_t limit = cells.size();
        for (std::size_t k = i; k + 1 < cells.size(); ++k) {
            if (cells[k + 1].rideEnd) {
                limit = k + 1; // the pad (cells[k]) is as far as a straight walk goes
                break;
            }
        }
        std::size_t far = i;
        for (std::size_t j = limit; j-- > i;) {
            if (clearLine(at, cells[j].point, false, true, avoid)) { // (no cutting across a hazard the path went round)
                far = j;
                break;
            }
        }
        path.push_back(cells[far].point);
        at = cells[far].point;
        i = far + 1;
    }
    return path;
}

const NavGrid::Link* NavGrid::linkAt(std::pair<int, int> at) const noexcept
{
    for (const auto& link : links_) {
        if (cellOf(link.from.first) == cellOf(at.first) && cellOf(link.from.second) == cellOf(at.second)) {
            return &link;
        }
    }
    return nullptr;
}

} // namespace nox::client
