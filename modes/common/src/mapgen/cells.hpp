// Cell arithmetic on the wall grid shared by the map builder's native parts (layout.cpp,
// blocks.cpp, map_writer.cpp). Floor cells sit on the x + y even checkerboard, so a cell's neighbours are diagonal
// steps.
#pragma once

#include "noxcore/mapgen/layout.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace nox::mapgen::detail {

/// A cell's four sides' neighbours in NE SE SW NW order (the iso diamond's edges; the bit order
/// of wallsList's shapes), and the four cells two away in a straight line, N E S W, which meet
/// a cell only at a corner.
inline constexpr std::array<Cell, 4> kSides = {Cell{1, -1}, Cell{1, 1}, Cell{-1, 1}, Cell{-1, -1}};
inline constexpr std::array<Cell, 4> kCorners = {Cell{0, -2}, Cell{2, 0}, Cell{0, 2}, Cell{-2, 0}};

constexpr Cell plus(Cell a, Cell b)
{
    return {a.first + b.first, a.second + b.second};
}

inline int manhattan(Cell a, Cell b)
{
    return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}

inline int chebyshev(Cell a, Cell b)
{
    return std::max(std::abs(a.first - b.first), std::abs(a.second - b.second));
}

} // namespace nox::mapgen::detail
