// The map editor's wall tool rules: facings and their arms, auto-facing, the line / rectangle /
// outline shapes, material variants, and the ops that place and remove walls with their neighbours
// re-faced.
//
// A wall piece's facing (WallMap direction, low 7 bits) is one of 13 shapes; each joins some of
// its four diagonal neighbours on the wall grid (its "arms"): NW (-1,-1), NE (+1,-1), SW (-1,+1),
// SE (+1,+1). The Wall spell's own join table (raiseWall's combine of a wall's shape with a new
// one) is the union of these arms, and 99.1% of the stock maps' walls have exactly the arms that
// lead to neighbours whose own facing points back (the mutual rule).
// Facings 11 and 12 are the window pieces of 0 and 1 (a map stores a window as facing 0 or 1 plus
// a WindowWalls entry; the client draws slot 11 / 12 for it).
#pragma once

#include "noxcore/map/wall_pieces.hpp"
#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

struct TileCell;

/// Wall arms, a facing's arms and the facing for arms: map/wall_pieces.hpp.
using map::WallArm;
using map::kArmNW;
using map::kArmNE;
using map::kArmSW;
using map::kArmSE;
using map::armsOfFacing;
using map::facingForArms;
/// The arm pointing the other way (NW <-> SE, NE <-> SW).
std::uint8_t oppositeArm(std::uint8_t arm);
/// The neighbour cell an arm leads to, or nothing off the grid.
std::optional<Cell> armNeighbour(Cell c, std::uint8_t arm);
/// Whether a facing is a straight piece (0 or 1): the only ones that take random variants and
/// draw as a window piece.
inline bool straightFacing(int facing)
{
    return facing == 0 || facing == 1;
}

/// The wall cells around: which of the four diagonal neighbours hold a wall.
std::uint8_t neighbourArms(const MapDocument& doc, Cell c);
/// The arms of the wall at `c` that lead to a wall (its piece's arms that join something).
std::uint8_t joinedArms(const MapDocument& doc, Cell c);

/// How many variants a material has for a facing (thing.bin: the images of that direction), at
/// least 1.
int wallVariantCount(const things::ThingDb& things, int material, int facing);
/// The material's thing.bin random-variation count (the Wall spell's straight pieces take
/// x % it): the default auto-variation range is [0, it - 1].
int wallRandomVariations(const things::ThingDb& things, int material);

/// The minimap group a wall placed at `c` gets: the first polygon containing its centre (as the
/// game's load assigns it), else 100.
std::uint8_t minimapGroupAt(const MapDocument& doc, Cell c);

// --- Shapes --------------------------------------------------------------------------------------

/// A wall line from `a` to `b` along the nearer diagonal (walls join only diagonally), every
/// cell on it (x + y keeps a's parity). `b` is moved onto that diagonal.
std::vector<Cell> wallLine(Cell a, Cell b);
/// The four sides of the rectangle with sides along the diagonals and corners at `a` and the
/// cell nearest `b` (the old editor's 45-degree rectangle), each cell once.
std::vector<Cell> wallRectangle(Cell a, Cell b);
/// The walls round the edge of the floor region holding tile `start` (the tiles joined to it by
/// sides, any floor, through borders no wall lies on): a wall on every vertex of a border
/// between a region tile and a tile outside it (or a hole), with the arms along those borders
/// (so a stepped edge isn't joined across the region). At most `limit` tiles; empty when the
/// region is bigger (an open map) or `start` has no tile.
std::map<Cell, std::uint8_t> wallOutline(const MapDocument& doc, TileCell start, std::size_t limit = 20000);
/// The cells of an outline (or any arms map), in order.
std::vector<Cell> cellsOf(const std::map<Cell, std::uint8_t>& arms);

// --- Placing and removing --------------------------------------------------------------------------

/// What the wall tool places.
struct WallBrush {
    std::uint8_t material = 0;
    /// Facing for a placed wall that auto-facing leaves alone (no neighbours), or every placed
    /// wall with `autoFacing` off.
    std::uint8_t facing = 0;
    bool autoFacing = true;
    /// Only the walls placed by this stroke count as neighbours: existing walls neither shape
    /// the new ones nor are re-faced (the old editor's Smart Draw).
    bool smartDraw = false;
    /// Straight pieces' variants, picked in [variantMin, variantMax] (clamped to the facing's
    /// count); others take 0. With autoVariation off, every piece takes variantMin.
    bool autoVariation = true;
    int variantMin = 0;
    int variantMax = 0;
    /// The minimap group, or nothing for the polygon's (minimapGroupAt).
    std::optional<std::uint8_t> minimapGroup;
    /// A window: sight passes it (the game's traces skip window walls); facings 0 and 1 draw the
    /// window piece (11 / 12), others look plain (stock maps use both).
    bool window = false;
    bool destructible = false;
    std::optional<map::SecretWall> secret; ///< pos is set per cell
    /// Placing on an existing wall replaces it; else such cells are left alone.
    bool replace = true;
    /// The seed variant picks are hashed with (the editor's stroke counter: never a clock).
    std::uint32_t seed = 0;
};

/// The default secret-wall record (the stock maps' commonest: shut, 3 s wait, no flags) at `c`.
inline map::SecretWall defaultSecretWall(Cell c)
{
    return map::defaultSecretWall({c.x, c.y});
}
/// A secret wall set to start open (state 3, fully open, seen by everyone: as stock maps store
/// those).
using map::setSecretStartOpen;

/// The op placing `brush` on `cells` (each once; off-grid and odd cells skipped), with the
/// facing and variants per the brush, and (auto-facing, not Smart Draw) every existing
/// neighbour re-faced to join the new walls. `arms`, if given, fixes the arms of the cells it
/// names (an outline's): they join only those ways. Empty when nothing changes.
Op placeWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::vector<Cell>& cells, const WallBrush& brush,
                const std::map<Cell, std::uint8_t>* arms = nullptr);
/// The op removing the walls at `cells`, each neighbour re-faced to the arms it still joins
/// (`refaceNeighbours`; a neighbour left joining nothing keeps its facing).
Op removeWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::vector<Cell>& cells, bool refaceNeighbours = true);
/// The op setting the walls' states as given (the properties panel's edits), facings as they
/// are: variants checked against the facing.
Op setWallsOp(const MapDocument& doc, const things::ThingDb& things, const std::map<Cell, WallState>& states, std::string label);

/// A variant pick for a straight piece at `c`: in [lo, hi], from the cell and seed.
int pickWallVariant(Cell c, std::uint32_t seed, int lo, int hi);

} // namespace nox::mapedit
