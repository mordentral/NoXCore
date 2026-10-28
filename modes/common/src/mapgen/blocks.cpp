// The map generator's native building blocks (blocks.hpp): what layout.cpp's plotting did per cell,
// in the same order, for the design in script to call.
#include "blocks.hpp"

#include "cells.hpp"

#include <algorithm>
#include <cstdlib>

namespace nox::mapgen::blocks {

namespace {

using detail::kCorners;
using detail::kSides;
using detail::manhattan;
using detail::plus;

/// The cell (u, v) from `c` in diagonal coordinates (always on the x + y even checkerboard).
Cell uv(Cell c, int u, int v)
{
    return {c.first + u + v, c.second + u - v};
}

// Opens the cells of c's checkerboard colour within Manhattan distance `radius` of it.
void openDiamond(CellSet& open, Cell c, int radius)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        const int w = radius - std::abs(dy);
        for (int dx = -w; dx <= w; ++dx) {
            if ((dx + dy) % 2 == 0) {
                open.insert({c.first + dx, c.second + dy});
            }
        }
    }
}

// Opens the cells with |u| <= a and |v| <= b round `c`, in diagonal coordinates (uv).
void openHall(CellSet& cells, Cell c, int a, int b)
{
    for (int u = -a; u <= a; ++u) {
        for (int v = -b; v <= b; ++v) {
            cells.insert(uv(c, u, v));
        }
    }
}

void hallCells(std::vector<Cell>& cells, Cell c, int a, int b)
{
    for (int u = -a; u <= a; ++u) {
        for (int v = -b; v <= b; ++v) {
            cells.push_back(uv(c, u, v));
        }
    }
}

/// A cavern's blobs (centre, radius), drawn again from its seed: the first round the centre,
/// each next one centred inside one before it (so they join), all within the cavern's radius.
std::vector<Chamber> cavernBlobs(const Chamber& room)
{
    std::mt19937 rng(room.seed * 2246822519u + 11u);
    // A core, then many small blobs spilling out from those before in every way (a ragged edge,
    // not a blocky one): 10 to 17 more, 2 to a quarter of the radius + 2 each.
    const int core = std::max(3, room.radius / 2);
    std::vector<Chamber> blobs = {{room.centre, std::min(core, room.radius)}};
    const int more = 10 + static_cast<int>(rng() % 8);
    for (int tries = 0; tries < 120 && static_cast<int>(blobs.size()) < 1 + more; ++tries) {
        const Chamber from = blobs[rng() % blobs.size()];
        const int reach = std::max(1, from.radius);
        const int u = static_cast<int>(rng() % static_cast<unsigned>(2 * reach + 1)) - reach;
        const int v = static_cast<int>(rng() % static_cast<unsigned>(2 * reach + 1)) - reach;
        if (std::max(std::abs(u), std::abs(v)) * 2 > from.radius) {
            continue; // (its centre inside the blob it grows from: they join)
        }
        const Cell at = uv(from.centre, u, v);
        const int r = 2 + static_cast<int>(rng() % static_cast<unsigned>(std::max(1, room.radius / 4 + 1)));
        if (manhattan(at, room.centre) + r <= room.radius) {
            blobs.push_back({at, r});
        }
    }
    return blobs;
}

} // namespace

void openRoom(CellSet& open, std::vector<std::vector<Cell>>& holes, const Chamber& room)
{
    switch (room.shape) {
    case RoomShape::Diamond:
        openDiamond(open, room.centre, room.radius);
        break;
    case RoomShape::Hall:
        openHall(open, room.centre, room.a, room.b);
        break;
    case RoomShape::Cross:
        openHall(open, room.centre, room.a, room.b);
        openHall(open, room.centre, room.b, room.a);
        break;
    case RoomShape::Ring:
        openHall(open, room.centre, room.a, room.b);
        hallCells(holes.emplace_back(), room.centre, room.a - 3, room.b - 3);
        break;
    case RoomShape::Pillars:
        openHall(open, room.centre, room.a, room.b);
        // (Two by two in diagonal steps: four wall cells, each with two wall neighbours, a small
        // diamond column; a lone cell would be a post no wall shape draws.)
        for (int u = -room.a + 2; u + 1 <= room.a - 2; u += 4) {
            for (int v = -room.b + 2; v + 1 <= room.b - 2; v += 4) {
                auto& pillar = holes.emplace_back();
                for (const auto& [du, dv] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
                    pillar.push_back(uv(room.centre, u + du, v + dv));
                }
            }
        }
        break;
    case RoomShape::Cavern:
        for (const Chamber& blob : cavernBlobs(room)) {
            openDiamond(open, blob.centre, blob.radius);
        }
        break;
    }
}

Chamber makeRoom(Cell at, int radius, RoomShape shape, std::mt19937& rng)
{
    Chamber room{at, radius, shape};
    const int half = std::max(2, radius / 2); // (a hall's |u|, |v| at most half: |dx| + |dy| = 2 max)
    switch (shape) {
    case RoomShape::Diamond:
        break;
    case RoomShape::Hall:
        room.a = half;
        room.b = 2 + static_cast<int>(rng() % static_cast<unsigned>(std::max(1, half - 1)));
        if (rng() % 2 == 0) {
            std::swap(room.a, room.b);
        }
        room.radius = 2 * std::max(room.a, room.b);
        break;
    case RoomShape::Cross:
        room.a = half;
        room.b = 1 + static_cast<int>(rng() % 2);
        room.radius = 2 * room.a;
        break;
    case RoomShape::Ring:
    case RoomShape::Pillars:
        room.a = half;
        room.b = std::max(half - static_cast<int>(rng() % 2), shape == RoomShape::Ring ? 5 : 4);
        if (rng() % 2 == 0) {
            std::swap(room.a, room.b);
        }
        room.radius = 2 * std::max(room.a, room.b);
        break;
    case RoomShape::Cavern:
        room.seed = static_cast<std::uint32_t>(rng());
        break;
    }
    return room;
}

// ---- Plot -------------------------------------------------------------------------------------

Plot::Plot()
    : buckets_(static_cast<std::size_t>(kSide * kSide))
{
}

Plot::Bucket& Plot::bucketFor(Cell c)
{
    const int bx = (c.first - kOrigin) / kBucket;
    const int by = (c.second - kOrigin) / kBucket;
    if (c.first < kOrigin || c.second < kOrigin || bx >= kSide || by >= kSide) {
        return outside_;
    }
    return buckets_[static_cast<std::size_t>(by * kSide + bx)];
}

std::size_t Plot::add(const Chamber& blob, int tag)
{
    const std::size_t i = blobs_.size();
    blobs_.push_back(blob);
    tags_.push_back(tag);
    Bucket& b = bucketFor(blob.centre);
    b.idx.push_back(static_cast<std::uint32_t>(i));
    b.maxR = std::max(b.maxR, blob.radius);
    maxR_ = std::max(maxR_, blob.radius);
    return i;
}

std::optional<std::size_t> Plot::firstNear(Cell c, int r, int gap, std::size_t from, std::size_t to, const Except* e1, const Except* e2) const
{
    to = std::min(to, blobs_.size());
    if (from >= to) {
        return std::nullopt;
    }
    const int reach = r + gap;
    std::size_t best = to;
    const auto excepted = [&](std::size_t k) {
        for (const Except* e : {e1, e2}) {
            if (e && (e->tag < 0 || e->tag == tags_[k]) && manhattan(blobs_[k].centre, e->at) < e->within) {
                return true;
            }
        }
        return false;
    };
    const auto scan = [&](const Bucket& b) {
        for (auto it = std::ranges::lower_bound(b.idx, static_cast<std::uint32_t>(from)); it != b.idx.end() && *it < best; ++it) {
            const Chamber& o = blobs_[*it];
            if (manhattan(c, o.centre) < reach + o.radius && !excepted(*it)) {
                best = *it;
                return;
            }
        }
    };
    // (Only centres less than reach + the largest radius away, on both axes, can be near.)
    const int span = reach + maxR_;
    const int x0 = std::max(c.first - span, kOrigin), x1 = std::min(c.first + span, kOrigin + kBucket * kSide - 1);
    const int y0 = std::max(c.second - span, kOrigin), y1 = std::min(c.second + span, kOrigin + kBucket * kSide - 1);
    for (int by = (y0 - kOrigin) / kBucket; y0 <= y1 && by <= (y1 - kOrigin) / kBucket; ++by) {
        for (int bx = (x0 - kOrigin) / kBucket; x0 <= x1 && bx <= (x1 - kOrigin) / kBucket; ++bx) {
            const Bucket& b = buckets_[static_cast<std::size_t>(by * kSide + bx)];
            if (b.idx.empty()) {
                continue;
            }
            // (The bucket's nearest cell to the point: none of its blobs reaches it if that's too far.)
            const int cx0 = kOrigin + bx * kBucket, cy0 = kOrigin + by * kBucket;
            const int ddx = std::max({0, cx0 - c.first, c.first - (cx0 + kBucket - 1)});
            const int ddy = std::max({0, cy0 - c.second, c.second - (cy0 + kBucket - 1)});
            if (ddx + ddy < reach + b.maxR) {
                scan(b);
            }
        }
    }
    scan(outside_);
    return best < to ? std::optional(best) : std::nullopt;
}

std::optional<std::vector<Cell>> Plot::run(Cell from, Cell way, int length, int margin, int width, int gap, std::size_t before, int tag)
{
    std::vector<Cell> cells;
    cells.reserve(static_cast<std::size_t>(std::max(length, 0)));
    Cell c = from;
    for (int k = 0; k < length; ++k) {
        c = plus(c, way);
        cells.push_back(c);
    }
    // (A straight line in a box: all its cells inside if the first and the last are.)
    if (cells.empty() || !inBounds(cells.front(), margin) || !inBounds(cells.back(), margin)) {
        return std::nullopt;
    }
    for (const Cell& cell : cells) {
        if (!clear(cell, width, gap, before)) {
            return std::nullopt;
        }
    }
    for (const Cell& cell : cells) {
        add({cell, width}, tag);
    }
    return cells;
}

std::optional<std::vector<Cell>> Plot::spur(Cell from, Cell way, int length, int margin, int width, int gap, Chamber& room, std::size_t skip,
                                            const Except& except)
{
    std::vector<Cell> cells;
    Cell c = from;
    for (int k = 0; k < length; ++k) {
        c = plus(c, way);
        cells.push_back(c);
    }
    if (cells.empty() || !inBounds(cells.front(), margin) || !inBounds(cells.back(), margin)) {
        return std::nullopt;
    }
    room.centre = cells.back();
    for (std::size_t k = std::min(skip, cells.size()); k < cells.size(); ++k) {
        if (firstNear(cells[k], width, gap, 0, blobs_.size(), &except)) {
            return std::nullopt;
        }
    }
    if (firstNear(room.centre, room.radius, gap, 0, blobs_.size(), &except)) {
        return std::nullopt;
    }
    for (const Cell& cell : cells) {
        add({cell, width});
    }
    add(room);
    return cells;
}

std::optional<std::vector<Cell>> Plot::lRun(Cell a, Cell b, bool uFirst, int width, int margin, int gap, int endsWithin, const Plot& route,
                                            int routeReach, int middle)
{
    const int dx = b.first - a.first;
    const int dy = b.second - a.second;
    const int u = (dx + dy) / 2;
    const int v = (dx - dy) / 2;
    const int au = std::abs(u), av = std::abs(v);
    const int length = au + av;
    // One diagonal step along u and one along v, each signed toward b.
    const Cell du{u > 0 ? 1 : -1, u > 0 ? 1 : -1};
    const Cell dv{v > 0 ? 1 : -1, v > 0 ? -1 : 1};
    std::vector<Cell> cells;
    cells.reserve(static_cast<std::size_t>(length));
    Cell c = a;
    // An L: all the u steps then all the v steps, or the other way round.
    for (int k = 0; k < length; ++k) {
        const bool alongU = uFirst ? k < au : k >= av;
        c = plus(c, alongU ? du : dv);
        cells.push_back(c);
    }
    if (c != b) {
        return std::nullopt;
    }
    const Except atA{a, endsWithin, -1};
    const Except atB{b, endsWithin, -1};
    const std::size_t n = cells.size();
    for (std::size_t k = 0; k < n; ++k) {
        const Cell& cell = cells[k];
        if (!inBounds(cell, margin) || firstNear(cell, width, gap, 0, blobs_.size(), &atA, &atB)) {
            return std::nullopt;
        }
        // (its middle stays clear of the route round its ends too)
        if (static_cast<int>(k) >= middle && k + static_cast<std::size_t>(middle) < n && route.firstNear(cell, routeReach, gap, 0, route.size())) {
            return std::nullopt;
        }
    }
    for (const Cell& cell : cells) {
        add({cell, width});
    }
    return cells;
}

std::vector<std::size_t> Plot::lEnds(std::size_t start, std::size_t minAlong, std::size_t tail, int minLen, int maxLen, int minEach) const
{
    std::vector<std::size_t> ends;
    const Cell s = blobs_[start].centre;
    for (std::size_t j = start + minAlong; j + tail < blobs_.size(); ++j) {
        const int dx = blobs_[j].centre.first - s.first;
        const int dy = blobs_[j].centre.second - s.second;
        const int u = (dx + dy) / 2;
        const int v = (dx - dy) / 2;
        const int length = std::abs(u) + std::abs(v);
        if (minLen <= length && length <= maxLen && minEach <= std::abs(u) && minEach <= std::abs(v)) {
            ends.push_back(j);
        }
    }
    return ends;
}

CellSet Plot::carve(const std::vector<Cell>& ways, int clearOf, Cell start, int startRadius, bool fillPinholes) const
{
    CellSet open;
    std::vector<std::vector<Cell>> holes;
    for (const Chamber& c : blobs_) {
        openRoom(open, holes, c);
    }
    if (!holes.empty()) {
        CellSet near;
        for (const Cell& w : ways) {
            for (int dy = -clearOf; dy <= clearOf; ++dy) {
                for (int dx = -clearOf; dx <= clearOf; ++dx) {
                    near.insert({w.first + dx, w.second + dy});
                }
            }
        }
        for (const std::vector<Cell>& hole : holes) {
            const bool free = std::ranges::none_of(hole, [&](const Cell& h) { return near.contains(h) || manhattan(h, start) <= startRadius; });
            if (free) {
                for (const Cell& h : hole) {
                    open.erase(h);
                }
            }
        }
    }
    // A lone cell left out with floor on all four sides (where blobs' edges meet just so) is floor
    // too: no wall shape draws a lone post, so it would be a hole in the floor.
    for (bool again = fillPinholes; again;) {
        again = false;
        std::vector<Cell> specks;
        for (const Cell c : open) {
            for (const Cell& s : kSides) {
                const Cell n = plus(c, s);
                if (!open.contains(n) && std::ranges::all_of(kSides, [&](const Cell& t) { return open.contains(plus(n, t)); })) {
                    specks.push_back(n);
                }
            }
        }
        for (const Cell& n : specks) {
            again = open.insert(n) || again;
        }
    }
    return open;
}

CellSet reachable(const CellSet& open, Cell from, const std::vector<Cell>& without)
{
    CellSet seen;
    if (!open.contains(from)) {
        return seen;
    }
    CellSet gone;
    for (const Cell& c : without) {
        if (CellSet::fits(c)) {
            gone.insert(c);
        }
    }
    if (gone.contains(from)) {
        return seen;
    }
    std::vector<Cell> todo = {from};
    seen.insert(from);
    while (!todo.empty()) {
        const Cell c = todo.back();
        todo.pop_back();
        for (const Cell& s : kSides) {
            const Cell n = plus(c, s);
            if (open.contains(n) && !gone.contains(n) && seen.insert(n)) {
                todo.push_back(n);
            }
        }
    }
    return seen;
}

// ---- The scans that place things -------------------------------------------------------------

std::vector<Cell> lineAcross(const CellSet& open, Cell at, Cell d, int most, const CellSet* gone)
{
    const auto floor = [&](Cell c) { return open.contains(c) && !(gone && gone->contains(c)); };
    if (!floor(at)) {
        return {};
    }
    const Cell e{d.first, -d.second};
    std::vector<Cell> line{at};
    for (const int s : {1, -1}) {
        for (int t = 1; t <= most; ++t) {
            const Cell c{at.first + s * t * e.first, at.second + s * t * e.second};
            if (!floor(c)) {
                break;
            }
            line.push_back(c);
        }
    }
    return line;
}

bool unusedAround(const CellSet& open, const CellSet& used, Cell c)
{
    return unused(open, used, c) && std::ranges::all_of(kSides, [&](const Cell& s) { return unused(open, used, plus(c, s)); });
}

std::vector<Cell> spotsNear(const std::vector<Cell>& path, Cell near, int from, int to, const CellSet& open, const CellSet& used, Cell start,
                            int minFromStart)
{
    std::vector<Cell> best;
    for (const Cell& c : path) {
        const int d = detail::chebyshev(c, near);
        if (from <= d && d <= to && unusedAround(open, used, c) && detail::chebyshev(c, start) >= minFromStart) {
            best.push_back(c);
        }
    }
    return best;
}

std::optional<Cell> firstAround(Cell c, int from, int to, const CellSet& open, const CellSet& used, bool around)
{
    for (int r = from; r <= to; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const Cell t{c.first + dx, c.second + dy};
                if ((t.first + t.second) % 2 == 0 && (around ? unusedAround(open, used, t) : unused(open, used, t))) {
                    return t;
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> wallMounts(const Backs& backs, const CellMap& walls, int shape, const CellSet& open, const CellSet& used,
                                    const std::vector<Cell>& path, std::size_t from, std::size_t to, int beside, int apart, std::size_t most)
{
    std::vector<std::size_t> mounts;
    for (std::size_t i = 0; i < backs.size(); ++i) {
        const auto& [w, front] = backs[i];
        if (walls.get(w) != shape || !unused(open, used, front)) {
            continue;
        }
        bool near = false;
        for (std::size_t j = from; j < to && j < path.size(); ++j) {
            near = near || detail::chebyshev(front, path[j]) <= beside;
        }
        if (near && std::ranges::all_of(mounts, [&](std::size_t m) { return detail::chebyshev(backs[m].first, w) >= apart; })) {
            mounts.push_back(i);
        }
        if (mounts.size() == most) {
            break;
        }
    }
    return mounts;
}

std::vector<std::size_t> backsNear(const Backs& backs, Cell c, int radius)
{
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < backs.size(); ++i) {
        if (detail::chebyshev(backs[i].second, c) <= radius) {
            out.push_back(i);
        }
    }
    return out;
}

std::optional<Cell> straightBack(const Backs& backs, const CellMap& walls, int shape, Cell near, const std::vector<Cell>& avoid)
{
    const Cell runA = shape == 0 ? kSides[0] : kSides[1];
    const Cell runB = shape == 0 ? kSides[2] : kSides[3]; // (the other way along it)
    std::optional<Cell> best;
    int bestD = 1 << 30;
    for (const auto& [w, front] : backs) {
        if (walls.get(w) != shape || walls.get(plus(w, runA)) != shape || walls.get(plus(w, runB)) != shape) {
            continue;
        }
        if (std::ranges::any_of(avoid, [&](const Cell& a) { return detail::chebyshev(a, w) < 5; })) {
            continue;
        }
        if (const int d = detail::chebyshev(w, near); d < bestD) {
            bestD = d;
            best = w;
        }
    }
    return best;
}

Backs backWalls(const CellMap& walls, const CellSet& open, const CellSet& exclude)
{
    Backs out;
    for (const Cell w : walls.keys()) {
        const int shape = walls.get(w);
        if (exclude.contains(w)) {
            continue;
        }
        if (shape == 0 && open.contains(plus(w, kSides[1]))) {
            out.push_back({w, plus(w, kSides[1])});
        } else if (shape == 1 && open.contains(plus(w, kSides[2]))) {
            out.push_back({w, plus(w, kSides[2])});
        }
    }
    return out;
}

bool deep(const CellSet& open, Cell c)
{
    if (!open.contains(c)) {
        return false;
    }
    for (const auto* around : {&kSides, &kCorners}) {
        for (const Cell& o : *around) {
            if (!open.contains(plus(c, o))) {
                return false;
            }
        }
    }
    return true;
}

void roughBlobs(CellMap& floor, CellMap& layer, std::minstd_rand& rng, const std::vector<Cell>& cells, std::size_t count,
                const std::vector<int>& floors, const std::vector<bool>& harms, const CellSet& open, const CellSet& avoid, int layerValue)
{
    if (cells.empty() || floors.empty()) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const Cell c = cells[rng() % cells.size()];
        const std::size_t which = rng() % floors.size();
        const int f = floors[which];
        const bool harming = harms[which];
        const int size = 2 + static_cast<int>(rng() % 3);
        for (int dy = -size; dy <= size; ++dy) {
            for (int dx = -size; dx <= size; ++dx) {
                const Cell t{c.first + dx, c.second + dy};
                // (a rough blob: the square's corners nibbled at random)
                if (deep(open, t) && std::max(std::abs(dx), std::abs(dy)) <= size - static_cast<int>(rng() % 2) && !(harming && avoid.contains(t))) {
                    floor.set(t, f);
                    layer.set(t, layerValue);
                }
            }
        }
    }
}

void paintWithin(CellMap& floor, CellMap& layer, Cell c, int radius, const CellSet& open, int f, int layerValue)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        const int w = radius - std::abs(dy);
        for (int dx = -w; dx <= w; ++dx) {
            const Cell t{c.first + dx, c.second + dy};
            if (open.contains(t)) {
                floor.set(t, f);
                layer.set(t, layerValue);
            }
        }
    }
}

void paintAround(CellMap& floor, CellMap& layer, const std::vector<Cell>& cells, const CellSet& open, int f, int layerValue)
{
    for (const Cell& at : cells) {
        for (const Cell& t : {at, plus(at, kSides[0]), plus(at, kSides[1]), plus(at, kSides[2]), plus(at, kSides[3])}) {
            if (deep(open, t)) {
                floor.set(t, f);
                layer.set(t, layerValue);
            }
        }
    }
}

void absorb(CellMap& floor, CellMap& layer, int base, const std::vector<int>& never, int passes)
{
    const std::vector<Cell> order = floor.keys().list(); // (the cells don't change, their floors may)
    for (int pass = 0; pass < passes; ++pass) {
        for (const Cell& t : order) {
            if (floor.get(t) != base) {
                continue;
            }
            // The other floors on its sides, counted, in ascending order (a std::map's walk).
            std::array<std::pair<int, int>, 4> around{};
            std::size_t kinds = 0;
            for (const Cell& s : kSides) {
                const int f = floor.get(plus(t, s));
                if (f >= 0 && f != base) {
                    std::size_t k = 0;
                    while (k < kinds && around[k].first != f) {
                        ++k;
                    }
                    if (k == kinds) {
                        around[kinds++] = {f, 0};
                    }
                    ++around[k].second;
                }
            }
            std::sort(around.begin(), around.begin() + static_cast<std::ptrdiff_t>(kinds));
            for (std::size_t k = 0; k < kinds; ++k) {
                const auto [other, n] = around[k];
                if (3 <= n && std::ranges::find(never, other) == never.end()) {
                    floor.set(t, other); // (a harming floor never spreads: a speck of ground in it stays)
                    for (const Cell& s : kSides) {
                        if (floor.get(plus(t, s)) == other) {
                            layer.set(t, std::max(std::max(0, layer.get(t)), std::max(0, layer.get(plus(t, s)))));
                        }
                    }
                }
            }
        }
    }
}

// ---- Objects ---------------------------------------------------------------------------------

CellMap wallsMap(const CellSet& open)
{
    CellMap out;
    for (const auto& [c, shape] : wallsList(open)) {
        out.set(c, shape);
    }
    return out;
}

std::vector<Cell> roomy(const CellSet& open)
{
    std::vector<Cell> out;
    for (const Cell c : open) {
        if (std::ranges::all_of(kSides, [&](const Cell& s) { return open.contains(plus(c, s)); })) {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<Cell> freeCell(std::minstd_rand& rng, const std::vector<Cell>& cells, CellSet& used, Cell start, int nearest,
                             std::optional<Cell> near, int farthest, int tries)
{
    for (int t = 0; t < tries; ++t) {
        const Cell c = cells[rng() % cells.size()];
        if (used.contains(c) || detail::chebyshev(c, start) < nearest || (near && detail::chebyshev(c, *near) > farthest)) {
            continue;
        }
        used.insert(c);
        return c;
    }
    return std::nullopt;
}

std::vector<Placed> scatter(std::minstd_rand& rng, const std::vector<Cell>& cells, int count, std::size_t kinds, int tries, CellSet& used,
                            const CellSet& clear, const CellSet* near, const CellSet* deepIn, Cell start, int minFromStart,
                            const std::vector<std::pair<CellSet*, int>>& marks)
{
    std::vector<Placed> out;
    if (kinds == 0) {
        return out;
    }
    for (int i = 0; i < count; ++i) {
        for (int t = 0; t < tries; ++t) {
            const Cell c = cells[rng() % cells.size()];
            if (used.contains(c) || clear.contains(c) || detail::chebyshev(c, start) < minFromStart || (deepIn && !deep(*deepIn, c)) ||
                (near && near->contains(c))) {
                continue;
            }
            out.push_back({c, static_cast<std::size_t>(rng() % kinds)});
            used.insert(c);
            for (const auto& [set, r] : marks) {
                for (int dx = -r; dx <= r; ++dx) {
                    for (int dy = -r; dy <= r; ++dy) {
                        set->insert({c.first + dx, c.second + dy});
                    }
                }
            }
            break;
        }
    }
    return out;
}

} // namespace nox::mapgen::blocks
