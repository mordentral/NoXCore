// Delving Mode's level layouts: where a generated map's floor is open,
// and the walls that enclose it.
//
// A long level is plotted as a path first: diagonal segments winding across the map, mostly
// eastward but doubling back now and then (Nox's straight walls run along the diagonals; a run
// along an axis is a zigzag), each with its own width, chambers at its start, its end and turns
// along the way (now and then an arena). Side branches leave the route at right angles, each
// ending in a dead-end room (the explorer's loot). No part of the route or a branch comes near
// another but where they join, so corridors never merge into one another. The plotting is the
// generator's script's (modes/common/script/mapgen/layout.luau, over the native carving blocks:
// blocks.hpp). The corridors and chambers are the open floor; walls are then fitted round it
// (wallsList), each cell's shape by which of its four diagonal neighbours are walls too, as the
// exe's own wall-combine table has the shapes: "/" 0, "\" 1, the corners 7-10, the T-junctions
// 3-6, the cross 2.
//
// Cells are wall-grid cells; walls and floor tiles both sit on the x + y even checkerboard, so
// every cell here is one of those. A cell's sides are its diagonal neighbours (x +- 1, y +- 1),
// its corners the cells two away along an axis.
#pragma once

#include "noxcore/mapgen/cell_set.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace nox::mapgen {

/// How a level's rooms are cut (a biome's: biome.hpp).
enum class LayoutStyle : std::uint8_t {
    Built,   ///< halls, pillared halls, crosses and rings off narrow corridors (dungeons, crypts, castles)
    Natural, ///< caverns: rooms of overlapping blobs, corridors of varied widths (caves, mines, swamps)
    Open,    ///< wide trails and big clearings (forests, grasslands, deserts, beaches)
};

/// A room's shape. Halls and their kin are cut in diagonal coordinates, (u, v) the cell
/// centre + (u + v, u - v): their sides run along the walls' diagonals, so they're straight
/// walls on screen (a rectangle there; one along the grid's axes would be a zigzag).
enum class RoomShape : std::uint8_t {
    Diamond, ///< |dx| + |dy| <= radius
    Hall,    ///< |u| <= a, |v| <= b
    Cross,   ///< two halls across each other: a x b and b x a (b the arms' half width)
    Ring,    ///< a hall round a block of rock (a - 3 x b - 3) : a loop
    Pillars, ///< a hall with two-by-two pillars of wall every four cells
    Cavern,  ///< overlapping blobs drawn from `seed`, all within the radius
};

struct Chamber {
    Cell centre;
    int radius = 0; ///< open within this of the centre (|dx| + |dy|), its walls two further out
    RoomShape shape = RoomShape::Diamond;
    int a = 0; ///< a hall's (cross's, ring's, pillared hall's) half sizes along its diagonals
    int b = 0;
    std::uint32_t seed = 0; ///< a cavern's draw
};

/// The walls that enclose `open`, in no order: each cell not open with an open one a side or a
/// corner away, shaped by its wall neighbours (sides NE, SE, SW, NW: "/" both NE and SW, "\" both
/// NW and SE, all four the cross; two at a corner 7 NE+NW, 8 NE+SE, 9 SE+SW, 10 SW+NW; three a T:
/// 3 / with NW, 4 \ with NE, 5 / with SE, 6 \ with SW; one alone, the run it ends).
std::vector<std::pair<Cell, std::uint8_t>> wallsList(const CellSet& open);

} // namespace nox::mapgen
