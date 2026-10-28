// Who sees whom, for the spells (and later the monsters).
//
//   - canSee (unitCanSee): not when blind; an invisible one (unless `flags` 1) only
//     with INFRAVISION (in a quest: only Hecubah and the Necromancer), a pixie by its owner's,
//     or while it moves faster than 6 a frame either way; a monster that hears no sneakers
//     (subclass 0x2000) doesn't see a warrior treading lightly.
//   - bodiesClear (mapCheck): nothing that casts a shadow (flag 0x10000) on the
//     line between them, a circle's within its radius of it, a box's edges or a door's leaf
//     across it; then the ray, flags 9.
//   - canInteract (unitCanInteractWith): both.
//   - searchTarget (spellFlySearchTarget): a hostile spell's (flag 0x20) caster
//     with an enemy under its cursor gets that; else the nearest live creature within `range`
//     of the point (from the caster when the point's out of its sight, ray flags 5): not
//     observing, enemy for a hostile spell, `mode` 0 not the searcher itself, seen and clear
//     from the searcher and its player.
#pragma once

#include "noxcore/server/world.hpp"

#include <cstdint>

namespace nox::server {

/// A door's leaf from its hinge at one of its 32 directions (the table).
std::pair<int, int> doorLeaf(int direction);
/// The foot of (px, py) on the line through (x0, y0)-(x1, y1), and whether it lies within the
/// segment's box (mathPointOnTheLine).
bool pointOnLine(float x0, float y0, float x1, float y1, float px, float py, float& fx, float& fy);
/// Whether two segments cross.
bool segmentsCross(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1);

bool canSee(const World& world, const ServerObject& viewer, const ServerObject& seen, int flags = 0);
bool bodiesClear(const World& world, const ServerObject& from, const ServerObject& to);
bool canInteract(const World& world, const ServerObject& a, const ServerObject& b, int flags = 0);
ServerObject* searchTarget(World& world, const float* point, ServerObject* searcher, std::uint32_t spellFlags, float range, int mode,
                           ServerObject* cursorOwner);
/// Whether any of `a` and its owners shares a team with, or is, any of `b` and its owners
/// (unitsHaveSameTeam; servCompareTeams: both on the same team).
bool sameSide(const World& world, const ServerObject& a, const ServerObject& b);
/// The nearest creature within `range` of `from` as finds it with the spec a
/// glyph going off and a spell projectile's end give it: not of
/// its side, not observing, live, not itself, seen and reachable from it (canInteract).
ServerObject* nearestCreature(World& world, ServerObject& from, float range);

} // namespace nox::server
