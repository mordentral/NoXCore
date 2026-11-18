// nox.observe and nox.camera, and the ends of the mode's hold (mod_camera.hpp).
#include "mod_camera.hpp"

#include "toolbox.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/world.hpp"

#include "lua.h"
#include "lualib.h"

namespace nox::modes::script {

namespace {

/// The map's size (world units): a camera a script places is kept on it.
constexpr double kMapExtent = 5888;

float coordArg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 0 && v <= kMapExtent, idx, "a point on the map (0 to 5888)");
    return static_cast<float>(v);
}

bool watching(const server::ServerObject& u)
{
    return u.player && (u.player->statusFlags & 1u) != 0;
}

int noxObserve(lua_State* L)
{
    // nox.observe(unit, on): on, the player out of play watching (where it stands, unseen, out of
    // reach), held out (its Action never brings it in) and its camera the mode's (nox.camera),
    // held where it is till then (on its unit, when it wasn't watching; no "observation mode"
    // told); off, back into play as nox.backIntoPlay brings it, the hold let go.
    toolbox::refuseInsideAsked(L, "nox.observe");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1); // (never a held-out stock player's)
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    server::PlayerControl& p = *u.player;
    server::Fight f = toolbox::fightOf(L);
    if (!lua_toboolean(L, 2)) {
        luaL_argcheck(L, watching(u) && !(u.flags & server::ObjFlag::Dead), 1, "not a player watching");
        bringIntoPlay(f, u);
        return 0;
    }
    luaL_argcheck(L, watching(u) || !(u.flags & server::ObjFlag::Dead), 1, "a player alive or watching");
    p.modCamera = true; // (first: goObserver then tells nothing and leaves the camera be)
    if (!watching(u)) {
        p.camX = u.x;
        p.camY = u.y;
        p.cameraFollow = 0;
        server::goObserver(f, u, false, true);
    }
    p.heldOut = true;
    p.modHeld = true; // (the mode's hold, not the host's: let go as it leaves watching by any path)
    return 0;
}

int noxCamera(lua_State* L)
{
    // nox.camera(unit, x, y): a watching player's camera at (x, y), held there (its own controls
    // no longer move it); nox.camera(unit, thing): held on the thing, following it while it's in
    // the world (where it was last, once it's gone); nox.camera(unit, nil): let go, the stock
    // observer's controls again (a held-out player stays out).
    toolbox::refuseInsideAsked(L, "nox.camera");
    server::ServerObject& u = toolbox::playerUnitArg(L, 1);
    server::PlayerControl& p = *u.player;
    if (lua_isnoneornil(L, 2)) {
        luaL_argcheck(L, lua_isnoneornil(L, 3), 3, "nothing after nil");
        p.modCamera = false;
        return 0;
    }
    luaL_argcheck(L, watching(u), 1, "a player watching (nox.observe)");
    if (lua_isnoneornil(L, 3)) {
        const server::ServerObject& t = toolbox::objectArg(L, 2);
        luaL_argcheck(L, !toolbox::hidden(L, t), 2, "no such thing (gone?)");
        p.modCamera = true;
        p.cameraFollow = t.extent;
        p.camX = t.x;
        p.camY = t.y;
        return 0;
    }
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    p.modCamera = true;
    p.cameraFollow = 0;
    p.camX = x;
    p.camY = y;
    return 0;
}

} // namespace

void bringIntoPlay(server::Fight& fight, server::ServerObject& unit)
{
    server::joinTeam(fight, unit);
    server::leaveObserver(fight, unit); // (the mode's hold and its camera's with it: modHeld, modCamera)
    unit.player->cameraFollow = 0;
}

void cameraReady(server::GameHost& host, std::uint8_t peer)
{
    server::ServerObject* u = host.refused(peer) ? nullptr : host.unitOf(peer);
    if (!u || !u->player || !u->player->modHeld || !watching(*u)) {
        return;
    }
    if (!server::enchanted(*u, server::Enchant::Invisible)) {
        server::enchant(host.server().world(), *u, server::Enchant::Invisible, 0, 5);
    }
    u->flags |= server::ObjFlag::NoCollide;
}

void releaseCameras(server::GameHost& host)
{
    for (server::ServerObject* u : host.server().world().playerUnits()) {
        if (!u->player) {
            continue;
        }
        if (u->player->modHeld) { // (the mode's hold only: a held-out stock player's is the host's)
            u->player->heldOut = false;
            u->player->modHeld = false;
        }
        u->player->modCamera = false;
    }
}

void addCameraToolbox(lua_State* L)
{
    lua_pushcfunction(L, noxObserve, "observe");
    lua_setfield(L, -2, "observe");
    lua_pushcfunction(L, noxCamera, "camera");
    lua_setfield(L, -2, "camera");
}

} // namespace nox::modes::script
