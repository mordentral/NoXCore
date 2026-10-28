// The map editor's selection, its geometry (where walls, tiles, objects and waypoints sit in the
// world, and which of them a click or a dragged region takes), and the ops a selection makes:
// delete, move, copy and paste (the clipboard, as text for the system clipboard).
//
// World units are the game's: a wall cell is 23 x 23, wall (x, y) covering [23x, 23x + 23) on
// each axis; a full-grid floor tile (gx + gy even) is a 46 x 46 diamond centred on
// (23 gx + 12, 23 gy + 35) (MapView's tile layout, the game's tileDrawImpl offsets).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

/// A full-grid floor tile position (gx + gy even).
struct TileCell {
    std::int16_t gx = 0;
    std::int16_t gy = 0;
    auto operator<=>(const TileCell& o) const { return std::tie(gy, gx) <=> std::tie(o.gy, o.gx); }
    bool operator==(const TileCell&) const = default;
};

struct WorldPoint {
    float x = 0;
    float y = 0;
};

constexpr float kWallCellSize = 23.0f;
/// The world's extent on each axis (256 wall cells).
constexpr float kWorldSize = 256 * kWallCellSize;

WorldPoint wallCentre(Cell c);
WorldPoint tileCentre(TileCell t);
/// The wall cell holding a world point (either parity), or nothing off the grid.
std::optional<Cell> wallCellAt(float x, float y);
/// The floor tile diamond holding a world point, or nothing off the grid.
std::optional<TileCell> tileAt(float x, float y);
/// Whether a tile can be at this position (gx + gy even, inside the grid).
bool validTile(int gx, int gy);

/// What a selection holds. Objects are world objects (inventory items go with their owner).
struct Selection {
    std::set<ObjId> objects;
    std::set<Cell> walls;
    std::set<TileCell> tiles;
    std::set<std::uint32_t> waypoints;
    std::set<std::uint32_t> polygons; ///< polygon keys (MapDocument::polygonKeys): the Polygon tool's

    bool empty() const { return objects.empty() && walls.empty() && tiles.empty() && waypoints.empty() && polygons.empty(); }
    std::size_t size() const { return objects.size() + walls.size() + tiles.size() + waypoints.size() + polygons.size(); }
    void clear() { *this = {}; }
    void add(const Selection& s);
    void remove(const Selection& s);
    /// Adds what isn't selected, removes what is.
    void toggle(const Selection& s);
    /// Drops what the document no longer has.
    void prune(const MapDocument& doc);
    bool operator==(const Selection&) const = default;
};

/// Which kinds a click or region takes.
struct KindFilter {
    bool objects = true;
    bool walls = true;
    bool tiles = true;
    bool waypoints = true;
};

/// A dragged selection region, in world units.
struct Region {
    enum class Shape {
        Box,   ///< screen-aligned rectangle between two corners
        Box45, ///< rectangle with sides along the floor's diagonals (the old editor's "45 degree" box)
        Lasso, ///< polygon through the points
    };
    Shape shape = Shape::Box;
    std::vector<WorldPoint> points; ///< Box / Box45: the two corners dragged; Lasso: the path

    bool contains(float x, float y) const;
    /// The region's outline for drawing (4 corners for the boxes, the path for a lasso).
    std::vector<WorldPoint> outline() const;
};

/// What a click at (x, y) takes: the nearest waypoint, else object, within `radius`, else the
/// wall under it, else the tile (each only if the filter allows it). Empty if nothing.
Selection pickAt(const MapDocument& doc, float x, float y, float radius, const KindFilter& filter);
/// Everything of the allowed kinds inside the region (objects and waypoints by position, walls
/// by cell centre, tiles by diamond centre).
Selection selectIn(const MapDocument& doc, const Region& region, const KindFilter& filter);
/// Everything of the allowed kinds.
Selection selectAll(const MapDocument& doc, const KindFilter& filter);
/// The world objects of one type.
Selection selectType(const MapDocument& doc, std::string_view type);

/// The world rectangle (min, max) the selection covers, or nothing for an empty one.
std::optional<std::pair<WorldPoint, WorldPoint>> bounds(const MapDocument& doc, const Selection& s);

/// Deletes the selection: objects with their inventories, walls with their secret / breakable /
/// window entries, tiles, waypoints (and other waypoints' links to them).
Op deleteOp(const MapDocument& doc, const Selection& s);

/// The whole-cell step a move of (dx, dy) world units takes walls and tiles by: rounded to wall
/// cells, with x + y even (a wall or tile keeps its grid parity), and clamped so nothing leaves
/// the grid.
std::pair<int, int> cellStep(const MapDocument& doc, const Selection& s, float dx, float dy);
/// Moves the selection: objects and waypoints by (dx, dy) (kept on the map), walls and tiles by
/// cellStep's whole cells (walls and tiles moved onto replace what was there). `moved` gets the
/// selection after the move.
Op moveOp(const MapDocument& doc, const Selection& s, float dx, float dy, Selection* moved = nullptr);

/// Copied map content: absolute source positions plus the cell it's placed by.
struct Clip {
    Cell anchor;
    std::vector<std::pair<Cell, WallState>> walls;
    std::vector<std::pair<TileCell, map::FloorTile>> tiles;
    std::vector<ObjectEntry> objects;     ///< owners before their items (source ids, roots' parent 0)
    std::vector<map::Waypoint> waypoints; ///< links only between copied waypoints

    bool empty() const { return walls.empty() && tiles.empty() && objects.empty() && waypoints.empty(); }
};

/// Which layers an area copy takes (the old editor fork's Map Copy checkboxes).
struct CopyLayers {
    bool walls = true;
    bool tiles = true;
    bool objects = true;
    bool waypoints = true;
};

Clip copySelection(const MapDocument& doc, const Selection& s, const CopyLayers& layers = {});

/// The part of a script name (an object's or a waypoint's) the game's lookups compare: what follows
/// its first ':' ("Map:Guard" answers to "Guard"; getObjectByScrName and the waypoint lookup compare
/// it exactly, case and all).
std::string_view scriptNameKey(std::string_view name);
/// `name` unchanged when its key isn't in `used` (keys); else its key's base (a trailing "_N"
/// dropped) with the next free "_N" (2, 3...), its "Map:" part kept: "Guard" -> "Guard_2" ->
/// "Guard_3".
std::string uniqueScriptName(std::string_view name, const std::set<std::string, std::less<>>& used);

/// Places the clip with its anchor on cell `at` (moved to x + y even). Objects get new ids and
/// extents (links between copied objects follow them: teleporters, elevators, a mover's carried
/// object and waypoints, a light's target; the load assigns script ids); waypoints get the lowest
/// free ids. Script names (objects' and waypoints') that the map already has get the next free
/// "_N" (uniqueScriptName) and a copied creature's escort naming a renamed copy follows it; names
/// that don't clash stay. What would land off the grid is left out. `pasted` gets what was placed,
/// as a selection.
Op pasteOp(const MapDocument& doc, const Clip& clip, Cell at, Selection* pasted = nullptr);
/// Places only the clip's objects and waypoints, moved by (dx, dy) world units exactly (no cell
/// rounding): new ids and extents, links between copied objects kept, names kept unique, as pasteOp.
Op pasteMarkersOp(const MapDocument& doc, const Clip& clip, float dx, float dy, Selection* pasted = nullptr);

/// The clip as text for the system clipboard ("NoX Core map clip" header, then base64).
std::string clipToText(const Clip& clip, const things::ThingDb& things);
/// Nothing for text that isn't a clip (or one that doesn't fit this thing.bin).
std::optional<Clip> clipFromText(std::string_view text, const things::ThingDb& things);

} // namespace nox::mapedit
