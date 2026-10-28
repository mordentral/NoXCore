// The things that act on their own (their UPDATE functions) and their fall (Z).
//
//   - The fall (updateFallLogic, after each one's update): something above the
//     ground or rising rises by its zVel, which falls by 1 a frame (falling: flag 0x20000), and
//     comes to rest on the ground; a bouncer (flag 0x800000) falls by 0.5 a frame and bounces
//     back at -zVel x bounce x 0.1 till that's under 2. Held up by an elevator (ON_OBJECT,
//     0x100000, set by the motion and cleared after each fall) it doesn't fall. Down a shaft
//     (IN_HOLE, 0x40000) it falls (no velocity, drawn 3 a frame to the shaft's middle) and,
//     past 50 below the floor, comes out at its elevator at 90 up.
//   - Elevators (ElevatorUpdate): enabled and a second at rest at the bottom, it rises
//     2 a frame to 64 (no longer SHORT past 20; from 32 up what's on it, within 10 of its
//     height, goes to its shaft at 64 below, or, too big for the opening, is dropped), rests a
//     second, and sinks again (SHORT again from 20). What's within 10 of its height and over
//     it stands on it (physics.cpp); what's lower meets it as a box. Its shaft
//     (ElevatorShaftUpdate), while it sinks below 33, sends what's on it (within 10
//     of its top) down onto it; with the elevator 10 or more below the floor, what walks over
//     the opening falls in. Clients get the height / 4 as each one's frame (MSG_DRAW_FRAME).
//   - FistUpdate: on the ground the first time it strikes (FX 0x8A thrice, the
//     ground shakes 30); bounced back up past 200, or 3 s old, it's gone. FistCollide
//     (collideFist): on the ground, what it meets takes its damage (type 2) from its caster.
//   - MeteorUpdate: on the ground it bursts: a MeteorExplode, the ground shakes 10,
//     its damage (type 7) about it (80, full within 30), the walls about it too.
//   - MeteorShowerUpdate: 5 s, a Meteor every 4-8 frames somewhere (a random way,
//     4-12 squared off) it sees, falling from 255 at 8.
//   - ToxicCloudUpdate: every 5-10 frames the creatures within 75 it sees take 3-10
//     of poison from its caster, and its caster's enemies are poisoned (1); gone at its time.
// Sounds: the strike (0x30), the burst (0x57), the elevators' (elevatorAud:
// by material and subclass, 0xF9-0x104), a hard landing (0x118).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/things/thing_db.hpp"

namespace nox::server {

/// Its death (unitDamageClear's call of its DIE function, also DieCollide's and
/// LifetimeUpdate's): CreateObjectDie, SpawnObjectDie, PolypDie, BoulderDie; none, it's deleted.
void objectDies(Fight& fight, ServerObject& o);

/// A point about (x, y) within `r` that it sees: from a random way, 64 tries
/// each 3/5 of a half turn on and r / 64 nearer; none clear, (x, y) itself.
std::pair<float, float> clearPointNear(const World& world, std::minstd_rand& rng, float x, float y, float r);

/// A scorch mark of `size` (0-2) at (x, y), gone in 10-20 s, a quest's 5-8
/// (sMakeScorch: a fireball's burst, 2; Death Ray's end, 1).
void makeScorch(World& world, const things::ThingDb& things, float x, float y, int size, std::minstd_rand& rng, std::uint32_t frame);

/// The ground shakes about (x, y) (earthquakeSend): MSG_FX_JIGGLE (0x97) to each player whose
/// camera is within 300, `strength` at the middle, fading to 0 at 300 with the squared distance.
void jiggle(World& world, float x, float y, int strength);

/// The scripts' Move to a waypoint (server_scriptMoveTo): a mover, or the movers
/// carrying `o`, set off for it (a monster's walk: with the monsters).
void scriptMoveTo(World& world, ServerObject& o, std::int32_t waypoint);
/// The scripts' CreateMover / GroupCreateMover: a new Mover where `o` is, its
/// speed (chopped) and first waypoint set, on and on the updatable list; its script id (0 if
/// none was made). Its carried thing's extent is never set, so it moves nothing
/// (an original bug).
std::int32_t createMover(World& world, const things::ThingDb& things, ServerObject& o, std::int32_t waypoint, float speed, std::uint32_t frame);

/// Its animation frame set and owed to the players (servMarkObjAnimFrame).
void markAnimFrame(ServerObject& o, std::uint32_t frame);
/// Turned on or off, owed to the players (unitSetOnOff).
void setOnOff(ServerObject& o, bool on);
/// xStatus bits set / cleared, owed to the players (unitSetXStatus,
/// unitUnsetXStatus; bit 1 is never sent).
void setXStatus(ServerObject& o, std::uint32_t bits);
void unsetXStatus(ServerObject& o, std::uint32_t bits);

struct Objects {
    Fight& fight;
    const things::ThingDb& things;

    /// Whether it has an UPDATE here (not a player's, a door's or a missile's).
    static bool acts(const ServerObject& o);
    /// Its UPDATE.
    void update(ServerObject& o);
    /// Its fall (updateFallLogic).
    static void fall(World& world, ServerObject& o, std::uint32_t frame);

private:
    int rechargeRate(const ServerObject* item) const;
    void obeliskUpdate(ServerObject& o);
    void trapDoorUpdate(ServerObject& o);
    void pentagramUpdate(ServerObject& o);
    void invisiblePentagramUpdate(ServerObject& o);
    void sendFromPentagram(ServerObject& o, bool shown);
    void triggerUpdate(ServerObject& o);
    void toggleUpdate(ServerObject& o);
    void elevatorUpdate(ServerObject& o);
    void shaftUpdate(ServerObject& o);
    /// BreakUpdate / OpenUpdate (`breaks`: a BreakUpdate's, NO_COLLIDE once broken).
    void breakUpdate(ServerObject& o, bool breaks);
    /// BreakAndRemoveUpdate: BreakUpdate's by xStatus 2 / 4 / 8, then deleted.
    void breakAndRemoveUpdate(ServerObject& o);
    void blackPowderBarrelUpdate(ServerObject& o);
    void skullUpdate(ServerObject& o);
    void blowUpdate(ServerObject& o);
    void sentryUpdate(ServerObject& o);
    void moverUpdate(ServerObject& o);
    bool skullSees(const ServerObject& o);
};

} // namespace nox::server
