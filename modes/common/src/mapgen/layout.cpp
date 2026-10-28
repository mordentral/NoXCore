// The walls fitted round a generated map's open floor (layout.hpp); the floor itself is plotted by
// the generator's script (modes/common/script/mapgen/layout.luau)
// over the native blocks (blocks.hpp).
#include "noxcore/mapgen/layout.hpp"

#include "cells.hpp"

#include "noxcore/map/wall_pieces.hpp"

#include <array>

namespace nox::mapgen {

using detail::kCorners;
using detail::kSides;
using detail::plus;

namespace {

// Wall neighbours: NE 1, SE 2, SW 4, NW 8 (kSides' order). Each set's shape is the facing for
// those arms (map/wall_pieces.hpp: one alone the straight run it ends, a lone post none, not
// drawn).
constexpr std::array<std::int8_t, 16> kShape = [] {
    std::array<std::int8_t, 16> shapes{};
    for (std::size_t bits = 0; bits < shapes.size(); ++bits) {
        const auto arms = static_cast<std::uint8_t>(((bits & 1) ? map::kArmNE : 0) | ((bits & 2) ? map::kArmSE : 0) |
                                                    ((bits & 4) ? map::kArmSW : 0) | ((bits & 8) ? map::kArmNW : 0));
        const auto facing = map::facingForArms(arms);
        shapes[bits] = facing ? static_cast<std::int8_t>(*facing) : std::int8_t{-1};
    }
    return shapes;
}();

} // namespace

std::vector<std::pair<Cell, std::uint8_t>> wallsList(const CellSet& open)
{
    // Every closed cell touching the floor by a side or a corner becomes a wall.
    CellSet walls;
    std::vector<Cell> list;
    for (const Cell c : open) {
        for (const auto* around : {&kSides, &kCorners}) {
            for (const Cell& o : *around) {
                const Cell w = plus(c, o);
                if (!open.contains(w) && walls.insert(w)) {
                    list.push_back(w);
                }
            }
        }
    }
    std::vector<std::pair<Cell, std::uint8_t>> shaped;
    shaped.reserve(list.size());
    for (const Cell& w : list) {
        int bits = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            if (walls.contains(plus(w, kSides[i]))) {
                bits |= 1 << i;
            }
        }
        if (kShape[static_cast<std::size_t>(bits)] >= 0) {
            shaped.emplace_back(w, static_cast<std::uint8_t>(kShape[static_cast<std::size_t>(bits)]));
        }
    }
    return shaped;
}

} // namespace nox::mapgen
