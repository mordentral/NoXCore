// What the toolbox's parts in their own files (mod_*.cpp)
// need from the script mode (scripted_mode.cpp defines each). Every function takes the mode's own
// thread (a toolbox function's L, or the one its hooks are called on). (Internal to the script
// modes' sources.)
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/server/combat.hpp"

#include <cstdint>
#include <string>

struct lua_State;

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
/// An error when called inside an asked hook (onDamage, onExit): `what` changes the world.
void refuseInsideAsked(lua_State* L, const char* what);
/// A script error reported (logged once in a while, kept in errors()).
void report(lua_State* L, std::string line);
/// The script's mode-table function `name` pushed; false: it has none (nothing pushed).
bool beginHook(lua_State* L, const char* name);
/// The function under its `nargs` arguments called, protected, on a fresh budget (an error
/// reported as `what`'s); false: it failed (nothing left), else its `nresults` on the stack.
bool callHook(lua_State* L, const char* what, int nargs, int nresults);

} // namespace toolbox

} // namespace nox::modes::script
