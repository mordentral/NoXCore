// The generator's level builder (arena.hpp): the design is its script (modes/common/script/mapgen/
// arena.luau and the scripts it requires), run in a VM of
// this level's own on the calling thread over the native blocks; the writing is MapWriter's, driven
// by it, and the files' encoding is here.
#include "noxcore/mapgen/arena.hpp"
#include "noxcore/mapgen/arena_lua.hpp"

#include "gen_bindings.hpp"
#include "lua_lists.hpp"
#include "map_writer.hpp"

#include "noxcore/map/map_save.hpp"
#include "noxcore/modes/script/gen_runtime.hpp"
#include "noxcore/util/x87.hpp"

#include "lua.h"

#include <bit>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nox::mapgen {

namespace {

void setString(lua_State* L, const char* name, const std::string& v)
{
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, name);
}

void setNumber(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

void setFlag(lua_State* L, const char* name, bool v)
{
    lua_pushboolean(L, v);
    lua_setfield(L, -2, name);
}

void setStrings(lua_State* L, const char* name, const std::vector<std::string>& list)
{
    modes::detail::pushStrings(L, list);
    lua_setfield(L, -2, name);
}

// A field read back (readArenaParams): its value pushed, and whether it's taken (else popped).
bool fieldAt(lua_State* L, int t, const char* name, ArenaRead how)
{
    if (how == ArenaRead::Typed) {
        lua_rawgetfield(L, t, name);
        return true;
    }
    lua_getfield(L, t, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

template <typename T>
void readNumber(lua_State* L, int t, const char* name, T& out, ArenaRead how)
{
    if (!fieldAt(L, t, name, how)) {
        return;
    }
    if (how == ArenaRead::Typed) {
        if (lua_isnumber(L, -1)) {
            out = static_cast<T>(lua_tonumber(L, -1));
        }
    } else {
        out = lua_isboolean(L, -1) ? static_cast<T>(lua_toboolean(L, -1)) : static_cast<T>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

void readFlag(lua_State* L, int t, const char* name, bool& out, ArenaRead how)
{
    if (how == ArenaRead::Present) {
        readNumber(L, t, name, out, how);
        return;
    }
    lua_rawgetfield(L, t, name);
    if (lua_isboolean(L, -1)) {
        out = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
}

// The string at the stack's top (Present: anything, "" for what has none; Typed: a string or a
// number), into `out`.
void stringOnTop(lua_State* L, std::string& out, ArenaRead how)
{
    if (how == ArenaRead::Typed && !lua_isstring(L, -1)) {
        return;
    }
    std::size_t n = 0;
    const char* s = lua_tolstring(L, -1, &n);
    out.assign(s ? s : "", s ? n : 0);
}

void readString(lua_State* L, int t, const char* name, std::string& out, ArenaRead how)
{
    if (fieldAt(L, t, name, how)) {
        stringOnTop(L, out, how);
        lua_pop(L, 1);
    }
}

void readStrings(lua_State* L, int t, const char* name, std::vector<std::string>& out, ArenaRead how)
{
    if (!fieldAt(L, t, name, how)) {
        return;
    }
    if (how == ArenaRead::Present || lua_istable(L, -1)) {
        out.clear();
        for (int i = 1, n = lua_objlen(L, -1); i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            std::string s;
            stringOnTop(L, s, ArenaRead::Present);
            out.push_back(std::move(s));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

void pushGates(lua_State* L, const char* name, const std::vector<ArenaParams::Gate>& gates)
{
    lua_createtable(L, static_cast<int>(gates.size()), 0);
    for (std::size_t i = 0; i < gates.size(); ++i) {
        lua_createtable(L, 0, 3);
        setString(L, "type", gates[i].type);
        setString(L, "toBiome", gates[i].toBiome);
        setString(L, "sign", gates[i].sign);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, -2, name);
}

void readGates(lua_State* L, int t, const char* name, std::vector<ArenaParams::Gate>& out, ArenaRead how)
{
    if (!fieldAt(L, t, name, how)) {
        return;
    }
    const int g = lua_gettop(L);
    if (how == ArenaRead::Present || lua_istable(L, g)) {
        out.clear();
        for (int i = 1, n = lua_objlen(L, g); i <= n; ++i) {
            lua_rawgeti(L, g, i);
            const int at = lua_gettop(L);
            ArenaParams::Gate gate;
            readString(L, at, "type", gate.type, how);
            readString(L, at, "toBiome", gate.toBiome, how);
            readString(L, at, "sign", gate.sign, how);
            out.push_back(std::move(gate));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

// ArenaParams' fields in ArenaField's order: each its bit, its name in a script's table, where it
// is (a number, a flag, a string, a list of strings, the gates).
using Member = std::variant<int ArenaParams::*, std::uint32_t ArenaParams::*, std::uint8_t ArenaParams::*, bool ArenaParams::*, std::string ArenaParams::*,
                            std::vector<std::string> ArenaParams::*, std::vector<ArenaParams::Gate> ArenaParams::*>;
struct Field {
    std::uint32_t bit;
    const char* name;
    Member member;
};
const Field kFields[] = {
    {ArenaField::Title, "title", &ArenaParams::title},
    {ArenaField::Author, "author", &ArenaParams::author},
    {ArenaField::GameFlags, "gameFlags", &ArenaParams::gameFlags},
    {ArenaField::MinPlayers, "minPlayers", &ArenaParams::minPlayers},
    {ArenaField::MaxPlayers, "maxPlayers", &ArenaParams::maxPlayers},
    {ArenaField::CenterX, "centerX", &ArenaParams::centerX},
    {ArenaField::CenterY, "centerY", &ArenaParams::centerY},
    {ArenaField::Radius, "radius", &ArenaParams::radius},
    {ArenaField::Segments, "segments", &ArenaParams::segments},
    {ArenaField::Corridor, "corridor", &ArenaParams::corridor},
    {ArenaField::EndRadius, "endRadius", &ArenaParams::endRadius},
    {ArenaField::WallSet, "wallSet", &ArenaParams::wallSet},
    {ArenaField::FloorName, "floorName", &ArenaParams::floorName},
    {ArenaField::Ambient, "ambient", &ArenaParams::ambient},
    {ArenaField::Biome, "biome", &ArenaParams::biome},
    {ArenaField::PlayerStarts, "playerStarts", &ArenaParams::playerStarts},
    {ArenaField::Items, "items", &ArenaParams::items},
    {ArenaField::Chests, "chests", &ArenaParams::chests},
    {ArenaField::Monsters, "monsters", &ArenaParams::monsters},
    {ArenaField::Centre, "centre", &ArenaParams::centre},
    {ArenaField::QuestExit, "questExit", &ArenaParams::questExit},
    {ArenaField::Gates, "gates", &ArenaParams::gates},
    {ArenaField::WriteGates, "writeGates", &ArenaParams::writeGates},
    {ArenaField::Signs, "signs", &ArenaParams::signs},
    {ArenaField::Traps, "traps", &ArenaParams::traps},
    {ArenaField::Puzzles, "puzzles", &ArenaParams::puzzles},
    {ArenaField::Ambushes, "ambushes", &ArenaParams::ambushes},
    {ArenaField::Doors, "doors", &ArenaParams::doors},
    {ArenaField::Obelisks, "obelisks", &ArenaParams::obelisks},
    {ArenaField::Depth, "depth", &ArenaParams::depth},
    {ArenaField::Seed, "seed", &ArenaParams::seed},
};

/// A {x, y} at the table on the stack's top (popping nothing).
ArenaSpots::Point pointOnTop(lua_State* L)
{
    ArenaSpots::Point pt;
    if (lua_istable(L, -1)) {
        lua_rawgetfield(L, -1, "x");
        pt.x = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgetfield(L, -1, "y");
        pt.y = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    return pt;
}

ArenaSpots::Point pointAt(lua_State* L, int idx, const char* name)
{
    lua_rawgetfield(L, idx, name);
    const ArenaSpots::Point pt = pointOnTop(L);
    lua_pop(L, 1);
    return pt;
}

/// What arena.luau's build returned: where the things went.
ArenaSpots spotsAt(lua_State* L, int idx)
{
    ArenaSpots s;
    s.end = pointAt(L, idx, "end");
    lua_rawgetfield(L, idx, "centre");
    for (int i = 1; i <= lua_objlen(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        s.centre.push_back(pointOnTop(L));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_rawgetfield(L, idx, "gates");
    for (int i = 1; i <= lua_objlen(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        const int g = lua_gettop(L);
        ArenaSpots::Gate gate;
        lua_rawgetfield(L, g, "type");
        gate.type = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        gate.at = pointAt(L, g, "at");
        gate.sign = pointAt(L, g, "sign");
        s.gates.push_back(std::move(gate));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_rawgetfield(L, idx, "features");
    for (int i = 1; i <= lua_objlen(L, -1); ++i) {
        lua_rawgeti(L, -1, i);
        s.features.emplace_back(lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return s;
}

} // namespace

void pushArenaParams(lua_State* L, const ArenaParams& p, std::uint32_t fields)
{
    lua_createtable(L, 0, std::popcount(fields));
    for (const Field& f : kFields) {
        if ((fields & f.bit) == 0) {
            continue;
        }
        std::visit(
            [&](auto member) {
                using T = std::remove_cvref_t<decltype(p.*member)>;
                if constexpr (std::is_same_v<T, bool>) {
                    setFlag(L, f.name, p.*member);
                } else if constexpr (std::is_arithmetic_v<T>) {
                    setNumber(L, f.name, p.*member);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    setString(L, f.name, p.*member);
                } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                    setStrings(L, f.name, p.*member);
                } else {
                    pushGates(L, f.name, p.*member);
                }
            },
            f.member);
    }
}

void readArenaParams(lua_State* L, int t, ArenaParams& p, std::uint32_t fields, ArenaRead how)
{
    t = lua_absindex(L, t);
    for (const Field& f : kFields) {
        if ((fields & f.bit) == 0) {
            continue;
        }
        std::visit(
            [&](auto member) {
                using T = std::remove_cvref_t<decltype(p.*member)>;
                if constexpr (std::is_same_v<T, bool>) {
                    readFlag(L, t, f.name, p.*member, how);
                } else if constexpr (std::is_arithmetic_v<T>) {
                    readNumber(L, t, f.name, p.*member, how);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    readString(L, t, f.name, p.*member, how);
                } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                    readStrings(L, t, f.name, p.*member, how);
                } else {
                    readGates(L, t, f.name, p.*member, how);
                }
            },
            f.member);
    }
}

map::MapFile buildArena(const things::ThingDb& things, const ArenaParams& p, ArenaSpots* spots)
{
    // (NoX Core's own generator: rounded to nearest wherever it runs, so a seed makes the same
    // level as it always did; the server's chop rounding is for the game's own figuring.)
    const util::RoundingScope nearest(false);
    modes::script::GenVm vm;
    detail::MapWriter writer(things);
    ArenaSpots out;
    vm.call("arena", "build", [&](lua_State* L) {
        // (Every field but the map's own: its title, author, game types and player counts are the
        // writer's, below.)
        using F = ArenaField;
        pushArenaParams(L, p, F::All & ~(F::Title | F::Author | F::GameFlags | F::MinPlayers | F::MaxPlayers));
        detail::pushMapWriter(L, writer);
        return 2;
    }, 1, [&](lua_State* L) { out = spotsAt(L, lua_gettop(L)); });
    if (spots) {
        *spots = std::move(out);
    }
    return writer.finish({.title = p.title, .author = p.author, .gameFlags = p.gameFlags, .minPlayers = p.minPlayers, .maxPlayers = p.maxPlayers});
}

EncodedMap encodeMapFiles(const map::MapFile& map, const things::ThingDb& things)
{
    return map::encodeMapBytes(map::serializeMapFileWithCrc(map, things));
}

std::filesystem::path writeEncodedMapFiles(const std::filesystem::path& mapsDir, const std::string& name, const EncodedMap& files)
{
    return map::writeMapFolder(mapsDir, name, files, "mapgen");
}

std::filesystem::path writeMapFiles(const std::filesystem::path& mapsDir, const std::string& name, const map::MapFile& map,
                                    const things::ThingDb& things)
{
    return writeEncodedMapFiles(mapsDir, name, encodeMapFiles(map, things));
}

} // namespace nox::mapgen
