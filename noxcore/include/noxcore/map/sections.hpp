// Typed map sections. Each struct decodes one RawSection and encodes back to identical bytes.
//
// Layouts are verified against the retail 1.2b readers, server_mapRW<Section> in GAME.EXE.
// Fields whose meaning isn't known yet are named unknownN and kept
// only so files round-trip.
//
// Strings are Nox's 8-bit text, kept as raw bytes.
#pragma once

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/map_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox {
class ByteWriter;
}

namespace nox::map {

/// A fixed-size, NUL-terminated char buffer kept byte for byte (including anything after the NUL).
template <std::size_t N>
struct FixedString {
    std::array<char, N> raw{};

    std::string_view view() const
    {
        return {raw.data(), static_cast<std::size_t>(std::find(raw.begin(), raw.end(), '\0') - raw.begin())};
    }
    /// Replaces the contents, zero-filling and truncating to N-1 characters.
    void set(std::string_view s)
    {
        raw.fill('\0');
        std::copy_n(s.begin(), std::min(s.size(), N - 1), raw.begin());
    }
    bool operator==(const FixedString&) const = default;
};

/// Wall-grid coordinates.
struct GridPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const GridPoint&) const = default;
};

// ---------------------------------------------------------------------------------------------
// MapInfo

/// Map title, credits and game-type flags.
struct MapInfo {
    static constexpr std::string_view kName = "MapInfo";

    std::uint16_t version = 2; // 0: nothing else follows; 2: +player counts; 3: +quest intro
    FixedString<64> title;
    FixedString<512> description;
    FixedString<16> mapVersion;
    FixedString<64> author;
    FixedString<64> email;
    FixedString<128> author2;
    FixedString<128> email2;
    FixedString<256> unknown;
    FixedString<128> copyright;
    FixedString<32> date;
    /// Game types the map supports (mapToGameFlags: 1 coop / solo, 2 quest, 4 arena,
    /// 8 CTF, 0x10 KotR, 0x20 elimination, 0x40 flagball, 0x80000000 chat).
    std::uint32_t gameFlags = 0;
    std::uint8_t minPlayers = 2;  // stored in v2 only
    std::uint8_t maxPlayers = 16; // stored in v2 only
    std::string questIntro;       // v3 only
    std::string questGraphic;     // v3 only

    static MapInfo parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

// ---------------------------------------------------------------------------------------------
// Walls and floor

/// The bounding box part of a grid header (min x, min y, width, height).
struct GridBox {
    std::int32_t minX = 0, minY = 0, width = 0, height = 0;
    bool operator==(const GridBox&) const = default;
};

/// Header shared by WallMap and FloorMap: the section version and the bounding box of the
/// stored cells (the game recomputes it on write and ignores it on a plain load).
struct GridHeader {
    std::uint16_t version = 0;
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool operator==(const GridHeader&) const = default;

    GridBox box() const { return {minX, minY, width, height}; }
    void setBox(const GridBox& b)
    {
        minX = b.minX;
        minY = b.minY;
        width = b.width;
        height = b.height;
    }
};

/// The least box holding every record's cell (records with an x and a y: walls, floor pairs), as
/// the game's writer derives a grid header's; nothing for no records.
template <class Records>
std::optional<GridBox> boxOf(const Records& records)
{
    if (records.empty()) {
        return std::nullopt;
    }
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    for (const auto& r : records) {
        minX = std::min<int>(minX, r.x);
        minY = std::min<int>(minY, r.y);
        maxX = std::max<int>(maxX, r.x);
        maxY = std::max<int>(maxY, r.y);
    }
    return GridBox{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

struct Wall {
    std::uint8_t x = 0; // 0xFF is the list terminator, so never a valid x
    std::uint8_t y = 0;
    std::uint8_t direction = 0; // low 7 bits: facing; bit 7 sets wall flag 0x80
    std::uint8_t material = 0;  // index into thing.bin WALL entries
    std::uint8_t variant = 0;
    std::uint8_t minimapGroup = 0; // from the enclosing polygon; 100 if none
    std::uint8_t unknown = 0;      // wall byte 0xC; written as 0 under game flag 0x200000
    bool operator==(const Wall&) const = default;
};

/// Wall cells (version 7; version 6 lacks minimapGroup).
struct WallMap {
    static constexpr std::string_view kName = "WallMap";

    GridHeader header{.version = 7};
    std::vector<Wall> walls;

    static WallMap parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// An edge blended over a tile: part of a neighbouring floor's texture, cut by an edge mask.
struct FloorEdge {
    std::uint8_t floor = 0; // index into thing.bin FLOR entries (the texture blended in)
    std::int16_t variant = 0;
    std::uint8_t edge = 0;      // index into thing.bin EDGE entries
    std::uint8_t direction = 0; // edge shape, 0..19 in shipped maps
    bool operator==(const FloorEdge&) const = default;
};

/// Record reader: tileReadOne.
struct FloorTile {
    std::uint8_t image = 0; // index into thing.bin FLOR entries
    std::int16_t variant = 0;
    std::uint8_t unknown1 = 0; // 0 in every shipped map
    std::uint8_t unknown2 = 0; // 0 in every shipped map
    std::vector<FloorEdge> edges; // at most 255
    bool operator==(const FloorTile&) const = default;
};

/// Floor tiles are stored in pairs. Pair (x, y) covers the full-grid tiles (2x+1, 2y-1) and
/// (2x, 2y); Nox's isometric floor only has tiles where the full-grid x + y is even.
///
/// Stored as a u16 (x << 8 | y), with 0x8000 flagging `odd` and 0x80 flagging `even`; the odd
/// tile is stored first. 0xFFFF ends the list.
struct FloorTilePair {
    std::uint8_t x = 0; // 0..127
    std::uint8_t y = 0; // 0..127
    std::optional<FloorTile> odd;  // full-grid tile (2x+1, 2y-1)
    std::optional<FloorTile> even; // full-grid tile (2x, 2y)
    bool operator==(const FloorTilePair&) const = default;
};

/// Where full-grid tile (gx, gy) (gx + gy even) is stored: pair (gx/2, gy/2)'s even slot for an
/// even gx, else pair ((gx-1)/2, (gy+1)/2)'s odd slot. No range check.
struct FloorPairSlot {
    int x = 0;
    int y = 0;
    bool odd = false;
};
constexpr FloorPairSlot floorPairOf(int gx, int gy)
{
    const bool odd = (gx & 1) != 0;
    return {odd ? (gx - 1) / 2 : gx / 2, odd ? (gy + 1) / 2 : gy / 2, odd};
}

/// Floor tiles (version 4; older versions use a different layout).
struct FloorMap {
    static constexpr std::string_view kName = "FloorMap";

    GridHeader header{.version = 4};
    std::vector<FloorTilePair> tiles; // as stored: a custom map can store a pair twice

    /// The floor as the game's FloorMap reader leaves it: one record per pair (in the order the
    /// pairs first appear), each tile from the last record that has it (a later tile replaces an
    /// earlier one, edges and all; a record without that tile leaves it), records with no tile
    /// dropped. The same as `tiles` for every shipped map.
    std::vector<FloorTilePair> readPairs() const;

    static FloorMap parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

struct SecretWall {
    GridPoint pos;
    std::uint32_t openWait = 0;
    std::uint8_t flags = 0;
    std::uint8_t state = 0;
    std::uint8_t openDelay = 0;
    std::uint32_t lastOpen = 0;
    std::uint32_t unknown = 0;
    bool operator==(const SecretWall&) const = default;
};

struct SecretWalls {
    static constexpr std::string_view kName = "SecretWalls";

    std::uint16_t version = 2;
    std::vector<SecretWall> walls;

    static SecretWalls parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Walls that can be broken.
struct DestructableWalls {
    static constexpr std::string_view kName = "DestructableWalls";

    std::uint16_t version = 1;
    std::vector<GridPoint> walls;

    static DestructableWalls parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Walls that can be seen through.
struct WindowWalls {
    static constexpr std::string_view kName = "WindowWalls";

    std::uint16_t version = 2;
    std::vector<GridPoint> walls;

    static WindowWalls parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

// ---------------------------------------------------------------------------------------------
// Waypoints, polygons, groups

struct WaypointLink {
    std::uint32_t target = 0; // waypoint id
    std::uint8_t flags = 0;
    bool operator==(const WaypointLink&) const = default;
};

struct Waypoint {
    std::uint32_t id = 0;
    float x = 0;
    float y = 0;
    std::string name;
    std::uint32_t flags = 0;
    std::vector<WaypointLink> links; // at most 255
    bool operator==(const Waypoint&) const = default;
};

struct Waypoints {
    static constexpr std::string_view kName = "WayPoints";

    std::uint16_t version = 4;
    std::vector<Waypoint> waypoints;

    static Waypoints parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// A script function to call on an event (reader xferReadScriptHandler; versions above 1 fail to load).
struct ScriptHandler {
    std::int16_t version = 1;
    std::string function; // under 1024 bytes; empty for none
    std::uint32_t unknown = 0;
    bool operator==(const ScriptHandler&) const = default;
};

struct PolygonPoint {
    std::uint32_t id = 0;
    float x = 0;
    float y = 0;
    bool operator==(const PolygonPoint&) const = default;
};

struct Polygon {
    std::string name;
    std::array<std::uint8_t, 3> ambientLight{}; // RGB
    std::uint8_t minimapGroup = 0;
    std::vector<std::uint32_t> points; // PolygonPoint ids
    ScriptHandler playerEnter;
    ScriptHandler monsterEnter;
    std::uint32_t flags = 0; // v4+; bit 0 is counted by the game on load
    bool operator==(const Polygon&) const = default;
};

/// Polygon points and polygons. Versions 3 and 4 only; earlier versions store extra per-point and colour
/// fields and are rejected.
struct Polygons {
    static constexpr std::string_view kName = "Polygons";

    std::uint16_t version = 4;
    std::vector<PolygonPoint> points;
    std::vector<Polygon> polygons;

    static Polygons parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Named group of objects, waypoints or walls, used by map scripts.
struct Group {
    /// Kind 3 exists in the reader and stores ids like kinds 0 and 1; its use is unknown.
    enum class Kind : std::uint8_t { Objects = 0, Waypoints = 1, Walls = 2, Unknown3 = 3 };

    std::string name; // stored with its NUL terminator counted in the length byte
    /// False when the file stored the name without its NUL (the community editor wrote groups
    /// that way). The game's reader accepts that (mapRWGroupData copies the length's bytes and
    /// never terminates them); the name is then the stored bytes. Kept so the bytes round-trip.
    bool terminated = true;
    Kind kind = Kind::Objects;
    std::int32_t id = 0;
    std::vector<std::int32_t> members; // every kind except Walls
    std::vector<GridPoint> walls;      // Walls
    bool operator==(const Group&) const = default;
};

/// Script groups (version 3; versions below 3 prefix names with the map name).
struct GroupData {
    static constexpr std::string_view kName = "GroupData";

    std::uint16_t version = 3;
    std::vector<Group> groups;

    static GroupData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

// ---------------------------------------------------------------------------------------------
// Scripts and misc

/// Compiled map script (NoxScript bytecode), undecoded.
struct ScriptObject {
    static constexpr std::string_view kName = "ScriptObject";

    std::uint16_t version = 1;
    std::vector<std::uint8_t> code;

    static ScriptObject parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Script runtime state (globals and pending timers). Only savegames carry it; decoding it
/// needs the global count from ScriptObject, so it's kept raw here.
struct ScriptData {
    static constexpr std::string_view kName = "ScriptData";

    std::uint16_t version = 1;
    std::uint8_t hasState = 0;
    std::vector<std::uint8_t> state; // present when hasState != 0

    static ScriptData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Debug key/value strings. The game reads keys into 80-byte and values into 256-byte buffers
/// without bounds checks, so never write longer strings.
struct DebugData {
    static constexpr std::string_view kName = "DebugData";

    std::uint16_t version = 1;
    std::vector<std::pair<std::string, std::string>> entries;

    static DebugData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Ambient light colour, components 0..255 stored as u32.
struct AmbientData {
    static constexpr std::string_view kName = "AmbientData";

    std::uint16_t version = 1;
    std::uint32_t r = 0;
    std::uint32_t g = 0;
    std::uint32_t b = 0;

    static AmbientData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

struct MapIntro {
    static constexpr std::string_view kName = "MapIntro";

    std::uint16_t version = 1;
    std::string text;
    /// The stored length when it is zero or negative (no text). The game's reader
    /// (mapRWMapIntro) reads text only for a length above 0, so the community editor's
    /// "protected" maps (length -1) load with no intro. Kept so the bytes round-trip; written
    /// only while `text` is empty.
    std::int32_t emptyLength = 0;

    static MapIntro parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Object type names used by ObjectData, keyed by a map-local id.
struct ObjectTOC {
    static constexpr std::string_view kName = "ObjectTOC";

    struct Entry {
        std::uint16_t id = 0;
        std::string name;
        bool operator==(const Entry&) const = default;
    };

    std::uint16_t version = 1;
    std::vector<Entry> entries;

    static ObjectTOC parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// One object record in ObjectData: its type and its xfer data, undecoded.
struct ObjectRecord {
    std::uint16_t tocId = 0; // ObjectTOC entry id
    /// Alignment bytes before the size. The game doesn't clear its block buffer, so some maps
    /// carry leftover bytes here; the game ignores them. Kept so files round-trip.
    std::vector<std::uint8_t> padding;
    std::uint32_t size = 0;   // as stored; equals data.size()
    std::uint32_t sizeHi = 0; // upper half of the size block, never read by the game
    std::vector<std::uint8_t> data;
    bool operator==(const ObjectRecord&) const = default;
};

/// Map objects. Saved by the game's xfer_saveObj.
///
/// Layout: u16 version; repeat { u16 tocId (0 ends the list); pad to 8 from the file start
/// (not always zero); u32 size; u32 unused; u8 data[size] }. `data` is the type's xfer (DefaultXfer etc.). An
/// object's inventory items follow it as ordinary records; the owner's data holds their count.
///
/// The section's data starts 8-aligned in the file, so padding here is counted from the start
/// of the section data.
struct ObjectData {
    static constexpr std::string_view kName = "ObjectData";

    std::uint16_t version = 1;
    std::vector<ObjectRecord> records;

    static ObjectData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
    void write(ByteWriter& w) const;
};

/// Decodes the first section named Section::kName. Throws FormatError if it's missing.
template <class Section>
Section decodeSection(const MapFile& map)
{
    const RawSection* raw = map.find(Section::kName);
    if (!raw) {
        throw FormatError("map has no " + std::string(Section::kName) + " section");
    }
    return Section::parse(raw->data);
}

} // namespace nox::map
