// The walls standing on a map, by where they are: a frame's line of sight asks for the walls in
// and around its view without looking at every wall the map has. No SDL: the tests use it too.
//
// The grid holds indices into the list it was built from (MapView::walls()), in blocks of
// kBlock x kBlock cells. Whoever owns that list builds the grid again whenever the list changes
// (MapView::setWallState: a map loaded, walls broken, secret walls opened or closed, Wall-spell
// walls raised or gone, the editor's walls set), so it never holds a stale index; a grid built for
// a list of another length is never asked (size()).
#pragma once

#include "noxcore/map/sections.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace noxsdl {

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
