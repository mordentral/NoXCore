// The map editor's document: a map's decoded sections, edited in place and written back losslessly.
//
// The document is the typed codec data (noxcore/map), never the live World: opening a map and
// saving it unchanged gives the same bytes, and a change touches only what it changes.
//
// What the writer derives from content on every save:
//   - the header's wall offsets (the least wall x/y)
//   - the WallMap and FloorMap bounding boxes and record order (rows y, then x)
//   - Polygons point ids (1..n in list order, corners renumbered to match)
//   - ObjectData record order (objects by order key, each inventory after its owner) and every
//     owner's inventory count
//   - each object's record size and xfer bytes (re-encoded from its typed fields)
//   - the header CRC (the game's rule), unless the bytes equal the file as opened (then the
//     stored CRC is kept, so an unchanged map is byte-identical)
//
// What it keeps as stored until the user edits it (artefacts of Westwood's editor the game
// ignores, kept so files don't churn):
//   - MapInfo's fixed text buffers, junk after the NUL included (FixedString keeps it; setting
//     a field zero-fills its buffer)
//   - ObjectData alignment bytes, while a record's alignment is unchanged
//   - the stored ObjectTOC (ids, listing order, unused entries), while every type the objects
//     use is in it; otherwise a fresh table (ids by first use, listed in thing.bin order)
//   - a stored record order that isn't the canonical one (walls, floor pairs): kept for the
//     records still there, new records after them in canonical order
//   - section order (new sections go where the game's section table puts them)
//
// And what custom maps store that the shipped maps never do (old community editors), each read the
// way the game reads it and kept as stored:
//   - a floor pair stored in two or more records, or a record with no tile in it: the game
//     reads each record in turn, a tile replacing the pair's earlier one (edges and all) and a
//     record without that tile leaving it, so each tile comes from the last record that has it.
//     The document holds that result; the stored records are written back while the pair is
//     still what the game read from them, and an edited pair is written as one record.
//   - bounding boxes (WallMap, FloorMap) and header wall offsets that aren't the ones the
//     content gives (an old editor's defaults; the game's map load reads them and ignores
//     them): kept while the content gives what it gave when the map was opened, else derived.
//
// Editor-only data never goes into the .map (the game fails on unknown sections); it lives in
// the sidecar (sidecar.hpp).
#pragma once

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

/// The game's 16 map sections, in its section table's order (the order a new map is written in).
extern const std::array<std::string_view, 16> kSectionOrder;

/// A wall-grid cell. Ordered by row, then column (the order the game writes walls in).
struct Cell {
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    auto operator<=>(const Cell& o) const { return std::tie(y, x) <=> std::tie(o.y, o.x); }
    bool operator==(const Cell&) const = default;
};

/// A record order as stored, when it isn't the canonical one: the stored keys, first to last.
/// Empty means canonical. The writer emits the stored keys still present, then the rest in
/// canonical order.
template <class Key>
struct StoredOrder {
    std::vector<Key> keys;
};

/// One floor pair cell (FloorMap pair x, y): the full-grid tiles (2x+1, 2y-1) and (2x, 2y).
struct FloorPair {
    std::optional<map::FloorTile> odd;
    std::optional<map::FloorTile> even;
    bool empty() const { return !odd && !even; }
    bool operator==(const FloorPair&) const = default;
};

/// A floor pair's records as a custom map stored them when they aren't one record with a tile
/// (two or more records for the pair, or a record with no tile), and the pair the game reads
/// from them (each tile from the last record that has it).
struct StoredFloorRecords {
    std::vector<map::FloorTilePair> records; // in file order
    FloorPair read;
};

/// A field the writer derives from content (a bounding box, the wall offsets) that the file
/// stored with another value: kept while the content still gives `derivedAtLoad`.
template <class T>
struct KeptDerived {
    T stored;
    T derivedAtLoad;
};

/// The bounding box part of a grid header (min x, min y, width, height).
using map::GridBox;

/// The 128x128 pair grid, dense.
class FloorGrid {
public:
    static constexpr int kSize = 128;

    FloorGrid();
    FloorPair& pair(int x, int y) { return pairs_[static_cast<std::size_t>(y) * kSize + x]; }
    const FloorPair& pair(int x, int y) const { return pairs_[static_cast<std::size_t>(y) * kSize + x]; }

    /// A full-grid tile (gx + gy even; gx 0..255, gy -1..254), or nullptr where none can be or is.
    const map::FloorTile* tile(int gx, int gy) const;
    /// Sets or clears a full-grid tile. Returns false for a position no tile can have.
    bool setTile(int gx, int gy, std::optional<map::FloorTile> tile);

    std::size_t tileCount() const;

private:
    std::vector<FloorPair> pairs_;
};

/// Document-unique object id, stable across edits (never the extent, which the file owns).
using ObjId = std::uint32_t;

/// One object record: an object in the world, or an item in an owner's inventory.
struct DocObject {
    std::string type; // thing.bin name
    std::uint16_t xferVersion = 0x3C;
    /// The common header. `details->inventoryCount` is derived on save from the children.
    map::ObjectHeader header;
    map::xfer::Data data;
    ObjId parent = 0;        // 0: in the world; else the owner (an inventory item)
    std::uint64_t order = 0; // record order among the objects with the same parent
    /// Kept stored bytes: the record's alignment bytes (written while the alignment matches)
    /// and the size block's unused upper half.
    std::vector<std::uint8_t> padding;
    std::uint32_t sizeHi = 0;
    /// The ObjectTOC id the record had (0 for a new object): used while the stored table is
    /// kept, so a table listing one name twice still writes each record's own id.
    std::uint16_t tocId = 0;
    bool operator==(const DocObject&) const = default;
};

/// Where the document came from (for the "never save over a stock map" rule).
struct Origin {
    std::string name; // map name, empty for a new map
    bool stock = false;
};

struct MapDocument {
    std::uint32_t magic = map::kMagic;
    std::uint32_t wallOffsetX = 0; // used only when there are no walls (else derived)
    std::uint32_t wallOffsetY = 0;
    /// Sections in file order. A section listed is written even when emptied.
    std::vector<std::string> sections;

    map::MapInfo info;

    std::map<Cell, map::Wall> walls; // Wall::x/y mirror the key
    map::GridHeader wallHeader{.version = 7}; // version; box used only when there are no walls
    StoredOrder<Cell> wallOrder;

    FloorGrid floor; // as the game reads the FloorMap
    map::GridHeader floorHeader{.version = 4};
    /// Pair keys (x << 8 | y), one per stored record (a pair stored twice is listed twice).
    StoredOrder<std::uint16_t> floorOrder;
    /// The pairs a custom map stored other than as one record with a tile, by pair key.
    std::map<std::uint16_t, StoredFloorRecords> floorStored;

    /// Derived fields stored with other values (kept while the content still gives what it gave).
    std::optional<KeptDerived<std::pair<std::uint32_t, std::uint32_t>>> keptWallOffsets;
    std::optional<KeptDerived<GridBox>> keptWallBox;
    std::optional<KeptDerived<GridBox>> keptFloorBox;

    map::SecretWalls secretWalls;
    map::DestructableWalls destructableWalls;
    map::WindowWalls windowWalls;
    map::Waypoints waypoints;
    map::DebugData debugData;
    map::GroupData groups;
    map::ScriptObject script;
    map::AmbientData ambient;
    map::Polygons polygons;
    map::MapIntro intro;
    map::ScriptData scriptData;
    /// The script's NoxScript source (editor-only: saved in the sidecar, never in the .map; the
    /// ScriptObject above is what it compiled to). Nothing: no source yet (a stock map's, until
    /// the script panel decompiles it).
    std::optional<std::string> scriptSource;

    /// Stable keys for the polygons and the groups, parallel to their lists (polygons.polygons,
    /// groups.groups): locks, selections and panels name a polygon or a group by its key, which
    /// stays the same when the list is reordered or another entry is added or removed. Editor
    /// state only (never written): a map opened gets 1..n in list order.
    std::vector<std::uint32_t> polygonKeys;
    std::vector<std::uint32_t> groupKeys;
    std::uint32_t nextPolygonKey = 1;
    std::uint32_t nextGroupKey = 1;
    /// The list position of the polygon / group with this key (nothing if none has it).
    std::optional<std::size_t> polygonIndex(std::uint32_t key) const;
    std::optional<std::size_t> groupIndex(std::uint32_t key) const;

    std::map<ObjId, DocObject> objects;
    ObjId nextObjId = 1;
    std::uint16_t objectDataVersion = 1;
    std::optional<map::ObjectTOC> storedToc;

    Origin origin;

    /// The CRC and content hash of the file as opened (the CRC is kept when nothing changed).
    std::uint32_t loadedCrc = 0;
    std::uint64_t loadedHash = 0;
    bool loaded = false;

    // --- Construction --------------------------------------------------------------------

    /// Decodes a map. Throws FormatError for anything the game wouldn't load (unknown sections,
    /// object types missing from thing.bin, records that don't decode exactly).
    static MapDocument fromMapFile(const map::MapFile& file, const things::ThingDb& things);
    /// Reads and decrypts a .map file.
    static MapDocument load(const std::filesystem::path& path, const things::ThingDb& things);
    /// A new, empty map: every section, MapInfo version 3, no walls, floor or objects.
    static MapDocument blank();

    // --- Small edit helpers (the op layer builds on these) -----------------------------------

    void setWall(const map::Wall& wall);
    /// Removes the wall and its secret, destructible and window entries.
    bool removeWall(Cell cell);
    /// Adds an object (in the world, or into `parent`'s inventory, last). A zero extent gets
    /// the next free one. Returns its id.
    ObjId addObject(DocObject obj);
    /// Removes an object and its inventory.
    bool removeObject(ObjId id);
    /// Objects with this parent, in record order.
    std::vector<ObjId> childrenOf(ObjId parent) const;
    /// The least extent no object uses (1 if none).
    std::uint32_t nextFreeExtent() const;

    /// Adds the section to `sections` where the game's table places it, if it isn't there.
    void ensureSection(std::string_view name);
};

/// The names of the 157 maps that ship with the game (ignoring case). Saving a document over one of
/// these names is refused (the editor saves a stock map's edits under a new name).
bool isStockMapName(std::string_view name);

} // namespace nox::mapedit
