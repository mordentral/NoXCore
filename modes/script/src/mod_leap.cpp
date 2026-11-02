// nox.setBusy, nox.busy, nox.leap and what the mode keeps of them (mod_leap.hpp).
#include "mod_leap.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/server/abilities.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/play_from_here.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>

namespace nox::modes::script {

namespace {

namespace oc = things::ObjectClass;

/// The map's size (world units): a point a script gives is kept inside it.
constexpr double kMapExtent = 5888;
/// The most frames nox.setBusy takes (a minute).
constexpr int kMaxBusyFrames = 30 * 60;

/// Whether a body of `radius` fits at (x, y): no wall (nor closed door) between the point and its
/// edge, along the axes and the diagonals.
bool bodyFits(const server::World& world, float x, float y, float radius)
{
    const float d = radius * 0.70710678f;
    const std::pair<float, float> edges[8] = {{radius, 0}, {-radius, 0}, {0, radius}, {0, -radius}, {d, d}, {d, -d}, {-d, d}, {-d, -d}};
    for (const auto& [ex, ey] : edges) {
        if (!world.traceRay(x, y, x + ex, y + ey, server::Trace::Doors)) {
            return false;
        }
    }
    return true;
}

/// `unit`'s busy entry, made when it has none; null when kMaxModeBusy units are busy.
server::World::ModeBusy* busyEntry(server::World& world, const server::ServerObject& unit)
{
    if (const auto it = world.modeBusy.find(unit.extent); it != world.modeBusy.end()) {
        return &it->second;
    }
    if (world.modeBusy.size() >= server::World::kMaxModeBusy) {
        return nullptr;
    }
    return &world.modeBusy.emplace(server::ObjectRef{unit.extent}, server::World::ModeBusy{}).first->second;
}

/// Whether `peer`'s client is told a leap: in the game, not held out, ModeUi named, client level 8.
bool reaches(const server::GameHost& host, std::uint8_t peer)
{
    return !host.refused(peer) && (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 &&
           host.clientLevel(peer) >= uimsg::kLeapLevel;
}

} // namespace

void sweepBusy(server::World& world)
{
    std::erase_if(world.modeBusy, [&](const auto& kv) {
        const server::ServerObject* o = world.byExtent(kv.first);
        return !o || o->gone() || (o->flags & server::ObjFlag::Dead) || (world.frameNow >= kv.second.until && world.frameNow >= kv.second.stillUntil);
    });
}

namespace {

/// Whether `peer`'s client is told a spin: in the game, not held out, ModeUi named, kSpinLevel.
bool spinReaches(const server::GameHost& host, std::uint8_t peer)
{
    return !host.refused(peer) && (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 &&
           host.clientLevel(peer) >= uimsg::kSpinLevel;
}

} // namespace

void Spins::tellNew(server::GameHost& host, server::World& world, Spin& s)
{
    const std::uint32_t left = s.until > world.frameNow ? s.until - world.frameNow : 0;
    if (left == 0) {
        return;
    }
    std::optional<std::vector<std::uint8_t>> m;
    for (const auto& [peer, p] : host.players()) {
        const std::uint32_t bit = 1u << (p.slot & 31);
        if ((s.told & bit) || !spinReaches(host, peer) || !(p.extent == s.unit || p.knows.contains(s.unit))) {
            continue;
        }
        if (!m) {
            m = uimsg::spinMessage({static_cast<std::uint16_t>(s.unit.key()), s.rate16,
                                    static_cast<std::uint16_t>(std::min<std::uint32_t>(left, uimsg::kMaxSpinFrames))});
        }
        world.broadcasts().push_back({*m, true, p.extent});
        s.told |= bit;
    }
}

void Spins::set(server::GameHost* host, server::World& world, const server::ServerObject& o, std::int16_t rate16, int frames)
{
    std::erase_if(spins_, [&](const Spin& s) { return s.unit == o.extent; });
    if (!host) {
        return;
    }
    if (rate16 == 0 || frames == 0) {
        // Stopped: every client that takes spins and knows it told (one told nothing ignores it).
        const auto m = uimsg::spinMessage({static_cast<std::uint16_t>(o.extent), 0, 0});
        for (const auto& [peer, p] : host->players()) {
            if (spinReaches(*host, peer) && (p.extent == o.extent || p.knows.contains(o.extent))) {
                world.broadcasts().push_back({m, true, p.extent});
            }
        }
        return;
    }
    Spin s{server::ObjectRef{o.extent}, rate16, world.frameNow + static_cast<std::uint32_t>(frames), 0};
    tellNew(*host, world, s);
    if (spins_.size() < kMax) {
        spins_.push_back(s);
    }
}

void Spins::step(server::GameHost& host)
{
    server::World& world = host.server().world();
    std::erase_if(spins_, [&](const Spin& s) {
        const server::ServerObject* o = world.byExtent(s.unit);
        return world.frameNow >= s.until || !o || o->gone();
    });
    for (Spin& s : spins_) {
        tellNew(host, world, s);
    }
}

bool setBusy(server::World& world, const server::ServerObject& unit, int frames)
{
    sweepBusy(world);
    if (frames <= 0) {
        if (const auto it = world.modeBusy.find(unit.extent); it != world.modeBusy.end()) {
            it->second.until = 0;
        }
        sweepBusy(world);
        return true;
    }
    server::World::ModeBusy* b = busyEntry(world, unit);
    if (!b) {
        return false;
    }
    // (Busy from the next frame on: the world's frameNow is the one just run.)
    b->until = world.frameNow + 1 + static_cast<std::uint32_t>(frames);
    return true;
}

Leaps::Path Leaps::pathOf(const server::World& world, const server::ServerObject& unit, float x, float y)
{
    const float dx = x - unit.x;
    const float dy = y - unit.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    Path path{unit.x, unit.y, false};
    if (dist < 0.5f) {
        return path; // (a hop where it stands)
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / kTraceStep)));
    float lastX = unit.x, lastY = unit.y;
    bool stopped = false;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float px = unit.x + dx * t;
        const float py = unit.y + dy * t;
        if (!world.traceRay(lastX, lastY, px, py, server::Trace::Doors | server::Trace::ThroughWindows) || !server::standableSpot(world, px, py)) {
            stopped = true;
            break;
        }
        lastX = px;
        lastY = py;
        if (bodyFits(world, px, py, unit.radius)) {
            path.x = px;
            path.y = py;
        }
    }
    path.cut = stopped || path.x != x || path.y != y;
    return path;
}

std::optional<Leaps::Path> Leaps::start(server::World& world, server::GameHost* host, server::ServerObject& unit, float x, float y,
                                        int frames, int height)
{
    if ((unit.classes & (oc::Monster | oc::Player)) == 0 || unit.gone() || (unit.flags & server::ObjFlag::Dead) || unit.health == 0 ||
        unit.holder != 0 || leaps_.size() >= kMax) {
        return std::nullopt;
    }
    if (server::enchanted(unit, server::Enchant::Held) || server::enchanted(unit, server::Enchant::Freeze) ||
        std::ranges::any_of(leaps_, [&](const Leap& l) { return l.unit == unit.extent; })) {
        return std::nullopt;
    }
    if (unit.player && (unit.player->state == 0xC || unit.player->observing || server::abilityActive(world, unit, 1))) {
        return std::nullopt; // (jumping, watching, or in a Berserker Charge)
    }
    sweepBusy(world);
    server::World::ModeBusy* busy = busyEntry(world, unit);
    if (!busy) {
        return std::nullopt; // (kMaxModeBusy units busy)
    }
    const Path path = pathOf(world, unit, x, y);
    frames = std::clamp(frames, 1, uimsg::kMaxLeapFrames);
    // A walker stops where it is (its state's own push would carry it on); nothing of its own
    // motion goes with it.
    if (unit.player && (unit.player->state == 0 || unit.player->state == 5)) {
        server::setPlayerState(world, unit, 0xD, world.frameNow);
    }
    unit.vx = unit.vy = 0;
    unit.forceX = unit.forceY = 0;
    Leap l;
    l.unit = unit.extent;
    l.fromX = unit.x;
    l.fromY = unit.y;
    l.toX = path.x;
    l.toY = path.y;
    l.start = world.frameNow;
    l.frames = frames;
    l.cut = path.cut;
    // (Still till it's down: its landing ends it. Still, bodies pass it: World::passesBodies.)
    busy->stillUntil = world.frameNow + 2 + static_cast<std::uint32_t>(frames);
    leaps_.push_back(l);
    if (host) {
        const auto m = uimsg::leapMessage({static_cast<std::uint16_t>(unit.extent), static_cast<std::uint16_t>(frames),
                                           static_cast<std::uint8_t>(std::clamp(height, 0, 255))});
        for (const auto& [peer, p] : host->players()) {
            if (reaches(*host, peer) && (p.extent == unit.extent || p.knows.contains(unit.extent))) {
                world.broadcasts().push_back({m, true, p.extent});
            }
        }
    }
    return path;
}

void Leaps::end(server::World& world, server::ObjectKey unit)
{
    if (const auto it = world.modeBusy.find(unit); it != world.modeBusy.end()) {
        it->second.stillUntil = 0;
    }
    sweepBusy(world);
}

std::vector<Leaps::Landing> Leaps::step(server::World& world)
{
    std::vector<Landing> down;
    std::erase_if(leaps_, [&](const Leap& l) {
        server::ServerObject* o = world.byExtent(l.unit);
        if (!o || o->gone() || (o->flags & server::ObjFlag::Dead) || o->holder != 0) {
            end(world, l.unit);
            return true; // (gone or dead mid-leap: no landing)
        }
        const float t = std::min(1.0f, static_cast<float>(world.frameNow - l.start) / static_cast<float>(l.frames));
        const float x = l.fromX + (l.toX - l.fromX) * t;
        const float y = l.fromY + (l.toY - l.fromY) * t;
        // Put there as the frame's own move puts a thing (its place, its next and the frame's start
        // the same; no motion of its own; synced; relinked in the grid), not as a teleport.
        o->x = o->newX = o->frameStartX = x;
        o->y = o->newY = o->frameStartY = y;
        o->vx = o->vy = 0;
        o->forceX = o->forceY = 0;
        o->touch();
        world.gridRelink(*o, x, y);
        if (t < 1.0f) {
            return false;
        }
        end(world, l.unit);
        down.push_back({o->extent, x, y, l.cut});
        return true;
    });
    return down;
}

void Leaps::reset(server::World* world)
{
    if (world) {
        for (const Leap& l : leaps_) {
            end(*world, l.unit);
        }
    }
    leaps_.clear();
}

namespace {

float coordArg(lua_State* L, int idx)
{
    const double v = luaL_checknumber(L, idx);
    luaL_argcheck(L, v >= 0 && v <= kMapExtent, idx, "a point on the map (0 to 5888)");
    return static_cast<float>(v);
}

int noxSetBusy(lua_State* L)
{
    // nox.setBusy(unit, frames) -> whether it took: the unit kept from acting (a player's attacks,
    // jumps, spells and abilities refused, a creature's AI paused) for that many frames, till it
    // dies; 0 lets it act again (a leap's stillness stays till it's down). False: 256 units busy.
    toolbox::refuseInsideAsked(L, "nox.setBusy");
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, (o.classes & (oc::Monster | oc::Player)) != 0, 1, "a creature or player");
    const double frames = luaL_checknumber(L, 2);
    luaL_argcheck(L, frames >= 0 && frames <= kMaxBusyFrames && frames == std::floor(frames), 2, "whole frames 0-1800");
    lua_pushboolean(L, setBusy(toolbox::worldOf(L), o, static_cast<int>(frames)));
    return 1;
}

int noxBusy(lua_State* L)
{
    // nox.busy(unit): the frames it stays busy (or still, in a leap); 0: it's free.
    const server::ServerObject& o = toolbox::objectArg(L, 1);
    const server::World& world = toolbox::worldOf(L);
    const server::World::ModeBusy* b = world.busyOf(o);
    const std::uint32_t until = b ? std::max(b->until, b->stillUntil) : 0;
    lua_pushnumber(L, until > world.frameNow + 1 ? until - world.frameNow - 1 : 0);
    return 1;
}

int noxLeap(lua_State* L)
{
    // nox.leap(unit, x, y, { time = frames (20), height = units (40) }) -> landX, landY, cut: the
    // unit carried to (x, y) over `time` frames, over bodies, short of a wall; nil when it can't
    // (dead, held, frozen, jumping, charging, already leaping, not a creature or player).
    toolbox::refuseInsideAsked(L, "nox.leap");
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, (o.classes & (oc::Monster | oc::Player)) != 0, 1, "a creature or player");
    const float x = coordArg(L, 2);
    const float y = coordArg(L, 3);
    int frames = 20;
    int height = 40;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_rawgetfield(L, 4, "time");
        if (!lua_isnil(L, -1)) {
            const double v = luaL_checknumber(L, -1);
            luaL_argcheck(L, v >= 1 && v <= uimsg::kMaxLeapFrames && v == std::floor(v), 4, "time: whole frames 1-300");
            frames = static_cast<int>(v);
        }
        lua_pop(L, 1);
        lua_rawgetfield(L, 4, "height");
        if (!lua_isnil(L, -1)) {
            const double v = luaL_checknumber(L, -1);
            luaL_argcheck(L, v >= 0 && v <= 255, 4, "height: 0-255");
            height = static_cast<int>(v);
        }
        lua_pop(L, 1);
    }
    const auto path = toolbox::partsOf(L).leaps.start(toolbox::worldOf(L), toolbox::hostOf(L), o, x, y, frames, height);
    if (!path) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, path->x);
    lua_pushnumber(L, path->y);
    lua_pushboolean(L, path->cut);
    return 3;
}

int noxSetSpin(lua_State* L)
{
    // nox.setSpin(unit, turnsPerSecond, frames): NoX Core clients of client level 8 that named
    // ModeUi and know the thing (now, or later while it lasts) draw its body turning through its
    // facings that fast (negative: the other way) for that many frames; 0 (either) stops it.
    // Nothing changes on the server, nor for any other client.
    toolbox::refuseInsideAsked(L, "nox.setSpin");
    const server::ServerObject& o = toolbox::objectArg(L, 1);
    const double rate = luaL_checknumber(L, 2);
    luaL_argcheck(L, rate >= -uimsg::kMaxSpin && rate <= uimsg::kMaxSpin, 2, "turns a second, -8 to 8");
    const double frames = luaL_checknumber(L, 3);
    luaL_argcheck(L, frames >= 0 && frames <= uimsg::kMaxSpinFrames && frames == std::floor(frames), 3, "whole frames 0-1800");
    toolbox::partsOf(L).spins.set(toolbox::hostOf(L), toolbox::worldOf(L), o, static_cast<std::int16_t>(std::lround(rate * 16)),
                                  static_cast<int>(frames));
    return 0;
}

} // namespace

void addLeapToolbox(lua_State* L)
{
    lua_pushcfunction(L, noxSetSpin, "setSpin");
    lua_setfield(L, -2, "setSpin");
    lua_pushcfunction(L, noxSetBusy, "setBusy");
    lua_setfield(L, -2, "setBusy");
    lua_pushcfunction(L, noxBusy, "busy");
    lua_setfield(L, -2, "busy");
    lua_pushcfunction(L, noxLeap, "leap");
    lua_setfield(L, -2, "leap");
}

} // namespace nox::modes::script
