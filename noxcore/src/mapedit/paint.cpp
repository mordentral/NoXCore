// Floor painting and auto-edge (paint.hpp).
#include "noxcore/mapedit/paint.hpp"

#include "noxcore/mapedit/walls.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <sstream>

namespace nox::mapedit {

namespace {

#include "edge_defaults.inc"

int mod(int a, int m)
{
    return ((a % m) + m) % m;
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

std::uint32_t hashCell(int gx, int gy, std::uint32_t a, std::uint32_t b)
{
    std::uint32_t h = mix(static_cast<std::uint32_t>(gx + 1024) * 0x9E3779B1u ^ static_cast<std::uint32_t>(gy + 1024) * 0x85EBCA77u);
    h = mix(h ^ (a + 0x632BE5ABu));
    return mix(h ^ (b * 0xC2B2AE3Du));
}

int floorIndex(const things::ThingDb& things, std::string_view name)
{
    for (std::size_t i = 0; i < things.floors.size(); ++i) {
        if (things.floors[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int edgeIndex(const things::ThingDb& things, std::string_view name)
{
    for (std::size_t i = 0; i < things.edges.size(); ++i) {
        if (things.edges[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::uint8_t wallArmsAt(const MapDocument& doc, int x, int y)
{
    if (x < 0 || y < 0 || x > 255 || y > 255) return 0;
    const auto it = doc.walls.find({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)});
    return it == doc.walls.end() ? 0 : armsOfFacing(it->second.direction & 0x7F);
}

TileCell offset(TileCell t, int n)
{
    return {static_cast<std::int16_t>(t.gx + kTileNeighbours[n].first), static_cast<std::int16_t>(t.gy + kTileNeighbours[n].second)};
}

const map::FloorTile* tileAtCell(const MapDocument& doc, TileCell t)
{
    return validTile(t.gx, t.gy) ? doc.floor.tile(t.gx, t.gy) : nullptr;
}

} // namespace

// --- Edge shapes and directions ------------------------------------------------------------------

int edgeShapeOf(int rows, int cols, int direction)
{
    if (rows <= 0 || cols <= 0 || direction < 0 || direction >= 2 * (rows + cols)) {
        return -1;
    }
    // The exe's mapping from a direction to its shape (as World::edgeShape).
    if (rows == 3 && cols == 3) return direction;
    if (direction == 0) return kEdgeS;
    if (direction <= rows - 2) return kEdgeSW;
    if (direction == rows - 1) return kEdgeW;
    const int side = (rows - 4) + cols * 2;
    if (direction < side) return (((direction ^ rows) & 1) != 0) ? kEdgeNW : kEdgeSE;
    if (direction == side) return kEdgeE;
    if (direction <= (cols + rows) * 2 - 6) return kEdgeNE;
    if (direction == (cols + rows) * 2 - 5) return kEdgeN;
    return direction + ((6 - cols) - rows) * 2;
}

std::vector<int> edgeDirections(int rows, int cols, int shape)
{
    std::vector<int> out;
    for (int d = 0; d < 2 * (rows + cols); ++d) {
        if (edgeShapeOf(rows, cols, d) == shape) out.push_back(d);
    }
    return out;
}

bool edgeDirectionUsable(const things::ThingDb& things, int edge, int direction)
{
    if (edge < 0 || static_cast<std::size_t>(edge) >= things.edges.size()) return false;
    const auto& e = things.edges[static_cast<std::size_t>(edge)];
    if (direction < 0 || direction >= 2 * (e.rows + e.cols) || static_cast<std::size_t>(direction) >= e.images.size()) return false;
    // DenseGrassEdge's image for its first SE side is a whole-tile fill, not a side.
    if (e.name == "DenseGrassEdge" && direction == 5) return false;
    return true;
}

int edgeDirectionFor(const things::ThingDb& things, int edge, int shape, std::uint32_t pick)
{
    if (edge < 0 || static_cast<std::size_t>(edge) >= things.edges.size()) return -1;
    const auto& e = things.edges[static_cast<std::size_t>(edge)];
    std::vector<int> usable;
    for (const int d : edgeDirections(e.rows, e.cols, shape)) {
        if (edgeDirectionUsable(things, edge, d)) usable.push_back(d);
    }
    if (usable.empty()) return -1;
    return usable[pick % usable.size()];
}

// --- Rules -----------------------------------------------------------------------------------------

EdgeRules EdgeRules::defaults(const things::ThingDb& things)
{
    EdgeRules r;
    for (const char* name : kDefaultPriority) {
        if (const int f = floorIndex(things, name); f >= 0 && std::ranges::find(r.stockList_, f) == r.stockList_.end()) {
            r.stockList_.push_back(f);
        }
    }
    for (const auto& s : kDefaultStyles) {
        const int a = floorIndex(things, s.blended), b = floorIndex(things, s.base), e = edgeIndex(things, s.edge);
        if (a >= 0 && b >= 0 && e >= 0) r.stock_[{a, b}] = e;
    }
    r.floorEdge_.assign(things.floors.size(), -1);
    for (const auto& s : kDefaultFloorEdges) {
        const int f = floorIndex(things, s.blended), e = edgeIndex(things, s.edge);
        if (f >= 0 && e >= 0) r.floorEdge_[static_cast<std::size_t>(f)] = e;
    }
    r.blendEdge_ = edgeIndex(things, "BlendEdge");
    if (r.blendEdge_ < 0 && !things.edges.empty()) r.blendEdge_ = 0;
    r.setOrder(r.stockList_);
    r.stockOrder_ = true;
    return r;
}

void EdgeRules::setOrder(std::vector<int> ascending)
{
    order_ = std::move(ascending);
    stockOrder_ = order_ == stockList_;
    int top = 0;
    for (const int f : order_) top = std::max(top, f + 1);
    rank_.assign(static_cast<std::size_t>(std::max<int>(top, static_cast<int>(floorEdge_.size()))), -1);
    for (std::size_t i = 0; i < order_.size(); ++i) {
        if (order_[i] >= 0) rank_[static_cast<std::size_t>(order_[i])] = static_cast<int>(i);
    }
}

int EdgeRules::rank(int floor) const
{
    return floor >= 0 && static_cast<std::size_t>(floor) < rank_.size() ? rank_[static_cast<std::size_t>(floor)] : -1;
}

EdgeStyle EdgeRules::style(int over, int base) const
{
    if (const auto it = user_.find({over, base}); it != user_.end()) {
        EdgeStyle s = it->second;
        if (s.kind != EdgeStyle::Kind::None && s.edge < 0) s.edge = blendEdge_;
        return s;
    }
    if (const auto it = stock_.find({over, base}); it != stock_.end()) return {EdgeStyle::Kind::Blend, it->second};
    if (const auto it = stock_.find({base, over}); it != stock_.end()) return {EdgeStyle::Kind::Blend, it->second};
    if (over >= 0 && static_cast<std::size_t>(over) < floorEdge_.size() && floorEdge_[static_cast<std::size_t>(over)] >= 0) {
        return {EdgeStyle::Kind::Blend, floorEdge_[static_cast<std::size_t>(over)]};
    }
    return {EdgeStyle::Kind::Blend, blendEdge_};
}

std::optional<EdgeStyle> EdgeRules::lap(int over, int base) const
{
    if (over == base) return std::nullopt;
    if (const auto it = user_.find({over, base}); it != user_.end()) {
        if (it->second.kind == EdgeStyle::Kind::None) return std::nullopt;
        return style(over, base);
    }
    if (rank(over) <= rank(base)) return std::nullopt;
    const EdgeStyle s = style(over, base);
    if (s.kind == EdgeStyle::Kind::None || s.edge < 0) return std::nullopt;
    return s;
}

void EdgeRules::setOverride(int over, int base, std::optional<EdgeStyle> s)
{
    if (s) {
        user_[{over, base}] = *s;
    } else {
        user_.erase({over, base});
    }
}

std::string EdgeRules::toText(const things::ThingDb& things) const
{
    std::string out;
    const auto floorName = [&](int f) { return f >= 0 && static_cast<std::size_t>(f) < things.floors.size() ? things.floors[static_cast<std::size_t>(f)].name : std::string("?"); };
    if (!stockOrder_) {
        out += "order";
        for (const int f : order_) out += "\t" + floorName(f);
        out += "\n";
    }
    for (const auto& [k, s] : user_) {
        const char* kind = s.kind == EdgeStyle::Kind::Blend ? "blend" : s.kind == EdgeStyle::Kind::Trim ? "trim" : "none";
        const std::string edge = s.edge >= 0 && static_cast<std::size_t>(s.edge) < things.edges.size() ? things.edges[static_cast<std::size_t>(s.edge)].name : "-";
        out += "pair\t" + floorName(k.first) + "\t" + floorName(k.second) + "\t" + kind + "\t" + edge + "\n";
    }
    return out;
}

void EdgeRules::applyText(std::string_view text, const things::ThingDb& things)
{
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        std::istringstream ls(line);
        for (std::string w; std::getline(ls, w, '\t');) f.push_back(w);
        if (f.empty()) continue;
        if (f[0] == "order") {
            std::vector<int> order;
            for (std::size_t i = 1; i < f.size(); ++i) {
                if (const int x = floorIndex(things, f[i]); x >= 0) order.push_back(x);
            }
            setOrder(std::move(order));
        } else if (f[0] == "pair" && f.size() >= 5) {
            const int a = floorIndex(things, f[1]), b = floorIndex(things, f[2]);
            if (a < 0 || b < 0) continue;
            EdgeStyle s;
            s.kind = f[3] == "trim" ? EdgeStyle::Kind::Trim : f[3] == "none" ? EdgeStyle::Kind::None : EdgeStyle::Kind::Blend;
            s.edge = edgeIndex(things, f[4]);
            user_[{a, b}] = s;
        }
    }
}

// --- Variants ------------------------------------------------------------------------------------

VariantAnchors VariantAnchors::learn(const MapDocument& doc, const things::ThingDb& things)
{
    std::map<int, std::map<std::pair<int, int>, int>> votes;
    for (int gy = -1; gy <= 254; ++gy) {
        for (int gx = (gy & 1); gx <= 255; gx += 2) {
            const auto* t = doc.floor.tile(gx, gy);
            if (!t || t->image >= things.floors.size()) continue;
            const auto& def = things.floors[t->image];
            const int cols = def.cols, rows = def.rows;
            if (cols <= 0 || rows <= 0 || cols * rows <= 1 || t->variant < 0 || t->variant >= cols * rows) continue;
            const int u = (gx + gy) / 2, v = (gx - gy) / 2;
            const int col = t->variant % cols, row = t->variant / cols;
            ++votes[t->image][{mod(col - u, cols), mod(row + v, rows)}];
        }
    }
    VariantAnchors a;
    for (const auto& [floor, m] : votes) {
        const auto best = std::max_element(m.begin(), m.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        a.anchors_[floor] = best->first;
    }
    return a;
}

std::pair<int, int> VariantAnchors::anchor(int floor) const
{
    const auto it = anchors_.find(floor);
    return it == anchors_.end() ? std::pair{0, 0} : it->second;
}

std::int16_t VariantAnchors::variant(const things::ThingDb& things, int floor, int gx, int gy) const
{
    if (floor < 0 || static_cast<std::size_t>(floor) >= things.floors.size()) return 0;
    const auto& def = things.floors[static_cast<std::size_t>(floor)];
    const int cols = def.cols, rows = def.rows;
    if (cols <= 0 || rows <= 0) return 0;
    const auto [a, b] = anchor(floor);
    const int u = (gx + gy) / 2, v = (gx - gy) / 2;
    return static_cast<std::int16_t>(mod(u + a, cols) + cols * mod(b - v, rows));
}

// --- Auto-edge -------------------------------------------------------------------------------------

bool wallBetween(const MapDocument& doc, TileCell t, int n)
{
    const int x = t.gx, y = t.gy;
    // The vertices: N (x, y), E (x + 1, y + 1), S (x, y + 2), W (x - 1, y + 1).
    switch (n) {
    case 0: return (wallArmsAt(doc, x, y) & kArmSE) || (wallArmsAt(doc, x + 1, y + 1) & kArmNW);          // NE: N-E
    case 1: return (wallArmsAt(doc, x + 1, y + 1) & kArmSW) || (wallArmsAt(doc, x, y + 2) & kArmNE);      // SE: E-S
    case 2: return (wallArmsAt(doc, x, y + 2) & kArmNW) || (wallArmsAt(doc, x - 1, y + 1) & kArmSE);      // SW: S-W
    case 3: return (wallArmsAt(doc, x - 1, y + 1) & kArmNE) || (wallArmsAt(doc, x, y) & kArmSW);          // NW: W-N
    case 4:
    case 6: {
        // N or S point: the tiles above and below the vertex, parted by arms on both its sides.
        const std::uint8_t a = n == 4 ? wallArmsAt(doc, x, y) : wallArmsAt(doc, x, y + 2);
        return (a & (kArmNE | kArmSE)) && (a & (kArmNW | kArmSW));
    }
    case 5:
    case 7: {
        // E or W point: the tiles left and right of the vertex, parted by arms above and below it.
        const std::uint8_t a = n == 5 ? wallArmsAt(doc, x + 1, y + 1) : wallArmsAt(doc, x - 1, y + 1);
        return (a & (kArmNW | kArmNE)) && (a & (kArmSW | kArmSE));
    }
    default: return false;
    }
}

std::set<TileCell> withNeighbours(const std::set<TileCell>& cells)
{
    std::set<TileCell> out = cells;
    for (const TileCell t : cells) {
        for (int n = 0; n < 8; ++n) {
            const TileCell m = offset(t, n);
            if (validTile(m.gx, m.gy)) out.insert(m);
        }
    }
    return out;
}

std::vector<map::FloorEdge> autoEdges(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules,
                                      const AutoEdgeOptions& o, const VariantAnchors& anchors, TileCell t)
{
    const map::FloorTile* tile = tileAtCell(doc, t);
    if (!tile) return {};
    const int base = tile->image;
    std::vector<map::FloorEdge> out;
    if (o.onlyFloor) {
        for (const auto& e : tile->edges) {
            if (e.floor != *o.onlyFloor || e.floor == base) out.push_back(e);
        }
    }
    // The lapping floors with their neighbour masks (sides NE 1 SE 2 SW 4 NW 8; corners N 1 E 2 S 4 W 8).
    struct Overlay {
        int floor = -1; ///< -1: the hole trim
        EdgeStyle style;
        int sides = 0, corners = 0;
        int rank = 0;
    };
    std::map<int, Overlay> overlays;
    const bool baseOnTop = o.onTop && *o.onTop == base;
    for (int n = 0; n < 8; ++n) {
        const TileCell m = offset(t, n);
        const map::FloorTile* nt = tileAtCell(doc, m);
        int f = nt ? nt->image : -1;
        if (f == base) continue;
        if (o.wallBlock && wallBetween(doc, t, n)) continue;
        std::optional<EdgeStyle> style;
        if (f < 0) {
            if (!o.holeTrim || !validTile(m.gx, m.gy)) continue;
            style = EdgeStyle{EdgeStyle::Kind::Trim, *o.holeTrim};
        } else {
            if (o.onlyFloor && f != *o.onlyFloor) continue;
            if (baseOnTop) continue;
            if (o.onTop && *o.onTop == f) {
                style = rules.style(f, base);
                if (style->kind == EdgeStyle::Kind::None) style->kind = EdgeStyle::Kind::Blend;
                if (style->edge < 0) style.reset();
            } else {
                style = rules.lap(f, base);
            }
        }
        if (!style) continue;
        Overlay& ov = overlays[f];
        ov.floor = f;
        ov.style = *style;
        ov.rank = f < 0 ? -1000000 : (o.onTop && *o.onTop == f) ? 1000000 : rules.rank(f);
        if (n < 4) ov.sides |= 1 << n;
        else ov.corners |= 1 << (n - 4);
    }
    std::vector<const Overlay*> ordered;
    for (const auto& [f, ov] : overlays) ordered.push_back(&ov);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Overlay* a, const Overlay* b) { return a->rank < b->rank; });
    constexpr int NE = 1, SE = 2, SW = 4, NW = 8;
    for (const Overlay* ov : ordered) {
        std::vector<int> shapes;
        const int s = ov->sides;
        int covered = 0;
        if (s == 15) {
            shapes = {kEdgeNENW, kEdgeSESW};
            covered = 15;
        } else {
            const struct {
                int a, b, shape;
            } pairs[] = {{NE, SE, kEdgeNESE}, {SE, SW, kEdgeSESW}, {SW, NW, kEdgeSWNW}, {NE, NW, kEdgeNENW}};
            for (const auto& p : pairs) {
                if ((s & p.a) && (s & p.b)) {
                    shapes.push_back(p.shape);
                    covered |= p.a | p.b;
                }
            }
            const struct {
                int side, shape;
            } singles[] = {{NE, kEdgeNE}, {SE, kEdgeSE}, {SW, kEdgeSW}, {NW, kEdgeNW}};
            for (const auto& p : singles) {
                if ((s & p.side) && !(covered & p.side)) shapes.push_back(p.shape);
            }
        }
        // Point corners whose two sides are clear.
        if ((ov->corners & 1) && !(s & (NW | NE))) shapes.push_back(kEdgeN);
        if ((ov->corners & 2) && !(s & (NE | SE))) shapes.push_back(kEdgeE);
        if ((ov->corners & 4) && !(s & (SE | SW))) shapes.push_back(kEdgeS);
        if ((ov->corners & 8) && !(s & (SW | NW))) shapes.push_back(kEdgeW);
        const bool trim = ov->style.kind == EdgeStyle::Kind::Trim;
        const int shown = trim ? base : (o.coverFloor ? *o.coverFloor : ov->floor);
        for (const int shape : shapes) {
            const int dir = edgeDirectionFor(things, ov->style.edge, shape, hashCell(t.gx, t.gy, static_cast<std::uint32_t>(ov->floor + 1) * 31u + static_cast<std::uint32_t>(shape), o.seed));
            if (dir < 0) continue;
            map::FloorEdge e;
            e.floor = static_cast<std::uint8_t>(shown);
            e.variant = trim ? tile->variant : anchors.variant(things, shown, t.gx, t.gy);
            e.edge = static_cast<std::uint8_t>(ov->style.edge);
            e.direction = static_cast<std::uint8_t>(dir);
            out.push_back(e);
        }
    }
    return out;
}

std::vector<TileCell> recomputeEdges(MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, const AutoEdgeOptions& o,
                                     const VariantAnchors& anchors, const std::set<TileCell>& cells, const std::set<TileCell>& custom)
{
    // Every tile's new edges from the floors as they are (edges never shape edges), then set.
    std::vector<std::pair<TileCell, std::vector<map::FloorEdge>>> next;
    for (const TileCell t : cells) {
        if (custom.contains(t)) continue;
        const map::FloorTile* tile = tileAtCell(doc, t);
        if (!tile) continue;
        if (o.excludeFloor && tile->image == *o.excludeFloor) continue;
        auto edges = autoEdges(doc, things, rules, o, anchors, t);
        if (edges != tile->edges) next.emplace_back(t, std::move(edges));
    }
    std::vector<TileCell> changed;
    for (auto& [t, edges] : next) {
        map::FloorTile tile = *doc.floor.tile(t.gx, t.gy);
        tile.edges = std::move(edges);
        doc.floor.setTile(t.gx, t.gy, std::move(tile));
        changed.push_back(t);
    }
    return changed;
}

Op autoEdgeOp(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, const AutoEdgeOptions& o,
              const std::set<TileCell>& cells, const std::set<TileCell>& custom)
{
    const VariantAnchors anchors = VariantAnchors::learn(doc, things);
    SetTiles part;
    for (const TileCell t : cells) {
        if (custom.contains(t)) continue;
        const map::FloorTile* tile = tileAtCell(doc, t);
        if (!tile || (o.excludeFloor && tile->image == *o.excludeFloor)) continue;
        auto edges = autoEdges(doc, things, rules, o, anchors, t);
        if (edges == tile->edges) continue;
        map::FloorTile next = *tile;
        next.edges = std::move(edges);
        part.tiles.push_back({t.gx, t.gy, std::move(next)});
    }
    Op op;
    op.label = "Auto-edge " + std::to_string(part.tiles.size()) + (part.tiles.size() == 1 ? " tile" : " tiles");
    if (!part.tiles.empty()) op.parts.push_back(std::move(part));
    return op;
}

std::set<TileCell> tilesAtWalls(const std::vector<Cell>& cells)
{
    // A tile's vertices are N (gx, gy), E (gx + 1, gy + 1), S (gx, gy + 2) and W (gx - 1, gy + 1)
    // in wall cells: the tiles with the wall at one of them.
    std::set<TileCell> out;
    for (const Cell c : cells) {
        const int x = c.x, y = c.y;
        for (const auto& [dx, dy] : {std::pair{0, 0}, std::pair{-1, -1}, std::pair{0, -2}, std::pair{1, -1}}) {
            if (validTile(x + dx, y + dy)) out.insert({static_cast<std::int16_t>(x + dx), static_cast<std::int16_t>(y + dy)});
        }
    }
    return out;
}

Op withEdgesAtWalls(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, const AutoEdgeOptions& options, Op wallOp,
                    const std::set<TileCell>& custom)
{
    std::vector<Cell> changed;
    for (const auto& part : wallOp.parts) {
        if (const auto* w = std::get_if<SetWalls>(&part)) {
            for (const auto& c : w->cells) changed.push_back(c.cell);
        }
    }
    if (changed.empty()) return wallOp;
    // The edges as they'd be with the walls changed (on a copy: the op goes through the history).
    MapDocument after = doc;
    applyOp(after, wallOp);
    const Op edges = autoEdgeOp(after, things, rules, options, tilesAtWalls(changed), custom);
    wallOp.parts.insert(wallOp.parts.end(), edges.parts.begin(), edges.parts.end());
    return wallOp;
}

// --- The brush -------------------------------------------------------------------------------------

namespace {

/// Smooth value noise over the full grid (about 3 tiles a lattice step), 0..1.
float valueNoise(float gx, float gy, std::uint32_t seed)
{
    const float sx = gx / 3.0f, sy = gy / 3.0f;
    const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
    const float fx = sx - static_cast<float>(ix), fy = sy - static_cast<float>(iy);
    const auto h = [&](int x, int y) { return static_cast<float>(hashCell(x, y, seed, 0x51ED27u) & 0xFFFF) / 65535.0f; };
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const float a = h(ix, iy) + (h(ix + 1, iy) - h(ix, iy)) * ux;
    const float b = h(ix, iy + 1) + (h(ix + 1, iy + 1) - h(ix, iy + 1)) * ux;
    return a + (b - a) * uy;
}

float smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

} // namespace

std::vector<TileCell> brushFootprint(const FloorBrush& b, WorldPoint at)
{
    const auto centre = tileAt(at.x, at.y);
    if (!centre) return {};
    const int n = std::clamp(b.size, 1, 16);
    constexpr float kPitch = 32.526912f; // between side neighbours' centres (23 sqrt 2)
    const float half = static_cast<float>(n) / 2.0f;
    const float hardness = std::clamp(b.hardness, 0.0f, 1.0f);
    const float inner = hardness, outer = 1.0f + (1.0f - hardness) * 0.5f;
    std::vector<TileCell> out;
    const int reach = n + 2;
    for (int dy = -2 * reach; dy <= 2 * reach; ++dy) {
        for (int dx = -2 * reach; dx <= 2 * reach; ++dx) {
            if (((dx + dy) & 1) != 0) continue;
            const TileCell t{static_cast<std::int16_t>(centre->gx + dx), static_cast<std::int16_t>(centre->gy + dy)};
            if (!validTile(t.gx, t.gy)) continue;
            const WorldPoint c = tileCentre(t);
            const float px = c.x - at.x, py = c.y - at.y;
            float d = 0;
            switch (b.shape) {
            case FloorBrush::Shape::Round: d = std::hypot(px, py) / (half * kPitch); break;
            case FloorBrush::Shape::Square: d = std::max(std::abs(px), std::abs(py)) / (half * kPitch); break;
            case FloorBrush::Shape::Diamond: d = std::max(std::abs(px + py), std::abs(px - py)) / 46.0f / half; break;
            }
            bool in = false;
            if (t == *centre) {
                in = true;
            } else if (hardness >= 1.0f) {
                in = d <= 1.0f;
            } else {
                const float coverage = 1.0f - smoothstep(inner, outer, d);
                const float threshold = 0.5f + std::clamp(b.noise, 0.0f, 1.0f) * (valueNoise(t.gx, t.gy, b.seed) - 0.5f) * 0.98f;
                in = coverage > threshold;
            }
            if (in) out.push_back(t);
        }
    }
    return out;
}

FloorStroke::FloorStroke(MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, FloorBrush brush, std::set<TileCell> custom)
    : doc_(doc)
    , things_(things)
    , rules_(rules)
    , brush_(brush)
    , custom_(std::move(custom))
    , anchors_(VariantAnchors::learn(doc, things))
{
}

void FloorStroke::remember(TileCell t)
{
    if (original_.contains(t)) return;
    const auto* tile = doc_.floor.tile(t.gx, t.gy);
    original_[t] = tile ? std::optional<map::FloorTile>(*tile) : std::nullopt;
}

std::optional<map::FloorTile> FloorStroke::baseTile(int floor, TileCell t) const
{
    if (floor < 0 || static_cast<std::size_t>(floor) >= things_.floors.size()) return std::nullopt;
    map::FloorTile tile;
    tile.image = static_cast<std::uint8_t>(floor);
    const auto& def = things_.floors[static_cast<std::size_t>(floor)];
    const int count = std::max(1, def.cols * def.rows);
    switch (brush_.variants) {
    case FloorBrush::Variants::Aligned: tile.variant = anchors_.variant(things_, floor, t.gx, t.gy); break;
    case FloorBrush::Variants::Random: tile.variant = static_cast<std::int16_t>(hashCell(t.gx, t.gy, brush_.seed, 0xA11CEu) % static_cast<std::uint32_t>(count)); break;
    case FloorBrush::Variants::Fixed: tile.variant = static_cast<std::int16_t>(std::clamp(brush_.fixedVariant, 0, count - 1)); break;
    }
    if (custom_.contains(t)) {
        if (const auto* now = doc_.floor.tile(t.gx, t.gy)) tile.edges = now->edges; // (hand-placed: kept)
    }
    return tile;
}

std::vector<TileCell> FloorStroke::dab(WorldPoint at)
{
    std::set<TileCell> floorChanged;
    std::set<TileCell> changed;
    const auto set = [&](TileCell t, std::optional<map::FloorTile> tile) {
        remember(t);
        doc_.floor.setTile(t.gx, t.gy, std::move(tile));
        floorChanged.insert(t);
        changed.insert(t);
    };
    const auto floorOf = [&](TileCell t) {
        const auto* tile = doc_.floor.tile(t.gx, t.gy);
        return tile ? static_cast<int>(tile->image) : -1;
    };
    using Mode = FloorBrush::Mode;
    if (brush_.mode == Mode::Fill) {
        if (filled_) return {};
        filled_ = true;
        const auto start = tileAt(at.x, at.y);
        if (!start) return {};
        const int target = floorOf(*start);
        if (target == brush_.floor) return {};
        std::set<TileCell> region{*start};
        std::deque<TileCell> todo{*start};
        while (!todo.empty()) {
            const TileCell t = todo.front();
            todo.pop_front();
            for (int n = 0; n < 4; ++n) {
                const TileCell m = offset(t, n);
                if (!validTile(m.gx, m.gy) || region.contains(m) || floorOf(m) != target || wallBetween(doc_, t, n)) continue;
                region.insert(m);
                if (region.size() > brush_.fillLimit) return {}; // (open ground: refused)
                todo.push_back(m);
            }
        }
        for (const TileCell t : region) set(t, baseTile(brush_.floor, t));
    } else if (brush_.mode == Mode::EdgesOnly) {
        std::set<TileCell> cells;
        for (const TileCell t : brushFootprint(brush_, at)) {
            remember(t);
            cells.insert(t);
        }
        for (const TileCell t : recomputeEdges(doc_, things_, rules_, brush_.edges, anchors_, cells, custom_)) changed.insert(t);
        return {changed.begin(), changed.end()};
    } else if (brush_.mode == Mode::Smooth) {
        // Simultaneous: every tile's majority from the floors as they were before this dab.
        std::vector<std::pair<TileCell, int>> next;
        for (const TileCell t : brushFootprint(brush_, at)) {
            std::map<int, int> votes;
            ++votes[floorOf(t)];
            for (int n = 0; n < 8; ++n) {
                const TileCell m = offset(t, n);
                ++votes[validTile(m.gx, m.gy) ? floorOf(m) : -1];
            }
            const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            if (best->second >= 5 && best->first != floorOf(t)) next.emplace_back(t, best->first);
        }
        for (const auto& [t, f] : next) set(t, f < 0 ? std::nullopt : baseTile(f, t));
    } else {
        for (const TileCell t : brushFootprint(brush_, at)) {
            const int now = floorOf(t);
            if (brush_.mode == Mode::Erase) {
                if (now >= 0) set(t, std::nullopt);
            } else if (brush_.mode == Mode::Replace) {
                if (now >= 0 && now == brush_.replaceFloor && now != brush_.floor) set(t, baseTile(brush_.floor, t));
            } else if (now != brush_.floor) {
                set(t, baseTile(brush_.floor, t));
            }
        }
    }
    if (brush_.autoEdge && !floorChanged.empty()) {
        const std::set<TileCell> dirty = withNeighbours(floorChanged);
        for (const TileCell t : dirty) remember(t);
        for (const TileCell t : recomputeEdges(doc_, things_, rules_, brush_.edges, anchors_, dirty, custom_)) changed.insert(t);
    }
    return {changed.begin(), changed.end()};
}

std::vector<TileCell> FloorStroke::cancel()
{
    std::vector<TileCell> changed;
    for (const auto& [t, tile] : original_) {
        const auto* now = doc_.floor.tile(t.gx, t.gy);
        const bool same = (!now && !tile) || (now && tile && *now == *tile);
        if (!same) {
            doc_.floor.setTile(t.gx, t.gy, tile);
            changed.push_back(t);
        }
    }
    original_.clear();
    return changed;
}

Op FloorStroke::finish(std::vector<TileCell>* restored)
{
    SetTiles part;
    for (const auto& [t, tile] : original_) {
        const auto* now = doc_.floor.tile(t.gx, t.gy);
        const bool same = (!now && !tile) || (now && tile && *now == *tile);
        if (!same) part.tiles.push_back({t.gx, t.gy, now ? std::optional<map::FloorTile>(*now) : std::nullopt});
    }
    const std::vector<TileCell> back = cancel();
    if (restored) *restored = back;
    Op op;
    if (part.tiles.empty()) return op;
    using Mode = FloorBrush::Mode;
    const std::string floor = brush_.floor < things_.floors.size() ? things_.floors[brush_.floor].name : "floor";
    const std::string n = std::to_string(part.tiles.size()) + (part.tiles.size() == 1 ? " tile" : " tiles");
    switch (brush_.mode) {
    case Mode::Paint: op.label = "Paint " + floor + " (" + n + ")"; break;
    case Mode::Erase: op.label = "Erase floor (" + n + ")"; break;
    case Mode::Replace: op.label = "Replace with " + floor + " (" + n + ")"; break;
    case Mode::Fill: op.label = "Fill with " + floor + " (" + n + ")"; break;
    case Mode::Smooth: op.label = "Smooth floor (" + n + ")"; break;
    case Mode::EdgesOnly: op.label = "Recompute edges (" + n + ")"; break;
    }
    op.parts.push_back(std::move(part));
    return op;
}

// --- Settings ------------------------------------------------------------------------------------

std::string PaintSettings::toText() const
{
    std::string out = "seed " + std::to_string(edgeSeed) + "\n";
    if (!customEdges.empty()) {
        out += "custom";
        for (const auto t : customEdges) out += " " + std::to_string(t.gx) + "," + std::to_string(t.gy);
        out += "\n";
    }
    std::istringstream in(rulesText);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) out += "r " + line + "\n";
    }
    return out;
}

PaintSettings PaintSettings::fromText(std::string_view text)
{
    PaintSettings s;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("seed ")) {
            s.edgeSeed = static_cast<std::uint32_t>(std::strtoul(line.c_str() + 5, nullptr, 10));
        } else if (line.starts_with("custom")) {
            std::istringstream ls(line.substr(6));
            for (std::string w; ls >> w;) {
                char* end = nullptr;
                const long gx = std::strtol(w.c_str(), &end, 10);
                if (!end || *end != ',') continue;
                const long gy = std::strtol(end + 1, nullptr, 10);
                if (validTile(static_cast<int>(gx), static_cast<int>(gy))) {
                    s.customEdges.insert({static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy)});
                }
            }
        } else if (line.starts_with("r ")) {
            s.rulesText += line.substr(2) + "\n";
        }
    }
    return s;
}

} // namespace nox::mapedit
