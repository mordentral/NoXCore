// Moving things at a stroke (teleport.hpp): setPosition and what it drags along (harpoons
// snap, pixies follow, the client forgets and is resent everything), the teleport checks,
// TeleportWake, Blink's random waypoint and who teleported whom.
#include "noxcore/server/teleport.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

} // namespace

void teleportPixie(World& world, ServerObject& pixie, const ServerObject& to)
{
    // teleportPixie: NewPos, PosVec and PrevPos where `to` is, relinked
    // (moveUpdateSpecial).
    pixie.x = pixie.newX = pixie.frameStartX = to.x;
    pixie.y = pixie.newY = pixie.frameStartY = to.y;
    pixie.lastX = to.x;
    pixie.lastY = to.y;
    world.gridRelink(pixie, pixie.x, pixie.y);
}

void setPosition(World& world, ServerObject& o, float x, float y, std::uint32_t frame)
{
    if (o.classes & oc::Immobile) {
        return;
    }
    // Only a change of whole-number spot needs syncing to the clients.
    if (static_cast<int>(o.x) != static_cast<int>(x) || static_cast<int>(o.y) != static_cast<int>(y)) {
        o.touch();
    }
    o.x = o.newX = o.frameStartX = x;
    o.y = o.newY = o.frameStartY = y;
    o.lastX = x; // (PrevPos too)
    o.lastY = y;
    world.gridRelink(o, x, y); // (moveUpdateSpecial)
    if (o.collides) {
        queueMotion(world, o); // (unitHasCollideOrUpdateFn: it meets what's there)
    }
    // (One walk of the missiles' list for what follows: a harpoon bolt flying, its own pixies.
    // Those made or carried in since the last new things' pass aren't on it yet, but are on the
    // owner's owned list in the exe at once (createAt): the new list first, newest first, then
    // the missiles' list, newest first.)
    bool bolt = false;
    std::vector<ServerObject*> pixies; // (allocates only when it has one)
    const auto look = [&](ServerObject& m) {
        bolt = bolt || m.updateFn == NameId::HarpoonUpdate;
        if (m.owner == o.extent && m.typeName == NameId::Pixie && !(m.flags & ObjFlag::Dead) && m.seekTarget == 0) {
            pixies.push_back(&m);
        }
    };
    const auto& fresh = world.newList();
    for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
        if (ServerObject* m = world.byExtent(*it); m && (m->classes & oc::Missile) && !m->grid.get()) {
            look(*m);
        }
    }
    world.grid().eachMissile(look);
    if (o.classes & (oc::Monster | oc::Player)) { // (each player's harpoon in it snaps)
        // (A harpoon holds only while its bolt flies: with none on the missiles' list, no player's
        // needs looking at.)
        if (bolt) {
            for (auto& [extent, w] : world.objects()) {
                if (w.player && w.player->harpoonTarget == o.extent) {
                    releaseHarpoon(world, w, true);
                }
            }
        }
    }
    if (o.monster) {
        // (a monster's path (its points' count) and waypoint path (
        // their count) dropped)
        o.monster->pathCount = 0;
        o.monster->waypointCount = 0;
    }
    // Its pixies follow it (teleportAllPixies): of its owned list (newest first:
    // unitSetOwner puts each at its head), each Pixie not DEAD without a target
    // put where it is (teleportPixie).
    std::ranges::sort(pixies, [](const ServerObject* a, const ServerObject* b) {
        return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
    });
    for (ServerObject* p : pixies) {
        teleportPixie(world, *p, o);
    }
    // (frameCounterSetCopyToNextFrame: a creature or player put somewhere, every
    // monster looks about afresh next frame)
    if (o.classes & (oc::Monster | oc::Player)) {
        world.monstersLookAt = frame + 1;
    }
    // A networked player's client is told to forget everything it knows (0xEA, then the frame
    // as 4 little-endian bytes); the network side then resends every object to it.
    if (o.player && world.network()) {
        world.forgetting().push_back(o.extent);
        world.broadcasts().push_back({{0xEA, static_cast<std::uint8_t>(frame), static_cast<std::uint8_t>(frame >> 8),
                                       static_cast<std::uint8_t>(frame >> 16), static_cast<std::uint8_t>(frame >> 24)},
                                      true,
                                      o.extent});
    }
    // A player's camera goes with it, unless it follows something or observes.
    if (o.player && o.player->cameraFollow == 0 && !(o.player->statusFlags & 3u)) {
        o.player->camX = x;
        o.player->camY = y;
    }
}

// Teleports `o` to (x, y) unless something keeps it in place (see teleport.hpp).
bool teleport(World& world, ServerObject& o, float x, float y, std::uint32_t frame)
{
    // ANCHORED, or object flag 2, stays put.
    if (enchanted(o, Enchant::Anchored) || (o.flags & 2u)) {
        return false;
    }
    // A quest's subclass-8 monsters can't be teleported.
    if (world.quest() && (o.classes & oc::Monster) && (o.subclasses & 8u)) {
        return false;
    }
    // Out of a solo game only creatures (monsters and players) teleport.
    if (!(world.gameFlags & GameFlag::Solo) && !(o.classes & (oc::Monster | oc::Player))) {
        return false;
    }
    setPosition(world, o, x, y, frame);
    return true;
}

// Leaves a TeleportWake where `unit` stood for a second; things that walk into it are sent
// on to (toX, toY) by its collide.
void teleportWake(World& world, const things::ThingDb& things, const ServerObject& unit, float toX, float toY, std::uint32_t frame)
{
    if (!things.findThing("TeleportWake")) {
        return;
    }
    ServerObject& wake = world.spawn(things, "TeleportWake", unit.x, unit.y);
    wake.owner = unit.extent;
    wake.born = frame;
    wake.destX = toX;
    wake.destY = toY;
    wake.timer = frame + 30; // (a second)
    wake.flags |= ObjFlag::Active;
}

bool randomWaypoint(World& world, std::minstd_rand& rng, const ServerObject* unit, float& x, float& y)
{
    // In CTF, the first flag on the map not of its team (servCompareTeams): a
    // waypoint in clear line (doors, near windows: 9) of that flag's home is passed over, after
    // the pick is made (so none may come of it).
    const ServerObject* enemyFlag = nullptr;
    if ((world.gameFlags & GameFlag::CaptureTheFlag) && unit) {
        for (const auto& [extent, o] : world.objects()) {
            const bool sameTeam = unit->team != 0 && o.team != 0 && unit->team == o.team;
            if ((o.classes & oc::Flag) && o.flag && !o.gone() && o.holder == 0 && !sameTeam) {
                enemyFlag = &o;
                break;
            }
        }
    }
    // Count the eligible waypoints, pick one by index, then walk them again to find it.
    int count = 0;
    for (const ServerWaypoint& wp : world.waypoints()) {
        if ((wp.linkFlags & 0x80u) && wp.enabled) {
            ++count;
        }
    }
    if (count == 0) {
        return false;
    }
    int pick = nox::util::uniform_int_distribution<int>(0, count - 1)(rng);
    for (const ServerWaypoint& wp : world.waypoints()) {
        if (!((wp.linkFlags & 0x80u) && wp.enabled)) {
            continue;
        }
        // The CTF pass-over comes after the pick: a passed-over waypoint doesn't count the pick
        // down, so a pick near the end may find nothing (kept as the exe does it).
        if (enemyFlag && world.traceRay(enemyFlag->flag->homeX, enemyFlag->flag->homeY, wp.x, wp.y, Trace::Doors | Trace::NearWindows)) {
            continue;
        }
        if (pick == 0) {
            x = wp.x;
            y = wp.y;
            return true;
        }
        --pick;
    }
    return false;
}

// Remembers on a teleported player which other player teleported it, and when.
void teleportedBy(const ServerObject* caster, ServerObject* target, std::uint32_t frame)
{
    if (caster && target && caster != target && caster->player && target->player) {
        target->player->teleportedBy = caster->extent;
        target->player->teleportedAt = frame;
    }
}

} // namespace nox::server
