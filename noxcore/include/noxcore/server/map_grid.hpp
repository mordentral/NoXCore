// The exe's map grid and object lists: what GAME.EXE keeps of where the objects on the map are,
// for its area searches, and the lists its units update walks.
//
//   - The grid (70 x 70 cells of 85, 16 bytes each): each
//     cell two lists, newest linked first. The point list holds every object linked, in the
//     cell of its NewPos (one node); the extent list every object but a missile, in
//     each cell its collision box (CollideP1 / P2: about its NewPos) meets,
//     row by row, at most four (four nodes and their count). The link puts one in
//     (not DESTROYED 0x20, not PARTITIONED 0x200 already, ACTIVE 4), the unlink takes one out,
//     moveUpdateSpecial does both (outside the grid: deleted instead).
//   - getUnitsInRect: the extent lists of the cells a rectangle covers, row by row
//     (y outer, x inner), each list from its head; each object once (a stamp per search,
//     two searches deep at most: a third inside them finds nothing), those whose box strictly
//     meets the rectangle. A cell's next is read after the call: what the call moves takes the
//     walk with it, as the exe's.
//   - The updatable list (unitAddToUpdatable puts one at its head; unitRemoveFromUpdatable takes
//     it off) and the missiles' list (the new things' pass puts each at its head), which the units
//     update walks (updateUnits).
//
// Data structures are ours: an object keeps one index (GridSlot) into a pool of entries, each its
// box, its stamps, its five nodes and its list links; links are indices, not pointers.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace nox::server {

struct ServerObject;

/// An object's entry in the map grid's pool (0: none). A copy of an object is in nothing: the
/// pool knows the object it was linked as; an object assigned over keeps its own entry.
class GridSlot {
public:
    GridSlot() = default;
    GridSlot(const GridSlot&) noexcept {}
    GridSlot& operator=(const GridSlot&) noexcept { return *this; }
    std::uint32_t get() const noexcept { return v_; }

private:
    friend class MapGrid;
    std::uint32_t v_ = 0;
};

class MapGrid {
public:
    static constexpr int kSize = 70;   ///< cells a side
    static constexpr float kCell = 85; ///< (1 / 85)

    struct Rect {
        float left = 0;
        float top = 0;
        float right = 0;
        float bottom = 0;
    };

    /// A coordinate's cell, chopped (the FPU's mode) from its x 1/85; below 0 the
    /// coordinate itself, as Math_FloatToInt truncates it (not divided: the exe's).
    static int cellOf(float v) noexcept
    {
        if (v < 0.0f) {
            return static_cast<int>(v);
        }
        return static_cast<int>(static_cast<double>(v) * static_cast<double>(1.0f / 85.0f));
    }
    /// Whether a point's cell is in the grid.
    static bool inside(float x, float y) noexcept
    {
        const int cx = cellOf(x);
        const int cy = cellOf(y);
        return -1 < cx && cx < kSize && -1 < cy && cy < kSize;
    }

    MapGrid() : cells_(static_cast<std::size_t>(kSize) * kSize) {}
    MapGrid(MapGrid&&) noexcept = default;
    MapGrid& operator=(MapGrid&&) noexcept = default;
    MapGrid(const MapGrid&) = delete;
    MapGrid& operator=(const MapGrid&) = delete;

    /// PARTITIONED (0x200): linked in the grid.
    bool linked(const ServerObject& o) const noexcept;
    /// Its collision box as last linked (CollideP1 / P2), if it has an entry.
    const Rect* box(const ServerObject& o) const noexcept;
    /// Links it (the caller tests its flags): `box` its CollideP about its NewPos
    /// (x, y), a missile in the point list only.
    void link(ServerObject& o, const Rect& box, float x, float y, bool missile);
    /// noxcpp: a linked missile's box replaced where it is (its shape changed by a mode: in the
    /// point list alone, so no cell changes and no list's order); anything else is left alone.
    void setMissileBox(ServerObject& o, const Rect& box) noexcept;
    /// Unlinks it.
    void unlink(ServerObject& o);
    /// Off the grid and the lists, its entry given back (servFinalizeDelObject, or
    /// a copy of it gone).
    void release(ServerObject& o);
    /// The entries given back since the last call reusable (no walk is under way).
    void recycle();
    /// Every entry dropped (a new map).
    void clear();

    // The updatable list (newest first) and the missiles' list.
    bool updatable(const ServerObject& o) const noexcept;
    /// unitAddToUpdatable (not a missile, not on it already: the caller asks the
    /// class).
    void addUpdatable(ServerObject& o);
    /// unitRemoveFromUpdatable.
    void removeUpdatable(ServerObject& o);
    void addMissile(ServerObject& o);
    void removeMissile(ServerObject& o);
    /// Onto the updatable list's / the missiles' list's tail, in the order given, each not on a
    /// list already: what a map change keeps, behind the new map's things (the exe's lists are
    /// the game's, which the new map's join at their heads).
    void appendUpdatable(std::span<ServerObject* const> objects);
    void appendMissiles(std::span<ServerObject* const> objects);
    ServerObject* firstUpdatable() const noexcept { return listFirst(updHead_); }
    /// The units update's first walk (updateUnits): each one's next taken before its turn, as the
    /// exe's; one taken off the list keeps its link, so the walk goes on from it.
    template <class Fn>
    void eachUpdatableNextFirst(Fn&& fn)
    {
        for (std::uint32_t h = updHead_; h != 0;) {
            const std::uint32_t n = pool_[h - 1].listNext;
            ServerObject* o = pool_[h - 1].object;
            fn(*o);
            h = n;
        }
    }
    /// A walk taking each one's next after its turn (updateUnits's last pass; the missiles' list's).
    template <class Fn>
    void eachUpdatable(Fn&& fn)
    {
        for (std::uint32_t h = updHead_; h != 0; h = pool_[h - 1].listNext) {
            ServerObject* o = pool_[h - 1].object;
            fn(*o);
        }
    }
    template <class Fn>
    void eachMissile(Fn&& fn)
    {
        for (std::uint32_t h = missileHead_; h != 0; h = pool_[h - 1].listNext) {
            ServerObject* o = pool_[h - 1].object;
            fn(*o);
        }
    }

    /// Whether a walk (inRect) is running: another one now finds nothing (the game's own rule).
    bool walking() const noexcept { return depth_ >= 1; }

    /// getUnitsInRect.
    template <class Fn>
    void inRect(const Rect& r, Fn&& fn)
    {
        inRectUntil(r, [&](ServerObject& o) {
            fn(o);
            return false;
        });
    }
    /// The same walk, ended at the first object `fn` answers true for: for a search whose
    /// callback does nothing more once it has its answer (any one across a line, say). (The
    /// entries it didn't reach keep their old stamps: a stamp counts only in its own search.)
    template <class Fn>
    void inRectUntil(const Rect& r, Fn&& fn)
    {
        if (depth_ >= 1) {
            return; // (triggersCount: two deep at most)
        }
        ++depth_;
        struct Leave {
            int& d;
            ~Leave() { --d; }
        } leave{depth_};
        const std::size_t d = static_cast<std::size_t>(depth_);
        const std::uint32_t stamp = ++stamps_[d];
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        span(r, x0, y0, x1, y1);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                for (std::uint32_t h = cell(cx, cy).extent; h != 0;) {
                    const std::uint32_t ei = (h - 1) / 5;
                    Entry& e = pool_[ei];
                    if (e.stamp[d] != stamp) {
                        e.stamp[d] = stamp;
                        if (e.box.left < r.right && r.left < e.box.right && e.box.top < r.bottom && r.top < e.box.bottom) {
                            ServerObject* o = e.object;
                            if (fn(*o)) {
                                return;
                            }
                        }
                    }
                    h = node(h).next;
                }
            }
        }
    }
    /// The missiles in the point lists of the cells a rectangle covers (no box, no
    /// stamp: each once, a missile being in one cell).
    template <class Fn>
    void missilesInRect(const Rect& r, Fn&& fn)
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        span(r, x0, y0, x1, y1);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                for (std::uint32_t h = cell(cx, cy).point; h != 0;) {
                    Entry& e = pool_[(h - 1) / 5];
                    if (e.missile) {
                        ServerObject* o = e.object;
                        fn(*o);
                    }
                    h = node(h).next;
                }
            }
        }
    }
    /// The extent list of the cell a point's in (clamped to the grid), all of it.
    template <class Fn>
    void inCell(float x, float y, Fn&& fn)
    {
        int cx = cellOf(x);
        int cy = cellOf(y);
        cx = cx < 0 ? 0 : (kSize <= cx ? kSize - 1 : cx);
        cy = cy < 0 ? 0 : (kSize <= cy ? kSize - 1 : cy);
        for (std::uint32_t h = cell(cx, cy).extent; h != 0;) {
            ServerObject* o = pool_[(h - 1) / 5].object;
            fn(*o);
            h = node(h).next;
        }
    }

    // The waypoints' lists (each cell's, waypointMapRegister: a waypoint at its
    // head, in the cell of where it was when made, never moved: MoveWaypoint doesn't
    // relink it) and their search stamp (each cell's too).
    /// waypointMapRegister (`id` 1 on; one outside the grid is put in none: the
    /// exe's writes past its columns, an original bug).
    void registerWaypoint(std::uint32_t id, float x, float y);
    /// How many waypoints (ids 1..n) are registered.
    std::uint32_t waypointsRegistered() const noexcept { return static_cast<std::uint32_t>(wpNext_.size()); }
    /// The waypoints' search stamp, a new one for a search.
    std::uint32_t newWaypointStamp() noexcept { return ++wpStamp_; }
    /// The waypoint search's walk: the cells x0..x1 by y0..y1 (clamped to the grid as it clamps), y
    /// outer, each not yet stamped with `stamp`: its waypoints from its head (by id), then the
    /// cell stamped.
    template <class Fn>
    void waypointCells(int x0, int y0, int x1, int y1, std::uint32_t stamp, Fn&& fn)
    {
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = kSize <= x1 ? kSize - 1 : x1;
        y1 = kSize <= y1 ? kSize - 1 : y1;
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                Cell& c = cell(cx, cy);
                if (c.waypointStamp == stamp) {
                    continue;
                }
                for (std::uint32_t id = c.waypoint; id != 0; id = wpNext_[id - 1]) {
                    fn(id);
                }
                c.waypointStamp = stamp;
            }
        }
    }

    /// For the tests: what each cell holds, head first (true: the extent list).
    std::vector<const ServerObject*> cellList(int cx, int cy, bool extent) const;
    /// For the tests and checks: the cells it's linked in (the point cell first).
    std::vector<std::pair<int, int>> cellsOf(const ServerObject& o) const;

private:
    struct Node {
        std::uint32_t next = 0; ///< a node handle: entry x 5 + its node + 1 (0: none)
        std::uint32_t prev = 0;
        std::int16_t cx = 0;
        std::int16_t cy = 0;
    };
    struct Entry {
        ServerObject* object = nullptr;
        Rect box{};
        std::array<std::uint32_t, 2> stamp{};
        std::array<Node, 5> nodes{}; ///< [0] the point list's, [1..4] the extent lists'
        std::uint32_t listNext = 0;  ///< its list's (entry + 1)
        std::uint32_t listPrev = 0;
        std::uint8_t count = 0;      ///< extent nodes linked
        std::uint8_t list = 0;       ///< 0 none, 1 updatable, 2 missiles
        bool linked = false;
        bool missile = false;
        bool live = false;           ///< its object's (not given back)
    };
    struct Cell {
        std::uint32_t point = 0;
        std::uint32_t extent = 0;
        std::uint32_t waypoint = 0;      ///< the head waypoint's id
        std::uint32_t waypointStamp = 0;
    };

    Cell& cell(int cx, int cy) noexcept { return cells_[static_cast<std::size_t>(cx) * kSize + static_cast<std::size_t>(cy)]; }
    const Cell& cell(int cx, int cy) const noexcept { return cells_[static_cast<std::size_t>(cx) * kSize + static_cast<std::size_t>(cy)]; }
    Node& node(std::uint32_t h) noexcept { return pool_[(h - 1) / 5].nodes[(h - 1) % 5]; }
    static void span(const Rect& r, int& x0, int& y0, int& x1, int& y1) noexcept
    {
        x0 = cellOf(r.left);
        y0 = cellOf(r.top);
        x1 = cellOf(r.right);
        y1 = cellOf(r.bottom);
        if (x0 < 0) {
            x0 = 0;
        }
        if (kSize <= x1) {
            x1 = kSize - 1;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (kSize <= y1) {
            y1 = kSize - 1;
        }
    }
    std::uint32_t entryOf(ServerObject& o);
    void addNode(std::uint32_t ei, int k, int cx, int cy, bool extent);
    void removeNode(std::uint32_t ei, int k, bool extent);
    void listAdd(ServerObject& o, std::uint32_t& head, std::uint8_t which);
    void listRemove(ServerObject& o, std::uint32_t& head);
    void listAppend(std::span<ServerObject* const> objects, std::uint32_t& head, std::uint8_t which);
    ServerObject* listFirst(std::uint32_t head) const noexcept { return head ? pool_[head - 1].object : nullptr; }

    std::vector<Entry> pool_;
    std::vector<std::uint32_t> free_;  ///< reusable entries
    std::vector<std::uint32_t> freed_; ///< given back since the last recycle
    std::vector<Cell> cells_;
    int depth_ = -1;
    std::array<std::uint32_t, 2> stamps_{};
    std::uint32_t updHead_ = 0;
    std::uint32_t missileHead_ = 0;
    std::vector<std::uint32_t> wpNext_; ///< each registered waypoint's next in its cell (by id - 1)
    std::uint32_t wpStamp_ = 0;
};

} // namespace nox::server
