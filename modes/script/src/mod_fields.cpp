// nox.flags, nox.setFlag, nox.monsterStatus, nox.cursor and nox.setWallDebris: a thing's own fields
// read, a few of them set, a player's cursor read, a wall type's debris for the map (mod_fields.hpp).
#include "mod_fields.hpp"

#include "toolbox.hpp"

#include "noxcore/server/world.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nox::modes::script {

namespace {

namespace oc = things::ObjectClass;
namespace ms = things::MonsterStatus;

/// The object flags nox.setFlag may change: SHORT (missiles and flyers pass over it), NO_COLLIDE,
/// NO_PUSH_CHARACTERS (creatures and players don't push it), and DEAD on a thing that isn't a
/// creature or a player (a chest with it is shut for good: collideChest opens none).
constexpr std::uint32_t kShort = 0x10;
constexpr std::uint32_t kSettable = kShort | server::ObjFlag::NoCollide | server::ObjFlag::NoPushCharacters | server::ObjFlag::Dead;

/// The creature status bits nox.monsterStatus may change: whether it may run, runs now, always or
/// never runs (the AI's own reads of them, its speed and its walk or run).
constexpr std::uint32_t kRunBits = ms::CanRun | ms::Running | ms::AlwaysRun | ms::NeverRun;

std::uint32_t bitsArg(lua_State* L, int idx, std::uint32_t allowed, const char* what)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v > 0 && v <= 4294967295.0 && v == std::floor(v) && (static_cast<std::uint32_t>(v) & ~allowed) == 0, idx, what);
    return static_cast<std::uint32_t>(v);
}

server::ServerObject& thingArg(lua_State* L, int idx)
{
    server::ServerObject& o = toolbox::objectArg(L, idx);
    luaL_argcheck(L, !toolbox::hidden(L, o) && !o.gone(), idx, "no such thing (gone?)");
    return o;
}

int noxFlags(lua_State* L)
{
    // nox.flags(id): the thing's object flags as the engine has them (read only).
    lua_pushnumber(L, thingArg(L, 1).flags);
    return 1;
}

int noxSetFlag(lua_State* L)
{
    // nox.setFlag(id, flag, on): one or more of the settable flags set (on, or nil) or cleared, as a
    // field: what reads it next frame sees it; nothing is sent.
    toolbox::refuseInsideAsked(L, "nox.setFlag");
    server::ServerObject& o = thingArg(L, 1);
    const std::uint32_t bits = bitsArg(L, 2, kSettable, "flags 0x10 (SHORT), 0x40 (NO_COLLIDE), 0x2000 (NO_PUSH_CHARACTERS), 0x8000 (DEAD)");
    const bool on = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    luaL_argcheck(L, !(bits & server::ObjFlag::Dead) || !(o.classes & (oc::Monster | oc::Player)), 2,
                  "DEAD on a creature or a player (they die by damage)");
    if (on) {
        o.flags |= bits;
    } else {
        o.flags &= ~bits;
    }
    return 0;
}

int noxMonsterStatus(lua_State* L)
{
    // nox.monsterStatus(id): a creature's status bits (monster.bin's STATUS as it plays: 0x4000
    // running...); nox.monsterStatus(id, bits, on): the run bits among them set or cleared.
    server::ServerObject& o = thingArg(L, 1);
    luaL_argcheck(L, static_cast<bool>(o.monster), 1, "a creature");
    if (lua_isnoneornil(L, 2)) {
        lua_pushnumber(L, o.monster->status);
        return 1;
    }
    toolbox::refuseInsideAsked(L, "nox.monsterStatus");
    const std::uint32_t bits = bitsArg(L, 2, kRunBits, "the run bits: 0x2000 (CAN_RUN), 0x4000 (RUNNING), 0x8000 (ALWAYS_RUN), 0x10000 (NEVER_RUN)");
    const bool on = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    if (on) {
        o.monster->status |= bits;
    } else {
        o.monster->status &= ~bits;
    }
    lua_pushnumber(L, o.monster->status);
    return 1;
}

int noxCursor(lua_State* L)
{
    // nox.cursor(unit): where the player's cursor is in the world, as its client last told it
    // (MSG_MOUSE: what aims its swings and spells); read only.
    const server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    lua_pushnumber(L, u.player->cursorX);
    lua_pushnumber(L, u.player->cursorY);
    return 2;
}

int noxSetWallDebris(lua_State* L)
{
    // nox.setWallDebris(wallType, names): what the breakable walls of a WALL type (thing.bin's name)
    // throw when hit or broken, for this map: a list of 0-8 thing types in place of thing.bin's
    // ({} none); nil, thing.bin's again. Each map loads with thing.bin's.
    toolbox::refuseInsideAsked(L, "nox.setWallDebris");
    const char* name = luaL_checkstring(L, 1);
    server::World& world = toolbox::worldOf(L);
    const int material = world.wallMaterial(name);
    luaL_argcheck(L, material >= 0, 1, "a wall type of thing.bin's");
    if (lua_isnoneornil(L, 2)) {
        world.setWallDebris(material, std::nullopt);
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    luaL_argcheck(L, n <= 8, 2, "at most 8 thing types");
    const things::ThingDb& things = toolbox::thingsOf(L);
    std::vector<std::string> names;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        luaL_argcheck(L, s && things.findThing(std::string_view(s, len)), 2, "a list of thing types (thing.bin's names)");
        names.emplace_back(s, len);
        lua_pop(L, 1);
    }
    world.setWallDebris(material, std::move(names));
    return 0;
}

} // namespace

void addFieldsToolbox(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"flags", noxFlags},   {"setFlag", noxSetFlag},           {"monsterStatus", noxMonsterStatus},
        {"cursor", noxCursor}, {"setWallDebris", noxSetWallDebris},
    };
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::modes::script
