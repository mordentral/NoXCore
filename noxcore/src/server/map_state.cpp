// The live world's map sections for a solo save (saveMapSections; map_state.hpp lists what each
// section holds and where it comes from).
#include "noxcore/server/map_state.hpp"

#include "noxcore/map/sections.hpp"
#include "noxcore/script/script_file.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::server {

namespace {

// A section of the map file, decoded, when the file has one.
template <class Section>
std::optional<Section> optionalSection(const map::MapFile& map)
{
    if (!map.find(Section::kName)) {
        return std::nullopt;
    }
    return map::decodeSection<Section>(map);
}

// A section back to its bytes, under its name.
template <class Section>
map::RawSection raw(const Section& s)
{
    return {std::string(Section::kName), s.serialize()};
}

/// A C string's text (the exe writes strlen of its buffers).
std::string cstr(std::string_view s, std::size_t most = std::string_view::npos)
{
    s = s.substr(0, s.find('\0'));
    return std::string(s.substr(0, most));
}

/// whether the point lies within `reach` of the segment (across it, and along
/// it between its ends). (It also moves the point onto the segment; the save's callers don't
/// look.) A segment of no length divides by 0: the comparisons that follow fail as the x87's
/// unordered ones do.
bool nearSegment(int px, int py, const int seg[4], float reach)
{
    const float x0 = static_cast<float>(seg[0]);
    const float y0 = static_cast<float>(seg[1]);
    const float dx = static_cast<float>(seg[2]) - x0;
    const float dy = static_cast<float>(seg[3]) - y0;
    using util::X87;
    const float len = util::sqrt(X87(dx) * dx + X87(dy) * dy).f();
    const float ux = (X87(dx) / len).f();
    const X87 uy = X87(dy) / len; // (kept on the stack)
    const float uyf = uy.f();
    const float ax = static_cast<float>(px) - x0;
    const float ay = static_cast<float>(py) - y0;
    const float across = util::fabs(X87(ax) * -uy + X87(ay) * ux).f();
    if (across > reach) {
        return false;
    }
    const X87 along = X87(ay) * uyf + X87(ax) * ux;
    if (along > X87(len)) {
        return false;
    }
    if (!(along.f() >= 0.0f)) {
        return false;
    }
    return true;
}

/// whether the point is within `reach` of one of the polygon's edges (taken as
/// polygon does: each corner in turn into one end, alternately).
bool nearPolygon(int px, int py, const MapPolygon& p, float reach)
{
    if (p.points.empty()) {
        return false;
    }
    int edge[4] = {p.points[0].first, p.points[0].second, 0, 0};
    const std::size_t n = p.points.size();
    for (std::size_t i = 1; i <= n; ++i) {
        const auto& [x, y] = p.points[i % n];
        if ((i & 1) == 0) {
            edge[0] = x;
            edge[1] = y;
        } else {
            edge[2] = x;
            edge[3] = y;
        }
        if (nearSegment(px, py, edge, reach)) {
            return true;
        }
    }
    return false;
}

/// A wall's minimap group (server_mapRWWallMap): the polygon its middle is in
/// (polygonIsPlayerInPolygon), else the first within 10 of it,
/// else 100.
std::uint8_t wallGroup(World& world, int x, int y)
{
    const int px = x * 23 + 11;  // (a wall cell is 23 units: its middle)
    const int py = y * 23 + 11;
    if (const std::uint32_t at = world.polygonAt(px, py, 0)) {
        return world.polygons()[at - 1].group;
    }
    for (const MapPolygon& p : world.polygons()) {
        if (nearPolygon(px, py, p, 10.0f)) {
            return p.group;
        }
    }
    return 100;
}

/// FloorMap (server_mapRWFloorMap's write side) from the floor grid the load left:
/// the loaded pairs, each into its cell (a later one's tile replacing an earlier one's).
map::RawSection floorSection(const map::MapFile& loaded)
{
    struct Cell {
        std::optional<map::FloorTile> odd;
        std::optional<map::FloorTile> even;
        bool any() const { return odd || even; }
    };
    std::vector<Cell> grid(128 * 128); // x * 128 + y
    if (const auto floor = optionalSection<map::FloorMap>(loaded)) {
        for (const auto& p : floor->tiles) {
            Cell& c = grid[static_cast<std::size_t>(p.x & 0x7F) * 128 + (p.y & 0x7F)];
            if (p.odd) {
                c.odd = p.odd;
            }
            if (p.even) {
                c.even = p.even;
            }
        }
    }
    const auto at = [&](int x, int y) -> Cell& { return grid[static_cast<std::size_t>(x) * 128 + static_cast<std::size_t>(y)]; };
    const auto rowHas = [&](int y) { return std::ranges::any_of(std::views::iota(0, 128), [&](int x) { return at(x, y).any(); }); };
    const auto columnHas = [&](int x) { return std::ranges::any_of(std::views::iota(0, 128), [&](int y) { return at(x, y).any(); }); };
    // The floor's box: the first and last rows and columns with a tile in them.
    int minY = 0;
    while (minY < 128 && !rowHas(minY)) {
        ++minY;
    }
    if (minY == 128) {
        // An empty floor: the version and nothing else (an original bug: the reader then takes
        // the next section's bytes for the box).
        return {std::string(map::FloorMap::kName), {4, 0}};
    }
    int maxY = 127;
    while (!rowHas(maxY)) {
        --maxY;
    }
    int minX = 0;
    while (!columnHas(minX)) {
        ++minX;
    }
    int maxX = 127;
    while (!columnHas(maxX)) {
        --maxX;
    }
    map::FloorMap out;
    out.header = {.version = 4, .minX = minX, .minY = minY, .width = maxX - minX + 1, .height = maxY - minY + 1};
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const Cell& c = at(x, y);
            if (c.any()) {
                out.tiles.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), c.odd, c.even});
            }
        }
    }
    return raw(out);
}

} // namespace

map::MapFile saveMapSections(World& world, const things::ThingDb& things, const map::MapFile& loaded)
{
    map::MapFile out;
    out.magic = map::kMagic;
    out.crc = 0;

    // The wall list (g_wallList, newest first) as wallForeachFn walks it: no
    // broken walls, no door leaves.
    std::vector<std::pair<std::pair<int, int>, WallCell*>> listed;
    for (auto& [cell, c] : world.walls()) {
        if (!c.broken && c.door == 0) {
            listed.emplace_back(cell, &c);
        }
    }
    std::ranges::sort(listed, [](const auto& a, const auto& b) { return a.second->serial > b.second->serial; });

    // The wall offsets (mapCountWallsMB).
    std::uint32_t offX = 0x100;
    std::uint32_t offY = 0x100;
    for (const auto& [cell, c] : listed) {
        if (cell.first < static_cast<int>(offX)) {
            offX = static_cast<std::uint32_t>(cell.first);
        }
        if (cell.second < static_cast<int>(offY)) {
            offY = static_cast<std::uint32_t>(cell.second);
        }
    }
    out.wallOffsetX = offX;
    out.wallOffsetY = offY;

    // mapSetWallInGlobalDir0pr1: the walls a Wall spell changed take their old
    // shape (the records newest first).
    auto& magicWalls = world.magicWalls();
    for (auto it = magicWalls.rbegin(); it != magicWalls.rend(); ++it) {
        if (it->replaced) {
            if (WallCell* c = world.wall(it->x, it->y)) {
                c->direction = it->oldDirection;
            }
        }
    }

    // MapInfo.
    map::MapInfo info;
    if (const auto loadedInfo = optionalSection<map::MapInfo>(loaded)) {
        info = *loadedInfo;
        if (loadedInfo->version < 3) {
            info.questIntro.clear(); // (98)
            info.questGraphic.clear();
        } else {
            info.questIntro = cstr(info.questIntro);
            info.questGraphic = cstr(info.questGraphic);
        }
    }
    info.version = 3;
    out.sections.push_back(raw(info));

    // WallMap.
    int magic = -1; // (wallTileByName)
    for (std::size_t i = 0; i < things.walls.size(); ++i) {
        if (things.walls[i].name == "MagicWallSystemUseOnly") {
            magic = static_cast<int>(i);
            break;
        }
    }
    {
        int minX = 0x100;
        int minY = 0x100;
        int maxX = 0;
        int maxY = 0;
        for (const auto& [cell, c] : listed) {
            minX = std::min(minX, cell.first);
            maxX = std::max(maxX, cell.first);
            minY = std::min(minY, cell.second);
            maxY = std::max(maxY, cell.second);
        }
        map::WallMap walls;
        walls.header = {.version = 7, .minX = minX, .minY = minY, .width = maxX - minX + 1, .height = maxY - minY + 1};
        const bool blankExtra = (world.gameFlags & 0x200000u) != 0;
        // (Both loops run to one past the box: minY + height, minX + width inclusive.)
        for (int y = minY; y <= minY + walls.header.height; ++y) {
            for (int x = minX; x <= minX + walls.header.width; ++x) {
                const WallCell* c = world.gridWall(x, y);
                if (!c || c->material == static_cast<std::uint8_t>(magic)) {
                    continue;
                }
                map::Wall w;
                w.x = static_cast<std::uint8_t>(x);
                w.y = static_cast<std::uint8_t>(y);
                w.direction = static_cast<std::uint8_t>(c->direction | (c->flag80 ? 0x80 : 0));
                w.material = c->material;
                w.variant = c->variant;
                w.minimapGroup = wallGroup(world, x, y);
                // (+0xC: the map's byte as WallMap's read left it, 1 once the host's client has
                // drawn the wall: drawWalls on the one cell. No client here: the map's.)
                const bool drawn = world.hostClient && world.hostClient->wallDrawn(x, y);
                w.unknown = blankExtra ? 0 : drawn ? std::uint8_t{1} : c->extra;
                walls.walls.push_back(w);
            }
        }
        out.sections.push_back(raw(walls));
    }

    out.sections.push_back(floorSection(loaded));

    // SecretWalls, DestructableWalls (and, after the waypoints and debug data, WindowWalls).
    map::SecretWalls secret;
    map::DestructableWalls breakable;
    map::WindowWalls windows;
    for (const auto& [cell, c] : listed) {
        const map::GridPoint pos{cell.first, cell.second};
        if (c->secret) {
            secret.walls.push_back({pos, c->secretWait, c->secretFlags, c->secretState, c->secretOpen,
                                    static_cast<std::uint32_t>(c->secretTimer), c->secretSeen});
        }
        if (c->destructible) {
            breakable.walls.push_back(pos);
        }
        if (c->window) {
            windows.walls.push_back(pos);
        }
    }
    secret.version = 2;
    breakable.version = 1;
    windows.version = 2;
    out.sections.push_back(raw(secret));
    out.sections.push_back(raw(breakable));

    // WayPoints.
    map::Waypoints waypoints;
    waypoints.version = 4;
    for (const ServerWaypoint& s : world.waypoints()) {
        map::Waypoint wp;
        wp.id = static_cast<std::uint32_t>(s.id);
        wp.x = s.x;
        wp.y = s.y;
        wp.name = cstr(s.name);
        wp.flags = s.enabled ? 1u : 0u;
        for (std::size_t i = 0; i < s.links.size(); ++i) {
            wp.links.push_back({static_cast<std::uint32_t>(s.links[i]), i < s.linkTypes.size() ? s.linkTypes[i] : std::uint8_t{0}});
        }
        waypoints.waypoints.push_back(std::move(wp));
    }
    out.sections.push_back(raw(waypoints));

    // DebugData.
    map::DebugData debug;
    debug.version = 1;
    if (const auto loadedDebug = optionalSection<map::DebugData>(loaded)) {
        for (auto it = loadedDebug->entries.rbegin(); it != loadedDebug->entries.rend(); ++it) {
            debug.entries.emplace_back(cstr(it->first), cstr(it->second));
        }
    }
    out.sections.push_back(raw(debug));

    out.sections.push_back(raw(windows));

    // GroupData.
    map::GroupData groups;
    groups.version = 3;
    for (auto it = world.groups().rbegin(); it != world.groups().rend(); ++it) {
        map::Group g;
        g.name = cstr(it->name, 75); // (server_mapLoadAddGroup: strncpy 0x4C, the last byte 0)
        g.kind = static_cast<map::Group::Kind>(it->kind);
        g.id = it->id;
        for (const GroupMember m : it->members) { // (ours kept in the list's order, its head first: World::load)
            g.members.push_back(m.raw());
        }
        for (const auto& [x, y] : it->walls) {
            g.walls.push_back({x, y});
        }
        groups.groups.push_back(std::move(g));
    }
    out.sections.push_back(raw(groups));

    // ScriptObject.
    map::ScriptObject script;
    script.version = 1;
    const auto loadedScript = optionalSection<map::ScriptObject>(loaded);
    if (loadedScript) {
        script.code = loadedScript->code;
    }
    out.sections.push_back(raw(script));

    // AmbientData.
    map::AmbientData ambient;
    ambient.version = 1;
    if (const auto loadedAmbient = optionalSection<map::AmbientData>(loaded)) {
        ambient.r = loadedAmbient->r;
        ambient.g = loadedAmbient->g;
        ambient.b = loadedAmbient->b;
    } else {
        ambient.r = ambient.g = ambient.b = 25; // (mapSwitchLevel)
    }
    out.sections.push_back(raw(ambient));

    // Polygons.
    {
        map::Polygons polys;
        polys.version = 4;
        const auto loadedPolys = optionalSection<map::Polygons>(loaded);
        const std::size_t count = world.polygons().size();
        if (count != 0 && (!loadedPolys || loadedPolys->polygons.size() != count)) {
            throw std::logic_error("saveMapSections: the world's polygons aren't the loaded map's");
        }
        // The functions the handlers name (over the map's script).
        std::optional<script::ScriptFile> file;
        if (!world.script && loadedScript) {
            try {
                file = script::ScriptFile::parse(loadedScript->code);
            } catch (const std::exception&) {
                file.reset();
            }
        }
        const auto handler = [&](const ServerObject::ScriptBlock& block) {
            map::ScriptHandler h;
            h.version = 1;
            h.unknown = block.flags;
            const std::string name = cstr(block.function);
            const bool known = !name.empty() && (world.script ? world.script->function(name).has_value() : file && file->find(name).has_value());
            if (known) {
                h.function = name;
            }
            return h;
        };
        if (loadedPolys) {
            // The corners numbered 1.. (polygonSetAngle), the polygons' by those
            // (the last corner of each map id; ids without one, and 0, as they are).
            for (std::size_t i = 0; i < loadedPolys->points.size(); ++i) {
                const auto& p = loadedPolys->points[i];
                polys.points.push_back({static_cast<std::uint32_t>(i + 1), p.x, p.y});
            }
            for (std::size_t i = 0; i < count; ++i) {
                const map::Polygon& from = loadedPolys->polygons[i];
                const MapPolygon& live = world.polygons()[i];
                map::Polygon p;
                p.name = cstr(from.name);
                p.ambientLight = from.ambientLight;
                p.minimapGroup = live.group;
                for (const std::uint32_t id : from.points) {
                    std::uint32_t index = id;
                    for (std::size_t k = loadedPolys->points.size(); k-- > 0;) {
                        if (loadedPolys->points[k].id != 0 && loadedPolys->points[k].id == id) {
                            index = static_cast<std::uint32_t>(k + 1);
                            break;
                        }
                    }
                    p.points.push_back(index);
                }
                p.playerEnter = handler(live.onEnter);
                p.monsterEnter = handler(live.onLeave);
                p.flags = live.flags;
                polys.polygons.push_back(std::move(p));
            }
        }
        out.sections.push_back(raw(polys));
    }

    // MapIntro.
    map::MapIntro intro;
    intro.version = 1;
    if (world.gameFlags & 0x200000u) {
        if (const auto loadedIntro = optionalSection<map::MapIntro>(loaded)) {
            intro.text = loadedIntro->text;
        }
    }
    out.sections.push_back(raw(intro));

    // The changed walls take the spells' shapes again.
    for (auto it = magicWalls.rbegin(); it != magicWalls.rend(); ++it) {
        if (it->replaced) {
            if (WallCell* c = world.wall(it->x, it->y)) {
                c->direction = it->direction;
            }
        }
    }
    return out;
}

} // namespace nox::server
