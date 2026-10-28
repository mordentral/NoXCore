// What the motion's touches do (collide, after the motion: each touch's COLLIDE
// functions, both ways, and the touch effects).
//
//   - For a touch of a with b (physics.hpp's Contact, newest first): a's COLLIDE (b, the
//     normal) and, b an object, the touch effects (collide) a on b; a harming floor tile (6):
//     a takes 2 of fire (type 0xC); then b's COLLIDE (a, the normal reversed) and b on a.
//   - The touch effects (collide): a SHOCK on one discharges into an enemy
//     creature it touches (ShockDamage at its power, type 9); invisibility ends touching a
//     creature not of its side (and a player touching class 0x20000).
//   - PlayerCollide: a charging warrior (Berserker Charge) stops against anything
//     solid: an object (but a creature that's dead, or a lighter thing that isn't immobile; an
//     unlocked door; a BELOW / ALLOW_OVERLAP thing; a missile), or a wall that's solid (the
//     WALL's flags 1 / 4): idle, the ground shakes (10), the charge ends. A wall, or a thing
//     not a creature, stuns it (HELD, BerserkerStunDuration) and hurts it
//     (BerserkerPainRatio of its health); a thing takes BerserkerDamage (type 2) and, unless
//     immobile, the two velocities trade as an elastic knock. It's put back where
//     it was. Then a DEATH with less than 14 s left on it passes on to a living player
//     touched (15 s).
//   - PickupCollide: something, not a monster, touching an item half a second old
//     picks it up (a player only while its pick-up flag, bit 1, is set: not yet, so
//     players don't).
// The other COLLIDE functions (doors, triggers, chests, exits, traps, glyphs, teleport
// wakes, the monsters', and the modes' flags and balls) are in collide.cpp, by name.
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/physics.hpp"

#include <vector>

namespace nox::server {

/// The frame's touches dispatched (collide).
void dispatchContacts(Fight& fight, const std::vector<Contact>& contacts);
/// `obj`'s own COLLIDE function with `other` (null: a wall), alone (a missile's path meeting
/// it, calls the body's after the missile's).
void collideFunction(Fight& fight, ServerObject& obj, ServerObject* other);

} // namespace nox::server
