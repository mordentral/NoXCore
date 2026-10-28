// The wall tool's rules (walls.hpp).
#include "noxcore/mapedit/walls.hpp"

#include "noxcore/mapedit/selection.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>

namespace nox::mapedit {

namespace {

constexpr std::array<std::uint8_t, 4> kAllArms = {kArmNW, kArmNE, kArmSW, kArmSE};

bool onGrid(int x, int y)
{
    return x >= 0 && y >= 0 && x < 255 && y < 256; // (x 255 ends the wall list)
}

std::uint32_t mix(std::uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// The facing a wall takes for `arms`, else `keep`.
std::uint8_t faceFor(std::uint8_t arms, std::uint8_t keep)
{
    const auto f = facingForArms(arms);
    return f ? *f : keep;
}

/// The wall's state with a new facing: its variant kept if the facing has it (else 0); the
/// direction's flag bit 7 kept.
WallState refaced(const things::ThingDb& things, WallState s, std::uint8_t facing)
{
    s.wall.direction = static_cast<std::uint8_t>((s.wall.direction & 0x80) | facing);
    if (s.wall.variant >= wallVariantCount(things, s.wall.material, facing)) {
        s.wall.variant = 0;
    }
    return s;
}

} // namespace

std::uint8_t oppositeArm(std::uint8_t arm)
{
    switch (arm) {
    case kArmNW: return kArmSE;
    case kArmSE: return kArmNW;
    case kArmNE: return kArmSW;
    case kArmSW: return kArmNE;
    default: return 0;
    }
}

std::optional<Cell> armNeighbour(Cell c, std::uint8_t arm)
{
    const int dx = (arm == kArmNE || arm == kArmSE) ? 1 : -1;
    const int dy = (arm == kArmSW || arm == kArmSE) ? 1 : -1;
    const int x = c.x + dx, y = c.y + dy;
    if (!onGrid(x, y)) {
        return std::nullopt;
    }
    return Cell{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
}

std::uint8_t neighbourArms(const MapDocument& doc, Cell c)
{
    std::uint8_t out = 0;
    for (const auto arm : kAllArms) {
        if (const auto n = armNeighbour(c, arm); n && doc.walls.contains(*n)) {
            out |= arm;
        }
    }
    return out;
}

std::uint8_t joinedArms(const MapDocument& doc, Cell c)
{
    const auto it = doc.walls.find(c);
    if (it == doc.walls.end()) {
        return 0;
    }
    return static_cast<std::uint8_t>(armsOfFacing(it->second.direction & 0x7F) & neighbourArms(doc, c));
}

int wallVariantCount(const things::ThingDb& things, int material, int facing)
{
    if (material < 0 || static_cast<std::size_t>(material) >= things.walls.size() || facing < 0 || facing >= 15) {
        return 1;
    }
    return std::max(1, static_cast<int>(things.walls[static_cast<std::size_t>(material)].directions[static_cast<std::size_t>(facing)].size()));
}

int wallRandomVariations(const things::ThingDb& things, int material)
{
    if (material < 0 || static_cast<std::size_t>(material) >= things.walls.size()) {
        return 0;
    }
    return things.walls[static_cast<std::size_t>(material)].randomVariations;
}

std::uint8_t minimapGroupAt(const MapDocument& doc, Cell c)
{
    // As the game's save assigns it: the polygon holding the cell's middle, else the first
    // within 10 of it, else 100.
    const float px = c.x * kWallCellSize + 11, py = c.y * kWallCellSize + 11;
    std::map<std::uint32_t, WorldPoint> pts;
    for (const auto& p : doc.polygons.points) {
        pts[p.id] = {p.x, p.y};
    }
    const auto outline = [&](const map::Polygon& poly) {
        std::vector<WorldPoint> v;
        for (const auto id : poly.points) {
            if (const auto it = pts.find(id); it != pts.end()) v.push_back(it->second);
        }
        return v;
    };
    for (const auto& poly : doc.polygons.polygons) {
        const auto v = outline(poly);
        bool inside = false;
        for (std::size_t i = 0, j = v.empty() ? 0 : v.size() - 1; i < v.size(); j = i++) {
            if ((v[i].y > py) != (v[j].y > py) && px < (v[j].x - v[i].x) * (py - v[i].y) / (v[j].y - v[i].y) + v[i].x) {
                inside = !inside;
            }
        }
        if (inside) return poly.minimapGroup;
    }
    for (const auto& poly : doc.polygons.polygons) {
        const auto v = outline(poly);
        for (std::size_t i = 0; i < v.size(); ++i) {
            const WorldPoint a = v[i], b = v[(i + 1) % v.size()];
            const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            const float t = len2 > 0 ? std::clamp(((px - a.x) * dx + (py - a.y) * dy) / len2, 0.0f, 1.0f) : 0;
            if (std::hypot(a.x + t * dx - px, a.y + t * dy - py) <= 10) return poly.minimapGroup;
        }
    }
    return 100;
}

// --- Shapes ------------------------------------------------------------------------------------

std::vector<Cell> wallLine(Cell a, Cell b)
{
    // Along the diagonal (1, 1) or (1, -1) whose line passes nearer b.
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int along = (dx + dy) / 2;   // steps along (1, 1)
    const int across = (dx - dy) / 2;  // steps along (1, -1)
    std::vector<Cell> out;
    const bool main = std::abs(along) >= std::abs(across);
    const int n = main ? along : across;
    const int sx = n < 0 ? -1 : 1;
    for (int i = 0; i <= std::abs(n); ++i) {
        const int x = a.x + sx * i;
        const int y = a.y + (main ? sx : -sx) * i;
        if (onGrid(x, y)) out.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)});
    }
    return out;
}

std::vector<Cell> wallRectangle(Cell a, Cell b)
{
    // In diagonal coordinates u = x + y, v = x - y (both even for a wall cell of x + y even;
    // a cell of the other parity keeps its own): corners (u0, v0), (u1, v1).
    const int u0 = a.x + a.y, v0 = a.x - a.y;
    int u1 = b.x + b.y, v1 = b.x - b.y;
    // b onto a's parity (u and v change together).
    if (((u1 - u0) & 1) != 0) {
        u1 += 1;
        v1 += 1;
    }
    std::vector<Cell> out;
    std::set<Cell> seen;
    const auto add = [&](int u, int v) {
        if (((u + v) & 1) != 0) return;
        const int x = (u + v) / 2, y = (u - v) / 2;
        if (!onGrid(x, y)) return;
        const Cell c{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
        if (seen.insert(c).second) out.push_back(c);
    };
    const int us = u1 >= u0 ? 2 : -2, vs = v1 >= v0 ? 2 : -2;
    for (int u = u0;; u += us) {
        add(u, v0);
        add(u, v1);
        if (u == u1 || (us > 0 ? u > u1 : u < u1)) break;
    }
    for (int v = v0;; v += vs) {
        add(u0, v);
        add(u1, v);
        if (v == v1 || (vs > 0 ? v > v1 : v < v1)) break;
    }
    return out;
}

std::vector<Cell> cellsOf(const std::map<Cell, std::uint8_t>& arms)
{
    std::vector<Cell> out;
    for (const auto& [c, a] : arms) out.push_back(c);
    return out;
}

std::map<Cell, std::uint8_t> wallOutline(const MapDocument& doc, TileCell start, std::size_t limit)
{
    if (!doc.floor.tile(start.gx, start.gy)) {
        return {};
    }
    // A tile's vertices are wall cells: N (gx, gy), E (gx+1, gy+1), S (gx, gy+2), W (gx-1, gy+1).
    // The side neighbours and the vertices of the border with each: NE (N-E), SE (E-S), SW (S-W),
    // NW (W-N).
    struct Side {
        int dx, dy;
        int ax, ay, bx, by; // the border's two vertices, as offsets from the tile
        std::uint8_t armA, armB; // the arm along the border from each end
    };
    static constexpr Side kSides[4] = {
        {1, -1, 0, 0, 1, 1, kArmSE, kArmNW},
        {1, 1, 1, 1, 0, 2, kArmSW, kArmNE},
        {-1, 1, 0, 2, -1, 1, kArmNW, kArmSE},
        {-1, -1, -1, 1, 0, 0, kArmNE, kArmSW},
    };
    const auto wallArms = [&](int x, int y) -> std::uint8_t {
        if (x < 0 || y < 0 || x > 255 || y > 255) return 0;
        const auto it = doc.walls.find({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)});
        return it == doc.walls.end() ? 0 : armsOfFacing(it->second.direction & 0x7F);
    };
    std::set<TileCell> region{start};
    std::deque<TileCell> todo{start};
    while (!todo.empty()) {
        const TileCell t = todo.front();
        todo.pop_front();
        for (const auto& s : kSides) {
            const TileCell n{static_cast<std::int16_t>(t.gx + s.dx), static_cast<std::int16_t>(t.gy + s.dy)};
            if (region.contains(n) || !doc.floor.tile(n.gx, n.gy)) continue;
            if ((wallArms(t.gx + s.ax, t.gy + s.ay) & s.armA) || (wallArms(t.gx + s.bx, t.gy + s.by) & s.armB)) continue;
            region.insert(n);
            if (region.size() > limit) return {};
            todo.push_back(n);
        }
    }
    std::map<Cell, std::uint8_t> out;
    for (const auto& t : region) {
        for (const auto& s : kSides) {
            const TileCell n{static_cast<std::int16_t>(t.gx + s.dx), static_cast<std::int16_t>(t.gy + s.dy)};
            if (region.contains(n)) continue;
            // A wall at each end of the border, each with the arm along it.
            const int ax = t.gx + s.ax, ay = t.gy + s.ay, bx = t.gx + s.bx, by = t.gy + s.by;
            if (onGrid(ax, ay)) out[{static_cast<std::uint8_t>(ax), static_cast<std::uint8_t>(ay)}] |= s.armA;
            if (onGrid(bx, by)) out[{static_cast<std::uint8_t>(bx), static_cast<std::uint8_t>(by)}] |= s.armB;
        }
    }
    return out;
}

// --- Placing and removing ------------------------------------------------------------------------

int pickWallVariant(Cell c, std::uint32_t seed, int lo, int hi)
{
    if (hi <= lo) {
        return std::max(lo, 0);
    }
    const std::uint32_t h = mix((static_cast<std::uint32_t>(c.x) | (static_cast<std::uint32_t>(c.y) << 8)) ^ mix(seed + 0x9E3779B9u));
    return lo + static_cast<int>(h % static_cast<std::uint32_t>(hi - lo + 1));
}

namespace {

/// The changes so far, over the document: the state a cell will have.
struct Plan {
    const MapDocument& doc;
    std::map<Cell, std::optional<WallState>> changes;

    std::optional<WallState> at(Cell c) const
    {
        if (const auto it = changes.find(c); it != changes.end()) return it->second;
        return wallState(doc, c);
    }
    bool has(Cell c) const { return at(c).has_value(); }
};

Op opFrom(const MapDocument& doc, const std::map<Cell, std::optional<WallState>>& changes, std::string label)
{
    SetWalls part;
    for (const auto& [c, s] : changes) {
        if (wallState(doc, c) == s) continue;
        part.cells.push_back({c, s});
    }
    Op op;
    op.label = std::move(label);
    if (!part.cells.empty()) op.parts.push_back(std::move(part));
    return op;
}

} // namespace

Op placeWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::vector<Cell>& cells, const WallBrush& brush,
                const std::map<Cell, std::uint8_t>* fixedArms)
{
    Plan plan{doc, {}};
    std::set<Cell> placed;
    for (const Cell c : cells) {
        if (!onGrid(c.x, c.y) || ((c.x + c.y) & 1) != 0 || placed.contains(c)) continue;
        if (!brush.replace && doc.walls.contains(c)) continue;
        placed.insert(c);
        WallState s;
        s.wall.x = c.x;
        s.wall.y = c.y;
        s.wall.material = brush.material;
        s.wall.direction = brush.facing;
        s.wall.minimapGroup = brush.minimapGroup ? *brush.minimapGroup : minimapGroupAt(doc, c);
        s.destructible = brush.destructible;
        if (brush.secret) {
            s.secret = *brush.secret;
            s.secret->pos = {c.x, c.y};
        }
        plan.changes[c] = s;
    }
    if (placed.empty()) {
        return {};
    }
    // Facings: each placed wall joins its wall neighbours (with Smart Draw, only placed ones).
    const auto counts = [&](Cell n) { return brush.smartDraw ? placed.contains(n) : plan.has(n); };
    std::set<Cell> neighbours;
    for (const Cell c : placed) {
        WallState s = *plan.changes[c];
        std::uint8_t facing = brush.facing;
        const std::uint8_t* fixed = nullptr;
        if (fixedArms) {
            if (const auto it = fixedArms->find(c); it != fixedArms->end()) fixed = &it->second;
        }
        if (brush.autoFacing) {
            std::uint8_t arms = 0;
            for (const auto arm : kAllArms) {
                if (fixed && !(*fixed & arm)) continue;
                if (const auto n = armNeighbour(c, arm); n && (fixed || counts(*n))) {
                    arms |= arm;
                    if (!placed.contains(*n)) neighbours.insert(*n);
                }
            }
            facing = faceFor(arms, brush.facing);
        }
        s.wall.direction = facing;
        const int count = wallVariantCount(things, brush.material, facing);
        const int lo = std::clamp(brush.variantMin, 0, count - 1);
        const int hi = std::clamp(brush.variantMax, lo, count - 1);
        s.wall.variant = static_cast<std::uint8_t>(straightFacing(facing) && brush.autoVariation ? pickWallVariant(c, brush.seed, lo, hi)
                                                                                                 : (straightFacing(facing) ? lo : 0));
        s.window = brush.window;
        plan.changes[c] = s;
    }
    // Existing neighbours join the new walls: their arms that join something, plus the ones to them.
    if (brush.autoFacing && !brush.smartDraw) {
        for (const Cell n : neighbours) {
            const auto cur = wallState(doc, n);
            if (!cur) continue;
            std::uint8_t arms = 0;
            const std::uint8_t own = armsOfFacing(cur->wall.direction & 0x7F);
            for (const auto arm : kAllArms) {
                const auto m = armNeighbour(n, arm);
                if (!m || !plan.has(*m)) continue;
                // A new wall joins it if the new wall's piece points back; others as it joins now.
                const bool join = placed.contains(*m) ? (armsOfFacing(plan.at(*m)->wall.direction & 0x7F) & oppositeArm(arm)) != 0 : (own & arm) != 0;
                if (join) arms |= arm;
            }
            const std::uint8_t facing = faceFor(arms, cur->wall.direction & 0x7F);
            if (facing != (cur->wall.direction & 0x7F)) {
                plan.changes[n] = refaced(things, *cur, facing);
            }
        }
    }
    const std::string name = brush.material < things.walls.size() ? things.walls[brush.material].name : "wall";
    return opFrom(doc, plan.changes, "Place " + std::to_string(placed.size()) + (placed.size() == 1 ? " wall (" : " walls (") + name + ")");
}

Op removeWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::vector<Cell>& cells, bool refaceNeighbours)
{
    Plan plan{doc, {}};
    std::set<Cell> removed;
    for (const Cell c : cells) {
        if (doc.walls.contains(c) && removed.insert(c).second) plan.changes[c] = std::nullopt;
    }
    if (removed.empty()) {
        return {};
    }
    if (refaceNeighbours) {
        std::set<Cell> neighbours;
        for (const Cell c : removed) {
            for (const auto arm : kAllArms) {
                if (const auto n = armNeighbour(c, arm); n && plan.has(*n) && !removed.contains(*n)) neighbours.insert(*n);
            }
        }
        for (const Cell n : neighbours) {
            const auto cur = wallState(doc, n);
            const std::uint8_t own = armsOfFacing(cur->wall.direction & 0x7F);
            // The arms that joined a wall still standing.
            std::uint8_t arms = 0;
            for (const auto arm : kAllArms) {
                if (const auto m = armNeighbour(n, arm); m && (own & arm) && plan.has(*m)) arms |= arm;
            }
            if (arms == (own & neighbourArms(doc, n))) continue; // (it joined none of the removed)
            const std::uint8_t facing = faceFor(arms, cur->wall.direction & 0x7F);
            if (facing != (cur->wall.direction & 0x7F)) {
                plan.changes[n] = refaced(things, *cur, facing);
            }
        }
    }
    return opFrom(doc, plan.changes, "Remove " + std::to_string(removed.size()) + (removed.size() == 1 ? " wall" : " walls"));
}

Op setWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::map<Cell, WallState>& states, std::string label)
{
    std::map<Cell, std::optional<WallState>> changes;
    for (auto [c, s] : states) {
        if (!doc.walls.contains(c)) continue;
        s.wall.x = c.x;
        s.wall.y = c.y;
        const int facing = s.wall.direction & 0x7F;
        if (s.wall.variant >= wallVariantCount(things, s.wall.material, facing)) s.wall.variant = 0;
        if (s.secret) s.secret->pos = {c.x, c.y};
        changes[c] = s;
    }
    return opFrom(doc, changes, std::move(label));
}

} // namespace nox::mapedit
