// Missiles: arrows and bolts so far.
//
//   - A bow or crossbow shot (Obj_ShootBowOrCrossbow -> shootBowCrossbow2): from the first worn
//     quiver with arrows left, an ArcherArrow (a bow) or ArcherBolt (a crossbow) just ahead of
//     the shooter (its radius + 4 along its facing, if no wall is between), owned by it, at the
//     missile's speed along its facing; a player's quiver loses one (MSG_REPORT_CHARGES 0x64)
//     and goes when empty. No quiver worn: in a quest a bow looses a WeakArcherArrow anyway; a
//     player then has one from the inventory put on ("pattack.c:ReloadQuiver"), or
//     "pattack.c:NoQuiver" (not after the quest's weak arrow), the dry click (0x377 a bow's, not
//     in a quest; 0x378 a crossbow's) and goes idle; a monster doesn't reload.
//   - In flight (the units update's second list), each frame it moves its
//     velocity and its path is searched, in pieces of 6 at most, for a body holding the piece's
//     end (the path search's filter: not another missile, not NO_COLLIDE, a MISSILE_HIT one always,
//     an airborne missile over BELOW / SHORT things) or a door's leaf (not a gate's) the piece
//     crosses, and for walls (the ray, flags 5); the nearer is hit: its COLLIDE, then the
//     body's (but a fist's: neither). ProjectileUpdate ends it after 40 frames.
//   - ArrowCollide: not its own shooter; a live one takes (strength - required) x
//     coefficient + least (modifier.bin's ArcherArrow / ArcherBolt) of type 11 from the
//     shooter's player; the arrow goes, unless it did nothing, or a bolt killed it (bolts fly
//     on through the dead). A wall stops it.
//   - Wands that fire (WandUse): a Fireball (SparkExplosionCollide n) bursts on what it meets
//     or at its time's end: a push of 50 within n x 2/3, n / 2 of type 1 to what it hit and to
//     all within n / 3 behind no wall (less past 15: linearly to nothing at the edge), MSG 0x93
//     (the burst) to everyone. A flare (YellowStarShotCollide n, WallReflectCollide) does n of
//     type 11 to a body not of its team and goes, and glances off walls (collideReflect: its velocity
//     mirrored on the wall's diagonal).
//   - Chakrams (playerAttack's bits 0x40 / 0x80, at CHAKRAM_STRIKE's middle; just ahead of the
//     thrower, its radius + 4, if no wall is between). A fan chakram looses a FanChakramInMotion
//     (ProjectileUpdate, ArrowCollide) and loses a charge, gone at none. A round chakram goes
//     itself, riding in a RoundChakramInMotion (out of the thrower's inventory):
//     ChakramInMotionUpdate steers it home at its thrower while on its way back and
//     the thrower's in sight; after 5 s, or with the thrower gone, it's lost.
//     ChakramInMotionCollide: its thrower catches it (back in the inventory, worn
//     if nothing is); an enemy takes its damage (type 0); off an enemy or a wall it bounces (4
//     times; then a turn back at random), and seeks the nearest enemy in sight within 400,
//     or, with no bounces left, heads home; lost, it drops where it is.
//   - Magic missiles (MagicMissileUpdate): for 3 s, while their owner lives, each
//     frame pushed at their speed along their heading, drag 0.8; the heading turns 42 (of 256)
//     a frame towards their target, sought every 8 frames when they have none (enemies within
//     600). BoomCollide: a wall turns them off it at half speed; a body takes
//     MagicMissileDamage (8, type 7); then, and at their time's end, MSG_FX_LESSER_EXPLOSION, the
//     splash (23 within 15, full within 5) and a push (20 within 50), and they're gone. They pass
//     through their owner's side (NO_COLLIDE_OWNER, 0x400: unitsHaveSameTeam).
//   - A warrior's HarpoonBolt (HarpoonUpdate, HarpoonCollide): flying, it
//     makes for the nearest enemy ahead of it within MaxHarpoonDistance of the warrior's cursor
//     and snaps past MaxHarpoonFlightDistance; what it hits takes HarpoonDamage (type 11) and,
//     an enemy, is held: the bolt rides it, the warrior pulls it in (player.cpp), till it's
//     further than MaxHarpoonDistance, closer than MinHarpoonDistance (let go), stuck
//     MaxHarpoonExistence frames, not moved a unit in a second, out of sight or gone. The rope:
//     MSG_FX_DURATION_SPELL 7 on, 14 off.
//   - Walls: a flight whose ray meets one is put back where it was (NewPos = Pos), and its
//     COLLIDE strikes that cell (damageToMap): an arrow or bolt its damage (type
//     11), a magic missile 8 (type 7), a chakram 1 (type 0), a death ball DeathBallCollideDamage
//     (type 2); a harpoon and a pixie the cell under them. Bursts strike the walls about them
//     too (mapDamageToWalls).
// Not yet: other missiles (monsters' arrows), enchantment effects, scorch marks. (The bow's
// "reload counter" is never set in the exe: not kept.)
// Sounds: a chakram thrown 0x37B (no room: 0x143), caught 0x37C, lost off a body 0x37D; a bow's
// 0x375, a crossbow's 0x376; a wand's its USE sound (spent: 0xDE); a magic missile's burst 0x54;
// a missile turned at a shield 0x7A (and 0x40 as it changes hands).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <utility>

namespace nox::server {

/// A homing spell missile (class MISSILE, subclass 2) after `to` turned on it
/// (changeOwner): its target becomes its owner, `to` its owner, its life starts
/// again, 0x40 at 	o; a Swap carried to its target (spell 0x73) is countered instead
/// (cspellRemoveSpell: MSG_FX 0x87, gone).
void turnSpellMissile(World& world, ServerObject& m, const ServerObject& to, std::uint32_t frame);

/// The normal a missile's COLLIDE is handed for the wall its move's ray met (projectileTraceHit's
/// table, by the wall's shape as a box meets it): by the side of the cell the move set out from
/// (fromX, fromY: its position) and, for the walls with more than one arm, the quarter of the
/// cell the ray met it in, (+-0.7071, +-0.7071) out of the arm on that side. False for a shape the
/// table hasn't (none reported, the move undone all the same).
bool projectileWallNormal(const World& world, const RayHit& hit, float fromX, float fromY, float& nx, float& ny);

// noxcpp: a mode's own size for one missile (ServerObject::ModSize; a script's nox.setHitRadius and
// nox.setBurstScale). Nothing here is the game's: a missile no mode sizes is as its type made it.

/// The radii a missile's hit shape may be given, by its type's extent: a point's (or none's) 1 to
/// 32, a circle's 1 to 4 x its radius, a box's 1 to 4 x its size (the larger side's half).
std::pair<float, float> hitRadiusRange(const ServerObject& m);
/// Its hit shape at `radius` (within hitRadiusRange): a point (or none) made a circle of it, a
/// circle's radius it, a box's two sides scaled by radius / its size. Its type's shape is kept to
/// give back (only that gives the stock hit back: the type's own flight meets only what holds its
/// centre); its box on the grid is refreshed where it is (no cell or list changes). That box is
/// what the out-of-sight notice reads, so for a sized missile alone the notice comes up to its
/// radius of travel later. Walls, and the substeps' wall test, stay its type's. Set in the frame
/// it's made (onSpellCast's `made`), it doesn't meet its owner by the growth until it has first
/// flown clear of it (Missiles::bodyNear).
void setHitRadius(World& world, ServerObject& m, float radius);
/// Its type's shape back (nothing when it has none of a mode's).
void restoreHitShape(World& world, ServerObject& m);
/// Its burst's reach and push reach times `scale` (1: its type's), read when it bursts.
void setBurstScale(ServerObject& m, float scale);
/// A thing's draw scale (nox.setDrawScale; any thing, not only a missile): what NoX Core clients
/// draw its sprite and its burst at (1: as its type's; the part let go when nothing else holds
/// it). Nothing on the server reads it but the host's onScaledSeen.
void setDrawScale(ServerObject& o, float scale);
/// Its draw scale (1: none).
float drawScaleOf(const ServerObject& o);

struct Missiles {
    World& world;
    const things::ThingDb& things;
    const things::ModifierDb* modifiers;
    std::minstd_rand& rng;
    std::uint32_t frame;

    /// A shot of `weapon` (a bow or crossbow) by `shooter` (Obj_ShootBowOrCrossbow); whether it
    /// had a worn quiver with arrows to shoot from (the exe's 1, even when a wall was too close).
    bool shoot(ServerObject& shooter, ServerObject& weapon);
    /// A WandUse wand's shot (useLesserFireballStaff): not while spent or within its delay; its missile (three,
    /// 8 apart, MULTI_SHOT) just ahead at its speed plus the shooter's (at the shooter itself
    /// when a wall is between); a charge less.
    bool wandShot(ServerObject& shooter, ServerObject& wand);
    /// A chakram thrown (a round one goes itself, a fan one looses a charge); whether it flew.
    bool throwChakram(ServerObject& thrower, ServerObject& weapon);
    /// The frame's flight of every missile.
    void fly();
    /// Its COLLIDE with nothing at all (as at its time's end): a fireball's or a magic
    /// missile's burst, a counterspell's doing.
    void burst(ServerObject& m) { collide(m, nullptr, 0, 0); }
    /// mapPushUnitsAround from a point, nothing spared (Push, Pull).
    void pushFrom(float x, float y, float reach, float full, float force) { pushAround(x, y, reach, full, force, nullptr); }
    /// mapDamageUnitsAround (see damageAround).
    void damageAt(float x, float y, float reach, float full, int damage, int type, ServerObject* source, const ServerObject* except,
                  bool spareSource)
    {
        damageAround(x, y, reach, full, damage, type, source, except, spareSource);
    }
    /// What a spell flying from `missile` goes after (spellFlySearchTarget with a
    /// hostile spell's flag): the nearest live creature, enemy to it, within `range` of (x, y)
    /// that it and `owner` both see.
    ServerObject* seekTarget(float x, float y, const ServerObject& missile, float range, const ServerObject* owner);

    /// Its COLLIDE function with what it met (`other` null: a wall, with its diagonal's normal
    /// in (nx, ny), or nothing at all, its time up).
    void collide(ServerObject& m, ServerObject* other, float nx, float ny);

    /// The wall cell this flight's ray met (kept while its flag is
    /// set, during the missile's COLLIDE), for its blow to it. Fly's own: leave it unset.
    std::optional<std::pair<int, int>> wallCell{};
    /// The flying missile's box as its move began (fly's objectUnkUpdateCoords).
    MapGrid::Rect flyBox_{};

private:
    /// shootBowCrossbow2: the arrow (`bit` 4) or bolt from `quiver`, or with no
    /// quiver a WeakArcherArrow; a player's quiver loses one.
    void loose(ServerObject& shooter, ServerObject* quiver, ServerObject& weapon, std::uint32_t bit);
    /// The blow of `amount` of `type` to the wall its ray met (damageToMap), if any.
    void wallBlow(ServerObject& m, int amount, int type);
    /// The blow to the wall cell under (x, y) (Math_FloatToInt of x / 23 and y / 23).
    void cellBlow(ServerObject& m, float x, float y, int amount, int type);
    /// What its piece of path from (fromX, fromY) to (x, y) meets: of the map grid's
    /// cell (x, y) is in (its extent list from the head), the last that holds the
    /// piece's end or a door's leaf the piece crosses (a leaf crossed puts the end
    /// back at the start for those after it).
    ServerObject* bodyAt(ServerObject& missile, float fromX, float fromY, float x, float y);
    /// noxcpp: bodyAt for a missile a mode gave a hit shape (ServerObject::ModSize): the same
    /// rule over the bodies whose boxes meet the box its shape sweeps over the piece (the grid's
    /// search, each once), a body met when its shape grown by the missile's radius holds the end;
    /// its owner, until first cleared, only as the stock test meets it (ModSize::ownerCleared).
    ServerObject* bodyNear(ServerObject& missile, float fromX, float fromY, float x, float y);
    /// The path search's filter: whether `missile` may meet the body `o` at all.
    bool mayMeet(const ServerObject& missile, const ServerObject& o);
    /// One missile's turn in fly().
    void flyOne(ServerObject* m);
    /// ArrowCollide: an arrow's or bolt's hit (the list above).
    void arrowHits(ServerObject& arrow, ServerObject* other);
    /// SparkExplosionCollide: a wand fireball's burst.
    void sparkExplosion(ServerObject& m, ServerObject* other);
    /// YellowStarShotCollide / WallReflectCollide: a flare's hit; it glances off walls.
    void starShot(ServerObject& m, ServerObject* other, float nx, float ny);
    /// ChakramInMotionUpdate: a round chakram's way home, or lost.
    void chakramUpdate(ServerObject& m);
    /// ChakramInMotionCollide: caught, a hit, or a bounce off a body or a wall.
    void chakramHits(ServerObject& m, ServerObject* other, float nx, float ny);
    /// The chakram it carries, out onto the ground where it is, and the carrier gone: `dropped` by
    /// its DROP (Obj_DropAt: lost off a wall, or its thrower gone), else set down off a body
    /// (Obj_RemoveFromInventory and createAt: no DROP, so no decay).
    void chakramDrops(ServerObject& m, ServerObject& chakram, bool dropped);
    /// MagicMissileUpdate: its homing, and its burst at its time's end.
    void magicMissileUpdate(ServerObject& m);
    /// HarpoonUpdate: seeking, riding what it holds, letting go.
    void harpoonUpdate(ServerObject& m);
    /// HarpoonCollide: what it hits is held, or it's done.
    void harpoonHits(ServerObject& m, ServerObject* other);
    /// DeathBallUpdate: two in reach annihilate; after 10 frames it harms what's
    /// about it every frame; 4 s and gone.
    void deathBallUpdate(ServerObject& m);
    /// PixieUpdate and PixieCollide.
    void pixieUpdate(ServerObject& m);
    void pixieHits(ServerObject& m, ServerObject* other, float nx, float ny);
    /// A pixie's prey: the nearest enemy creature it can see within 250 (a
    /// quest's 640), a VILLAIN at a third of its distance.
    ServerObject* pixieTarget(const ServerObject& m);
    /// BoomCollide: a magic missile's hit and burst.
    void boom(ServerObject& m, ServerObject* other, float nx, float ny);
    /// A spell flying to its target (Magic): SpellProjectileUpdate (at its lifetime's
    /// end casts it on the nearest creature within 50) and SpellProjectileCollide
    /// (spellFlyCollide).
    void spellFlyUpdate(ServerObject& m);
    void spellFlyHits(ServerObject& m, ServerObject* other, float nx, float ny);
    /// mapDamageUnitsAround: `damage` of `type` from `source` to all with health
    /// within `reach` of (x, y), no wall between, less past `full` (linearly, none at the edge);
    /// not `except`. The source itself too, unless `spareSource` (the exe's flag that
    /// gameSetWallsDamage sets, back to 1 after each call: Death Ray's 0 spares it, Mana Bomb's 1 doesn't).
    void damageAround(float x, float y, float reach, float full, int damage, int type, ServerObject* source, const ServerObject* except,
                      bool spareSource = false);
    /// mapPushUnitsAround: the movable within `reach` of (x, y), no wall between,
    /// pushed off it by `force` x 10 / their mass, less past `full` (none at the edge).
    void pushAround(float x, float y, float reach, float full, float force, const ServerObject* source);
    /// Up the missile's owners to a player, or the last of them.
    ServerObject* shooterPlayer(const ServerObject& m);
};

} // namespace nox::server
