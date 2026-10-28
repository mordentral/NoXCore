// The solo game's pause, timed pause, level-up moment and save requests, and the solo team set
// up for a campaign map. solo.hpp describes each.
#include "noxcore/server/solo.hpp"

#include "noxcore/server/host_client.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/things/object_class.hpp"

namespace nox::server {

namespace {

/// The host's client's book flying an award to its bars.
bool bookFlying(const World& world)
{
    return world.hostClient && world.hostClient->bookFlying();
}

} // namespace

void setPaused(World& world, bool paused, PauseReason why)
{
    if (!(world.gameFlags & GameFlag::Solo)) {
        return;
    }
    if (world.modeHooks && !(paused ? world.modeHooks->mayPause(why) : world.modeHooks->mayUnpause(why))) {
        return; // (noxcpp: a mode that refuses this pause, or holds the one there is)
    }
    if (paused) {
        world.gameFlags |= GameFlag::Paused;
    } else if (world.pauseUntilTick == 0) {
        world.gameFlags &= ~GameFlag::Paused; // (and ticks_reset: the host's)
    }
}

void pauseFor(World& world, std::uint32_t ticks)
{
    if (ticks == 0) {
        world.pauseUntilTick = 0;
        return;
    }
    setPaused(world, true, PauseReason::Timed);
    world.pauseUntilTick = world.ticks + ticks;
}

void pauseTick(World& world)
{
    if (world.pauseUntilTick != 0 && world.pauseUntilTick <= world.ticks) {
        world.pauseUntilTick = 0;
        // (not while the host's client shows a briefing)
        if (!(world.hostClient && world.hostClient->briefingShown())) {
            setPaused(world, false, PauseReason::Timed);
        }
    }
}

bool hostMayOpenMenu(const World& world, std::uint32_t frame)
{
    // Asked by the host's client before its in-game menu.
    if (!(world.gameFlags & GameFlag::Solo)) {
        return true;
    }
    const ServerObject* host = hostPlayer(world); // (Plr_GetByIndex(0x1F)->PlayerUnit)
    // Not without a host player, with a save asked for or waiting, during a timed pause, or within
    // 30 frames of a saved game's load.
    if (!host || saveBusy(world) || world.pauseUntilTick != 0 || frame - world.savedGameLoadedAt <= 0x1Du) {
        return false;
    }
    if ((world.hostClient && world.hostClient->wideScreenOn()) || (host->flags & ObjFlag::Airborne)) {
        return false; // (guiCursor; ObjFlags 0x4000)
    }
    return mayAct(world, *host, frame);
}

void levelUpMoment(Fight& fight, ServerObject* unit, int kind)
{
    World& world = fight.world;
    if (world.levelUp.active || (world.gameFlags & GameFlag::Paused)) {
        return;
    }
    if (unit) {
        world.levelUp.unit = unit->extent;
    }
    ServerObject* who = world.levelUp.unit ? world.byExtent(world.levelUp.unit) : nullptr;
    world.levelUp.object = 0;
    if ((kind == 0 || kind == 1) && who && fight.things) {
        if (const ServerObject* o = world.createAt(*fight.things, kind == 0 ? "LevelUp" : "OblivionUp", who->x, who->y, fight.frame)) {
            world.levelUp.object = o->extent;
        }
    }
    if ((kind == 0 || kind == 1) && who) {
        pointFx(world, 0x9A, who->x, who->y);
    }
    if (kind == 0 && who) {
        playSound(world, 0x386, *who, 2, netCodeOf(*who));
        world.textTo(*who, "expLevel.c:LevelUP");
    }
    if (who && who->player) {
        // (the level-up state, its animation at 4)
        if (setPlayerState(world, *who, 0x1E, fight.frame)) {
            who->player->animFrame = 4;
        }
    }
    world.levelUp.timed = kind == 0 || kind == 1; // (its length 5000 ms; another kind 0)
    world.levelUp.kind = kind;
    world.levelUp.active = true;
    setPaused(world, true, kind == 0 || kind == 1 ? PauseReason::LevelUp : PauseReason::BookFlight);
    world.levelUp.startTick = world.ticks;
}

void levelUpTick(Fight& fight)
{
    World& world = fight.world;
    if (!world.levelUp.active || !world.levelUp.timed) {
        return;
    }
    // More than 5000 ms since it began.
    if ((world.ticks - world.levelUp.startTick) * 1000u / 30u <= 5000u) {  // ticks to ms, 30 ticks a second
        return;
    }
    levelUpEnd(fight);
}

void levelUpEnd(Fight& fight)
{
    World& world = fight.world;
    if (!world.levelUp.active) {
        return;
    }
    ServerObject* who = world.levelUp.unit ? world.byExtent(world.levelUp.unit) : nullptr;
    if (who && (world.levelUp.kind == 0 || world.levelUp.kind == 1)) {
        pointFx(world, 0x9A, who->x, who->y);
    }
    if (ServerObject* o = world.levelUp.object ? world.byExtent(world.levelUp.object) : nullptr) {
        o->pendingDelete = true;
    }
    world.levelUp.object = 0;
    if (who && who->player) {
        setPlayerState(world, *who, 0xD, fight.frame);
    }
    world.levelUp.unit = 0;
    if (!bookFlying(world)) {
        setPaused(world, false, world.levelUp.kind == 0 || world.levelUp.kind == 1 ? PauseReason::LevelUp : PauseReason::BookFlight);
    }
    world.levelUp.active = false;
}

void requestSave(World& world, bool pending, std::uint32_t exit, std::uint32_t delay, std::uint32_t frame)
{
    world.saveRequest.delay = delay;
    world.saveRequest.at = frame;
    world.saveRequest.exit = exit;
    world.saveByExit = exit != 0;
    world.saveRequest.pending = pending;
    // (none asked: the client's save window's flag set to 0 too)
}

bool saveBusy(const World& world)
{
    return world.saveRequest.pending || world.saveRequest.delay != 0;
}

bool mayAct(const World& world, const ServerObject& unit, std::uint32_t frame)
{
    if (world.saveRequest.delay != 0 && frame < world.saveRequest.at + world.saveRequest.delay) {
        return false;
    }
    if (unit.player && unit.player->dialogWith != 0) { // (DialogWith: talking to an NPC, dialog.hpp)
        return false;
    }
    if (unit.flags & (ObjFlag::NoUpdate | ObjFlag::Dead)) {
        return false;
    }
    return !bookFlying(world);
}

bool isUnit(const World& world, const ServerObject& o)
{
    return !world.network() && (o.classes & things::ObjectClass::Monster) && (o.subclasses & 0x100u);
}

bool playersPixie(World& world, const ServerObject& o)
{
    if (!(o.classes & things::ObjectClass::Missile) || !(world.gameFlags & GameFlag::Solo) || o.typeName != "Pixie") {
        return false;
    }
    // (findParentChainPlayer: up the owners to a player, or the last)
    const ServerObject* p = &o;
    // Up the owner chain (16 steps at most) to a player.
    for (int guard = 0; p && !(p->classes & things::ObjectClass::Player) && p->owner != 0 && guard < 16; ++guard) {
        p = world.byExtent(p->owner);
    }
    return p && (p->classes & things::ObjectClass::Player);
}

ServerObject* hostPlayer(World& world)
{
    for (ServerObject* o : world.playerUnits()) {
        if (!o->gone() && o->player->index == 0x1F) {
            return o;
        }
    }
    return nullptr;
}

const ServerObject* hostPlayer(const World& world)
{
    for (const ServerObject* o : world.playerUnits()) {
        if (!o->gone() && o->player->index == 0x1F) {
            return o;
        }
    }
    return nullptr;
}

void soloTeam(World& world, std::minstd_rand& rng, std::uint32_t frame)
{
    world.gameFlags |= GameFlag::Coop;
    ServerTeam* team = teamById(world, 1);
    if (!team) {
        team = createTeam(world, 1);
    }
    // (Team_GetObjectTeamByNetCode(player_netCode): the host's own player.)
    if (ServerObject* host = hostPlayer(world); host && team) {
        addTeamMember(world, team->id, *host, false, false, rng, frame);
        team = teamById(world, 1);
    }
    if (team) {
        setTeamName(*team, world.text("team.c:COOP"), false);
    }
    world.gameplayFlags &= ~1u; // (UnsetGameplayFlags)
}

} // namespace nox::server
