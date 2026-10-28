// The map generator's writer (map_writer.hpp): sections, records and the script as the game's
// files hold them.
#include "map_writer.hpp"

#include "cells.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/wall_pieces.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace nox::mapgen::detail {

namespace {

/// The index of the first entry named `name` in a thing.bin list (floors, wall sets, edges).
template <class List>
int indexByName(const List& list, std::string_view name, std::string_view what)
{
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name == name) {
            return static_cast<int>(i);
        }
    }
    throw FormatError("mapgen: no " + std::string(what) + " named " + std::string(name));
}

/// The variant shipped maps give a tile of a cols x rows floor block: along a painted run,
/// Estate's 3x3 floors follow (2Y - X) mod 9 exactly (the offset is the painter's origin).
std::int16_t floorVariant(const things::FloorDef& def, int x, int y)
{
    const int block = def.cols * def.rows;
    if (block <= 1) {
        return 0;
    }
    return static_cast<std::int16_t>(((2 * y - x) % block + block) % block);
}

/// One object record with the common header only (DefaultXfer types, and the base every
/// typed xfer starts with). The xfer version 0x3C is DefaultXfer's newest readable (a typed
/// object's caller raises it to its own xfer's); the header has its own version, 0x3D here so
/// its optional fields stay absent.
map::ObjectXfer bareObject(std::uint32_t extent, float x, float y)
{
    map::ObjectXfer o;
    o.xferVersion = 0x3C;
    o.header.version = 0x3D;
    o.header.extent = extent;
    o.header.scriptId = 0;
    o.header.x = x;
    o.header.y = y;
    o.header.detailsFlag = 0; // no details: the object as its type makes it, enabled
    return o;
}

/// A type's object flags for a record's details (which replace those under 0x11408162): its
/// FLAGS line and ENABLED (bit 24: every type is made enabled). Not the type reader's NO_COLLIDE
/// for a type without a COLLIDE function, which server::typeFlags adds: generated levels have
/// always been written without it, and adding it now would change every seed's level (a record's
/// details flags), so this variant keeps its own name.
std::uint32_t typeFlagsWithoutNoCollide(const things::ThingDef& t)
{
    return things::thingFlags(t) | 0x1000000u;
}

/// A teleporter's or a light's record holds its block (TransporterXfer's partner, InvisibleLightXfer's
/// light): one written without it is short, bytes past its end no reader takes. Refused at the call
/// that would write it without (w:object, w:thing with no transporter or light).
void refuseWithoutBlock(const std::string& type, std::optional<std::string_view> xfer)
{
    if (xfer && *xfer == "TransporterXfer") {
        throw FormatError("mapgen: " + type + " needs its spec's transporter (its partner: { target = id }, 0 for none)");
    }
    if (xfer && *xfer == "InvisibleLightXfer") {
        throw FormatError("mapgen: " + type + " needs its spec's light ({ intensity, color, ... })");
    }
}

/// The map's square, 256 cells of 23 a side. Nothing is written outside it: a client links each
/// thing it draws into its grid by its place, unclamped, and a wall's or a floor tile's cell is a
/// byte that would wrap to another cell.
constexpr int kMapCells = 256;
constexpr float kMapEdge = kMapCells * 23.0f;

/// Refuses a place off the map (NaN too): `what` names the call's thing in the error.
void onMap(std::string_view what, float x, float y)
{
    if (!(x >= 0 && x <= kMapEdge && y >= 0 && y <= kMapEdge)) {
        throw FormatError("mapgen: " + std::string(what) + " at " + std::to_string(x) + ", " + std::to_string(y) +
                          " is off the map (0 to 5888)");
    }
}

/// Refuses a cell off the map's grid.
void onMap(std::string_view what, Cell c)
{
    if (c.first < 0 || c.first >= kMapCells || c.second < 0 || c.second >= kMapCells) {
        throw FormatError("mapgen: " + std::string(what) + " at cell " + std::to_string(c.first) + ", " +
                          std::to_string(c.second) + " is off the map (0 to 255)");
    }
}

} // namespace

int MapWriter::floorIndex(std::string_view name) const
{
    return indexByName(things_.floors, name, "floor");
}

int MapWriter::wallIndex(std::string_view name) const
{
    return indexByName(things_.walls, name, "wall set");
}

int MapWriter::edgeIndex(std::string_view name) const
{
    return indexByName(things_.edges, name, "edge");
}

std::string_view MapWriter::floorName(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= things_.floors.size()) {
        throw std::out_of_range("mapgen: no floor numbered " + std::to_string(index));
    }
    return things_.floors[static_cast<std::size_t>(index)].name;
}

std::size_t MapWriter::floorCount() const
{
    return things_.floors.size();
}

std::uint32_t kindsFromText(const std::string& text)
{
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
}

bool MapWriter::isThing(std::string_view type) const
{
    return things_.findThing(type) != nullptr;
}

bool MapWriter::isPlain(std::string_view type) const
{
    const things::ThingDef* def = things_.findThing(type);
    const auto xfer = def ? def->attribute("XFER") : std::nullopt;
    return def && (!xfer || *xfer == "DefaultXfer");
}

void MapWriter::walls(const CellMap& shapes, std::uint8_t material)
{
    map::WallMap walls;
    for (const Cell cell : shapes.keys()) {
        onMap("a wall", cell);
        map::Wall w;
        w.x = static_cast<std::uint8_t>(cell.first);
        w.y = static_cast<std::uint8_t>(cell.second);
        w.direction = static_cast<std::uint8_t>(shapes.get(cell));
        w.material = material;
        w.variant = 0;
        // The minimap draws a wall seen of the group the viewer is in, and outside every
        // polygon (these maps have none) that's 1: the editor's 100 (no polygon) is never drawn.
        w.minimapGroup = 1;
        walls.walls.push_back(w);
    }
    std::ranges::sort(walls.walls, [](const map::Wall& a, const map::Wall& b) { return std::pair(a.y, a.x) < std::pair(b.y, b.x); });
    // (A layout always has walls: the box is never missing.)
    walls.header.setBox(map::boxOf(walls.walls).value_or(map::GridBox{}));
    walls_ = std::move(walls);
}

/// The edges a cell gets where a floor laid over its own meets it (the editor's blending, as
/// shipped maps hold it: the floor laid over is blended onto the cells under it, never the other
/// way; ours by the layers, so a patch blends onto a room's floor and the path onto both, not
/// only onto the base). Per floor over it: two neighbouring sides it holds, their one piece
/// (SE+SW 16, SW+NW 17, NE+NW 18, NE+SE 19); a side left over, one of its three (NE 12-14, SE
/// 5/7/9, SW 1-3, NW 6/8/10); a corner it touches at its point alone (N 15, E 11, S 0, W 4).
std::vector<map::FloorEdge> MapWriter::blendEdges(const CellMap& floor, const CellMap& layer, Cell t, std::uint8_t mask,
                                                  std::minstd_rand& rng) const
{
    static constexpr std::array<std::array<std::uint8_t, 3>, 4> kSideDirs = {
        std::array<std::uint8_t, 3>{12, 13, 14}, {5, 7, 9}, {1, 2, 3}, {6, 8, 10}};
    static constexpr std::array<std::uint8_t, 4> kCornerDirs = {15, 11, 0, 4};
    struct Pair {
        int a, b;
        std::uint8_t dir;
    };
    static constexpr std::array<Pair, 4> kPairs = {Pair{1, 2, 16}, Pair{2, 3, 17}, Pair{0, 3, 18}, Pair{0, 1, 19}};
    std::vector<map::FloorEdge> edges;
    // (A neighbour's floor counts where it lies over this cell's: a higher layer, another floor.)
    const auto layerAt = [&](Cell c) { return std::max(0, layer.get(c)); };
    const int own = layerAt(t);
    const int ownFloor = floor.get(t);
    const auto floorAt = [&](Cell o) {
        const int f = floor.get(plus(t, o));
        return f < 0 || f == ownFloor || layerAt(plus(t, o)) <= own ? -1 : f;
    };
    // The floors over it, ascending, once each (a std::set's walk).
    std::array<int, 8> overlays{};
    std::size_t count = 0;
    for (const auto* around : {&kSides, &kCorners}) {
        for (const Cell& o : *around) {
            const int f = floorAt(o);
            const auto seen = overlays.begin() + static_cast<std::ptrdiff_t>(count);
            if (f >= 0 && std::find(overlays.begin(), seen, f) == seen) {
                overlays[count++] = f;
            }
        }
    }
    std::sort(overlays.begin(), overlays.begin() + static_cast<std::ptrdiff_t>(count));
    for (std::size_t n = 0; n < count; ++n) {
        const int f = overlays[n];
        const things::FloorDef& def = things_.floors[static_cast<std::size_t>(f)];
        std::array<bool, 4> side{};
        for (std::size_t i = 0; i < 4; ++i) {
            side[i] = floorAt(kSides[i]) == f;
        }
        std::array<bool, 4> done{};
        const auto edge = [&](std::uint8_t dir) {
            edges.push_back({.floor = static_cast<std::uint8_t>(f),
                             .variant = floorVariant(def, t.first, t.second),
                             .edge = mask,
                             .direction = dir});
        };
        for (const Pair& pr : kPairs) {
            if (side[static_cast<std::size_t>(pr.a)] && side[static_cast<std::size_t>(pr.b)]) {
                edge(pr.dir);
                done[static_cast<std::size_t>(pr.a)] = done[static_cast<std::size_t>(pr.b)] = true;
            }
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (side[i] && !done[i]) {
                edge(kSideDirs[i][rng() % 3]);
            }
        }
        // Corner i lies between sides i - 1 and i (N: NW and NE, E: NE and SE...).
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t before = (i + 3) % 4;
            if (floorAt(kCorners[i]) == f && !side[before] && !side[i]) {
                edge(kCornerDirs[i]);
            }
        }
    }
    return edges;
}

void MapWriter::floors(const CellMap& floor, const CellMap& layer, std::optional<std::uint8_t> edgeMask, std::minstd_rand& rng)
{
    // Each cell's tile in the cells' order (the edges drawn in it), into its pair's slot; the pairs
    // in their slots' order (a std::map of them's walk), a slot's two tiles as they came.
    struct Slotted {
        Cell slot;
        std::size_t order;
        bool odd;
        map::FloorTile tile;
    };
    for (const Cell t : floor.keys()) { // (every floor one thing.bin has, on the map, before anything's drawn)
        onMap("a floor tile", t);
        if (static_cast<std::size_t>(floor.get(t)) >= things_.floors.size()) {
            throw std::out_of_range("mapgen: no floor numbered " + std::to_string(floor.get(t)));
        }
    }
    std::vector<Slotted> tiles;
    tiles.reserve(floor.size());
    for (const Cell t : floor.keys()) {
        const auto [x, y] = t;
        const int f = floor.get(t);
        map::FloorTile tile;
        tile.image = static_cast<std::uint8_t>(f);
        tile.variant = floorVariant(things_.floors[static_cast<std::size_t>(f)], x, y);
        if (edgeMask) {
            tile.edges = blendEdges(floor, layer, t, *edgeMask, rng);
        }
        const map::FloorPairSlot slot = map::floorPairOf(x, y);
        tiles.push_back({{slot.x, slot.y}, tiles.size(), slot.odd, std::move(tile)});
    }
    std::ranges::sort(tiles, [](const Slotted& a, const Slotted& b) { return a.slot != b.slot ? a.slot < b.slot : a.order < b.order; });
    map::FloorMap out;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (i == 0 || tiles[i].slot != tiles[i - 1].slot) {
            map::FloorTilePair pair;
            pair.x = static_cast<std::uint8_t>(tiles[i].slot.first);
            pair.y = static_cast<std::uint8_t>(tiles[i].slot.second);
            out.tiles.push_back(std::move(pair));
        }
        map::FloorTilePair& pair = out.tiles.back();
        (tiles[i].odd ? pair.odd : pair.even) = std::move(tiles[i].tile);
    }
    out.header.setBox(map::boxOf(out.tiles).value_or(map::GridBox{})); // (never missing: a layout has floor)
    floor_ = std::move(out);
}

void MapWriter::add(const std::string& type, const map::ObjectXfer& x)
{
    if (!things_.findThing(type)) {
        throw FormatError("mapgen: no thing named " + type);
    }
    auto [it, fresh] = tocIds_.try_emplace(type, static_cast<std::uint16_t>(tocIds_.size() + 1));
    if (fresh) {
        toc_.entries.push_back({it->second, type});
    }
    map::ObjectRecord rec;
    rec.tocId = it->second;
    rec.data = x.serialize();
    rec.size = static_cast<std::uint32_t>(rec.data.size());
    objects_.records.push_back(std::move(rec));
}

map::xfer::TypeInfo MapWriter::typeInfo(std::string_view type) const
{
    const things::ThingDef* def = things_.findThing(type);
    if (!def) {
        throw FormatError("mapgen: no thing named " + std::string(type));
    }
    return {things::thingClasses(*def), things::thingSubclasses(*def)};
}

void MapWriter::object(const std::string& type, float x, float y, std::optional<std::uint32_t> kinds, const std::string& onDeath)
{
    const things::ThingDef* def = things_.findThing(type);
    if (!def) {
        throw FormatError("mapgen: no thing named " + type);
    }
    const auto xfer = def->attribute("XFER");
    refuseWithoutBlock(type, xfer);
    onMap(type, x, y);
    const map::xfer::TypeInfo kind = typeInfo(type);
    map::ObjectXfer o = bareObject(extent_++, x, y);
    if (xfer && *xfer == "MonsterXfer") {
        o.xferVersion = 0x40; // MonsterXfer's newest readable
        // What the editor writes for a creature placed as it comes (every one in G_Forest and
        // G_Crypts): sight 150, aggression 0.5, retreat 0 / resume 1, idle (a shopkeeper
        // guarding, blind), its status, levels and spells its type's (the four 1s),
        // spell power 1, the default spell delays, no polygon (0xDEADFACE). Zeros leave it
        // blind and still.
        const bool keeper = (kind.subclasses & things::ObjectSubclass::Shopkeeper) != 0;
        map::xfer::Monster m;
        m.direction = {-1, 0};
        m.retreatLevel = std::bit_cast<std::uint32_t>(0.0f);
        m.resumeLevel = std::bit_cast<std::uint32_t>(1.0f);
        m.sightRange = std::bit_cast<std::uint32_t>(keeper ? 0.0f : 150.0f);
        m.aggression = std::bit_cast<std::uint32_t>(0.5f);
        m.unknown338 = std::bit_cast<std::uint32_t>(1.0f);
        m.unknown329 = std::bit_cast<std::uint32_t>(30.0f);
        m.unknown330 = std::bit_cast<std::uint32_t>(0.5f);
        m.unknown510 = 1;
        m.unknown333 = 0xFF;
        m.action = keeper ? "ACTION_GUARD" : "ACTION_IDLE";
        m.unknown0 = 0xDEADFACEu;
        m.unknown337 = {1, 1, 1, 1};
        m.defaultSpells = {"SPELL_INVALID", "SPELL_INVALID", "SPELL_INVALID"};
        m.spellDelays = {0, 15, 90, 300, 90, 180, 0, 90, 0, 180};
        const auto health = def->lastAttribute("HEALTH");
        m.health = static_cast<std::uint16_t>(health ? std::atoi(std::string(*health).c_str()) : 0);
        m.subclassBits = kind.subclasses & 0x180u;
        m.scripts.death.function = onDeath; // (an ambush's count: features.luau)
        if (keeper) {
            // (Its prices' multipliers as MonsterCreate gives a shopkeeper, 1.0 and 0x3EAAAA3B:
            // zeros made everything cost 1.)
            m.shop = map::xfer::Shop{};
            m.shop->unknown1 = std::bit_cast<std::uint32_t>(1.0f);
            m.shop->unknown2 = 0x3EAAAA3Bu;
        }
        o.extra = map::xfer::encode(m, o.xferVersion, kind, o.extraOffset());
    } else if (xfer && *xfer == "RewardMarkerXfer") {
        o.xferVersion = 0x3F; // RewardMarkerXfer's newest readable
        map::xfer::RewardMarker m;
        m.rewardType = kinds.value_or(0x7Fu);
        m.rareOrSpecial = kinds ? 1 : 0; // (flags 1: always placed, chooseMarkers)
        o.extra = map::xfer::encode(m, o.xferVersion, kind, o.extraOffset());
    } else if (xfer && *xfer == "ReadableXfer") {
        o.extra = map::xfer::encode(map::xfer::Readable{}, o.xferVersion, kind, o.extraOffset());
    }
    add(type, o);
}

void MapWriter::bare(const std::string& type, float x, float y)
{
    onMap(type, x, y);
    add(type, bareObject(extent_++, x, y));
}

void MapWriter::sign(const std::string& type, float x, float y)
{
    onMap(type, x, y);
    map::ObjectXfer o = bareObject(extent_++, x, y);
    o.extra = map::xfer::encode(map::xfer::Readable{}, o.xferVersion, typeInfo(type), o.extraOffset());
    add(type, o);
}

void MapWriter::exit(const std::string& type, float x, float y)
{
    onMap(type, x, y);
    onMap(type + "'s way out", x + 190, y + 190);
    map::ObjectXfer o = bareObject(extent_++, x, y);
    map::xfer::Exit e;
    e.x = x + 190;
    e.y = y + 190;
    o.extra = map::xfer::encode(e, o.xferVersion, typeInfo(type), o.extraOffset());
    add(type, o);
}

std::uint32_t MapWriter::thing(const std::string& type, float x, float y, const ThingSpec& s)
{
    const std::uint32_t mine = extent_;
    const bool details = !s.name.empty() || !s.enabled || s.status != 0;
    const bool typed = s.trigger || s.glyph || s.hole || s.door || s.mover || s.transporter || s.light;
    if (!details && !typed) {
        // ("Type@kinds" read here too, as object()'s callers do: an ambush's monsters are the
        // level's own list, the kinds' text read before the type is looked up.)
        const std::size_t at = type.find('@');
        const std::optional<std::uint32_t> kinds = at == std::string::npos ? std::nullopt : std::optional(kindsFromText(type.substr(at + 1)));
        object(type.substr(0, at), x, y, kinds, s.onDeath);
        return mine;
    }
    const things::ThingDef* def = things_.findThing(type);
    if (!def) {
        throw FormatError("mapgen: no thing named " + type);
    }
    // (A teleporter's or a light's block read by its type's own xfer: on another type's record
    // it would be bytes no reader takes.)
    const auto requireXfer = [&](std::string_view want) {
        const auto xfer = def->attribute("XFER");
        if (!xfer || *xfer != want) {
            throw FormatError("mapgen: " + type + " isn't a " + std::string(want) + " thing");
        }
    };
    const bool older = s.trigger || s.glyph || s.hole || s.door || s.mover; // (taken first, below)
    const bool block = s.transporter || s.light;
    if (block) {
        // One block a record: a teleporter's or a light's with another would be silently dropped.
        // (Specs naming two of the older xfers are written as they always were: the first taken.)
        const int given = int(s.trigger.has_value()) + int(s.glyph.has_value()) + int(s.hole.has_value()) + int(s.door.has_value()) +
                          int(s.mover.has_value()) + int(s.transporter.has_value()) + int(s.light.has_value());
        if (given > 1) {
            throw FormatError("mapgen: " + type + ": a thing takes one of trigger, glyph, hole, door, mover, transporter, light");
        }
        requireXfer(s.transporter ? "TransporterXfer" : "InvisibleLightXfer");
    } else if (!older) {
        refuseWithoutBlock(type, def->attribute("XFER")); // (details alone: no block)
    }
    onMap(type, x, y);
    if (s.glyph) {
        onMap(type + "'s target", s.glyph->targetX, s.glyph->targetY);
    }
    if (s.hole) {
        onMap(type + "'s fall", static_cast<float>(s.hole->fallX), static_cast<float>(s.hole->fallY));
    }
    // A teleporter's or a light's header as the shipped maps hold theirs (the game's own saver,
    // mapReadWriteObjData): version 0x40, its details flag 0xFF, an empty pickup handler and no
    // decay. (The writer's older records keep their 0x3D headers: their bytes don't change.)
    const bool stockHeader = block;
    map::ObjectXfer o = bareObject(extent_++, x, y);
    if (stockHeader) {
        o.header.version = 0x40;
    }
    if (details) {
        map::ObjectHeader::Details d;
        d.flags = typeFlagsWithoutNoCollide(*def) & 0x11408162u; // Only the flags a record's details carry.
        if (!s.enabled) {
            d.flags &= ~0x1000000u; // Clear ENABLED: placed switched off.
        }
        d.scriptName = s.name;
        d.statusFlags = s.status;
        if (stockHeader) {
            d.onPickup = map::ScriptHandler{};
            d.decayFrames = 0;
        }
        o.header.detailsFlag = stockHeader ? 0xFF : 1;
        o.header.details = std::move(d);
    }
    const auto encodeExtra = [&](const auto& xfer) {
        o.extra = map::xfer::encode(xfer, o.xferVersion, typeInfo(type), o.extraOffset());
    };
    if (s.trigger) {
        o.xferVersion = 0x3D; // TriggerXfer's newest readable (its state and frame)
        encodeExtra(*s.trigger);
    } else if (s.glyph) {
        encodeExtra(*s.glyph);
    } else if (s.hole) {
        encodeExtra(*s.hole);
    } else if (s.door) {
        encodeExtra(*s.door);
    } else if (s.mover) {
        encodeExtra(*s.mover);
    } else if (s.transporter) {
        encodeExtra(*s.transporter); // (TransporterXfer: 0x3C, the newest it reads)
        teleporters_[mine] = s.transporter->targetExtent;
    } else if (s.light) {
        encodeExtra(*s.light); // (InvisibleLightXfer: 0x3C, its whole block)
    }
    add(type, o);
    return mine;
}

void MapWriter::objectGroup(const std::string& name, const std::vector<std::uint32_t>& extents)
{
    map::Group g;
    g.name = name;
    g.kind = map::Group::Kind::Objects;
    g.id = static_cast<std::int32_t>(groups_.groups.size());
    g.members.assign(extents.begin(), extents.end());
    groups_.groups.push_back(std::move(g));
}

void MapWriter::wallGroup(const std::string& name, const std::vector<Cell>& cells)
{
    map::Group g;
    g.name = name;
    g.kind = map::Group::Kind::Walls;
    g.id = static_cast<std::int32_t>(groups_.groups.size());
    for (const Cell& c : cells) {
        onMap("a wall group's wall", c);
        g.walls.push_back({c.first, c.second});
    }
    groups_.groups.push_back(std::move(g));
}

void MapWriter::secretWall(Cell c, bool open)
{
    // (as the shipped maps' scripted ones: worked by the script alone, 3 s their wait; shut,
    // or standing open as an ambush's: flag 8, open all the way)
    onMap("a secret wall", c);
    map::SecretWall s = map::defaultSecretWall({c.first, c.second});
    if (open) {
        map::setSecretStartOpen(s, true);
        s.flags = 8;
    }
    secretWalls_.walls.push_back(s);
}

std::uint32_t MapWriter::waypoint(const std::string& name, float x, float y)
{
    // (ids 1.. in the file's order: what the roaming graph's lookups assume)
    onMap("waypoint " + name, x, y);
    map::Waypoint w;
    w.id = static_cast<std::uint32_t>(waypoints_.waypoints.size() + 1);
    w.x = x;
    w.y = y;
    w.name = name;
    w.flags = 1; // (on)
    waypoints_.waypoints.push_back(std::move(w));
    return waypoints_.waypoints.back().id;
}

void MapWriter::link(std::uint32_t a, std::uint32_t b)
{
    // (flag 2, as the shipped movers' paths: off the creatures' roaming graph, 0x80)
    for (const std::uint32_t id : {a, b}) {
        if (id < 1 || id > waypoints_.waypoints.size()) {
            throw std::out_of_range("mapgen: no waypoint " + std::to_string(id));
        }
    }
    waypoints_.waypoints[a - 1].links.push_back({b, 2});
    waypoints_.waypoints[b - 1].links.push_back({a, 2});
}

map::MapFile MapWriter::finish(const MapHeading& heading)
{
    // A teleporter sends to its partner and sets the partner's own state (PentagramUpdate): a
    // partner that isn't one of the teleporters written is the generator's mistake.
    for (const auto& [extent, partner] : teleporters_) {
        if (partner != 0 && !teleporters_.contains(partner)) {
            throw FormatError("mapgen: the teleporter " + std::to_string(extent) + " leads to " + std::to_string(partner) +
                              ", which isn't a teleporter written here");
        }
    }
    // The map header and MapInfo (game type, players).
    map::MapFile out;
    out.magic = map::kMagic;
    map::MapInfo info;
    info.version = 2;
    info.title.set(heading.title);
    info.author.set(heading.author);
    info.date.set("generated");
    info.gameFlags = heading.gameFlags;
    info.minPlayers = heading.minPlayers;
    info.maxPlayers = heading.maxPlayers;
    // The map header's wall offsets (mapCountWallsMB: the smallest x and y, the wall box's).
    out.wallOffsetX = static_cast<std::uint32_t>(walls_.header.minX);
    out.wallOffsetY = static_cast<std::uint32_t>(walls_.header.minY);

    // The sections, in the order shipped maps carry them.
    const auto raw = [&out](std::string_view name, std::vector<std::uint8_t> bytes) {
        out.sections.push_back({std::string(name), std::move(bytes)});
    };
    raw(map::MapInfo::kName, info.serialize());
    raw(map::WallMap::kName, walls_.serialize());
    raw(map::FloorMap::kName, floor_.serialize());
    raw(map::SecretWalls::kName, secretWalls_.serialize());
    raw(map::DestructableWalls::kName, map::DestructableWalls{}.serialize());
    raw(map::Waypoints::kName, waypoints_.serialize());
    raw(map::DebugData::kName, map::DebugData{}.serialize());
    raw(map::WindowWalls::kName, map::WindowWalls{}.serialize());
    raw(map::GroupData::kName, groups_.serialize());
    map::ScriptObject scriptObject;
    if (!script_.empty()) {
        scriptObject.code = script_.build().serialize();
    }
    raw(map::ScriptObject::kName, scriptObject.serialize());
    map::AmbientData ambient;
    ambient.r = ambient.g = ambient.b = ambient_;
    raw(map::AmbientData::kName, ambient.serialize());
    raw(map::Polygons::kName, map::Polygons{}.serialize());
    raw(map::MapIntro::kName, map::MapIntro{}.serialize());
    raw(map::ScriptData::kName, map::ScriptData{}.serialize());
    raw(map::ObjectTOC::kName, toc_.serialize());
    raw(map::ObjectData::kName, objects_.serialize());
    return out;
}

} // namespace nox::mapgen::detail
