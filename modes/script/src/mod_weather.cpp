// nox.weather and what the server keeps of it (mod_weather.hpp).
#include "mod_weather.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/server/game_host.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <string_view>

namespace nox::modes::script {

namespace {

/// Whether a client at `peer` is at client level 1 or above (lobby::kClientLevel): the one
/// place the weather's gate asks it. Asked each time, with no minimum assumed: a client let in can
/// still read 0 (its query lost, or its identification run out).
bool levelOne(const server::GameHost& host, std::uint8_t peer)
{
    return host.clientLevel(peer) >= 1;
}

} // namespace

bool Weather::reaches(const server::GameHost& host, std::uint8_t peer)
{
    return host.players().contains(peer) && !host.refused(peer) &&
           (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 && levelOne(host, peer);
}

Weather::Peer& Weather::peer(std::uint8_t p)
{
    const auto [it, made] = peers_.try_emplace(p);
    if (made) {
        it->second.want = all_; // (everyone's, till it's given its own)
        it->second.fade = allFade_;
    }
    return it->second;
}

void Weather::set(const server::GameHost* host, std::optional<std::uint8_t> p, const weather::State& state, std::uint16_t fade)
{
    used_ = true;
    dirty_ = true;
    if (p) {
        // (A player not in the game, or held out: nothing kept for it.)
        if (host && host->players().contains(*p) && !host->refused(*p)) {
            Peer& r = peer(*p);
            r.want = state;
            r.fade = fade;
        }
        return;
    }
    all_ = state;
    allFade_ = fade;
    for (auto& [q, r] : peers_) {
        r.want = state;
        r.fade = fade;
    }
    if (host) {
        for (const auto& [q, hp] : host->players()) {
            if (!host->refused(q)) {
                peer(q); // (everyone's from now)
            }
        }
    }
}

void Weather::ready(const server::GameHost& host, std::uint8_t p)
{
    if (!used_ || !host.players().contains(p) || host.refused(p)) {
        return;
    }
    Peer& r = peer(p);
    r.has = {}; // (its client cleared its own with the map's load)
    r.instant = true;
    dirty_ = true;
}

void Weather::flush(server::GameHost& host)
{
    if (!dirty_) {
        return;
    }
    dirty_ = false;
    for (auto& [p, r] : peers_) {
        if (r.want == r.has || !reaches(host, p)) {
            continue;
        }
        if (host.reliableRoom(p) == 0) {
            dirty_ = true; // (next frame, or once its acknowledgements catch up)
            continue;
        }
        if (host.sendToIdentified(p, weather::message(r.want, r.instant ? 0 : r.fade))) {
            r.has = r.want;
            r.instant = false;
            r.sentAny = true;
        }
    }
}

void Weather::reset(server::GameHost* host)
{
    if (host) {
        for (const auto& [p, r] : peers_) {
            if (r.sentAny && reaches(*host, p)) {
                host->sendToIdentified(p, weather::message({}, 0));
            }
        }
    }
    *this = Weather{};
}

namespace {

/// A number field of the table at `t` (absent: `fallback`); an argument error unless a number in
/// [lo, hi].
double numberField(lua_State* L, int t, const char* key, double fallback, double lo, double hi, const char* what)
{
    lua_rawgetfield(L, t, key);
    double v = fallback;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) {
            luaL_error(L, "nox.weather: %s", what);
        }
        v = lua_tonumber(L, -1);
        if (!(v >= lo && v <= hi)) { // (NaN too)
            luaL_error(L, "nox.weather: %s", what);
        }
    }
    lua_pop(L, 1);
    return v;
}

int noxWeather(lua_State* L)
{
    // nox.weather(peer | nil, {kind = "rain" | "snow" | "storm" | "none", strength = 0..1,
    // wind = {angle = degrees, speed = 0..1}, fade = frames}): nothing back; bad arguments an
    // error.
    std::optional<std::uint8_t> peer;
    if (!lua_isnoneornil(L, 1)) {
        const double p = lua_type(L, 1) == LUA_TNUMBER ? lua_tonumber(L, 1) : -1;
        luaL_argcheck(L, p >= 0 && p <= 255 && p == std::floor(p), 1, "a player's peer, or nil for everyone");
        peer = static_cast<std::uint8_t>(p);
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_rawgetfield(L, 2, "kind");
    const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
    const std::string_view k = name ? name : "";
    weather::Kind kind = weather::Kind::None;
    if (k == "rain") {
        kind = weather::Kind::Rain;
    } else if (k == "snow") {
        kind = weather::Kind::Snow;
    } else if (k == "storm") {
        kind = weather::Kind::Storm;
    } else if (k != "none") {
        luaL_error(L, "nox.weather: kind is \"rain\", \"snow\", \"storm\" or \"none\"");
    }
    lua_pop(L, 1);
    const double strength = numberField(L, 2, "strength", 1.0, 0.0, 1.0, "strength is a number 0-1");
    const double fade = numberField(L, 2, "fade", 0.0, 0.0, 65535.0, "fade is a whole number of frames, 0-65535");
    if (fade != std::floor(fade)) {
        luaL_error(L, "nox.weather: fade is a whole number of frames, 0-65535");
    }
    double angle = 0.0;
    double speed = 0.0;
    lua_rawgetfield(L, 2, "wind");
    if (lua_istable(L, -1)) {
        const int w = lua_gettop(L);
        angle = numberField(L, w, "angle", 0.0, -1e9, 1e9, "wind.angle is a number of degrees (clockwise on screen, 0 up)");
        speed = numberField(L, w, "speed", 0.0, 0.0, 1.0, "wind.speed is a number 0-1");
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "nox.weather: wind is a table {angle = degrees, speed = 0..1}");
    }
    lua_pop(L, 1);
    toolbox::partsOf(L).weather.set(toolbox::hostOf(L), peer, weather::make(kind, strength, angle, speed),
                                    static_cast<std::uint16_t>(fade));
    return 0;
}

} // namespace

void addWeatherToolbox(lua_State* L)
{
    lua_pushcfunction(L, noxWeather, "weather");
    lua_setfield(L, -2, "weather");
}

} // namespace nox::modes::script
