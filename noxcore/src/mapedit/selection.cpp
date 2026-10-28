// The map editor's selection: geometry, picking, region selects, and delete / move / copy / paste.
#include "noxcore/mapedit/selection.hpp"
#include "noxcore/mapedit/markers.hpp"

#include "noxcore/io/byte_reader.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <span>
#include <variant>

namespace nox::mapedit {

namespace {

constexpr int kMaxWallX = 254; // x 255 ends the stored wall list
constexpr int kMaxWallY = 255;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

float dist2(float ax, float ay, float bx, float by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

/// Every full-grid tile the document has, with its position.
template <class F>
void forEachTile(const MapDocument& d, F f)
{
    for (int y = 0; y < FloorGrid::kSize; ++y) {
        for (int x = 0; x < FloorGrid::kSize; ++x) {
            const FloorPair& p = d.floor.pair(x, y);
            if (p.odd) {
                f(TileCell{static_cast<std::int16_t>(2 * x + 1), static_cast<std::int16_t>(2 * y - 1)}, *p.odd);
            }
            if (p.even) {
                f(TileCell{static_cast<std::int16_t>(2 * x), static_cast<std::int16_t>(2 * y)}, *p.even);
            }
        }
    }
}

/// Whether `id`'s owner chain reaches an object in `set`.
bool ownedByAny(const MapDocument& d, ObjId id, const std::set<ObjId>& set)
{
    for (ObjId p = d.objects.at(id).parent; p != 0;) {
        if (set.contains(p)) {
            return true;
        }
        const auto it = d.objects.find(p);
        p = it == d.objects.end() ? 0 : it->second.parent;
    }
    return false;
}

void subtree(const MapDocument& d, ObjId id, std::vector<ObjectEntry>& out)
{
    out.push_back({id, d.objects.at(id)});
    for (const ObjId child : d.childrenOf(id)) {
        subtree(d, child, out);
    }
}

/// `c` moved to x + y even (one cell right, or left at the grid's edge).
Cell evenCell(Cell c)
{
    if (((c.x + c.y) & 1) != 0) {
        c.x = static_cast<std::uint8_t>(c.x < kMaxWallX ? c.x + 1 : c.x - 1);
    }
    return c;
}

// --- base64 -----------------------------------------------------------------------------------

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(std::span<const std::uint8_t> b)
{
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4 + b.size() / 57 + 1);
    for (std::size_t i = 0; i < b.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t(b[i]) << 16) | (i + 1 < b.size() ? std::uint32_t(b[i + 1]) << 8 : 0) |
                                (i + 2 < b.size() ? std::uint32_t(b[i + 2]) : 0);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < b.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < b.size() ? kB64[v & 63] : '=';
        if ((i / 3 + 1) % 19 == 0) {
            out += '\n'; // (76-character lines)
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> unbase64(std::string_view s)
{
    std::vector<std::uint8_t> out;
    std::uint32_t v = 0;
    int bits = 0;
    for (const char c : s) {
        int d = -1;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=') break;
        else if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        else return std::nullopt;
        v = (v << 6) | static_cast<std::uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(v >> bits));
        }
    }
    return out;
}

constexpr std::string_view kClipHeader = "NoX Core map clip 1\n";

} // namespace

// --- Geometry -------------------------------------------------------------------------------------

WorldPoint wallCentre(Cell c)
{
    return {c.x * kWallCellSize + kWallCellSize / 2, c.y * kWallCellSize + kWallCellSize / 2};
}

WorldPoint tileCentre(TileCell t)
{
    return {t.gx * kWallCellSize + 12, t.gy * kWallCellSize + 35};
}

std::optional<Cell> wallCellAt(float x, float y)
{
    if (x < 0 || y < 0) {
        return std::nullopt;
    }
    const int cx = static_cast<int>(x / kWallCellSize);
    const int cy = static_cast<int>(y / kWallCellSize);
    if (cx > 255 || cy > 255) {
        return std::nullopt;
    }
    return Cell{static_cast<std::uint8_t>(cx), static_cast<std::uint8_t>(cy)};
}

bool validTile(int gx, int gy)
{
    return ((gx + gy) & 1) == 0 && gx >= 0 && gx <= 255 && gy >= -1 && gy <= 254;
}

std::optional<TileCell> tileAt(float x, float y)
{
    // Rotated onto the diamonds' axes, the nearest centre is a rounding away.
    const float u = (x - 12) / kWallCellSize;
    const float v = (y - 35) / kWallCellSize;
    const int p = static_cast<int>(std::lround((u + v) / 2));
    const int q = static_cast<int>(std::lround((u - v) / 2));
    const int gx = p + q;
    const int gy = p - q;
    if (!validTile(gx, gy)) {
        return std::nullopt;
    }
    return TileCell{static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy)};
}

// --- Selection ------------------------------------------------------------------------------------

void Selection::add(const Selection& s)
{
    objects.insert(s.objects.begin(), s.objects.end());
    walls.insert(s.walls.begin(), s.walls.end());
    tiles.insert(s.tiles.begin(), s.tiles.end());
    waypoints.insert(s.waypoints.begin(), s.waypoints.end());
    polygons.insert(s.polygons.begin(), s.polygons.end());
}

void Selection::remove(const Selection& s)
{
    for (const auto v : s.objects) objects.erase(v);
    for (const auto v : s.walls) walls.erase(v);
    for (const auto v : s.tiles) tiles.erase(v);
    for (const auto v : s.waypoints) waypoints.erase(v);
    for (const auto v : s.polygons) polygons.erase(v);
}

void Selection::toggle(const Selection& s)
{
    const auto flip = [](auto& set, const auto& from) {
        for (const auto& v : from) {
            if (!set.erase(v)) {
                set.insert(v);
            }
        }
    };
    flip(objects, s.objects);
    flip(walls, s.walls);
    flip(tiles, s.tiles);
    flip(waypoints, s.waypoints);
    flip(polygons, s.polygons);
}

void Selection::prune(const MapDocument& d)
{
    std::erase_if(objects, [&](ObjId id) { return !d.objects.contains(id); });
    std::erase_if(walls, [&](Cell c) { return !d.walls.contains(c); });
    std::erase_if(tiles, [&](TileCell t) { return d.floor.tile(t.gx, t.gy) == nullptr; });
    std::erase_if(waypoints, [&](std::uint32_t id) {
        return std::ranges::find(d.waypoints.waypoints, id, &map::Waypoint::id) == d.waypoints.waypoints.end();
    });
    std::erase_if(polygons, [&](std::uint32_t key) { return !d.polygonIndex(key); });
}

bool Region::contains(float x, float y) const
{
    if (points.empty()) {
        return false;
    }
    if (shape == Shape::Box || shape == Shape::Box45) {
        const WorldPoint a = points.front();
        const WorldPoint b = points.back();
        if (shape == Shape::Box) {
            return x >= std::min(a.x, b.x) && x <= std::max(a.x, b.x) && y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y);
        }
        // Along the diagonals: r = x + y, s = x - y.
        const float r = x + y, s = x - y;
        const float ra = a.x + a.y, rb = b.x + b.y, sa = a.x - a.y, sb = b.x - b.y;
        return r >= std::min(ra, rb) && r <= std::max(ra, rb) && s >= std::min(sa, sb) && s <= std::max(sa, sb);
    }
    // Lasso: even-odd rule.
    bool in = false;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const WorldPoint& p = points[i];
        const WorldPoint& q = points[j];
        if ((p.y > y) != (q.y > y) && x < (q.x - p.x) * (y - p.y) / (q.y - p.y) + p.x) {
            in = !in;
        }
    }
    return in;
}

std::vector<WorldPoint> Region::outline() const
{
    if (points.empty() || shape == Shape::Lasso) {
        return points;
    }
    const WorldPoint a = points.front();
    const WorldPoint b = points.back();
    if (shape == Shape::Box) {
        return {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}};
    }
    const float ra = a.x + a.y, rb = b.x + b.y, sa = a.x - a.y, sb = b.x - b.y;
    const auto at = [](float r, float s) { return WorldPoint{(r + s) / 2, (r - s) / 2}; };
    return {at(ra, sa), at(ra, sb), at(rb, sb), at(rb, sa)};
}

Selection pickAt(const MapDocument& d, float x, float y, float radius, const KindFilter& f)
{
    Selection s;
    const float r2 = radius * radius;
    if (f.waypoints) {
        float best = r2;
        std::optional<std::uint32_t> id;
        for (const auto& w : d.waypoints.waypoints) {
            if (const float dd = dist2(w.x, w.y, x, y); dd <= best) {
                best = dd;
                id = w.id;
            }
        }
        if (id) {
            s.waypoints.insert(*id);
            return s;
        }
    }
    if (f.objects) {
        float best = r2;
        std::optional<ObjId> id;
        for (const auto& [oid, o] : d.objects) {
            if (o.parent != 0) {
                continue;
            }
            if (const float dd = dist2(o.header.x, o.header.y, x, y); dd <= best) {
                best = dd;
                id = oid;
            }
        }
        if (id) {
            s.objects.insert(*id);
            return s;
        }
    }
    if (f.walls) {
        if (const auto c = wallCellAt(x, y); c && d.walls.contains(*c)) {
            s.walls.insert(*c);
            return s;
        }
    }
    if (f.tiles) {
        if (const auto t = tileAt(x, y); t && d.floor.tile(t->gx, t->gy)) {
            s.tiles.insert(*t);
        }
    }
    return s;
}

Selection selectIn(const MapDocument& d, const Region& region, const KindFilter& f)
{
    Selection s;
    if (f.objects) {
        for (const auto& [id, o] : d.objects) {
            if (o.parent == 0 && region.contains(o.header.x, o.header.y)) {
                s.objects.insert(id);
            }
        }
    }
    if (f.waypoints) {
        for (const auto& w : d.waypoints.waypoints) {
            if (region.contains(w.x, w.y)) {
                s.waypoints.insert(w.id);
            }
        }
    }
    if (f.walls) {
        for (const auto& [c, w] : d.walls) {
            const WorldPoint p = wallCentre(c);
            if (region.contains(p.x, p.y)) {
                s.walls.insert(c);
            }
        }
    }
    if (f.tiles) {
        forEachTile(d, [&](TileCell t, const map::FloorTile&) {
            const WorldPoint p = tileCentre(t);
            if (region.contains(p.x, p.y)) {
                s.tiles.insert(t);
            }
        });
    }
    return s;
}

Selection selectAll(const MapDocument& d, const KindFilter& f)
{
    Selection s;
    if (f.objects) {
        for (const auto& [id, o] : d.objects) {
            if (o.parent == 0) {
                s.objects.insert(id);
            }
        }
    }
    if (f.waypoints) {
        for (const auto& w : d.waypoints.waypoints) {
            s.waypoints.insert(w.id);
        }
    }
    if (f.walls) {
        for (const auto& [c, w] : d.walls) {
            s.walls.insert(c);
        }
    }
    if (f.tiles) {
        forEachTile(d, [&](TileCell t, const map::FloorTile&) { s.tiles.insert(t); });
    }
    return s;
}

Selection selectType(const MapDocument& d, std::string_view type)
{
    Selection s;
    for (const auto& [id, o] : d.objects) {
        if (o.parent == 0 && o.type == type) {
            s.objects.insert(id);
        }
    }
    return s;
}

std::optional<std::pair<WorldPoint, WorldPoint>> bounds(const MapDocument& d, const Selection& s)
{
    bool any = false;
    WorldPoint lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    const auto take = [&](float x0, float y0, float x1, float y1) {
        lo = {std::min(lo.x, x0), std::min(lo.y, y0)};
        hi = {std::max(hi.x, x1), std::max(hi.y, y1)};
        any = true;
    };
    for (const ObjId id : s.objects) {
        if (const auto it = d.objects.find(id); it != d.objects.end()) {
            take(it->second.header.x, it->second.header.y, it->second.header.x, it->second.header.y);
        }
    }
    for (const auto& w : d.waypoints.waypoints) {
        if (s.waypoints.contains(w.id)) {
            take(w.x, w.y, w.x, w.y);
        }
    }
    for (const Cell c : s.walls) {
        take(c.x * kWallCellSize, c.y * kWallCellSize, (c.x + 1) * kWallCellSize, (c.y + 1) * kWallCellSize);
    }
    for (const TileCell t : s.tiles) {
        const WorldPoint p = tileCentre(t);
        take(p.x - 23, p.y - 23, p.x + 23, p.y + 23);
    }
    for (const std::uint32_t key : s.polygons) {
        if (const auto i = d.polygonIndex(key)) {
            for (const auto& c : polygonCorners(d, *i)) take(c.x, c.y, c.x, c.y);
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return std::pair{lo, hi};
}

// --- Ops ------------------------------------------------------------------------------------------

Op deleteOp(const MapDocument& d, const Selection& s)
{
    Op op;
    RemoveObjects objects;
    std::set<ObjId> chosen;
    for (const ObjId id : s.objects) {
        if (d.objects.contains(id)) {
            chosen.insert(id);
        }
    }
    for (const ObjId id : chosen) {
        if (!ownedByAny(d, id, chosen)) {
            objects.ids.push_back(id);
        }
    }
    SetWalls walls;
    for (const Cell c : s.walls) {
        if (d.walls.contains(c)) {
            walls.cells.push_back({c, std::nullopt});
        }
    }
    SetTiles tiles;
    for (const TileCell t : s.tiles) {
        if (d.floor.tile(t.gx, t.gy)) {
            tiles.tiles.push_back({t.gx, t.gy, std::nullopt});
        }
    }
    SetWaypoints waypoints;
    std::set<std::uint32_t> gone;
    for (const auto& w : d.waypoints.waypoints) {
        if (s.waypoints.contains(w.id)) {
            gone.insert(w.id);
        }
    }
    if (!gone.empty()) {
        // Links to the deleted ones go first (the game fails a load on a link to nothing).
        for (const auto& w : d.waypoints.waypoints) {
            if (gone.contains(w.id)) {
                continue;
            }
            if (std::ranges::any_of(w.links, [&](const map::WaypointLink& l) { return gone.contains(l.target); })) {
                map::Waypoint kept = w;
                std::erase_if(kept.links, [&](const map::WaypointLink& l) { return gone.contains(l.target); });
                waypoints.waypoints.push_back({w.id, std::move(kept)});
            }
        }
        for (const std::uint32_t id : gone) {
            waypoints.waypoints.push_back({id, std::nullopt});
        }
    }
    Op polygons = deletePolygonsOp(d, s.polygons);
    const std::size_t n = objects.ids.size() + walls.cells.size() + tiles.tiles.size() + gone.size() + (polygons.empty() ? 0 : s.polygons.size());
    op.label = "Delete " + std::to_string(n) + (n == 1 ? " item" : " items");
    if (!objects.ids.empty()) op.parts.push_back(std::move(objects));
    if (!walls.cells.empty()) op.parts.push_back(std::move(walls));
    if (!tiles.tiles.empty()) op.parts.push_back(std::move(tiles));
    if (!waypoints.waypoints.empty()) op.parts.push_back(std::move(waypoints));
    for (auto& p : polygons.parts) op.parts.push_back(std::move(p));
    return op;
}

std::pair<int, int> cellStep(const MapDocument& d, const Selection& s, float dx, float dy)
{
    int cx = static_cast<int>(std::lround(dx / kWallCellSize));
    int cy = static_cast<int>(std::lround(dy / kWallCellSize));
    if (((cx + cy) & 1) != 0) {
        // The axis whose rounding was further off takes the extra cell.
        const float ex = dx / kWallCellSize - static_cast<float>(cx);
        const float ey = dy / kWallCellSize - static_cast<float>(cy);
        if (std::fabs(ex) >= std::fabs(ey)) {
            cx += ex >= 0 ? 1 : -1;
        } else {
            cy += ey >= 0 ? 1 : -1;
        }
    }
    int loX = -1000, hiX = 1000, loY = -1000, hiY = 1000;
    for (const Cell c : s.walls) {
        loX = std::max(loX, -c.x);
        hiX = std::min(hiX, kMaxWallX - c.x);
        loY = std::max(loY, -c.y);
        hiY = std::min(hiY, kMaxWallY - c.y);
    }
    for (const TileCell t : s.tiles) {
        loX = std::max(loX, -t.gx);
        hiX = std::min(hiX, 255 - t.gx);
        loY = std::max(loY, -1 - t.gy);
        hiY = std::min(hiY, 254 - t.gy);
    }
    (void)d;
    const int x = std::clamp(cx, loX, hiX);
    const int y = std::clamp(cy, loY, hiY);
    if (x != cx || y != cy) {
        cx = x;
        cy = y;
        if (((cx + cy) & 1) != 0) {
            // Clamped onto an odd step: one cell back toward no move (always in range).
            if (cx != 0) cx += cx > 0 ? -1 : 1;
            else cy += cy > 0 ? -1 : 1;
        }
    }
    return {cx, cy};
}

Op moveOp(const MapDocument& d, const Selection& s, float dx, float dy, Selection* moved)
{
    Op op;
    // Objects and waypoints stay on the map.
    float lo = 0, hi = kWorldSize - 1;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    const auto note = [&](float x, float y) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    };
    for (const ObjId id : s.objects) {
        if (const auto it = d.objects.find(id); it != d.objects.end()) {
            note(it->second.header.x, it->second.header.y);
        }
    }
    for (const auto& w : d.waypoints.waypoints) {
        if (s.waypoints.contains(w.id)) {
            note(w.x, w.y);
        }
    }
    for (const std::uint32_t key : s.polygons) {
        if (const auto i = d.polygonIndex(key)) {
            for (const auto& c : polygonCorners(d, *i)) note(c.x, c.y);
        }
    }
    if (minX <= maxX) {
        dx = std::clamp(dx, lo - minX, hi - maxX);
        dy = std::clamp(dy, lo - minY, hi - maxY);
    }
    MoveObjects objects;
    for (const ObjId id : s.objects) {
        if (const auto it = d.objects.find(id); it != d.objects.end()) {
            objects.moves.push_back({id, it->second.header.x + dx, it->second.header.y + dy});
        }
    }
    SetWaypoints waypoints;
    for (const auto& w : d.waypoints.waypoints) {
        if (s.waypoints.contains(w.id)) {
            map::Waypoint m = w;
            m.x += dx;
            m.y += dy;
            waypoints.waypoints.push_back({w.id, std::move(m)});
        }
    }
    const auto [cx, cy] = cellStep(d, s, dx, dy);
    SetWalls walls;
    SetTiles tiles;
    Selection after;
    after.objects = s.objects;
    after.waypoints = s.waypoints;
    after.polygons = s.polygons;
    Op polygons = movePolygonsOp(d, s.polygons, dx, dy);
    if (cx != 0 || cy != 0) {
        // Everything lifted first, then put down, so a selection can move over itself.
        std::vector<std::pair<Cell, WallState>> lifted;
        for (const Cell c : s.walls) {
            if (auto st = wallState(d, c)) {
                lifted.emplace_back(c, std::move(*st));
                walls.cells.push_back({c, std::nullopt});
            }
        }
        for (auto& [c, st] : lifted) {
            const Cell to{static_cast<std::uint8_t>(c.x + cx), static_cast<std::uint8_t>(c.y + cy)};
            walls.cells.push_back({to, std::move(st)});
            after.walls.insert(to);
        }
        std::vector<std::pair<TileCell, map::FloorTile>> raised;
        for (const TileCell t : s.tiles) {
            if (const auto* tile = d.floor.tile(t.gx, t.gy)) {
                raised.emplace_back(t, *tile);
                tiles.tiles.push_back({t.gx, t.gy, std::nullopt});
            }
        }
        for (auto& [t, tile] : raised) {
            const TileCell to{static_cast<std::int16_t>(t.gx + cx), static_cast<std::int16_t>(t.gy + cy)};
            tiles.tiles.push_back({to.gx, to.gy, std::move(tile)});
            after.tiles.insert(to);
        }
    } else {
        after.walls = s.walls;
        after.tiles = s.tiles;
    }
    const bool still = dx == 0 && dy == 0;
    if (!still && !objects.moves.empty()) op.parts.push_back(std::move(objects));
    if (!still && !waypoints.waypoints.empty()) op.parts.push_back(std::move(waypoints));
    if (!walls.cells.empty()) op.parts.push_back(std::move(walls));
    if (!tiles.tiles.empty()) op.parts.push_back(std::move(tiles));
    for (auto& p : polygons.parts) op.parts.push_back(std::move(p));
    const std::size_t n = s.size();
    op.label = "Move " + std::to_string(n) + (n == 1 ? " item" : " items");
    if (moved) {
        *moved = std::move(after);
    }
    return op;
}

// --- Clipboard ------------------------------------------------------------------------------------

Clip copySelection(const MapDocument& d, const Selection& s, const CopyLayers& layers)
{
    Clip clip;
    Selection taken;
    if (layers.walls) {
        for (const Cell c : s.walls) {
            if (auto st = wallState(d, c)) {
                clip.walls.emplace_back(c, std::move(*st));
                taken.walls.insert(c);
            }
        }
    }
    if (layers.tiles) {
        for (const TileCell t : s.tiles) {
            if (const auto* tile = d.floor.tile(t.gx, t.gy)) {
                clip.tiles.emplace_back(t, *tile);
                taken.tiles.insert(t);
            }
        }
    }
    if (layers.objects) {
        std::set<ObjId> chosen;
        for (const ObjId id : s.objects) {
            if (d.objects.contains(id)) {
                chosen.insert(id);
            }
        }
        for (const ObjId id : chosen) {
            if (ownedByAny(d, id, chosen)) {
                continue;
            }
            const std::size_t first = clip.objects.size();
            subtree(d, id, clip.objects);
            clip.objects[first].object.parent = 0;
            taken.objects.insert(id);
        }
    }
    if (layers.waypoints) {
        for (const auto& w : d.waypoints.waypoints) {
            if (s.waypoints.contains(w.id)) {
                map::Waypoint c = w;
                std::erase_if(c.links, [&](const map::WaypointLink& l) { return !s.waypoints.contains(l.target); });
                clip.waypoints.push_back(std::move(c));
                taken.waypoints.insert(w.id);
            }
        }
    }
    if (const auto b = bounds(d, taken)) {
        const auto c = wallCellAt(std::clamp((b->first.x + b->second.x) / 2, 0.0f, kWorldSize - 1),
                                  std::clamp((b->first.y + b->second.y) / 2, 0.0f, kWorldSize - 1));
        clip.anchor = evenCell(c.value_or(Cell{}));
    }
    return clip;
}

namespace {

/// The clip's objects and waypoints moved by (wx, wy) world units, as new ones (new ids, extents,
/// the lowest free waypoint ids; links inside the clip follow them).
void pasteMarkers(const MapDocument& d, const Clip& clip, float wx, float wy, Selection& placed, AddObjects& objects, SetWaypoints& waypoints)
{
    {
        std::map<ObjId, ObjId> ids;               // clip id -> new id
        std::map<std::uint32_t, std::uint32_t> extents; // clip extent -> new extent
        ObjId nextId = d.nextObjId;
        std::uint32_t nextExtent = d.nextFreeExtent();
        std::uint64_t order = 0;
        bool anyRoot = false;
        for (const auto& [id, o] : d.objects) {
            if (o.parent == 0) {
                order = std::max(order, o.order);
                anyRoot = true;
            }
        }
        for (const ObjectEntry& e : clip.objects) {
            DocObject o = e.object;
            if (o.parent != 0) {
                const auto owner = ids.find(o.parent);
                if (owner == ids.end()) {
                    continue; // (its owner was left out)
                }
                o.parent = owner->second;
            } else {
                const float x = o.header.x + wx, y = o.header.y + wy;
                if (x < 0 || y < 0 || x >= kWorldSize || y >= kWorldSize) {
                    continue;
                }
                o.order = anyRoot ? ++order : order++;
                anyRoot = true;
            }
            o.header.x += wx;
            o.header.y += wy;
            extents[o.header.extent] = nextExtent;
            o.header.extent = nextExtent++;
            o.header.scriptId = 0; // (assigned when the map loads)
            o.tocId = 0;
            o.padding.clear();
            o.sizeHi = 0;
            const ObjId newId = nextId++;
            ids[e.id] = newId;
            objects.objects.push_back({newId, std::move(o)});
            if (e.object.parent == 0) {
                placed.objects.insert(newId);
            }
        }
        // Links between copied objects follow them.
        const auto relink = [&](std::uint32_t& extent) {
            if (const auto it = extents.find(extent); extent != 0 && it != extents.end()) {
                extent = it->second;
            }
        };
        for (ObjectEntry& e : objects.objects) {
            std::visit(Overloaded{
                           [&](map::xfer::Transporter& v) { relink(v.targetExtent); },
                           [&](map::xfer::Elevator& v) { relink(v.linkedExtent); },
                           [&](map::xfer::ElevatorShaft& v) { relink(v.linkedExtent); },
                           [&](map::xfer::Mover& v) { relink(v.movedExtent); },
                           [&](map::xfer::InvisibleLight& v) { relink(v.target); },
                           [](auto&) {},
                       },
                       e.object.data);
        }
        // Script names kept unique (scripts find the first object of a name), and the names the
        // copies give each other (a creature's escort) following them.
        std::set<std::string, std::less<>> used;
        for (const auto& [id, o] : d.objects) {
            if (o.header.details && !o.header.details->scriptName.empty()) used.emplace(scriptNameKey(o.header.details->scriptName));
        }
        std::map<std::string, std::string, std::less<>> renamed; // a copy's old name (whole) -> its new one
        std::set<std::string, std::less<>> kept;                 // names a copy kept (the one a lookup finds)
        for (ObjectEntry& e : objects.objects) {
            if (!e.object.header.details) continue;
            std::string& name = e.object.header.details->scriptName;
            if (name.empty()) continue;
            const std::string now = uniqueScriptName(name, used);
            used.emplace(scriptNameKey(now));
            if (now == name) {
                kept.insert(name);
            } else {
                if (!kept.contains(name)) renamed.try_emplace(name, now);
                name = now;
            }
        }
        if (!renamed.empty()) {
            for (ObjectEntry& e : objects.objects) {
                const auto follow = [&](std::string& escort) {
                    // (getObjectByScrName: a name with a ':' is matched whole, one without
                    // against what follows an object's own first ':'.)
                    if (escort.empty() || escort.starts_with("**")) return;
                    const bool bare = escort.find(':') == std::string::npos;
                    for (const auto& [was, now] : renamed) {
                        if (bare ? scriptNameKey(was) == escort : was == escort) {
                            escort = bare ? std::string(scriptNameKey(now)) : now;
                            return;
                        }
                    }
                };
                if (auto* m = std::get_if<map::xfer::Monster>(&e.object.data)) follow(m->unknown341);
                if (auto* n = std::get_if<map::xfer::Npc>(&e.object.data)) follow(n->unknown341);
            }
        }
    }

    std::map<std::uint32_t, std::uint32_t> waypointIds; // clip id -> new id
    {
        std::set<std::uint32_t> used;
        for (const auto& w : d.waypoints.waypoints) {
            used.insert(w.id);
        }
        std::map<std::uint32_t, std::uint32_t>& ids = waypointIds;
        std::uint32_t next = 1;
        std::vector<map::Waypoint> kept;
        // Waypoint names kept unique too (the scripts' waypoint lookup finds the first of a name).
        std::set<std::string, std::less<>> names;
        for (const auto& w : d.waypoints.waypoints) {
            if (!w.name.empty()) names.emplace(scriptNameKey(w.name));
        }
        for (const auto& w : clip.waypoints) {
            const float x = w.x + wx, y = w.y + wy;
            if (x < 0 || y < 0 || x >= kWorldSize || y >= kWorldSize) {
                continue;
            }
            while (used.contains(next)) {
                ++next;
            }
            ids[w.id] = next;
            used.insert(next);
            map::Waypoint c = w;
            c.id = next;
            c.x = x;
            c.y = y;
            if (!c.name.empty()) {
                c.name = uniqueScriptName(c.name, names);
                names.emplace(scriptNameKey(c.name));
            }
            kept.push_back(std::move(c));
        }
        for (auto& w : kept) {
            std::erase_if(w.links, [&](const map::WaypointLink& l) { return !ids.contains(l.target); });
            for (auto& l : w.links) {
                l.target = ids.at(l.target);
            }
            placed.waypoints.insert(w.id);
            const std::uint32_t id = w.id;
            waypoints.waypoints.push_back({id, std::move(w)});
        }
    }
    // A copied mover's waypoints follow the copied waypoints (ones left behind it keeps).
    for (ObjectEntry& e : objects.objects) {
        if (auto* m = std::get_if<map::xfer::Mover>(&e.object.data)) {
            for (std::uint32_t* w : {&m->waypoint, &m->waypointStart, &m->waypointEnd}) {
                if (const auto it = waypointIds.find(*w); *w != 0 && it != waypointIds.end()) *w = it->second;
            }
        }
    }
}

} // namespace

std::string_view scriptNameKey(std::string_view name)
{
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string uniqueScriptName(std::string_view name, const std::set<std::string, std::less<>>& used)
{
    const std::string_view key = scriptNameKey(name);
    if (!used.contains(key)) return std::string(name);
    const std::string_view scope = name.substr(0, name.size() - key.size()); // ("Map:" or nothing)
    // The base: a trailing "_N" dropped (a copy of Guard_2 is Guard_3, not Guard_2_2).
    std::string_view base = key;
    if (const auto u = base.rfind('_'); u != std::string_view::npos && u + 1 < base.size() &&
                                        std::ranges::all_of(base.substr(u + 1), [](char c) { return c >= '0' && c <= '9'; })) {
        base = base.substr(0, u);
    }
    for (std::uint32_t n = 2;; ++n) {
        std::string k = std::string(base) + "_" + std::to_string(n);
        if (!used.contains(k)) return std::string(scope) + k;
    }
}

Op pasteOp(const MapDocument& d, const Clip& clip, Cell at, Selection* pasted)
{
    at = evenCell(at);
    const int ox = at.x - clip.anchor.x;
    const int oy = at.y - clip.anchor.y;
    const float wx = static_cast<float>(ox) * kWallCellSize;
    const float wy = static_cast<float>(oy) * kWallCellSize;
    Selection placed;
    Op op;

    SetWalls walls;
    for (const auto& [c, st] : clip.walls) {
        const int x = c.x + ox, y = c.y + oy;
        if (x < 0 || x > kMaxWallX || y < 0 || y > kMaxWallY) {
            continue;
        }
        const Cell to{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
        walls.cells.push_back({to, st});
        placed.walls.insert(to);
    }
    SetTiles tiles;
    for (const auto& [t, tile] : clip.tiles) {
        const int gx = t.gx + ox, gy = t.gy + oy;
        if (!validTile(gx, gy)) {
            continue;
        }
        tiles.tiles.push_back({static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy), tile});
        placed.tiles.insert({static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy)});
    }
    AddObjects objects;
    SetWaypoints waypoints;
    pasteMarkers(d, clip, wx, wy, placed, objects, waypoints);

    const std::size_t n = placed.size();
    op.label = "Paste " + std::to_string(n) + (n == 1 ? " item" : " items");
    if (!walls.cells.empty()) op.parts.push_back(std::move(walls));
    if (!tiles.tiles.empty()) op.parts.push_back(std::move(tiles));
    if (!objects.objects.empty()) op.parts.push_back(std::move(objects));
    if (!waypoints.waypoints.empty()) op.parts.push_back(std::move(waypoints));
    if (pasted) {
        *pasted = std::move(placed);
    }
    return op;
}

Op pasteMarkersOp(const MapDocument& d, const Clip& clip, float dx, float dy, Selection* pasted)
{
    Selection placed;
    AddObjects objects;
    SetWaypoints waypoints;
    pasteMarkers(d, clip, dx, dy, placed, objects, waypoints);
    Op op;
    const std::size_t n = placed.size();
    op.label = "Paste " + std::to_string(n) + (n == 1 ? " item" : " items");
    if (!objects.objects.empty()) op.parts.push_back(std::move(objects));
    if (!waypoints.waypoints.empty()) op.parts.push_back(std::move(waypoints));
    if (pasted) {
        *pasted = std::move(placed);
    }
    return op;
}

std::string clipToText(const Clip& clip, const things::ThingDb& things)
{
    // The clip travels as an op's byte form: its label the anchor, its parts the content.
    Op op;
    op.label = std::to_string(clip.anchor.x) + " " + std::to_string(clip.anchor.y);
    SetWalls walls;
    for (const auto& [c, st] : clip.walls) {
        walls.cells.push_back({c, st});
    }
    SetTiles tiles;
    for (const auto& [t, tile] : clip.tiles) {
        tiles.tiles.push_back({t.gx, t.gy, tile});
    }
    SetWaypoints waypoints;
    for (const auto& w : clip.waypoints) {
        waypoints.waypoints.push_back({w.id, w});
    }
    op.parts.push_back(std::move(walls));
    op.parts.push_back(std::move(tiles));
    op.parts.push_back(AddObjects{clip.objects});
    op.parts.push_back(std::move(waypoints));
    return std::string(kClipHeader) + base64(serializeOp(op, things));
}

std::optional<Clip> clipFromText(std::string_view text, const things::ThingDb& things)
{
    if (!text.starts_with(kClipHeader)) {
        return std::nullopt;
    }
    const auto bytes = unbase64(text.substr(kClipHeader.size()));
    if (!bytes) {
        return std::nullopt;
    }
    try {
        const Op op = parseOp(*bytes, things);
        if (op.parts.size() != 4) {
            return std::nullopt;
        }
        Clip clip;
        int ax = -1, ay = -1;
        const char* end = op.label.data() + op.label.size();
        const auto [p, ec] = std::from_chars(op.label.data(), end, ax);
        if (ec != std::errc() || p == end || std::from_chars(p + 1, end, ay).ec != std::errc() || ax < 0 || ax > 255 || ay < 0 ||
            ay > 255) {
            return std::nullopt;
        }
        clip.anchor = {static_cast<std::uint8_t>(ax), static_cast<std::uint8_t>(ay)};
        for (const auto& c : std::get<SetWalls>(op.parts[0]).cells) {
            if (c.state) {
                clip.walls.emplace_back(c.cell, *c.state);
            }
        }
        for (const auto& t : std::get<SetTiles>(op.parts[1]).tiles) {
            if (t.tile) {
                clip.tiles.emplace_back(TileCell{t.gx, t.gy}, *t.tile);
            }
        }
        clip.objects = std::get<AddObjects>(op.parts[2]).objects;
        for (const auto& w : std::get<SetWaypoints>(op.parts[3]).waypoints) {
            if (w.waypoint) {
                clip.waypoints.push_back(*w.waypoint);
            }
        }
        return clip;
    } catch (const std::exception&) {
        return std::nullopt; // (not ours, or types this thing.bin hasn't)
    }
}

} // namespace nox::mapedit
