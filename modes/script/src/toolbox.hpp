// What the toolbox's parts in their own files (mod_*.cpp)
// need from the script mode (scripted_mode.cpp defines each). Every function takes the mode's own
// thread (a toolbox function's L, or the one its hooks are called on). (Internal to the script
// modes' sources.)
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/server/combat.hpp"

#include "lua.h"
#include "lualib.h"

#include <cstdint>
#include <optional>
#include <string>

namespace nox::server {
class GameHost;
class World;
struct ServerObject;
} // namespace nox::server
namespace nox::things {
struct ThingDb;
struct ModifierDb;
} // namespace nox::things

namespace nox::modes::script {

struct Manifest;
struct Parts;

namespace toolbox {

/// The mode's parts (mod_parts.hpp).
Parts& partsOf(lua_State* L);
/// What the host gave the mode, and its mode.json.
const ServerContext& contextOf(lua_State* L);
const Manifest& manifestOf(lua_State* L);
/// The host (null before start or after stop).
server::GameHost* hostOf(lua_State* L);
/// The game's world and its fight; an error before start.
server::World& worldOf(lua_State* L);
server::Fight fightOf(lua_State* L);
/// thing.bin (an error when the host gave none), modifier.bin (null when the host has none).
const things::ThingDb& thingsOf(lua_State* L);
const things::ModifierDb* modifiersOf(lua_State* L);
/// The thing whose id is at `idx` (an error: gone).
server::ServerObject& objectArg(lua_State* L, int idx);
/// A player's unit at `idx`, never a held-out stock player's (an error for either).
server::ServerObject& playerUnitArg(lua_State* L, int idx);
/// Whether `o` is a held-out stock player's unit (the script never hears of it).
bool hidden(lua_State* L, const server::ServerObject& o);
/// A unit's peer (its player's), none for a unit without one (a test's, a bot's own) or a
/// held-out stock player's.
std::optional<std::uint8_t> peerOfUnit(lua_State* L, std::uint32_t extent);
/// An error when called inside an asked hook (onDamage, onExit): `what` changes the world.
void refuseInsideAsked(lua_State* L, const char* what);
/// A script error reported (logged once in a while, kept in errors()).
void report(lua_State* L, std::string line);
/// The script's mode-table function `name` pushed; false: it has none (nothing pushed).
bool beginHook(lua_State* L, const char* name);
/// The function under its `nargs` arguments called, protected, on a fresh budget (an error
/// reported as `what`'s); false: it failed (nothing left), else its `nresults` on the stack.
bool callHook(lua_State* L, const char* what, int nargs, int nresults);
/// A script's number as the float the engine takes: the nearest float (ties to even) whatever the
/// thread's rounding, as a compiled map script's float literal is. (The server's figuring rounds
/// toward zero, as the game's FPU: a plain conversion there would take a decimal such as 0.1 a
/// float lower, and differently from a call made outside it.) Every number a script hands the
/// engine as a float goes through it; the engine's own figuring keeps the game's rounding.
inline float toFloat(double v) noexcept
{
    return nearestFloat(v);
}

/// The map's extent: 256 wall cells of 23.
inline constexpr double kMapExtent = 256.0 * 23.0;

/// A coordinate (or a distance) on the map: a number from 0 to the map's extent, never NaN or
/// infinite (they would reach the engine's casts and every client).
inline float coordArg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 0 && v <= kMapExtent, idx, "a point on the map (0 to 5888)");
    return toFloat(v);
}

/// A number from lo to hi as the float the engine takes: never NaN or infinite.
inline float floatArg(lua_State* L, int idx, double lo, double hi, const char* what)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= lo && v <= hi, idx, what);
    return toFloat(v);
}

/// The table on top of the stack's field `name` set to the number `v`.
inline void setField(lua_State* L, const char* name, double v)
{
    lua_pushnumber(L, v);
    lua_setfield(L, -2, name);
}

} // namespace toolbox

} // namespace nox::modes::script
