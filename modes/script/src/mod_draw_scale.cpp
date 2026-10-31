// nox.setDrawScale and what the server keeps of it (mod_draw_scale.hpp).
#include "mod_draw_scale.hpp"

#include "mod_parts.hpp"
#include "script_links.hpp"
#include "toolbox.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>

namespace nox::modes::script {

namespace {

/// Whether `peer`'s client knows the thing at `extent` (its record sent, not told it's out of
/// sight since).
bool knows(const server::GameHost& host, std::uint8_t peer, std::uint32_t extent)
{
    const auto it = host.players().find(peer);
    return it != host.players().end() && it->second.knows.contains(extent);
}

} // namespace

bool DrawScales::reaches(const server::GameHost& host, std::uint8_t peer)
{
    return host.players().contains(peer) && !host.refused(peer) &&
           (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 && host.clientLevel(peer) >= uimsg::kDrawScaleLevel;
}

void DrawScales::tell(server::GameHost& host, std::uint8_t peer, const server::ServerObject& o)
{
    if (!reaches(host, peer)) {
        return;
    }
    if (host.reliableRoom(peer) > 0 &&
        host.sendToIdentified(peer, uimsg::drawScaleMessage(static_cast<std::uint16_t>(o.extent), o.type, server::drawScaleOf(o)))) {
        return;
    }
    // (No room: owed, once, till its acknowledgements catch up.)
    const bool already = std::ranges::any_of(owed_, [&](const Owed& w) { return w.peer == peer && w.extent == o.extent; });
    if (!already && owed_.size() < kMaxOwed) {
        owed_.push_back({peer, server::ObjectRef{o.extent}, o.type});
    }
}

void DrawScales::set(server::GameHost* host, server::ServerObject& o, float scale)
{
    used_ = true;
    const float was = server::drawScaleOf(o);
    server::setDrawScale(o, scale);
    if (!host || was == server::drawScaleOf(o)) {
        return;
    }
    for (const auto& [peer, p] : host->players()) {
        if (p.knows.contains(o.extent)) {
            tell(*host, peer, o);
        }
    }
}

void DrawScales::seen(server::GameHost& host, std::uint8_t peer, const server::ServerObject& o)
{
    tell(host, peer, o);
}

void DrawScales::flush(server::GameHost& host)
{
    server::World& world = host.server().world();
    std::deque<Owed> still;
    for (const Owed& w : owed_) {
        const server::ServerObject* o = world.byExtent(w.extent);
        if (!o || o->type != w.type || !reaches(host, w.peer) || !knows(host, w.peer, w.extent)) {
            continue; // (gone, another thing, or out of its sight: it's told again when it comes back)
        }
        if (host.reliableRoom(w.peer) == 0 ||
            !host.sendToIdentified(w.peer, uimsg::drawScaleMessage(static_cast<std::uint16_t>(o->extent), o->type, server::drawScaleOf(*o)))) {
            still.push_back(w);
        }
    }
    owed_ = std::move(still);
}

void DrawScales::left(std::uint8_t peer)
{
    std::erase_if(owed_, [&](const Owed& w) { return w.peer == peer; });
}

namespace {

int noxSetDrawScale(lua_State* L)
{
    // nox.setDrawScale(id, scale): the thing drawn `scale` x its type's (0.5-3) by NoX Core clients
    // of client level 5, its sprite about its middle and, a missile's, its burst; nil or 1: as
    // its type's. Nothing changes on the server; stock and older clients draw it as before.
    toolbox::refuseInsideAsked(L, "nox.setDrawScale"); // (not inside onDamage or onExit)
    server::ServerObject& o = toolbox::objectArg(L, 1);
    namespace oc = things::ObjectClass;
    luaL_argcheck(L, (o.classes & (oc::Simple | oc::Complex)) != 0 && (o.classes & (oc::Immobile | oc::ClientPredict)) == 0, 1,
                  "a thing the clients are sent (not one they draw from the map, nor a hidden kind)");
    float scale = 1.0f;
    if (!lua_isnoneornil(L, 2)) {
        const double v = luaL_checknumber(L, 2);
        luaL_argcheck(L, v >= uimsg::kMinDrawScale && v <= uimsg::kMaxDrawScale, 2, "a scale from 0.5 to 3");
        // (In the wire's 1/256 steps, so the server's and the clients' scale are the same number.)
        scale = static_cast<float>(static_cast<int>(v * uimsg::kDrawScaleUnit + 0.5)) / uimsg::kDrawScaleUnit;
    }
    toolbox::partsOf(L).drawScales.set(toolbox::hostOf(L), o, scale);
    return 0;
}

} // namespace

void addDrawScaleToolbox(lua_State* L)
{
    lua_pushcfunction(L, noxSetDrawScale, "setDrawScale");
    lua_setfield(L, -2, "setDrawScale");
}

} // namespace nox::modes::script
