// The map editor's Play From Here (play_from_here.hpp).
#include "noxcore/server/play_from_here.hpp"

#include "noxcore/server/game_host.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/wall_shapes.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace nox::server {

namespace {
constexpr float kCell = 23.0f;
constexpr int kGrid = 256;

int cellOf(float v)
{
    return static_cast<int>(std::floor(v / kCell));
}

/// A cell holds a wall of any kind (wall_shapes' cells: a door's leaf, a window and a secret wall
/// included; a broken wall is none, as the game's wall tests pass over it).
bool walled(const World& w, int cx, int cy)
{
    const auto it = w.walls().find({cx, cy});
    return it != w.walls().end() && !it->second.broken;
}

bool standableCell(const World& w, int cx, int cy, float x, float y)
{
    return cx >= 1 && cy >= 1 && cx < 255 && cy < 255 && !walled(w, cx, cy) && (!w.hasFloor() || w.floorAt(x, y) >= 0);
}

/// Where a player can walk, by the walls' lines: each cell split by its two diagonals into four
/// triangles (0 its top, 1 right, 2 bottom, 3 left); two triangles of a cell meet unless a wall's
/// half diagonal lies between them, and a triangle meets the one across its cell side always (the
/// game's walls lie only on the diagonals). Labels: 0 not reached, 1 the point's side, 2 a side
/// reached from a start.
class WalkMap {
public:
    explicit WalkMap(const World& w)
        : arms_(kGrid * kGrid, 0)
        , label_(static_cast<std::size_t>(kGrid) * kGrid * 4, 0)
    {
        using namespace wall_shapes;
        for (const auto& [at, c] : w.walls()) {
            if (at.first < 0 || at.first >= kGrid || at.second < 0 || at.second >= kGrid || c.broken || c.door != 0) {
                continue; // (a door opens: walked through)
            }
            if (c.secret && c.secretOpen >= 12) {
                continue; // (a secret wall half open or more: wallShape's none)
            }
            if (c.direction > 10) {
                continue;
            }
            const auto& [back, slash] = kShapes[c.direction];
            std::uint8_t a = 0;
            if (back.present && back.from < 11.5f) a |= kTopLeft;
            if (back.present && back.to > 11.5f) a |= kBottomRight;
            if (slash.present && slash.from < 11.5f) a |= kBottomLeft;
            if (slash.present && slash.to > 11.5f) a |= kTopRight;
            arms_[static_cast<std::size_t>(at.second) * kGrid + static_cast<std::size_t>(at.first)] = a;
        }
    }

    /// The triangle (x, y) is in; -1 off the grid.
    static int triangleAt(float x, float y)
    {
        const int cx = cellOf(x);
        const int cy = cellOf(y);
        if (cx < 0 || cy < 0 || cx >= kGrid || cy >= kGrid) {
            return -1;
        }
        const float lx = x - static_cast<float>(cx) * kCell;
        const float ly = y - static_cast<float>(cy) * kCell;
        const bool ne = ly < lx;         // (above the \ diagonal)
        const bool nw = ly < kCell - lx; // (above the / one)
        const int k = ne ? (nw ? 0 : 1) : (nw ? 3 : 2);
        return (cy * kGrid + cx) * 4 + k;
    }

    /// Floods from `start` what isn't labelled yet with `label`.
    void flood(int start, std::uint8_t label)
    {
        if (start < 0 || label_[static_cast<std::size_t>(start)] != 0) {
            return;
        }
        std::vector<int> todo{start};
        label_[static_cast<std::size_t>(start)] = label;
        while (!todo.empty()) {
            const int t = todo.back();
            todo.pop_back();
            const int cell = t / 4;
            const int k = t % 4;
            const int cx = cell % kGrid;
            const int cy = cell / kGrid;
            const std::uint8_t a = arms_[static_cast<std::size_t>(cell)];
            const auto visit = [&](int n) {
                if (label_[static_cast<std::size_t>(n)] == 0) {
                    label_[static_cast<std::size_t>(n)] = label;
                    todo.push_back(n);
                }
            };
            // Within the cell: across the half diagonals that aren't there.
            const int base = cell * 4;
            switch (k) {
            case 0:
                if (!(a & kTopRight)) visit(base + 1);
                if (!(a & kTopLeft)) visit(base + 3);
                if (cy > 0) visit(((cy - 1) * kGrid + cx) * 4 + 2);
                break;
            case 1:
                if (!(a & kTopRight)) visit(base + 0);
                if (!(a & kBottomRight)) visit(base + 2);
                if (cx + 1 < kGrid) visit((cy * kGrid + cx + 1) * 4 + 3);
                break;
            case 2:
                if (!(a & kBottomRight)) visit(base + 1);
                if (!(a & kBottomLeft)) visit(base + 3);
                if (cy + 1 < kGrid) visit(((cy + 1) * kGrid + cx) * 4 + 0);
                break;
            default:
                if (!(a & kBottomLeft)) visit(base + 2);
                if (!(a & kTopLeft)) visit(base + 0);
                if (cx > 0) visit((cy * kGrid + cx - 1) * 4 + 1);
                break;
            }
        }
    }

    std::uint8_t label(int triangle) const { return triangle < 0 ? 0 : label_[static_cast<std::size_t>(triangle)]; }
    /// A wall-free cell's label (its four triangles meet).
    std::uint8_t cellLabel(int cx, int cy) const { return label_[static_cast<std::size_t>((cy * kGrid + cx) * 4)]; }

private:
    static constexpr std::uint8_t kTopLeft = 1, kTopRight = 2, kBottomRight = 4, kBottomLeft = 8;
    std::vector<std::uint8_t> arms_;  ///< by cell: its half diagonals
    std::vector<std::uint8_t> label_; ///< by triangle
};

} // namespace

void rejoinFromObserving(Fight& fight, ServerObject& unit)
{
    joinTeam(fight, unit);
    leaveObserver(fight, unit);
    unit.player->cameraFollow = 0;
}

void placePlayerAt(World& world, ServerObject& unit, float x, float y, std::uint32_t frame)
{
    setPosition(world, unit, x, y, frame);
    if (unit.player) {
        unit.player->camX = x;
        unit.player->camY = y;
    }
}

bool standableSpot(const World& world, float x, float y)
{
    return standableCell(world, cellOf(x), cellOf(y), x, y);
}

std::optional<std::pair<float, float>> nearestOpenSpot(const World& world, float x, float y, int maxCells,
                                                       std::span<const std::pair<float, float>> from)
{
    if (standableSpot(world, x, y)) {
        return std::pair{x, y};
    }
    // Where a player can walk from: the map's starts, the players in play (wherever the game has
    // them now: a script's opened wall, a teleporter) and the character's own place.
    std::vector<int> sources;
    for (const auto& [extent, o] : world.objects()) {
        if (o.gone()) {
            continue;
        }
        if (o.typeName == "PlayerStart" || (o.player && !o.player->observing && !(o.flags & ObjFlag::Dead))) {
            sources.push_back(WalkMap::triangleAt(o.x, o.y));
        }
    }
    for (const auto& [fx, fy] : from) {
        sources.push_back(WalkMap::triangleAt(fx, fy));
    }
    std::erase(sources, -1);
    WalkMap walk(world);
    const int here = WalkMap::triangleAt(x, y);
    walk.flood(here, 1); // (the point's side of the wall)
    bool hereReached = false;
    for (const int s : sources) {
        hereReached = hereReached || walk.label(s) == 1;
        walk.flood(s, 2);
    }
    const int cx = cellOf(x);
    const int cy = cellOf(y);
    // What a player shouldn't be put in or on near there: solid things (a pillar, a statue, a
    // door, a generator: pushed out of them by the next frame's motion, maybe into a wall), and a
    // pit, a fire, a trap or a teleporter's pad (each a reach of its own).
    namespace oc = things::ObjectClass;
    struct Keep {
        float x, y, reach;
    };
    std::vector<Keep> keepOff;
    const float reachOut = (static_cast<float>(maxCells) + 2.0f) * kCell;
    for (const auto& [extent, o] : world.objects()) {
        if (o.gone() || o.holder != 0 || std::abs(o.x - x) > reachOut || std::abs(o.y - y) > reachOut) {
            continue;
        }
        const bool solid = o.collides && !(o.flags & ObjFlag::AllowOverlap) && (o.classes & (oc::Immobile | oc::Obstacle | oc::Door | oc::MonsterGenerator));
        const bool harm = (o.classes & (oc::Hole | oc::Dangerous | oc::Fire | oc::Transporter)) != 0;
        if (solid || harm) {
            keepOff.push_back({o.x, o.y, std::max(o.radius, 0.71f * std::max(o.boxWidth, o.boxHeight)) + 12.0f});
        }
    }
    const auto clearOfThings = [&](float mx, float my) {
        return std::ranges::none_of(keepOff, [&](const Keep& k) { return std::hypot(k.x - mx, k.y - my) < k.reach; });
    };
    bool avoidThings = true;
    // Ring by ring (the point's own cell first); in a ring the cell whose middle is nearest it.
    const auto search = [&](const auto& wanted) -> std::optional<std::pair<float, float>> {
        for (int r = 0; r <= maxCells; ++r) {
            std::optional<std::pair<float, float>> best;
            float bestD = 0;
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (std::max(std::abs(dx), std::abs(dy)) != r || nx < 1 || ny < 1 || nx >= 255 || ny >= 255) continue;
                    const float mx = (static_cast<float>(nx) + 0.5f) * kCell;
                    const float my = (static_cast<float>(ny) + 0.5f) * kCell;
                    if (!standableCell(world, nx, ny, mx, my) || world.noTeleportAt(mx, my) || !wanted(walk.cellLabel(nx, ny)) ||
                        (avoidThings && !clearOfThings(mx, my))) {
                        continue;
                    }
                    const float d = (mx - x) * (mx - x) + (my - y) * (my - y);
                    if (!best || d < bestD) {
                        best = std::pair{mx, my};
                        bestD = d;
                    }
                }
            }
            if (best) return best;
        }
        return std::nullopt;
    };
    // The point's side when it can be walked to; any side that can; and only with none of those in
    // reach (a quest's rooms behind walls its script opens later), the point's side, then any. Clear
    // of things first in each; beside them only when that pass has no clear spot.
    const auto pass = [&](const auto& wanted) -> std::optional<std::pair<float, float>> {
        avoidThings = true;
        if (auto spot = search(wanted)) {
            return spot;
        }
        avoidThings = false;
        return search(wanted);
    };
    if (hereReached) {
        if (auto spot = pass([](std::uint8_t l) { return l == 1; })) {
            return spot;
        }
    }
    if (auto spot = pass([hereReached](std::uint8_t l) { return l == 2 || (hereReached && l == 1); })) {
        return spot;
    }
    if (auto spot = pass([](std::uint8_t l) { return l == 1; })) {
        return spot;
    }
    return pass([](std::uint8_t) { return true; });
}

PlayFromHere::PlayFromHere(float x, float y, std::string map, bool quest)
    : x_(x)
    , y_(y)
    , map_(std::move(map))
    , quest_(quest)
{
}

bool PlayFromHere::tick(GameHost& g)
{
    World& world = g.server().world();
    // (A quest's test starts in its chat map: the test's map is up once the quest has loaded it.)
    if (!util::equalsNoCase(world.mapName, map_)) {
        return placed_; // (placed and the map changed since: nothing more to keep)
    }
    const std::uint32_t frame = g.server().clock().frame();
    if (placed_) {
        ServerObject* unit = world.byExtent(extent_);
        if (!unit || unit->gone() || frame - placedFrame_ > settleFrames) {
            return true;
        }
        // A move no walk makes, onto a player start (the game's own placing at a start, after
        // ours): back to the point. (A teleporter walked into goes where it goes.)
        const float dx = unit->x - last_.first, dy = unit->y - last_.second;
        const auto atStart = [&] {
            for (const auto& [extent, o] : world.objects()) {
                if (o.typeName == "PlayerStart" && std::abs(o.x - unit->x) < 60 && std::abs(o.y - unit->y) < 60) return true;
            }
            return false;
        };
        // A death in the settling frames ends the watch: its respawn at a start is the game's own.
        if (unit->flags & ObjFlag::Dead) {
            return true;
        }
        if (std::sqrt(dx * dx + dy * dy) > 46.0f && atStart()) {
            ++undone_;
            placePlayerAt(world, *unit, spot_.first, spot_.second, frame);
        }
        last_ = {unit->x, unit->y};
        return false;
    }
    ServerObject* unit = nullptr;
    for (const auto& [peer, p] : g.players()) {
        if (p.ready && (g.isHostPeer(peer) || g.players().size() == 1)) {
            unit = world.byExtent(p.extent);
        }
    }
    if (!unit && world.solo()) {
        unit = hostPlayer(world); // (a solo game's, once its player is ready)
        if (unit && std::ranges::none_of(g.players(), [](const auto& e) { return e.second.ready; })) {
            unit = nullptr;
        }
    }
    if (!unit || unit->gone() || (unit->flags & ObjFlag::Dead)) {
        return false;
    }
    const bool watched = unit->player && unit->player->observing;
    if (watched) {
        // Watching (a network game's join): brought into play as the observer's rejoin brings it
        // (its team, out of observing, its camera its own), at the point instead of a player
        // start. A quest brings its players in itself: put there once it has.
        if (world.quest()) {
            return false;
        }
        Fight fight = g.server().fight();
        rejoinFromObserving(fight, *unit);
    }
    // (Where it stands counts as a place it can walk from, unless it was only watching.)
    const std::pair<float, float> own{unit->x, unit->y};
    spot_ = nearestOpenSpot(world, x_, y_, 12, watched ? std::span<const std::pair<float, float>>() : std::span(&own, 1)).value_or(own);
    placePlayerAt(world, *unit, spot_.first, spot_.second, frame);
    placed_ = true;
    placedFrame_ = frame;
    extent_ = unit->extent;
    last_ = {unit->x, unit->y};
    return false;
}

} // namespace nox::server
