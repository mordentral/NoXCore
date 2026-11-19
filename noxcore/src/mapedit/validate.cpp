// Map document checks (validate.hpp).
#include "noxcore/mapedit/validate.hpp"

#include "doc_types.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/script/compiler.hpp"
#include "noxcore/script/script_file.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>

namespace nox::mapedit {

namespace {

using l10n::format;

/// A message's text in the language asked for.
std::string say(IssueText text, const l10n::Key& key)
{
    return text == IssueText::English ? std::string(key.en) : l10n::tr(key);
}

class Checker {
public:
    Checker(const MapDocument& d, const things::ThingDb& t, IssueText text) : d_(d), things_(t), text_(text) {}

    std::vector<Issue> run()
    {
        versions();
        walls();
        floor();
        markers();
        objects();
        script();
        writes();
        return std::move(out_);
    }

private:
    void error(std::string where, std::string message) { out_.push_back({Severity::Error, std::move(where), std::move(message)}); }
    void warn(std::string where, std::string message) { out_.push_back({Severity::Warning, std::move(where), std::move(message)}); }

    std::string say(const l10n::Key& key) const { return mapedit::say(text_, key); }

    bool listed(std::string_view name) const { return std::ranges::find(d_.sections, name) != d_.sections.end(); }
    /// A wall list's or group's point names a wall the document has.
    bool wallAt(const map::GridPoint& p) const
    {
        return p.x >= 0 && p.y >= 0 && p.x <= 0xFF && p.y <= 0xFF && d_.walls.contains(Cell{static_cast<std::uint8_t>(p.x), static_cast<std::uint8_t>(p.y)});
    }

    // The section readers' version limits (each rejects a version above its limit).
    void versions()
    {
        const auto atMost = [&](std::string_view name, unsigned v, unsigned limit) {
            if (v > limit) error(std::string(name), format(say(NOX_TRK("editor.validate.version_past", "version {v} (the game reads up to {limit})")), {{"v", v}, {"limit", limit}}));
        };
        atMost(map::MapInfo::kName, d_.info.version, 3);
        atMost(map::WallMap::kName, d_.wallHeader.version, 7);
        atMost(map::FloorMap::kName, d_.floorHeader.version, 4);
        atMost(map::Waypoints::kName, d_.waypoints.version, 4);
        atMost(map::Polygons::kName, d_.polygons.version, 4);
        atMost(map::SecretWalls::kName, d_.secretWalls.version, 2);
        atMost(map::WindowWalls::kName, d_.windowWalls.version, 2);
        atMost(map::DestructableWalls::kName, d_.destructableWalls.version, 1);
        atMost(map::ScriptData::kName, d_.scriptData.version, 1);
        atMost(map::ObjectData::kName, d_.objectDataVersion, 1);
        if (d_.storedToc) atMost(map::ObjectTOC::kName, d_.storedToc->version, 1);
        const auto atLeast1 = [&](std::string_view name, unsigned v) {
            if (listed(name) && v < 1) error(std::string(name), say(NOX_TRK("editor.validate.version_0", "version 0 (the game reads 1 and up)")));
        };
        atLeast1(map::ScriptObject::kName, d_.script.version);
        atLeast1(map::AmbientData::kName, d_.ambient.version);
        atLeast1(map::DebugData::kName, d_.debugData.version);
    }

    void walls()
    {
        for (const auto& [c, w] : d_.walls) {
            const std::string where = "wall " + std::to_string(c.x) + "," + std::to_string(c.y);
            if (c.x == 0xFF) error(where, say(NOX_TRK("editor.validate.wall_x_255", "x 255 ends the wall list")));
            // The game finds walls only on cells whose x + y is even (its grid wall lookup).
            if (((c.x + c.y) & 1) != 0) warn(where, say(NOX_TRK("editor.validate.wall_odd", "x + y is odd: the game's wall lookups never find it")));
            if (w.material >= things_.walls.size()) {
                error(where, format(say(NOX_TRK("editor.validate.wall_material", "material {n} is not in thing.bin")), {{"n", w.material}}));
                continue;
            }
            const unsigned dir = w.direction & 0x7F;
            const auto& def = things_.walls[w.material];
            if (dir >= def.directions.size() || def.directions[dir].empty()) {
                warn(where, format(say(NOX_TRK("editor.validate.wall_direction", "direction {n} has no image in {name}")), {{"n", dir}, {"name", def.name}}));
            }
        }
        const auto onWall = [&](std::string_view list, const map::GridPoint& p) {
            if (!wallAt(p)) {
                warn(std::string(list), format(say(NOX_TRK("editor.validate.list_no_wall", "entry at {x},{y} has no wall")), {{"x", p.x}, {"y", p.y}}));
            }
        };
        for (const auto& w : d_.secretWalls.walls) onWall(map::SecretWalls::kName, w.pos);
        for (const auto& p : d_.destructableWalls.walls) onWall(map::DestructableWalls::kName, p);
        for (const auto& p : d_.windowWalls.walls) onWall(map::WindowWalls::kName, p);
    }

    void floor()
    {
        for (int y = 0; y < FloorGrid::kSize; ++y) {
            for (int x = 0; x < FloorGrid::kSize; ++x) {
                const auto& p = d_.floor.pair(x, y);
                const auto check = [&](const std::optional<map::FloorTile>& t, int gx, int gy) {
                    if (!t) return;
                    const std::string where = "tile " + std::to_string(gx) + "," + std::to_string(gy);
                    if (t->image >= things_.floors.size()) error(where, format(say(NOX_TRK("editor.validate.tile_floor", "floor {n} is not in thing.bin")), {{"n", t->image}}));
                    if (t->edges.size() > 0xFF) error(where, say(NOX_TRK("editor.validate.tile_edges", "more than 255 edges")));
                    for (const auto& e : t->edges) {
                        if (e.floor >= things_.floors.size()) error(where, format(say(NOX_TRK("editor.validate.edge_floor", "edge floor {n} is not in thing.bin")), {{"n", e.floor}}));
                        if (e.edge >= things_.edges.size()) {
                            error(where, format(say(NOX_TRK("editor.validate.edge", "edge {n} is not in thing.bin")), {{"n", e.edge}}));
                            continue;
                        }
                        // The game draws image[direction] of the set with no check (a frame
                        // holds 2 (rows + cols) of them).
                        const auto& def = things_.edges[e.edge];
                        const std::size_t perFrame = 2u * (def.rows + def.cols);
                        if (e.direction >= def.images.size())
                            error(where, format(say(NOX_TRK("editor.validate.edge_direction_images", "edge direction {n} is past {name}'s {images} images (the game reads past them)")),
                                                {{"n", e.direction}, {"name", def.name}, {"images", def.images.size()}}));
                        else if (e.direction >= perFrame)
                            warn(where, format(say(NOX_TRK("editor.validate.edge_direction_shapes", "edge direction {n} is past {name}'s {shapes} shapes (draws another frame's image)")),
                                               {{"n", e.direction}, {"name", def.name}, {"shapes", perFrame}}));
                    }
                };
                check(p.odd, 2 * x + 1, 2 * y - 1);
                check(p.even, 2 * x, 2 * y);
            }
        }
        // Pairs a custom map stored in several records, still written so (unedited): the game
        // reads them in turn, each tile from the last record that has it.
        int agreeing = 0, empty = 0;
        for (const auto& [key, stored] : d_.floorStored) {
            const int x = key >> 8, y = key & 0xFF;
            if (d_.floor.pair(x, y) != stored.read) continue; // (edited: written as one record)
            int withTiles = 0;
            bool differ = false;
            for (const auto& r : stored.records) {
                if (!r.odd && !r.even) {
                    ++empty;
                    continue;
                }
                ++withTiles;
                differ |= (r.odd && r.odd != stored.read.odd) || (r.even && r.even != stored.read.even);
            }
            if (withTiles < 2) continue;
            if (differ) {
                warn("tile pair " + std::to_string(x) + "," + std::to_string(y),
                     say(NOX_TRK("editor.validate.tile_pair_differ",
                                 "two floor records for one tile pair, with different tiles; the game uses the later one (each tile from the last "
                                 "record that has it), which is what the editor shows; editing the pair stores it once")));
            } else {
                ++agreeing;
            }
        }
        if (agreeing > 0) {
            warn(std::string(map::FloorMap::kName),
                 format(say(NOX_TRK("editor.validate.tile_pairs_agree",
                                    "{n} tile pairs have two floor records for one tile pair (the same tiles: an old editor's output); "
                                    "the game uses the later one; kept as stored until the pair is edited")),
                        {{"n", agreeing}}));
        }
        if (empty > 0) {
            warn(std::string(map::FloorMap::kName),
                 format(say(NOX_TRK("editor.validate.empty_floor_records", "{n} floor records with no tile in them; the game skips them; kept as stored until the pair is edited")),
                        {{"n", empty}}));
        }
    }

    void markers()
    {
        std::set<std::uint32_t> ids;
        for (const auto& wp : d_.waypoints.waypoints) {
            if (!ids.insert(wp.id).second) error("WayPoints #" + std::to_string(wp.id), say(NOX_TRK("editor.validate.waypoint_same_id", "two waypoints share the id")));
        }
        for (const auto& wp : d_.waypoints.waypoints) {
            const std::string where = "WayPoints #" + std::to_string(wp.id);
            if (wp.links.size() > 0xFF) error(where, say(NOX_TRK("editor.validate.waypoint_links", "more than 255 links")));
            // Bit 1 is the game's own "registered in the grid" mark: stored, the waypoint is never
            // registered and the nearest-waypoint search never finds it (OB-209).
            if (wp.flags & 2u) warn(where, say(NOX_TRK("editor.validate.waypoint_bit1", "flag bit 1 is set: the game never files it in the map grid (searches miss it)")));
            for (const auto& l : wp.links) {
                // The game's link resolution fails the whole load on a link to a missing waypoint.
                if (!ids.contains(l.target)) error(where, format(say(NOX_TRK("editor.validate.waypoint_link_missing", "link to missing waypoint {id}")), {{"id", l.target}}));
            }
        }
        std::set<std::uint32_t> points;
        for (const auto& p : d_.polygons.points) points.insert(p.id);
        for (const auto& poly : d_.polygons.polygons) {
            for (const auto id : poly.points) {
                if (!points.contains(id)) warn("polygon " + poly.name, format(say(NOX_TRK("editor.validate.polygon_corner", "corner {id} is not a point")), {{"id", id}}));
            }
        }
        // The quest intro's title and picture go into 32-byte buffers unchecked (OB-208).
        if (d_.info.version >= 3) {
            if (d_.info.questIntro.size() > 31) error(std::string(map::MapInfo::kName), say(NOX_TRK("editor.validate.quest_title", "the quest intro's title is longer than 31 bytes")));
            if (d_.info.questGraphic.size() > 31) error(std::string(map::MapInfo::kName), say(NOX_TRK("editor.validate.quest_picture", "the quest intro's picture is longer than 31 bytes")));
        }
        for (const auto& [key, value] : d_.debugData.entries) {
            // The game reads them into 80- and 256-byte buffers without bounds checks.
            if (key.size() >= 80) error("DebugData " + key, say(NOX_TRK("editor.validate.debug_key", "key longer than 79 bytes")));
            if (value.size() >= 256) error("DebugData " + key, say(NOX_TRK("editor.validate.debug_value", "value longer than 255 bytes")));
        }
        std::set<std::uint32_t> extents;
        for (const auto& [id, o] : d_.objects) extents.insert(o.header.extent);
        std::map<std::int32_t, int> groupIds;
        for (const auto& g : d_.groups.groups) ++groupIds[g.id];
        for (const auto& g : d_.groups.groups) {
            const std::string where = "group " + g.name;
            // Scripts reach a group by its id (the name is turned into the id; the last read with
            // an id wins), so two groups with one id leave one of them unreachable.
            if (groupIds[g.id] > 1) warn(where, format(say(NOX_TRK("editor.validate.group_same_id", "its id {id} is another group's too: scripts reach only one of them")), {{"id", g.id}}));
            if (g.kind == map::Group::Kind::Unknown3) {
                // Kind 3 is a group of groups (members are group ids); an empty one crashes the
                // game's for-each walk (OB-158).
                if (g.members.empty()) warn(where, say(NOX_TRK("editor.validate.group_empty", "an empty group of groups: the game's for-each on it crashes")));
                for (const auto m : g.members) {
                    if (!groupIds.contains(m)) warn(where, format(say(NOX_TRK("editor.validate.group_member_group", "member group id {id} is no group")), {{"id", m}}));
                }
            }
            // The game reads the stored name (its NUL included) into a 76-byte stack buffer
            // with no bounds check (mapRWGroupData).
            if (g.name.size() + (g.terminated ? 1 : 0) > 76) error(where, say(NOX_TRK("editor.validate.group_name_long", "name longer than 75 bytes")));
            if (!g.terminated)
                warn(where, say(NOX_TRK("editor.validate.group_name_unterminated",
                                        "name stored without its NUL (an old editor's group): the game reads whatever follows it in its buffer; "
                                        "renaming the group stores it properly")));
            for (const auto m : g.members) {
                if (g.kind == map::Group::Kind::Objects && !extents.contains(static_cast<std::uint32_t>(m)))
                    warn(where, format(say(NOX_TRK("editor.validate.group_member_extent", "member extent {n} is no object")), {{"n", m}}));
                if (g.kind == map::Group::Kind::Waypoints && !ids.contains(static_cast<std::uint32_t>(m)))
                    warn(where, format(say(NOX_TRK("editor.validate.group_member_waypoint", "member {n} is no waypoint")), {{"n", m}}));
            }
            for (const auto& p : g.walls) {
                if (!wallAt(p)) warn(where, format(say(NOX_TRK("editor.validate.group_member_wall", "member wall {x},{y} has no wall")), {{"x", p.x}, {"y", p.y}}));
            }
        }
    }

    void objects()
    {
        std::map<std::uint32_t, int> extents;
        bool playerStart = false;
        for (const auto& [id, o] : d_.objects) {
            const std::string where = "object " + std::to_string(o.header.extent) + " (" + o.type + ")";
            ++extents[o.header.extent];
            playerStart |= o.type == "PlayerStart";
            std::optional<detail::TypeXfer> type;
            try {
                type = detail::typeXfer(things_, o.type);
            } catch (const FormatError& ex) {
                error(where, ex.what());
                continue;
            }
            if (o.header.version < 0x3D || o.header.version > 0x40) error(where, format(say(NOX_TRK("editor.validate.object_header_version", "header version {n}")), {{"n", o.header.version}}));
            if (o.header.details && o.header.details->scriptName.size() > 0xFF) error(where, say(NOX_TRK("editor.validate.object_script_name", "script name longer than 255 bytes")));
            if (o.parent != 0 && !d_.objects.contains(o.parent)) error(where, say(NOX_TRK("editor.validate.object_owner_gone", "inventory owner is gone")));
            // Fields the game reads into fixed buffers or tables without a check.
            if (const auto* x = std::get_if<map::xfer::Exit>(&o.data); x && x->mapName.substr(0, x->mapName.find('\0')).size() > 79)
                error(where, say(NOX_TRK("editor.validate.exit_map_name", "the map name is longer than the game's 79 bytes (it runs over the exit's point: OB-207)")));
            if (const auto* x = std::get_if<map::xfer::Glyph>(&o.data); x && x->spells.size() > 5)
                warn(where, say(NOX_TRK("editor.validate.glyph_spells", "more than 5 glyph spells: the game's glyph holds 5 and the rest run over its data (OB-024)")));
            if (const auto* x = std::get_if<map::xfer::MonsterGenerator>(&o.data)) {
                for (const auto r : x->unknown1) {
                    if (r > 4) warn(where, say(NOX_TRK("editor.validate.spawn_rate", "a spawn rate past 4: the game takes an address as the interval and never spawns (OB-206)")));
                }
                if (o.xferVersion > 0x3E && x->unknown6 == 0) warn(where, say(NOX_TRK("editor.validate.spawn_mode", "its spawn mode is 0: it never spawns")));
                for (const auto& group : x->groups) {
                    for (const auto& t : group) {
                        for (const auto& item : t.inventory) {
                            if (templateItemType(d_, item.tocId, things_).empty())
                                error(where, format(say(NOX_TRK("editor.validate.generator_item", "{type} (a monster it makes) carries an item whose type id {id} names no type")),
                                                    {{"type", t.type}, {"id", item.tocId}}));
                        }
                    }
                }
            }
            if (const auto* x = std::get_if<map::xfer::Trigger>(&o.data); x && (x->sizeX > 60 || x->sizeY > 60 || x->sizeX < 0 || x->sizeY < 0))
                warn(where, say(NOX_TRK("editor.validate.trigger_size", "a size past 60 (the server takes 60; the client draws it as stored) or below 0")));
            try {
                map::ObjectXfer x;
                x.xferVersion = o.xferVersion;
                x.header = o.header;
                const auto bytes = map::xfer::encode(o.data, o.xferVersion, type->info, x.extraOffset());
                // The server fails the whole load on a record that doesn't decode exactly.
                const auto back = map::xfer::decode(type->xfer, o.xferVersion, bytes, type->info, x.extraOffset());
                if (map::xfer::encode(back, o.xferVersion, type->info, x.extraOffset()) != bytes) {
                    error(where, say(NOX_TRK("editor.validate.type_data_differs", "type data doesn't read back the same")));
                }
            } catch (const FormatError& ex) {
                error(where, format(say(NOX_TRK("editor.validate.type_data_error", "type data: {error}")), {{"error", ex.what()}}));
            }
        }
        for (const auto& [extent, n] : extents) {
            if (n > 1) warn("extent " + std::to_string(extent), format(say(NOX_TRK("editor.validate.extent_shared", "{n} objects share it (links and groups name extents)")), {{"n", n}}));
        }
        const bool soloOrQuest = (d_.info.gameFlags & 3u) != 0;
        if (!playerStart && !soloOrQuest) warn(std::string(map::ObjectData::kName), say(NOX_TRK("editor.validate.no_player_start", "no PlayerStart: nobody can join")));
    }

    void script()
    {
        // (An empty script is fine: stock maps without scripts store one, and the game skips it.)
        if (!listed(map::ScriptObject::kName) || d_.script.code.empty()) return;
        try {
            const auto file = script::ScriptFile::parse(d_.script.code);
            if (script::usesObjectVariables(file)) {
                warn(std::string(map::ScriptObject::kName),
                     say(NOX_TRK("editor.validate.script_object_vars",
                                 "this map needs a NoX Core server: its script uses object variables, a NoX Core-only feature the original game "
                                 "crashes on (OB-210)")));
            }
        } catch (const std::exception& ex) {
            error(std::string(map::ScriptObject::kName), format(say(NOX_TRK("editor.validate.script_unparsed", "the game can't parse it: {error}")), {{"error", ex.what()}}));
        }
    }

    // Anything the codec itself refuses (strings past their length fields and so on).
    void writes()
    {
        bool fatal = false;
        for (const auto& i : out_) fatal |= i.severity == Severity::Error;
        if (fatal) return; // (the codec would only repeat an error already listed)
        try {
            (void)map::serializeMapFileWithCrc(toMapFile(d_, things_), things_);
        } catch (const std::exception& ex) {
            error("map", format(say(NOX_TRK("editor.validate.cant_write", "can't be written: {error}")), {{"error", ex.what()}}));
        }
    }

    const MapDocument& d_;
    const things::ThingDb& things_;
    IssueText text_;
    std::vector<Issue> out_;
};

} // namespace

std::vector<Issue> validate(const MapDocument& doc, const things::ThingDb& things, IssueText text)
{
    return Checker(doc, things, text).run();
}

std::optional<std::string> mapNameProblem(std::string_view name, IssueText text)
{
    if (name.empty()) return say(text, NOX_TRK("editor.validate.map_name_empty", "a map needs a name"));
    // The game's map scan reads each folder's name into 9 bytes (8 characters) and looks for that
    // map, so a longer name's map is never listed (ours lists maps the same way: GameHost::scanMaps)
    // and may not show right where it is.
    if (name.size() > 8) {
        return say(text, NOX_TRK("editor.validate.map_name_long", "map names are at most 8 characters, as the game's: its map lists cut a longer name short and don't find the map"));
    }
    // The game takes any folder in its maps folder as a map (its map scan; custom maps use '!',
    // '#', '[' and so on): only what a folder or file name can't hold is refused, and spaces
    // (console commands and server lists split names at them).
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return say(text, NOX_TRK("editor.validate.map_name_ascii", "map names use printable ASCII characters without spaces"));
        if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) return say(text, NOX_TRK("editor.validate.map_name_chars", "map names can't hold \\ / : * ? \" < > |"));
    }
    if (name.front() == '.' || name.back() == '.') return say(text, NOX_TRK("editor.validate.map_name_dot", "a map name can't start or end with '.'"));
    {
        // Windows' device names (CON, NUL, COM1...) can't be folders.
        std::string upper(name.substr(0, name.find('.')));
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        static constexpr std::string_view kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                                        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        if (std::ranges::find(kDevices, upper) != std::end(kDevices)) return format(say(text, NOX_TRK("editor.validate.map_name_device", "{name} is a name Windows keeps for a device")), {{"name", name}});
    }
    if (isStockMapName(name)) return format(say(text, NOX_TRK("editor.validate.map_name_stock", "{name} is a map that ships with the game: save your edits under a new name")), {{"name", name}});
    // The scan also leaves out the solo maps' names (wiz, war or con, then a digit), whatever case.
    const auto lower = [&](std::size_t i) { return static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))); };
    if (name.size() > 3 && name[3] >= '0' && name[3] <= '9') {
        const std::string p{lower(0), lower(1), lower(2)};
        if (p == "wiz" || p == "war" || p == "con") {
            return say(text, NOX_TRK("editor.validate.map_name_solo", "wiz, war or con followed by a digit names a solo map: the game's map lists leave it out"));
        }
    }
    return std::nullopt;
}

std::string suggestedMapName(std::string_view from)
{
    // "<name>_ed" when it fits the game's 8 characters, else "ed_" and the name's start; never a
    // solo map's prefix or a stock map's name.
    std::string base;
    for (const char c : from) {
        base += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string out = base + "_ed";
    if (mapNameProblem(out)) out = "ed_" + base.substr(0, 5);
    if (mapNameProblem(out)) out = "newmap";
    return out;
}

} // namespace nox::mapedit
