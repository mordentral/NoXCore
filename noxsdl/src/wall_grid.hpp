// The walls standing on a map, by where they are: a frame's line of sight, its scene draw and the
// 3D view ask for the walls in and around the view without looking at every wall the map has. No
// SDL: the tests use it too.
//
// The grid holds indices into the list it was built from (MapView::walls()), in blocks of
// kBlock x kBlock cells. Whoever owns that list builds the grid again whenever the list changes
// (MapView::setWallState, with the list standingWalls makes: a map loaded, walls broken, secret
// walls opened or closed by the server or a script, Wall-spell walls raised or gone, the editor's
// walls set), so it never holds a stale index; a grid built for a list of another length is never
// asked (size()). A map change makes a new MapView, and a grid of its own.
#pragma once

#include "noxcore/map/sections.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl {

/// The walls standing (MapView::setWallState's list, in its order): the map's (`mapWalls`) but
/// the broken ones (`destroyed`: DestructableWalls ids, `breakable` their cells by id) and the open
/// secret ones (`secret`: SecretWalls' cells and "starts open", by id; open when `opened` has its
/// id, or it starts open and `closed` hasn't), and but any in a Wall-spell wall's cell; then the
/// Wall-spell walls (`magic`, by cell) at the end. `breakableStanding`: `breakable`'s cells whose
/// map wall stands (a Wall-spell wall raised in a broken one's cell is a new wall, not breakable).
/// Cells are keyed (x << 8) | y, as MapView::wallKey.
void standingWalls(std::span<const nox::map::Wall> mapWalls, std::span<const std::uint16_t> breakable,
                   std::span<const std::pair<std::uint16_t, bool>> secret, const std::set<std::uint16_t>& destroyed,
                   const std::set<std::uint16_t>& opened, const std::set<std::uint16_t>& closed,
                   std::span<const std::pair<std::pair<int, int>, nox::map::Wall>> magic, std::vector<nox::map::Wall>& walls,
                   std::set<std::uint16_t>& breakableStanding);

class WallGrid {
public:
    static constexpr int kCells = 256;              ///< a wall's cell is a byte each way
    static constexpr int kBlock = 8;                ///< cells a side of a block
    static constexpr int kBlocks = kCells / kBlock; ///< blocks a side

    /// The grid of these walls (indices into `walls`), in place of what it held.
    void build(std::span<const nox::map::Wall> walls);

    /// How many walls the grid was built from.
    std::size_t size() const { return count_; }

    /// The indices of the walls whose cell is in [x0, x1] x [y0, y1] (either may reach past the
    /// map's cells), ascending: the order of the list the grid was built from. `out` is replaced.
    /// (One thread at a time: it marks what it finds in the grid's own scratch.)
    void find(int x0, int y0, int x1, int y1, std::vector<std::uint32_t>& out) const;

    /// find() for a test on where walls stand in world units (a wall's cell (x, y) at x * 23,
    /// y * 23): the walls in every cell whose point may be within `reach` of the box
    /// [left, right] x [top, bottom], and a cell more each way. The caller's own test on each then
    /// keeps exactly the walls a walk of the whole list keeps, in the list's order.
    void findAround(float left, float top, float right, float bottom, float reach, std::vector<std::uint32_t>& out) const;

private:
    struct Entry {
        std::uint32_t index = 0;
        std::uint8_t x = 0;
        std::uint8_t y = 0;
    };
    /// Block b's walls are entries_[start_[b], start_[b + 1]), each block's in the list's order.
    std::array<std::uint32_t, kBlocks * kBlocks + 1> start_{};
    std::vector<Entry> entries_;
    std::size_t count_ = 0;
    /// find()'s marks, a bit a wall by its index: all clear between calls.
    mutable std::vector<std::uint64_t> marks_;
};

} // namespace noxsdl
