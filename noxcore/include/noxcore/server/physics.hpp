// Motion as GAME.EXE's units update makes it (updateUnits), for the objects
// that can move (joinsMotion):
//   - the motion queue (queueMotion: unitHasCollideOrUpdateFn) holds
//     what moves this frame, in the order it joined; the steps run over it and then empty it.
//     It's joined by: being made (the tick's end, queueNewObjects), the units
//     update's test at each one's turn (settleOrQueue: a force, a velocity, a move under way, a
//     height or a rise beyond 0.01; else it comes to rest: zeroed, RESTING), being placed
//     (setPosition), switched on, pushed (applyForce and the like), landing, a door's spring,
//     an elevator's shaft (each frame), a secret wall moving over it, a mover's touch while at
//     rest (the steps), and, after the steps, the touches' pass (collide.hpp: a harming floor's
//     victim, the other of a pair with an ALLOW_OVERLAP member). Nothing at rest is in it by
//     having an UPDATE;
//   - each one's update puts its own force on it (a player's walking: player.hpp);
//   - five steps of 0.2: in each, every mover gathers its pushes from the
//     bodies whose box meets its own (circle against circle: collisionCheckCircleCircle, 40 x the
//     overlap, with a glancing pull for pairs not both creatures) and from the walls in its cells (a circle:
//     100 x the overlap, along the nearer wall diagonal, an end rounded only where no
//     wall carries on; inward velocity taken off), then its velocity takes
//     (push + force - drag x velocity) x 0.2 and it moves on by velocity x 0.2 unless the wall
//     ray (World::traceRay, Doors, ThroughWindows when airborne) says it can't;
//   - then each takes its new position, its force is cleared and its speed is set for the next
//     frame ((5 - slowed) x (base + bonus) x 0.2).
//   - a circle against a box (out of the side or corner it's past, 40 x the depth)
//     or a door's leaf (Obj_CollideDoorWithCircle: a 32-unit edge from the hinge; the circle is pushed off it
//     at 100 x the depth, damped, and the door, on its own step, swings away by the depth x
//     the circle's mass, unless locked; it turns at most 4 of 256 a step and no
//     further than 12 directions from shut).
// Also: a box against a box or a door's leaf (Obj_CollideDoorWithBox), a box against the walls
// in its cells (each wall as two lines in the turned frame: put back short of a line it went
// past, else 100 x its depth, damped, times the share of its side the line covers), elevators
// and their shafts holding up what's on them, and the touches handed to collide.hpp.
// Not yet: springs, buffs.
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/util/x87.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace nox::server {

/// The unit vector of a direction, 0-255 from east through south.
std::pair<float, float> directionVector(int direction);
/// A vector's direction, 0-255 (the exe's: (atan2 + 2 pi) x 128 / pi + 0.5, chopped).
int directionOf(float x, float y);

/// sincosTable16: about 16 x (cos, sin) of an angle 0-255, the exe's integers.
std::pair<int, int> sinCos16(int angle);

/// Its collision box about its position (CollideP1 / P2, objectUnkUpdateCoords): left, top, right,
/// bottom; none for a shapeless one.
std::optional<std::array<float, 4>> collisionBox(const ServerObject& o);

/// How far a circle of `radius` at (px, py) is into a box object, and which way out (n);
/// negative when apart. `rng` picks a way out from dead centre.
float boxDepth(const ServerObject& box, float px, float py, float radius, float& nx, float& ny, std::minstd_rand& rng);

/// Whether (px, py) is inside a box object's (45-degree) rectangle about (bx, by)
/// (strictly, against its corners).
bool insideBox(const ServerObject& box, float bx, float by, float px, float py);

/// noxcpp's substeps (not the exe's): whether, where the substep put it, `o` is further into a body
/// the frame's motion would push it off than it was at the frame's start (circles against circles
/// and boxes, the motion's own pairing rules). Such a substep is taken back: the next frame decides
/// how far it really gets, so a player running into another isn't shown past where it stops.
bool substepPressesBody(World& world, const ServerObject& o);
/// The same for walls (noxcpp's substeps): further into a wall, as the motion's wall test measures
/// it, than at the frame's start. The frame would push it back out: a substep held against a
/// wall showed it going in and back out each frame.
bool substepPressesWall(World& world, ServerObject& o);

/// The distance between two objects' edges (calcDistance): their centres'
/// less each one's radius (a box's: half its larger side), at least 0.01.
float edgeDistance(const ServerObject& a, const ServerObject& b);
/// The same as the x87 leaves it (53 bits, unstored): what calcDistance's callers that compare
/// it at once compare.
util::X87 edgeDistance53(const ServerObject& a, const ServerObject& b);

/// objectApplyForce: a push of `force` x 10 / its mass away from (x, y), on what
/// can move (not DEAD, DESTROYED, NO_COLLIDE, ALLOW_OVERLAP; not IMMOBILE); it joins the motion.
void applyForce(World& world, ServerObject& o, float x, float y, float force);

/// Whether it takes part in motion at all (unitHasCollideOrUpdateFn): it updates, or collides and isn't
/// NO_COLLIDE; and it isn't IMMOBILE or ALLOW_OVERLAP, unless it's a door or an elevator's.
bool joinsMotion(const ServerObject& o);

/// unitHasCollideOrUpdateFn: it joins the motion queue, at its end, if it can move
/// (joinsMotion) and isn't in it already; an obstacle or a fire (0x2008) leaves the path finder's
/// marks to be made afresh. True if it joined just now.
bool queueMotion(World& world, ServerObject& o);

/// updateUnits's test at one's turn in the units update, after its update and fall: a force, a
/// velocity, a move under way (NewPos - Pos), a height or a rise, any beyond 0.01 either way:
/// it joins the motion (not RESTING); else it comes to rest: force,
/// velocity and rise 0, down to the floor (unitRaise), NewPos where it is, RESTING.
void settleOrQueue(World& world, ServerObject& o);

/// The new things' pass (unitsNewAddToList: a map's load, a join, the tick's end):
/// each put into the world since (World::noteNew), newest first, on the map: a missile on the
/// missiles' list, one with an UPDATE or a velocity (not a door) on the updatable list, linked in
/// the map grid, and one with a COLLIDE in the motion.
void queueNewObjects(World& world);

/// A door's own update (DoorUpdate): clients told when it has turned; untouched for
/// half a second, it swings back towards shut.
void updateDoor(World& world, ServerObject& door, std::uint32_t frame);

/// A touch the motion found (collSysAddCollision): `a` met `b` (an object's
/// extent, or kWallContact / kFloorContact: the exe's NULL and 6 in place of an object), along
/// the normal (nx, ny) as the finding routine gives it. Each pair once a frame, whichever way
/// round it was found first.
inline constexpr std::uint32_t kWallContact = 0xFFFFFFFFu;  ///< a wall
inline constexpr std::uint32_t kFloorContact = 0xFFFFFFFEu; ///< a harming floor tile
struct Contact {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    float nx = 0;
    float ny = 0;
};

/// The frame's motion; `rng` breaks ties (two bodies on one spot). The touches it found go in
/// `contacts`, newest first (the exe's list order), for their COLLIDE functions
/// (collide.hpp).
void moveObjects(World& world, std::minstd_rand& rng, std::uint32_t frame, std::vector<Contact>* contacts = nullptr);

} // namespace nox::server
