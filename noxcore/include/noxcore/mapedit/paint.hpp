// The map editor's floor painting: the brush, tile variants, and the auto-edge rules that blend
// neighbouring floors.
//
// Edges. A tile's edges are drawn over it in order (the exe's tileCallDrawEdges): each takes
// image[direction] of its EDGE set, a 46 x 46 mask of which pixels keep the tile, take the
// edge's own colour or show the blended floor (FloorEdge::floor, variant). GAME.EXE indexes the
// set's images by the direction byte with no check (drawTexEdgesProbably), and an edge set of
// rows x cols holds 2 (rows + cols) images a frame laid out as: S point, rows - 2 SW sides,
// W point, SE and NW sides alternating (cols - 2 each), E point, rows - 2 NE sides, N point,
// then the inner corners SE+SW, SW+NW, NE+NW, NE+SE. The server reads them back to the same
// 12 shapes (World::edgeShape for floorAt). So a 3 x 3 set (MudEdge, DirtDarkedge,
// GrassSparseEdge: 12 images) takes directions 0-11 only, and the 6 x 6 DirtLightEdge 0-23;
// a direction past them draws past the set's images. The editor writes every edge through
// edgeDirections, so it never does.
//
// Auto-edge: a tile's edges are recomputed from its neighbours' floors alone, so running
// it again changes nothing (idempotent) and erasing is just a recompute. For each neighbouring
// floor that laps over the tile's (priority, or a pair override), in ascending priority: adjacent
// sides give inner corners, the rest single sides (a seeded one of their variants), and point
// corners whose two sides are clear give points. Walls stop laps across the borders their arms
// lie on (wall block).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

// --- Edge shapes and directions ------------------------------------------------------------------

/// The 12 edge shapes (the exe's numbering).
enum EdgeShape : int {
    kEdgeS = 0,    ///< point: touches the tile's S vertex only
    kEdgeSW = 1,   ///< side
    kEdgeW = 2,    ///< point
    kEdgeSE = 3,   ///< side
    kEdgeNW = 4,   ///< side
    kEdgeE = 5,    ///< point
    kEdgeNE = 6,   ///< side
    kEdgeN = 7,    ///< point
    kEdgeSESW = 8, ///< inner corner (two adjacent sides)
    kEdgeSWNW = 9,
    kEdgeNENW = 10,
    kEdgeNESE = 11,
};

/// The shape `direction` draws in an edge set of rows x cols; -1 past its images a frame.
int edgeShapeOf(int rows, int cols, int direction);
/// The directions that draw `shape` in a rows x cols set (a side has rows - 2 or cols - 2).
std::vector<int> edgeDirections(int rows, int cols, int shape);
/// Whether auto-edge may write this direction of edge set `edge`: in range, and not an image
/// that isn't its slot's shape. One such image ships: DenseGrassEdge's first SE side (direction
/// 5) is a whole-tile fill (the old editor skipped it too; no stock map uses that set).
bool edgeDirectionUsable(const things::ThingDb& things, int edge, int direction);
/// The direction auto-edge writes for `shape` in set `edge`: one of the usable variants, picked
/// by `pick`; -1 if the set has none.
int edgeDirectionFor(const things::ThingDb& things, int edge, int shape, std::uint32_t pick);

// --- Rules -----------------------------------------------------------------------------------

/// How one floor laps over another.
struct EdgeStyle {
    enum class Kind : std::uint8_t {
        Blend, ///< the lapping floor's texture through the mask (FloorEdge::floor = it)
        Trim,  ///< the edge's colour over the tile's own floor (FloorEdge::floor = the tile's)
        None,  ///< doesn't lap
    };
    Kind kind = Kind::Blend;
    int edge = -1; ///< thing.bin EDGE index
    bool operator==(const EdgeStyle&) const = default;
};

/// Floor priorities and edge styles: defaults from the stock maps (edge_defaults.inc: which floor
/// laps over which, the edge used most for each pair), plus the user's overrides.
class EdgeRules {
public:
    static EdgeRules defaults(const things::ThingDb& things);

    /// Floors in ascending priority (a later one laps over an earlier one). Floors not listed
    /// rank below all of them.
    const std::vector<int>& order() const { return order_; }
    void setOrder(std::vector<int> ascending);
    int rank(int floor) const;
    /// Whether `over` laps onto a tile of `base`, and how. A pair override decides; else the
    /// higher priority laps, styled by the stock table (that pair, else the other way round,
    /// else the floor's commonest edge, else BlendEdge).
    std::optional<EdgeStyle> lap(int over, int base) const;
    /// The style a pair would use if it lapped (the override's, else the defaults').
    EdgeStyle style(int over, int base) const;

    /// The user's pair overrides: (over, base) -> style (None: never laps).
    const std::map<std::pair<int, int>, EdgeStyle>& overrides() const { return user_; }
    void setOverride(int over, int base, std::optional<EdgeStyle> style);

    /// The overrides as text for the sidecar (by thing.bin names), and back. Unknown names are
    /// skipped.
    std::string toText(const things::ThingDb& things) const;
    void applyText(std::string_view text, const things::ThingDb& things);

private:
    std::vector<int> order_;
    std::vector<int> rank_;
    bool stockOrder_ = true;
    std::vector<int> stockList_;
    std::map<std::pair<int, int>, int> stock_; // (over, base) -> edge
    std::vector<int> floorEdge_;               // floor -> its commonest edge (-1)
    int blendEdge_ = -1;
    std::map<std::pair<int, int>, EdgeStyle> user_;
};

/// Auto-edge's per-stroke options.
struct AutoEdgeOptions {
    /// No lap across a border a wall's arm lies on (or a point a wall separates).
    bool wallBlock = true;
    /// This floor laps over every other, whatever the priorities say (the old brush's rule).
    std::optional<int> onTop;
    /// Tiles of this floor keep their edges (the old "tile to ignore").
    std::optional<int> excludeFloor;
    /// Only this floor's laps are recomputed; a tile's other edges stay (the old "ignore all but").
    std::optional<int> onlyFloor;
    /// Laps show this floor's texture instead of the lapping floor's (the old "preserve").
    std::optional<int> coverFloor;
    /// A trim with this edge where a tile meets a hole (none by default).
    std::optional<int> holeTrim;
    /// The side variants' seed (a document's, from its sidecar: never a clock).
    std::uint32_t seed = 0;
};

/// The variant rule's anchor per floor: variant = (u + a) mod cols + cols ((b - v) mod rows), u =
/// (gx + gy) / 2, v = (gx - gy) / 2; learned per floor as the anchor most of the document's tiles
/// of it follow, else (0, 0).
class VariantAnchors {
public:
    static VariantAnchors learn(const MapDocument& doc, const things::ThingDb& things);
    std::int16_t variant(const things::ThingDb& things, int floor, int gx, int gy) const;
    std::pair<int, int> anchor(int floor) const;

private:
    std::map<int, std::pair<int, int>> anchors_;
};

/// A tile's 8 neighbours as full-grid offsets: the sides NE, SE, SW, NW, then the point corners
/// N, E, S, W. A tile's vertices are the wall cells N (gx, gy), E (gx + 1, gy + 1), S (gx, gy + 2)
/// and W (gx - 1, gy + 1).
inline constexpr std::pair<int, int> kTileNeighbours[8] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}, {0, -2}, {2, 0}, {0, 2}, {-2, 0}};
/// Whether a wall stands between the tile and its neighbour `n` (an index into kTileNeighbours):
/// for a side, a wall arm along their shared border; for a point corner, a wall at the shared
/// vertex whose arms part the two.
bool wallBetween(const MapDocument& doc, TileCell t, int n);

/// The tile and its 8 neighbours (4 sides, 4 point corners), each once, for every cell.
std::set<TileCell> withNeighbours(const std::set<TileCell>& cells);

/// The edges auto-edge gives the tile at `t` (none for a hole), from the floors around it.
/// Kept: the tile's edges `options` says to keep (onlyFloor's others).
std::vector<map::FloorEdge> autoEdges(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules,
                                      const AutoEdgeOptions& options, const VariantAnchors& anchors, TileCell t);
/// Recomputes the edges of `cells` in the document (custom cells and excluded floors skipped).
/// Returns the tiles that changed.
std::vector<TileCell> recomputeEdges(MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules,
                                     const AutoEdgeOptions& options, const VariantAnchors& anchors, const std::set<TileCell>& cells,
                                     const std::set<TileCell>& custom = {});

// --- The brush -----------------------------------------------------------------------------------

struct FloorBrush {
    enum class Shape : std::uint8_t { Round, Square, Diamond };
    enum class Mode : std::uint8_t {
        Paint,     ///< set the floor
        Erase,     ///< remove tiles (holes)
        Replace,   ///< set the floor only on tiles of `replaceFloor`
        Fill,      ///< flood the region of the clicked tile's floor (or hole), bounded by walls
        Smooth,    ///< each tile takes its 3 x 3 window's majority floor (5 of 9 or more)
        EdgesOnly, ///< recompute edges, floors unchanged
    };
    enum class Variants : std::uint8_t { Aligned, Random, Fixed };

    Shape shape = Shape::Round;
    int size = 3;          ///< tiles across, 1-16
    float hardness = 1.0f; ///< 1 crisp; lower: a soft rim where `noise` raggs the edge
    float noise = 0.0f;    ///< 0-1: how ragged the rim is
    Mode mode = Mode::Paint;
    std::uint8_t floor = 0;
    Variants variants = Variants::Aligned;
    int fixedVariant = 0;
    std::uint8_t replaceFloor = 0;
    bool autoEdge = true;  ///< off: tiles placed or removed with no edges touched (the old Place/Remove)
    /// Fill stops at walls' arms; a hole fill covers at most this many tiles.
    std::size_t fillLimit = 20000;
    AutoEdgeOptions edges;
    /// The stroke's seed (noise, random variants): the editor's stroke counter, never a clock.
    std::uint32_t seed = 0;
};

/// The tiles a dab of the brush at a world point covers (its shape, size, hardness and noise).
std::vector<TileCell> brushFootprint(const FloorBrush& brush, WorldPoint at);

/// One brush stroke: dabs change the document at once (the live preview), and finish() turns the
/// whole stroke into ONE op (one SetTiles part with every changed tile's final value, its
/// neighbours' recomputed edges included) after putting the document back as it was, so the
/// history applies it.
class FloorStroke {
public:
    FloorStroke(MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, FloorBrush brush,
                std::set<TileCell> custom = {});
    /// Applies a dab; returns the tiles whose content changed (for the view).
    std::vector<TileCell> dab(WorldPoint at);
    /// The document back as it was; the stroke as one op (empty if nothing changed). Also gives
    /// the tiles the restore changed (the view's update), if wanted.
    Op finish(std::vector<TileCell>* restored = nullptr);
    /// The document back as it was, no op; returns the tiles changed.
    std::vector<TileCell> cancel();
    std::size_t touched() const { return original_.size(); }

private:
    void remember(TileCell t);
    std::optional<map::FloorTile> baseTile(int floor, TileCell t) const;

    MapDocument& doc_;
    const things::ThingDb& things_;
    const EdgeRules& rules_;
    FloorBrush brush_;
    std::set<TileCell> custom_;
    VariantAnchors anchors_;
    std::map<TileCell, std::optional<map::FloorTile>> original_;
    bool filled_ = false;
};

/// The op recomputing the edges of `cells` (the edge tool's auto mode, "Recompute edges").
Op autoEdgeOp(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, const AutoEdgeOptions& options,
              const std::set<TileCell>& cells, const std::set<TileCell>& custom = {});

/// The tiles a wall at each cell is a vertex of (the four round it: N, E, S and W of their
/// diamonds), whose edges a wall there can block (AutoEdgeOptions::wallBlock).
std::set<TileCell> tilesAtWalls(const std::vector<Cell>& cells);
/// `wallOp` (walls placed, removed or re-faced) with the floor edges round the walls it changes
/// recomputed (the tiles they are a vertex of: wall block), as one op: the walls' parts, then one
/// SetTiles part for the tiles whose edges change (none: the op as given).
Op withEdgesAtWalls(const MapDocument& doc, const things::ThingDb& things, const EdgeRules& rules, const AutoEdgeOptions& options,
                    Op wallOp, const std::set<TileCell>& custom = {});

/// The paint settings a document keeps in its sidecar: the rules' overrides, the cells with
/// hand-placed edges the brush leaves alone, and the side variants' seed.
struct PaintSettings {
    std::set<TileCell> customEdges;
    std::uint32_t edgeSeed = 0;
    std::string rulesText; ///< EdgeRules::toText
    std::string toText() const;
    static PaintSettings fromText(std::string_view text);
};

} // namespace nox::mapedit
