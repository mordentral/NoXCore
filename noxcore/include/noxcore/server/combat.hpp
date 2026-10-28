// Fighting, as GAME.EXE's server does it: an attack's reach and who it hits, damage, death and
// respawning.
//
//   - An attack (playerTraceAttack) hits the nearest one in its reach, a circle
//     about the attacker: an enemy creature (Obj_IsEnemyTo) or a thing, not dead,
//     BELOW, ALLOW_OVERLAP or NO_COLLIDE, it can see through the walls (ray flags 9), within
//     60 degrees of its facing (the cosine over 0.5; a creature preferred to a
//     thing; a circle's distance less its radius). Then it must be on the
//     attacker's front side (checkReflectShield's sectors) and a wall ray (flags 5) clear, and takes the
//     damage (its Damage function); a hook (punch 0x19) knocks it back.
//   - A player's damage (PlayerDamage): nothing while dead; while INVULNERABLE taken
//     but nothing done; a solo player isn't hurt by its own doing (but type 0xF); a
//     REFLECTIVE_SHIELD turns missiles and rays met at its front; blocks; then armour by the
//     damage type (its cases: blows (1 - armour), falls (1 - armour / 2), fire worn but
//     not lessened, magic more through metal armour, the rest none), the fraction carried to
//     the next hit (playerDecrementHPMana), the worn pieces wearing what the armour took; a quest's
//     multiplier; and the kill feed's weapon or type noted (killType / killKind).
//   - DefaultDamage: nothing to the INVULNERABLE or dead; none from its own side
//     (gameplay flag 1 aside) nor from its own doing in a quest; none from a friend's hand
//     but a hammer's; SHOCK discharges at one striking it hand to hand; monsters' immunities;
//     fire and electricity less by their protections (the enchantments' tables, at most
//     0.6); INVISIBLE ends; what hurt it noted; VAMPIRISM heals its attacker; a recoil past
//     19 (state 0x1E); a SHIELD takes half (all when it would fail: left on 2) out of its
//     strength; then its health down (unitDamageClear) and at 0 dead: its DIE function.
//   - A player's death (PlayerDie): the kill feed to everyone (INFORM 0x0E: killer,
//     assist, victim, weapon, kind), dying (state 3; a second later dead, 4: ALLOW_OVERLAP,
//     SHORT), MSG_PLAYER_DIED to everyone. Dead, the Action key respawns it (Plr_Respawn):
//     full health, its flags cleared, idle, MSG_REPORT_TOTAL_HEALTH, MSG_PLAYER_RESPAWN to
//     everyone, at a player start, 5 seconds invulnerable.
// Not yet: armour, shields and blocking, weapons, items dropped and given back, scores,
// monsters' deaths, floating damage numbers, sounds, the assist.
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/x87.hpp"

#include <cstdint>
#include <random>
#include <utility>

namespace nox::server {

/// The 8-way direction of a 0-255 one (xferIndexedDirection): dx and dy each
/// -1, 0 or 1, from sincosTable16 against 6.
std::pair<int, int> gridDirection(int direction);

/// Which sides of one at (fx, fy) facing `direction` the point (tx, ty) is on (checkReflectShield): bit
/// 1 its front.
int facingSides(float fx, float fy, int direction, float tx, float ty);

/// An attack (playerAttack's record for playerTraceAttack).
struct Attack {
    float damage = 0;
    std::uint8_t type = 0;     ///< the damage type (10: a punch)
    float range = 0;           ///< its reach about the attacker
    std::uint32_t weapon = 0;  ///< 0: bare hands
    bool hitsOverlapping = false; ///< ALLOW_OVERLAP ones too
    std::uint8_t sides = 1;    ///< the facing sides it reaches
    /// A war hammer's (its weapon's subclass 0x4000): everything in reach of (x, y), its head
    /// ahead, not just the nearest.
    bool area = false;
    float x = 0;
    float y = 0;
};

/// A weapon's blow (calcBoltDamage): past its required strength so much a point
/// (damageCoefficient), and its least (damageMin); in a solo game (game flag 0x800) an
/// ArcherBolt's least is gamedata's BoltSoloDamageMin instead; without its definition, 10. (On
/// the x87: FILD, FMUL, FIADD (the solo bolt's FADDP of the gamedata number), returned on the
/// stack: boltDamage53; most callers FSTP it: boltDamage.)
inline util::X87 boltDamage53(const World& world, const things::EquipDef* def, int strength)
{
    if (!def) {
        return util::X87(10.0f);
    }
    const util::X87 least = (world.gameFlags & GameFlag::Solo) && def->name == "ArcherBolt" ? util::X87(world.number("BoltSoloDamageMin"))
                                                                                           : util::X87::fromDouble(def->damageMin);
    return util::X87::fromDouble(strength - def->requiredStrength) * def->damageCoefficient + least;
}
inline float boltDamage(const World& world, const things::EquipDef* def, int strength) { return boltDamage53(world, def, strength).f(); }

/// A blow's damage made whole as the strike and the weapon missiles' touches do it: the float
/// plus the double 0.5, __ftol (rounded, halves up).
inline int roundedDamage(float damage) { return util::ftol(util::X87(damage) + util::X87::fromDouble(0.5)); }

struct Fight {
    World& world;
    std::minstd_rand& rng;
    std::uint32_t frame;
    const things::ThingDb* things = nullptr;        ///< for items (none: no drops or outfit)
    const things::ModifierDb* modifiers = nullptr;

    /// Swings `attack`; whether it hit anything.
    bool attack(ServerObject& attacker, const Attack& attack);
    /// `amount` of `type` from `source` (and `weapon`, the thing that hit) to `target`, by its
    /// Damage function (a player's PlayerDamage, else DefaultDamage); what that returns (0: it
    /// took none at all).
    int damage(ServerObject& target, ServerObject* source, std::uint32_t weapon, int amount, int type);
    /// Its health down by `amount`, and at 0 dead (unitDamageClear).
    void takeHealth(ServerObject& target, int amount);
    /// Brings a dead player back (Plr_Respawn). (noxcpp: `keepItems`, a NoX Core mode's respawn
    /// that keeps what it holds, as a quest's does: no new outfit, MSG_PLAYER_RESPAWN's flag 0.)
    void respawn(ServerObject& player, bool keepItems = false);
    /// Up its owners to a player, or the last of them (findParentChainPlayer).
    ServerObject* parentPlayer(ServerObject* o);
    /// A player's state change (Plr_SetState: player.hpp's setPlayerState).
    void setPlayerState(ServerObject& player, std::uint8_t state);
    /// Poisons it (Obj_ApplyPoison): not the frozen-out, the INVULNERABLE, an
    /// observer or a poison-proof monster; its protection's chance in 100 to resist
    /// ("Health.c:ResistPoison"); else its level up by `amount` to at most `most`.
    bool applyPoison(ServerObject& t, int amount, int most);
    /// Its poison level set (setSomePoisonData): a player's status 0x400 with
    /// it; the fall timer at 1000.
    void setPoison(ServerObject& t, int level);
    /// Its poison down by `amount` (Obj_ReducePoison; gone: "Health.c:PoisonFade").
    void reducePoison(ServerObject& t, int amount);
    /// All of it gone (removePoison).
    void removePoison(ServerObject& t);
    /// A player's status flags (Plr_SetStatusFlags / ClearStatusFlags): the reported ones
    /// (0x423) told to everyone (MSG_REPORT_CLIENT_STATUS 0x6A).
    void setStatus(ServerObject& player, std::uint32_t flags, bool on);
    /// The units update's last pass for one (updateUnits): the slowness and poison fall every
    /// 1000 frames, its enchantments count down, the poison bites.
    void unitTail(ServerObject& o);

private:
    /// The attack's hit on what's in its reach (the hit check's part of attack()); whether it hit,
    /// the nearest picked in `struck` (none for a
    /// hammer's).
    bool strike(ServerObject& a, const Attack& atk, ServerObject*& struck);
    /// PlayerDie: the kill feed, dying, the mode's scoring, its things dropped.
    void playerDies(ServerObject& player);
    /// PlayerDamage (players and NPCs): shields, blocks and armour, then defaultDamage.
    int playerDamage(ServerObject& t, ServerObject* src, ServerObject* obj3, int dmg, int type);
    /// DefaultDamage: immunities, protections, notes and enchantments, then its health down.
    int defaultDamage(ServerObject& t, ServerObject* src, ServerObject* obj3, int dmg, int type);
    /// Fire or lightning protection: worn items' (at most 0.5) plus the enchantment's, at most 0.6.
    float protection(const ServerObject& t, int enchantment, const char* table) const;
    /// The whole part of `amount` (with last blow's fraction added); the new fraction kept.
    int carryDamage(ServerObject& t, float amount);
    /// playerDamageItems: the worn armour wears from what it took.
    void wearArmor(ServerObject& t, ServerObject* src, ServerObject* obj3, int amount, int type);
    /// 1 plus the worn metal armour's values: electricity's multiplier through metal.
    float metalFactor(const ServerObject& t) const;
    /// The Shield spell's share of a blow: half; a blow that would kill breaks it instead (left on 2).
    void shieldTakes(ServerObject& t, int& dmg, int type, ServerObject* by);
    /// The Shield spell's strength down by `amount`; at none it ends.
    void shieldWears(ServerObject& t, int amount);
    void pointFx(std::uint8_t op, float x, float y);
};

/// Where `unit` (re)appears (Map_FindPlayerStart): the usable PlayerStart farthest from
/// its nearest enemy player, a random one with none about.
std::pair<float, float> findPlayerStart(World& world, const ServerObject* unit, std::minstd_rand& rng);

/// Whether `a` is hostile to `b` (Obj_IsEnemyTo): never what shares its owning
/// player or its team, a charmed one or an observer; the special creatures (fish, frogs, rats,
/// the Polyp, generators) as the game has them.
bool isEnemy(const World& world, const ServerObject& a, const ServerObject& b);

/// unitAdjustHP: its health up by `amount`, to its most; nothing in
/// elimination's sudden death (game flag 0x4000000); a monster's owner told (informOwnerHealth).
void adjustHealth(World& world, ServerObject& o, int amount);
/// mobInformOwnerHP: a player's creature, its player told its health
/// (MSG_REPORT_UNIT_HEALTH 0x41: its code, half its health).
void informOwnerHealth(World& world, const ServerObject& creature);
/// What's under a player's cursor (findObjectAtCursor, each frame into
/// PlayerControl::cursorObject): of the units within 100 of it (the map grid's circle search, its order), the
/// creatures (and the Polyp), not itself, the dead or the invisible (unless it has INFRAVISION),
/// nor an observing player, in its sight (mapCheck: the bodies in the way and the
/// ray's flags 9), whose drawn shape (its circle at its height; a box's: its box about there)
/// holds the cursor, the frontmost.
ServerObject* objectAtCursor(World& world, const ServerObject& player);
/// A missile turned back off what it met (projectileReflect): its velocity's
/// part along the line from `by` reversed, the rest kept; facing round half a turn; this
/// frame's move undone (NewPos = PrevPos).
void reflectMissile(ServerObject& m, const ServerObject& by);
/// A worn piece's share of what the armour took, as playerDamageItems figures it on
/// the x87 in the game's chop mode (util::X87): FDIV by the whole, FIMUL the amount (each chopped
/// to 53 bits), FSTP to the float equipDamage takes. (A whole of 0 gives the exe infinities; none
/// taken here.)
float armorShare(float value, float total, int amount);

} // namespace nox::server
