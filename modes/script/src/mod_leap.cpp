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
#include "noxcore/things/draw.hpp"
#include "noxcore/things/object_class.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>

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

namespace {

/// Whether `peer`'s client is told an animation: in the game, not held out, ModeUi named, kAnimLevel.
bool animReaches(const server::GameHost& host, std::uint8_t peer)
{
    return !host.refused(peer) && (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 &&
           host.clientLevel(peer) >= uimsg::kAnimLevel;
}

} // namespace

void Anims::tellNew(server::GameHost& host, server::World& world, Anim& a)
{
    const std::uint32_t gone = world.frameNow - a.start;
    if (gone >= a.frames) {
        return;
    }
    std::optional<std::vector<std::uint8_t>> m;
    for (const auto& [peer, p] : host.players()) {
        const std::uint32_t bit = 1u << (p.slot & 31);
        if ((a.told & bit) || !animReaches(host, peer) || !(p.extent == a.unit || p.knows.contains(a.unit))) {
            continue;
        }
        if (!m) {
            m = uimsg::animMessage({static_cast<std::uint16_t>(a.unit.key()), static_cast<uimsg::AnimKind>(a.kind), a.state,
                                    static_cast<std::uint16_t>(a.frames), static_cast<std::uint16_t>(gone), a.weapon, a.hold});
        }
        world.broadcasts().push_back({*m, true, p.extent});
        a.told |= bit;
    }
}

void Anims::tellStop(server::GameHost& host, server::World& world, server::ObjectKey object)
{
    // (Every client that takes them and knows it: one told nothing ignores it.)
    const auto m = uimsg::animMessage({static_cast<std::uint16_t>(object), uimsg::AnimKind::Player, 0, 0, 0, uimsg::kNoWeaponLayer, uimsg::kNoHold});
    for (const auto& [peer, p] : host.players()) {
        if (animReaches(host, peer) && (p.extent == object || p.knows.contains(object))) {
            world.broadcasts().push_back({m, true, p.extent});
        }
    }
}

void Anims::set(server::GameHost* host, server::World& world, const server::ServerObject& o, std::uint8_t kind, std::uint8_t state,
                int frames, std::uint8_t weapon, std::uint8_t hold)
{
    std::erase_if(anims_, [&](const Anim& a) { return a.unit == o.extent; });
    if (!host) {
        return;
    }
    if (frames == 0) {
        tellStop(*host, world, o.extent);
        return;
    }
    Anim a{server::ObjectRef{o.extent}, kind, state, weapon, hold, world.frameNow, static_cast<std::uint32_t>(frames), 0};
    tellNew(*host, world, a);
    if (anims_.size() < kMax) {
        anims_.push_back(a);
    }
}

void Anims::step(server::GameHost& host)
{
    server::World& world = host.server().world();
    std::erase_if(anims_, [&](const Anim& a) {
        const server::ServerObject* o = world.byExtent(a.unit);
        if (!o || o->gone() || world.frameNow - a.start >= a.frames) {
            return true;
        }
        if (o->flags & server::ObjFlag::Dead) {
            tellStop(host, world, a.unit); // (its death drawn, not the animation; nor after, if it's raised)
            return true;
        }
        return false;
    });
    for (Anim& a : anims_) {
        tellNew(host, world, a);
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

/// `name`'s index in `table`, whatever its letters' case; none: not there.
template <std::size_t N>
std::optional<std::uint8_t> nameIndex(const std::array<std::string_view, N>& table, std::string_view name, std::size_t from = 0)
{
    for (std::size_t i = from; i < N; ++i) {
        const std::string_view t = table[i];
        if (t.size() == name.size() && std::ranges::equal(t, name, [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
            })) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

/// A player's strike with what it holds (its weapon class bits): the player animation whose
/// layers draw that weapon swung (kPlayerStates' index); bare hands, PUNCH_RIGHT.
std::uint8_t heldStrike(std::uint32_t weaponEquip)
{
    struct Strike {
        std::uint32_t bits;
        std::uint8_t state;
    };
    static constexpr Strike kStrikes[] = {
        {0x7FF8000u, 29}, // the staffs (the Oblivion ones too): STAFF_STRIKE
        {0x4u, 33},       // BOW: SHOOT_BOW
        {0x8u, 34},       // CROSSBOW: SHOOT_CROSSBOW
        {0xC0u, 44},      // CHAKRAM, SHURIKEN: CHAKRAM_STRIKE (no shuriken drawn in it)
        {0x200u, 28},     // LONG_SWORD: LONG_SWORD_STRIKE
        {0x100u, 27},     // SWORD: SWORD_STRIKE
        {0x400u, 37},     // GREAT_SWORD: GREAT_SWORD_STRIKE
        {0x4000u, 39},    // HAMMER: HAMMER_STRIKE
        {0x800u, 26},     // MACE: MACE_STRIKE
        {0x1000u, 35},    // AXE: AXE_STRIKE
        {0x2000u, 39},    // OGRE_AXE: HAMMER_STRIKE (AXE_STRIKE has no layer of it)
    };
    for (const Strike& s : kStrikes) {
        if (weaponEquip & s.bits) {
            return s.state;
        }
    }
    return 24;
}

int noxPlayAnim(lua_State* L)
{
    // nox.playAnim(unit, name, frames, { weapon = layer, hold = frame }): NoX Core clients of client
    // level 9 that named ModeUi and know the thing (now, or later while it lasts) draw its body in
    // its own animation `name` (a player's: "HAMMER_STRIKE", "WARCRY"..., or "ATTACK", the strike
    // with what it holds now; a creature's: "ATTACK", "CAST_SPELL"...), its frames spread over
    // `frames`, then as the server has it again; 0 stops it. `hold` ("first", "last" or a frame
    // number from 1) holds that one frame the while instead. A player's `weapon` (a weapon layer:
    // "HAMMER", "OGRE_AXE"...) is drawn in place of what it holds the while. Every layer follows
    // the body's frame and facing (a spin's too). Nothing changes on the server, nor for any other
    // client; a dead unit's is none (one that dies the while stops).
    toolbox::refuseInsideAsked(L, "nox.playAnim");
    const server::ServerObject& o = toolbox::objectArg(L, 1);
    const bool player = (o.classes & oc::Player) != 0;
    luaL_argcheck(L, player || (o.classes & oc::Monster) != 0, 1, "a creature or player");
    std::size_t len = 0;
    const char* s = luaL_checklstring(L, 2, &len);
    const std::string_view name(s, len);
    std::optional<std::uint8_t> state;
    if (player && nameIndex(std::array<std::string_view, 1>{"ATTACK"}, name)) {
        state = heldStrike(o.player ? o.player->weaponEquip : 0);
    } else {
        state = player ? nameIndex(things::kPlayerStates, name) : nameIndex(things::kMonsterStates, name);
    }
    if (!state) {
        luaL_argerror(L, 2, player ? "a player's animation (\"ATTACK\", \"HAMMER_STRIKE\", \"WARCRY\"...)"
                                   : "a creature's animation (\"ATTACK\", \"CAST_SPELL\"...)");
    }
    const double frames = luaL_checknumber(L, 3);
    luaL_argcheck(L, frames >= 0 && frames <= uimsg::kMaxAnimFrames && frames == std::floor(frames), 3, "whole frames 0-1800");
    std::uint8_t weapon = uimsg::kNoWeaponLayer;
    std::uint8_t hold = uimsg::kNoHold;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_rawgetfield(L, 4, "weapon");
        if (!lua_isnil(L, -1)) {
            luaL_argcheck(L, player, 4, "weapon: a player's only");
            std::size_t wlen = 0;
            const char* w = luaL_checklstring(L, -1, &wlen);
            const auto layer = nameIndex(things::kWeaponParts, std::string_view(w, wlen), 2); // (not the flag, nor the quiver)
            luaL_argcheck(L, layer.has_value(), 4, "weapon: a weapon layer (\"HAMMER\", \"SWORD\"...)");
            weapon = *layer;
        }
        lua_pop(L, 1);
        lua_rawgetfield(L, 4, "hold");
        if (lua_isstring(L, -1) && !lua_isnumber(L, -1)) {
            const std::string_view h = lua_tostring(L, -1);
            luaL_argcheck(L, h == "first" || h == "last", 4, "hold: \"first\", \"last\" or a frame number 1-254");
            hold = h == "first" ? 0 : uimsg::kHoldLast;
        } else if (!lua_isnil(L, -1)) {
            const double v = luaL_checknumber(L, -1);
            luaL_argcheck(L, v >= 1 && v <= 254 && v == std::floor(v), 4, "hold: \"first\", \"last\" or a frame number 1-254");
            hold = static_cast<std::uint8_t>(v - 1);
        }
        lua_pop(L, 1);
    }
    if (frames > 0 && (o.gone() || (o.flags & server::ObjFlag::Dead))) {
        return 0;
    }
    toolbox::partsOf(L).anims.set(toolbox::hostOf(L), toolbox::worldOf(L), o,
                                  static_cast<std::uint8_t>(player ? uimsg::AnimKind::Player : uimsg::AnimKind::Creature), *state,
                                  static_cast<int>(frames), weapon, hold);
    return 0;
}

} // namespace

void addLeapToolbox(lua_State* L)
{
    lua_pushcfunction(L, noxPlayAnim, "playAnim");
    lua_setfield(L, -2, "playAnim");
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
