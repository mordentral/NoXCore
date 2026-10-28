// Moving things at a stroke: the teleport spells', the wakes', and anything else's.
//
//   - setPosition (Obj_SetPosition): not the IMMOBILE; its position, next and last all
//     there (synced if its whole-number spot changed); a harpoon in it snaps; a
//     monster's paths dropped; its pixies with it (teleportAllPixies); a creature's or
//     player's move makes every monster look about next frame (frameCounterSetCopyToNextFrame); a networked player's
//     client forgets what it knew (MSG_FORGET_DRAWABLES 0xEA and every object resent,
//     World::forgetting); its camera with it.
//   - teleport (teleportToMB): not the ANCHORED or flag 2 ones, not a quest's
//     subclass-8 monster; only creatures out of a solo game.
//   - wake (spellTeleportCreateWake): a TeleportWake left where it went from for
//     a second; what walks into it follows (TeleportWakeCollide: creatures, food, staves,
//     weapons and armour, not the ANCHORED; in a quest, only a player's).
//   - randomWaypoint (waypoint): Blink's: an enabled waypoint whose links carry
//     flag 0x80, picked at random (in CTF one in sight of its enemies' flag home is passed over
//     and nothing chosen, the pick then failing).
//   - teleportedBy: a player who teleports another is remembered on it.
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <cstdint>
#include <random>

namespace nox::server {

/// Puts `o` at (x, y) at once, with everything that comes with it (the list above).
void setPosition(World& world, ServerObject& o, float x, float y, std::uint32_t frame);
/// teleportPixie: a pixie put where `to` is (its PrevPos too), relinked.
void teleportPixie(World& world, ServerObject& pixie, const ServerObject& to);
/// Whether it went.
bool teleport(World& world, ServerObject& o, float x, float y, std::uint32_t frame);
/// A TeleportWake at `unit` for a second, sending what walks in on to (toX, toY).
void teleportWake(World& world, const things::ThingDb& things, const ServerObject& unit, float toX, float toY, std::uint32_t frame);
/// Blink's destination into (x, y); false when none was picked.
bool randomWaypoint(World& world, std::minstd_rand& rng, const ServerObject* unit, float& x, float& y);
/// Notes on a teleported player which player teleported it, and the frame.
void teleportedBy(const ServerObject* caster, ServerObject* target, std::uint32_t frame);

} // namespace nox::server
