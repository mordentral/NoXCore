// The map generator's native building blocks: the parts of
// a level's making that walk thousands of cells, done in C++ for the design in script to call per
// decision. Each takes its parameters and the stream it draws from, and draws in the order the
// C++ generator always did. None of them knows a mode: a mod could build its own levels from them.
//
// Cells are wall-grid cells; floor and walls sit on the x + y even checkerboard. A cell's sides
// are its diagonal neighbours (x +- 1, y +- 1), its corners the cells two away along an axis.
#pragma once

#include "noxcore/mapgen/cell_set.hpp"
#include "noxcore/mapgen/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace nox::mapgen::blocks {

/// The map's usable wall grid: room left round the edge for the outermost walls.
inline constexpr int kLow = 10;
inline constexpr int kHigh = 244;

/// `c` inside the usable grid with `margin` cells to spare on every side.
inline bool inBounds(Cell c, int margin)
{
    return kLow + margin <= c.first && c.first <= kHigh - margin && kLow + margin <= c.second && c.second <= kHigh - margin;
}

/// A room of `shape` within `radius` of `at` (its bounding radius may come out a little less):
/// a hall's (cross's, ring's, pillared hall's) sizes, or a cavern's seed, drawn from `rng`.
Chamber makeRoom(Cell at, int radius, RoomShape shape, std::mt19937& rng);

/// A room's floor into `open`, and the cells inside it closed again after (a ring's block, a
/// pillared hall's pillars) each a hole into `holes`. A cavern's blobs are drawn again from its
/// seed.
void openRoom(CellSet& open, std::vector<std::vector<Cell>>& holes, const Chamber& room);

/// A disc the nearness checks pass over: a centre less than `within` from (x, y), of tag `tag`
/// (-1: any).
struct Except {
    Cell at;
    int within = 0;
    int tag = -1;
};

/// The open floor being plotted, as blobs (a corridor cell with its half width, a chamber with its
/// radius; each with a tag) in the order they were laid, kept in buckets by their centres for the
/// nearness checks.
class Plot {
public:
    Plot();
    /// Its index (from 0).
    std::size_t add(const Chamber& blob, int tag = 0);
    std::size_t size() const { return blobs_.size(); }
    const Chamber& at(std::size_t i) const { return blobs_[i]; }

    /// The first blob of from..to - 1 whose disc comes within `gap` of the disc (c, r):
    /// |c - its centre| < r + its radius + gap, passing over the excepted.
    std::optional<std::size_t> firstNear(Cell c, int r, int gap, std::size_t from, std::size_t to, const Except* e1 = nullptr,
                                         const Except* e2 = nullptr) const;
    /// No blob of the first `before` near (c, r).
    bool clear(Cell c, int r, int gap, std::size_t before) const { return before == 0 || !firstNear(c, r, gap, 0, before); }

    /// A straight corridor `length` steps `way` from `from`, `width` wide, every cell `margin`
    /// inside the grid and clear (`gap`) of the first `before` blobs: its cells, laid as blobs of
    /// `tag`; else nothing laid.
    std::optional<std::vector<Cell>> run(Cell from, Cell way, int length, int margin, int width, int gap, std::size_t before, int tag);

    /// A side corridor the same way, ending in `room` (centred on its last cell): its cells past
    /// the first `skip` and the room clear of every blob but `except`; its cells, the corridor and
    /// the room laid (tag 0); else nothing laid. `room` comes back with its centre.
    std::optional<std::vector<Cell>> spur(Cell from, Cell way, int length, int margin, int width, int gap, Chamber& room, std::size_t skip,
                                          const Except& except);

    /// An L from `a` to `b` (all its u steps then its v steps, or the other way round; u along
    /// (1, 1), v along (1, -1)), `width` wide: every cell `margin` inside the grid, clear (`gap`)
    /// of every blob but those within `endsWithin` of either end, and its cells past the first
    /// `middle` and before the last `middle` clear (`gap`) of `route`'s points by `routeReach`. Its
    /// cells, laid; else nothing laid.
    std::optional<std::vector<Cell>> lRun(Cell a, Cell b, bool uFirst, int width, int margin, int gap, int endsWithin, const Plot& route,
                                          int routeReach, int middle);

    /// Points of this plot (a route's: radius 0) from `start + minAlong` to `size - 1 - tail`
    /// (from 0) whose L from point `start` is `minLen` to `maxLen` long with each run `minEach` or
    /// more: their indices.
    std::vector<std::size_t> lEnds(std::size_t start, std::size_t minAlong, std::size_t tail, int minLen, int maxLen, int minEach) const;

    /// The floor: every blob's (a room by its shape), then the rooms' holes closed, each whole or not
    /// at all: not where a way runs through or past (`clearOf` cells round `ways`) nor within
    /// `startRadius` of `start`; then a lone cell left out with floor on all four sides (where blobs'
    /// edges meet just so) floored too, again till there are none.
    CellSet carve(const std::vector<Cell>& ways, int clearOf, Cell start, int startRadius, bool fillPinholes = true) const;

private:
    static constexpr int kBucket = 8;
    static constexpr int kOrigin = -128;
    static constexpr int kSide = 64;
    struct Bucket {
        std::vector<std::uint32_t> idx; ///< (ascending)
        int maxR = -1;
    };
    Bucket& bucketFor(Cell c);

    std::vector<Chamber> blobs_;
    std::vector<int> tags_;
    std::vector<Bucket> buckets_;
    Bucket outside_;
    int maxR_ = 0;

public:
    /// What it holds natively (what a script is charged for one): its buckets, and about this
    /// much a blob (the blob, its tag and its bucket's index, with the vectors' room to grow).
    static constexpr std::size_t kBucketBytes = static_cast<std::size_t>(kSide) * kSide * sizeof(Bucket);
    static constexpr std::size_t kBlobBytes = 2 * (sizeof(Chamber) + sizeof(int) + sizeof(std::uint32_t));
};

/// Every open cell reachable from `from` by side steps, less `without`.
CellSet reachable(const CellSet& open, Cell from, const std::vector<Cell>& without);

// ---- The scans that place things -------------------------------------------------------------

/// The cells of `open` (less `gone`) in a straight line across a corridor going `d` (a side
/// step) at `at`: `at`, then along the other diagonal one way and the other till the floor ends,
/// `most` each way at the most. None when `at` isn't open.
std::vector<Cell> lineAcross(const CellSet& open, Cell at, Cell d, int most, const CellSet* gone = nullptr);

/// Open and not used.
inline bool unused(const CellSet& open, const CellSet& used, Cell c)
{
    return open.contains(c) && !used.contains(c);
}
/// The same, and its four sides too.
bool unusedAround(const CellSet& open, const CellSet& used, Cell c);

/// The cells of `path`, in its order, `from` to `to` from `near` (on both axes), unused all round
/// and `minFromStart` or more from `start`.
std::vector<Cell> spotsNear(const std::vector<Cell>& path, Cell near, int from, int to, const CellSet& open, const CellSet& used, Cell start,
                            int minFromStart);

/// The first cell (x + y even) round `c`, in squares of growing reach `from`..`to` (each square
/// scanned row by row, the inner ones again), unused (all round, with `around`).
std::optional<Cell> firstAround(Cell c, int from, int to, const CellSet& open, const CellSet& used, bool around);

/// Back walls: each straight wall cell the view looks at and the open cell before it.
using Backs = std::vector<std::pair<Cell, Cell>>;

/// The back walls of `shape` whose fronts are unused and within `beside` (on both axes) of a cell
/// of `path` from `from` to before `to` (indices from 0), each `apart` or more from those before:
/// at most `most`, their indices.
std::vector<std::size_t> wallMounts(const Backs& backs, const CellMap& walls, int shape, const CellSet& open, const CellSet& used,
                                    const std::vector<Cell>& path, std::size_t from, std::size_t to, int beside, int apart, std::size_t most);

/// The back walls whose fronts lie within `radius` (on both axes) of `c`: their indices, in order.
std::vector<std::size_t> backsNear(const Backs& backs, Cell c, int radius);

/// A straight back wall of `shape` (0 "/", 1 "\"): three cells of it in a line, nearest `near` (on
/// both axes; the first of the nearest) and five or more from each of `avoid`.
std::optional<Cell> straightBack(const Backs& backs, const CellMap& walls, int shape, Cell near, const std::vector<Cell>& avoid);

/// The back walls' fronts: for each straight wall cell the view looks at ("/" with the open floor
/// south-east of it, "\" with it south-west), the open cell in front of it, in the walls' order;
/// none on a cell of `exclude`.
Backs backWalls(const CellMap& walls, const CellSet& open, const CellSet& exclude);

// ---- Floors (a floor plan: a CellMap of each cell's floor and one of its layer) ----------------

/// Open, and its four sides and four corners open too: two in from any wall.
bool deep(const CellSet& open, Cell c);

/// `count` rough square blobs: each centred on one of `cells` (drawn), one of `floors` (drawn),
/// its reach 2 to 4 (drawn); each cell of the square deep in `open` and not past the reach less a
/// draw of 0 or 1 (the corners nibbled), and not in `avoid` for a floor `harms` says harms, painted
/// with its floor and `layerValue`.
void roughBlobs(CellMap& floor, CellMap& layer, std::minstd_rand& rng, const std::vector<Cell>& cells, std::size_t count,
                const std::vector<int>& floors, const std::vector<bool>& harms, const CellSet& open, const CellSet& avoid, int layerValue);

/// The open cells within `radius` (|dx| + |dy|) of `c` painted.
void paintWithin(CellMap& floor, CellMap& layer, Cell c, int radius, const CellSet& open, int f, int layerValue);

/// Each of `cells` and its four sides, where deep in `open`, painted.
void paintAround(CellMap& floor, CellMap& layer, const std::vector<Cell>& cells, const CellSet& open, int f, int layerValue);

/// `passes` passes, each over the plan's cells in order: a cell of the `base` floor with three or
/// four sides of one other floor (not one of `never`) taken into it, its layer raised to the
/// highest of those sides'.
void absorb(CellMap& floor, CellMap& layer, int base, const std::vector<int>& never, int passes);

// ---- Objects ---------------------------------------------------------------------------------

/// The walls round `open` (layout.hpp's wallsList) as a CellMap of their shapes.
CellMap wallsMap(const CellSet& open);

/// The open cells with open sides all round (one in from any wall), in order: where free things go.
std::vector<Cell> roomy(const CellSet& open);

/// Up to `tries` draws of one of `cells`: the first not used, `nearest` or more from `start` (and,
/// with `near`, no further than `farthest` from it), made used; none when every try fails.
std::optional<Cell> freeCell(std::minstd_rand& rng, const std::vector<Cell>& cells, CellSet& used, Cell start, int nearest,
                             std::optional<Cell> near, int farthest, int tries);

/// A thing scatter placed: its cell and its kind (from 0).
struct Placed {
    Cell cell;
    std::size_t kind = 0;
};

/// `count` things of `kinds` kinds, each on up to `tries` draws of one of `cells`: the first not in
/// `used`, `clear` nor `near`, `minFromStart` or more from `start` (and, with `deepIn`, deep in it),
/// its kind drawn, made used, and a square of each mark's reach round it added to that mark's set.
std::vector<Placed> scatter(std::minstd_rand& rng, const std::vector<Cell>& cells, int count, std::size_t kinds, int tries, CellSet& used,
                            const CellSet& clear, const CellSet* near, const CellSet* deepIn, Cell start, int minFromStart,
                            const std::vector<std::pair<CellSet*, int>>& marks);

} // namespace nox::mapgen::blocks
