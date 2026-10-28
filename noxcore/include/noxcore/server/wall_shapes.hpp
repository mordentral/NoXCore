// The wall shapes' geometry, shared by the ray (World::traceRay) and the walls' pushes
// (physics.cpp). A wall cell is 23 units square; its shape (WallCell::direction, 0-10) is made
// of up to two diagonals: the \ one from the cell's top left corner along (1, 1), and the / one
// from its top right along (-1, 1), each between two x offsets in the cell (half ones end at its
// centre, 11.5).
#pragma once

#include <cstdint>
#include <utility>

namespace nox::server::wall_shapes {

inline constexpr float kCell = 23;

struct Segment {
    bool present;
    float from;
    float to;
};

/// Each shape's (\, /) diagonals.
inline constexpr std::pair<Segment, Segment> kShapes[11] = {
    {{false, 0, 0}, {true, 0, 23}},         {{true, 0, 23}, {false, 0, 0}},
    {{true, 0, 23}, {true, 0, 23}},         {{true, 0, 11.5f}, {true, 0, 23}},
    {{true, 0, 23}, {true, 11.5f, 23}},     {{true, 11.5f, 23}, {true, 0, 23}},
    {{true, 0, 23}, {true, 0, 11.5f}},      {{true, 0, 11.5f}, {true, 11.5f, 23}},
    {{true, 11.5f, 23}, {true, 11.5f, 23}}, {{true, 11.5f, 23}, {true, 0, 11.5f}},
    {{true, 0, 11.5f}, {true, 0, 11.5f}},
};

/// Which neighbours each shape's diagonals run on into: 1 up and right (end),
/// 2 up and left (\ start), 4 down and right (\ end), 8 down and left (/ start).
inline constexpr std::uint8_t kNeighbours[11] = {9, 6, 15, 11, 7, 13, 14, 3, 5, 12, 10};

} // namespace nox::server::wall_shapes
