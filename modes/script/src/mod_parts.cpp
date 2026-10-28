// A script mode's parts' toolbox functions and configure's quest options (mod_parts.hpp).
#include "mod_parts.hpp"

#include "mod_items.hpp"
#include "toolbox.hpp"

#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/world.hpp"

#include "mode_shared.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <optional>
#include <stdexcept>

namespace nox::modes::script {

namespace {

namespace ev = server::ModeEvent;

/// The event bits a script may set (Use, 0x20, is kept for later).
constexpr std::uint8_t kWatchable = ev::Death | ev::Destroyed | ev::Damage | ev::Damaged | ev::Drop | ev::Equip;

std::uint32_t frameNow(lua_State* L)
{
    server::GameHost* host = toolbox::hostOf(L);
    return host ? host->server().world().frameNow : 0;
}

std::string_view currentMap(lua_State* L)
{
    server::GameHost* host = toolbox::hostOf(L);
    return host ? std::string_view(host->server().world().mapName) : std::string_view();
}

// ---- the scheduler (13.8)

int noxAfter(lua_State* L)
{
    return toolbox::partsOf(L).scheduler.add(L, frameNow(L), false);
}

int noxEvery(lua_State* L)
{
    return toolbox::partsOf(L).scheduler.add(L, frameNow(L), true);
}

int noxCancelTimer(lua_State* L)
{
    const double id = luaL_checknumber(L, 1);
    const bool was = id >= 1 && id <= 4294967295.0 && id == std::floor(id) &&
                     toolbox::partsOf(L).scheduler.cancel(L, static_cast<std::uint32_t>(id));
    lua_pushboolean(L, was);
    return 1;
}

// ---- the events (13.3)

int noxWatchEvents(lua_State* L)
{
    // nox.watchEvents(id, mask): the thing's event bits (nox.events') set; the old ones.
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, !toolbox::hidden(L, o), 1, "no such thing (gone?)");
    const double mask = luaL_checknumber(L, 2);
    luaL_argcheck(L, mask >= 0 && mask <= 255 && mask == std::floor(mask) && (static_cast<unsigned>(mask) & ~kWatchable) == 0, 2,
                  "a sum of nox.events' bits (death, destroyed, damage, damaged, drop, equip)");
    lua_pushnumber(L, o.modeEvents & kWatchable);
    o.modeEvents = static_cast<std::uint8_t>((o.modeEvents & ~kWatchable) | static_cast<std::uint8_t>(mask));
    return 1;
}

void pushEvents(lua_State* L)
{
    static constexpr std::pair<const char*, std::uint8_t> kBits[] = {
        {"death", ev::Death}, {"destroyed", ev::Destroyed}, {"damage", ev::Damage},
        {"damaged", ev::Damaged}, {"drop", ev::Drop},       {"equip", ev::Equip},
    };
    lua_createtable(L, 0, static_cast<int>(std::size(kBits)));
    for (const auto& [name, bit] : kBits) {
        lua_pushnumber(L, bit);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
}

// ---- the generator (13.5)

int noxGenerate(lua_State* L)
{
    // nox.generate{map = tag, module = ..., fn = ..., params = ..., seed = ..., title = ...,
    // ahead = true} or {map = tag, arena = {...}, ...}: ahead (a worker; onGenerated tells): true,
    // or false and why; now: true and the result, or false and why.
    toolbox::refuseInsideAsked(L, "nox.generate");
    Generation& gen = toolbox::partsOf(L).generation;
    Generation::Request r = gen.requestArg(L, 1);
    lua_rawgetfield(L, 1, "ahead");
    const bool ahead = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (util::equalsNoCase(r.name, currentMap(L))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "in use (the map being played)");
        return 2;
    }
    if (ahead) {
        std::string error;
        if (!gen.start(std::move(r), error)) {
            lua_pushboolean(L, 0);
            lua_pushlstring(L, error.data(), error.size());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    const Generation::Built b = gen.runNow(r, currentMap(L));
    lua_pushboolean(L, b.ok);
    Generation::pushResult(L, b);
    return 2;
}

int noxMapName(lua_State* L)
{
    const auto name = toolbox::partsOf(L).generation.nameOf(luaL_checkstring(L, 1));
    luaL_argcheck(L, name.has_value(), 1, "a tag (1-3 of a-z and 0-9, a letter first) of a mod with a mapPrefix");
    lua_pushlstring(L, name->data(), name->size());
    return 1;
}

int noxCurrentMap(lua_State* L)
{
    // nox.currentMap(): the loaded map's name ("" before the game's first map).
    const std::string_view name = currentMap(L);
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

int noxGenerating(lua_State* L)
{
    const auto state = toolbox::partsOf(L).generation.state(luaL_checkstring(L, 1));
    state ? (void)lua_pushlstring(L, state->data(), state->size()) : lua_pushnil(L);
    return 1;
}

int noxCancelGenerate(lua_State* L)
{
    lua_pushboolean(L, toolbox::partsOf(L).generation.cancel(luaL_checkstring(L, 1)));
    return 1;
}

} // namespace

void addPartsToolbox(lua_State* L, bool store)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"after", noxAfter},          {"every", noxEvery},        {"cancelTimer", noxCancelTimer},
        {"watchEvents", noxWatchEvents}, {"generate", noxGenerate}, {"mapName", noxMapName},
        {"generating", noxGenerating}, {"cancelGenerate", noxCancelGenerate}, {"currentMap", noxCurrentMap},
    };
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    pushEvents(L);
    lua_setfield(L, -2, "events");
    if (store) {
        pushStoreTable(L);
        lua_setfield(L, -2, "store");
    }
    addItemsToolbox(L);
    addWeatherToolbox(L);
}

void applyHostOptions(lua_State* L, int t, server::GameHost::Options& o, const Generation& gen, const std::string& firstMap)
{
    t = lua_absindex(L, t);
    const auto flag = [&](const char* key) -> std::optional<bool> {
        lua_rawgetfield(L, t, key);
        std::optional<bool> v;
        if (lua_isboolean(L, -1)) {
            v = lua_toboolean(L, -1) != 0;
        } else if (!lua_isnil(L, -1)) {
            lua_pop(L, 1);
            throw std::runtime_error(std::string("configure's ") + key + " is true or false");
        }
        lua_pop(L, 1);
        return v;
    };
    const bool quest = flag("quest").value_or(false);
    if (quest) {
        // (The Delve's and the Tide's run options: the mode picks its maps; the players' own
        // characters never written: quest saves off, whatever else the table says.)
        server::detail::runHostOptions(o, firstMap);
        if (firstMap.empty()) {
            o.questMapsOnly.clear(); // (a map the host names: none of its own to keep the quest to)
        }
        if (flag("questScreens").value_or(false)) {
            o.questScreens = true;
        }
        lua_rawgetfield(L, t, "questMaps");
        if (lua_istable(L, -1)) {
            const int n = lua_objlen(L, -1);
            for (int i = 1; i <= n && i <= 64; ++i) {
                lua_rawgeti(L, -1, i);
                const char* s = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
                if (!s) {
                    lua_pop(L, 2);
                    throw std::runtime_error("configure's questMaps lists maps' names or tags");
                }
                const auto byTag = gen.nameOf(s);
                o.questMapsOnly.push_back(byTag ? *byTag : std::string(s));
                lua_pop(L, 1);
            }
        } else if (!lua_isnil(L, -1)) {
            lua_pop(L, 1);
            throw std::runtime_error("configure's questMaps is a list");
        }
        lua_pop(L, 1);
        lua_rawgetfield(L, t, "forceClass");
        if (lua_isstring(L, -1)) {
            const std::string_view c = lua_tostring(L, -1);
            if (c == "warrior") {
                o.forceClass = 0;
            } else if (c == "wizard") {
                o.forceClass = 1;
            } else if (c == "conjurer") {
                o.forceClass = 2;
            } else {
                lua_pop(L, 1);
                throw std::runtime_error("configure's forceClass is warrior, wizard or conjurer");
            }
        }
        lua_pop(L, 1);
    }
    if (const auto m = flag("multiclass")) {
        o.multiclass = *m;
    }
    if (const auto m = flag("monstersAlert")) {
        o.monstersAlert = *m;
    }
    if (const auto m = flag("keepThingsOnDeath")) {
        o.keepThingsOnDeath = *m; // (in a quest it does nothing: the quest's own rules)
    }
    lua_rawgetfield(L, t, "serverName");
    if (lua_isstring(L, -1)) {
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        if (n == 0 || n > 15) {
            lua_pop(L, 1);
            throw std::runtime_error("configure's serverName is 1-15 letters");
        }
        o.serverName.assign(s, n);
    }
    lua_pop(L, 1);
}

} // namespace nox::modes::script
