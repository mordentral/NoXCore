// The map generator's writing half: what a generated
// level's design chose, written as the game's files need it. Nothing here chooses anything: it
// writes walls, floors, objects, groups, secret walls, waypoints and the map's script as it's told,
// each the way the shipped maps and the editor hold them, and checks the names it's given against
// the thing database (a FormatError for one that isn't there).
//
//   MapWriter w(things);
//   w.walls(shapes, material);                 // the wall section (and the header's offsets)
//   w.floors(floor, layer, edgeMask, rng);     // the floor section, blended, drawing from rng
//   w.object("Bat", x, y);                     // records, their extents in call order
//   ...
//   map::MapFile file = w.finish(info);        // every section in the shipped maps' order
#pragma once

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/mapgen/cell_set.hpp"
#include "noxcore/mapgen/layout.hpp"
#include "noxcore/mapgen/script_builder.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapgen::detail {

/// A thing's record beyond its type and place: a script name, placed off, a status, or one
/// typed xfer of its own (a feature's: features.luau).
struct ThingSpec {
    std::string name; ///< its script name (Object("name")); empty: none
    bool enabled = true;
    std::optional<map::xfer::Trigger> trigger;
    std::optional<map::xfer::Glyph> glyph;
    std::optional<map::xfer::Hole> hole;
    std::optional<map::xfer::Door> door;
    std::optional<map::xfer::Mover> mover;
    /// A teleport pentagram's (a TransporterXfer type): its partner, another written
    /// teleporter's extent (checked by finish), or 0 for none.
    std::optional<map::xfer::Transporter> transporter;
    /// A light's whole block (an InvisibleLightXfer type: ColorLight and the like): a client
    /// draws a map light from these, never from the server.
    std::optional<map::xfer::InvisibleLight> light;
    std::string onDeath;      ///< a monster's death handler
    std::uint32_t status = 0; ///< its xStatus (a crumbling floor's 2, as shipped)
};

/// The MapInfo fields a level sets.
struct MapHeading {
    std::string title;
    std::string author;
    std::uint32_t gameFlags = 0;
    std::uint8_t minPlayers = 1;
    std::uint8_t maxPlayers = 6;
};

/// A reward marker's kinds from a "Type@kinds" spec's text after the '@', read as the C++
/// generator always read it: std::stoul in base 16 (leading space, a sign, a 0x; the digits up to
/// the first that isn't one), made 32 bits. std::invalid_argument for no digits, std::out_of_range
/// past unsigned long, as it threw.
std::uint32_t kindsFromText(const std::string& text);

class MapWriter {
public:
    explicit MapWriter(const things::ThingDb& things) : things_(things) {}

    // ---- The thing database -------------------------------------------------------------
    /// The index of a thing.bin floor, wall set or edge by name (a FormatError for none).
    int floorIndex(std::string_view name) const;
    int wallIndex(std::string_view name) const;
    int edgeIndex(std::string_view name) const;
    /// A floor's name by its index (std::out_of_range for none).
    std::string_view floorName(int index) const;
    std::size_t floorCount() const;
    bool isThing(std::string_view type) const;
    /// A DefaultXfer thing (no state of its own: scenery).
    bool isPlain(std::string_view type) const;

    // ---- Walls and floors ---------------------------------------------------------------
    /// The wall section from each wall cell's shape, of one material.
    void walls(const CellMap& shapes, std::uint8_t material);
    /// The floor section: each cell's floor (a thing.bin FLOR index: std::out_of_range for none),
    /// its block variant, and, with an edge mask, its edges blended where a higher layer's floor
    /// meets it (drawn from `rng`, cell by cell in the map's order).
    void floors(const CellMap& floor, const CellMap& layer, std::optional<std::uint8_t> edgeMask, std::minstd_rand& rng);

    // ---- Objects (each one's extent the next, in call order) ----------------------------
    /// The next record's extent.
    std::uint32_t nextExtent() const { return extent_; }
    /// A thing as its type makes it, its xfer's part as its type wants: a monster as the editor
    /// places one (`onDeath` its death handler), a reward marker (of `kinds`, always placed, or
    /// of every kind but the Ankh's), a sign with no text; else the bare header.
    void object(const std::string& type, float x, float y, std::optional<std::uint32_t> kinds = std::nullopt,
                const std::string& onDeath = {});
    /// The bare header only (a player start, scenery).
    void bare(const std::string& type, float x, float y);
    /// A readable with no text (a sign), whatever its xfer.
    void sign(const std::string& type, float x, float y);
    /// An exit (a quest exit, a gate), its way out 190 on.
    void exit(const std::string& type, float x, float y);
    /// A thing with details or a typed xfer (spec), else as object() (a "Type@kinds" type split
    /// there, its kinds read by kindsFromText first); its extent. A transporter or a light needs
    /// a type of that xfer (a FormatError for another).
    std::uint32_t thing(const std::string& type, float x, float y, const ThingSpec& spec);

    // ---- Groups, secret walls, waypoints, the script ------------------------------------
    void objectGroup(const std::string& name, const std::vector<std::uint32_t>& extents);
    void wallGroup(const std::string& name, const std::vector<Cell>& cells);
    /// A scripted secret wall (worked by the script alone); `open`: standing open (an ambush's).
    void secretWall(Cell c, bool open);
    /// A waypoint, on (ids from 1 in the file's order); its id.
    std::uint32_t waypoint(const std::string& name, float x, float y);
    /// Two waypoints linked both ways (flag 2: off the creatures' roaming graph); std::out_of_range
    /// for an id with no waypoint.
    void link(std::uint32_t a, std::uint32_t b);
    std::size_t waypointCount() const { return waypoints_.waypoints.size(); }
    ScriptBuilder& script() { return script_; }
    void ambient(std::uint32_t light) { ambient_ = light; }

    /// The map: its header (the wall offsets) and every section in the shipped maps' order. A
    /// teleporter whose partner isn't a teleporter written here is a FormatError.
    map::MapFile finish(const MapHeading& heading);

private:
    void add(const std::string& type, const map::ObjectXfer& x);
    map::xfer::TypeInfo typeInfo(std::string_view type) const;
    std::vector<map::FloorEdge> blendEdges(const CellMap& floor, const CellMap& layer, Cell t, std::uint8_t mask, std::minstd_rand& rng) const;

    const things::ThingDb& things_;
    map::WallMap walls_;
    map::FloorMap floor_;
    map::ObjectTOC toc_;
    map::ObjectData objects_;
    std::map<std::string, std::uint16_t> tocIds_;
    std::uint32_t extent_ = 1;
    map::GroupData groups_;
    map::SecretWalls secretWalls_;
    map::Waypoints waypoints_;
    ScriptBuilder script_;
    std::uint32_t ambient_ = 0;
    /// The teleporters written, by extent, and their partners (0: none).
    std::map<std::uint32_t, std::uint32_t> teleporters_;
};

} // namespace nox::mapgen::detail
