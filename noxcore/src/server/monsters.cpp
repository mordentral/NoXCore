// The monsters' AI, as the exe's server runs it. Each monster keeps a stack of actions
// (IDLE, GUARD, FIGHT, MOVE_TO...) with conditions (DEPENDENCY_*) pushed under them; each
// frame update() keeps the stack's targets up, runs the main AI (think), checks the
// conditions, then starts and updates the top action. Also here: creature ownership and
// orders, summoning, the death functions and the monster generators. monsters.hpp has the
// overview; pathfind.cpp finds the paths, npc.cpp does the NPCs' weapons.
#include "noxcore/server/monsters.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/clock.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/pathfind.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/draw.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
namespace ms = things::MonsterStatus;
namespace dep = Dependency;
using Item = ServerObject::Monster::Item;
using MonsterAction::Idle;
using util::hi;
using util::lo;
using util::X87;

/// A walker's speed as the exe holds it on the x87's stack: running, monster.bin's run
/// multiplier times SpeedCur (not stored); else SpeedCur.
X87 walkSpeedOnStack(const ServerObject& o)
{
    const auto& m = *o.monster;
    return (m.status & ms::Running) && m.def ? X87(m.def->runMultiplier) * o.speedCur : X87(o.speedCur);
}

/// monsterMoveAudio: a walker's step (its set's MOVE_SOUND) at monster.bin's two
/// step frames of its walk. One drawn as a player (subclass 0x30) counts the player walk's frames
/// (animation 4) off its net code and the frame, the step when that frame is new.
void stepSound(World& world, std::uint32_t frame, const ServerObject& o)
{
    const auto& m = *o.monster;
    if (!m.def) {
        return;
    }
    const auto step = static_cast<std::uint32_t>(m.def->moveSoundFrameA);
    const auto step2 = static_cast<std::uint32_t>(m.def->moveSoundFrameB);
    if (!(o.subclasses & 0x30u)) {
        if ((m.animFrame == step || m.animFrame == step2) && m.animDelay == 0) {
            playCreatureSound(world, o, things::SoundSlot::Move);
        }
        return;
    }
    const auto [frames, delay] = world.playerAnimations ? (*world.playerAnimations)[4] : std::pair{0, 0};
    if (frames <= 0) {
        return; // (the exe would divide by 0)
    }
    const std::uint32_t t = o.extent + frame;
    const std::uint32_t per = static_cast<std::uint32_t>(delay) + 1;
    const std::uint32_t now = (t / per) % static_cast<std::uint32_t>(frames);
    const std::uint32_t before = ((t - 1) / per) % static_cast<std::uint32_t>(frames);
    if (now != before && (now == step || now == step2)) {
        playCreatureSound(world, o, things::SoundSlot::Move);
    }
}

constexpr const char* kActionNames[] = {
    "ACTION_IDLE", "ACTION_WAIT", "ACTION_WAIT_RELATIVE", "ACTION_ESCORT", "ACTION_GUARD", "ACTION_HUNT", "ACTION_RETREAT",
    "ACTION_MOVE_TO", "ACTION_FAR_MOVE_TO", "ACTION_DODGE", "ACTION_ROAM", "ACTION_PICKUP_OBJECT", "ACTION_DROP_OBJECT",
    "ACTION_FIND_OBJECT", "ACTION_RETREAT_TO_MASTER", "ACTION_FIGHT", "ACTION_MELEE_ATTACK", "ACTION_MISSILE_ATTACK",
    "ACTION_CAST_SPELL_ON_OBJECT", "ACTION_CAST_SPELL_ON_LOCATION", "ACTION_CAST_DURATION_SPELL", "ACTION_BLOCK_ATTACK",
    "ACTION_BLOCK_FINISH", "ACTION_WEAPON_BLOCK", "ACTION_FLEE", "ACTION_FACE_LOCATION", "ACTION_FACE_OBJECT",
    "ACTION_FACE_ANGLE", "ACTION_SET_ANGLE", "ACTION_RANDOM_WALK", "ACTION_DYING", "ACTION_DEAD", "ACTION_REPORT",
    "ACTION_MORPH_INTO_CHEST", "ACTION_MORPH_BACK_TO_SELF", "ACTION_GET_UP", "ACTION_CONFUSED", "ACTION_MOVE_TO_HOME",
    "ACTION_INVALID",
};
/// The conditions' (the debug table from 40).
constexpr const char* kDependencyNames[] = {
    "DEPENDENCY_OR", "DEPENDENCY_TIME", "DEPENDENCY_ALIVE", "DEPENDENCY_UNDER_ATTACK", "DEPENDENCY_NOT_UNDER_ATTACK",
    "DEPENDENCY_CAN_SEE", "DEPENDENCY_CANNOT_SEE", "DEPENDENCY_BLOCKED_LINE_OF_FIRE", "DEPENDENCY_OBJECT_AT_VISIBLE_LOCATION",
    "DEPENDENCY_OBJECT_FARTHER_THAN", "DEPENDENCY_OBJECT_CLOSER_THAN", "DEPENDENCY_LOCATION_FARTHER_THAN",
    "DEPENDENCY_LOCATION_CLOSER_THAN", "DEPENDENCY_VISIBLE_ENEMY", "DEPENDENCY_VISIBLE_FRIEND", "DEPENDENCY_VISIBLE_FOOD",
    "DEPENDENCY_NO_VISIBLE_ENEMY", "DEPENDENCY_NO_VISIBLE_FRIEND", "DEPENDENCY_NO_VISIBLE_FOOD",
    "DEPENDENCY_NO_INTERESTING_SOUND", "DEPENDENCY_NO_NEW_ENEMY", "DEPENDENCY_UNINTERRUPTABLE", "DEPENDENCY_IS_ENCHANTED",
    "DEPENDENCY_ENEMY_CLOSER_THAN", "DEPENDENCY_NOT_HEALTHY", "DEPENDENCY_WAIT_FOR_STAMINA", "DEPENDENCY_ENEMY_FARTHER_THAN",
    "DEPENDENCY_UNDER_CURSOR", "DEPENDENCY_NOT_CORNERED", "DEPENDENCY_LOCATION_IS_SAFE", "DEPENDENCY_NOT_FRUSTRATED",
    "DEPENDENCY_NOT_MOVED",
};

/// Each action's arguments (the exe's action table: per action its number, a count, then each one's kind; an
/// argument takes two of Args): 0 a position, 1 an object, 3 a number or angle, 4 a distance,
/// 5 a frame, 2 / 7 the roam's.
struct ArgSpec {
    int count;
    int kinds[2];
};
constexpr ArgSpec kArgs[72] = {
    {1, {5, 0}}, {1, {5, 0}}, {1, {3, 0}}, {2, {0, 1}}, {2, {0, 3}}, {0, {0, 0}}, {0, {0, 0}}, {2, {0, 1}},
    {2, {0, 1}}, {1, {0, 0}}, {2, {2, 7}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {2, {0, 5}},
    {0, {0, 0}}, {2, {0, 1}}, {2, {3, 1}}, {2, {3, 0}}, {2, {3, 1}}, {1, {5, 0}}, {0, {0, 0}}, {0, {0, 0}},
    {2, {0, 1}}, {1, {0, 0}}, {1, {1, 0}}, {1, {3, 0}}, {1, {3, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}},
    {1, {3, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {2, {0, 1}}, {0, {0, 0}}, {0, {0, 0}},
    // The conditions (0x28-0x47).
    {0, {0, 0}}, {1, {5, 0}}, {1, {1, 0}}, {1, {5, 0}}, {0, {0, 0}}, {1, {1, 0}}, {1, {1, 0}}, {1, {1, 0}},
    {2, {0, 1}}, {2, {4, 1}}, {2, {4, 1}}, {2, {4, 0}}, {2, {4, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}},
    {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {0, {0, 0}}, {1, {1, 0}}, {0, {0, 0}}, {1, {3, 0}}, {1, {4, 0}},
    {0, {0, 0}}, {0, {0, 0}}, {1, {4, 0}}, {0, {0, 0}}, {0, {0, 0}}, {1, {0, 0}}, {0, {0, 0}}, {0, {0, 0}},
};

std::uint32_t bitsOf(float f) { return std::bit_cast<std::uint32_t>(f); }
float floatOf(std::uint32_t u) { return std::bit_cast<float>(u); }

bool isType(const ServerObject& o, std::string_view name) { return o.typeName == name; }
bool isZombie(const ServerObject& o) { return isType(o, "Zombie") || isType(o, "VileZombie"); }
bool isMimic(const ServerObject& o) { return isType(o, "Mimic"); }
bool isPlant(const ServerObject& o) { return isType(o, "CarnivorousPlant"); }
bool creature(const ServerObject& o) { return (o.classes & (oc::Monster | oc::Player)) != 0; }

/// The actions over 0x27 are conditions (monsterActionIsCondition).
bool isCondition(int action) { return 0x27 < action; }

/// A new top: its timers and flags afresh, its path and waypoints dropped.
void freshTop(ServerObject::Monster& m, std::uint32_t frame)
{
    m.animFrame = 0;
    m.animDelay = 0;
    m.animDone = 0;
    m.pathCount = 0;
    m.pathIndex = 0;
    m.waypointCount = 0;
    m.waypointIndex = 0;
    m.actionFrame = frame;
    m.field1f0 = frame;
}

/// The stack down to a lone IDLE that never started its update.
bool loneIdle(const ServerObject::Monster& m) { return m.top == 0 && m.stack[0].action == Idle; }

// Its temper (the aggression): attacking at will over 0.66 (monsterCanAttackAtWill), defending
// between 0.33 and 0.66, cowardly under 0.08.
bool atWill(const ServerObject::Monster& m) { return 0.66f < m.aggression; }
bool defends(const ServerObject::Monster& m) { return m.aggression < 0.66f && 0.33f < m.aggression; }
bool cowardly(const ServerObject::Monster& m) { return m.aggression < 0.08f; }
/// Whether it can move at all (monsterIsMoveing: its base speed).
bool canMove(const ServerObject& o) { return 0.01f <= o.speedBase; }
/// Cornered lately (mobGetMoveAttemptTime: within 3 s of when it last moved away).
bool cornered(const ServerObject::Monster& m, std::uint32_t frame) { return frame - m.movedAwayAt < 90u; }
/// Casting (its top a spell's, 0x12-0x14).
bool casting(const ServerObject::Monster& m)
{
    const int a = m.stack[static_cast<std::size_t>(m.top)].action;
    return 0x11 < a && a < 0x15;
}
/// Runs unless it never does; walks again unless it always runs.
void run(ServerObject::Monster& m)
{
    if (!(m.status & ms::NeverRun)) {
        m.status |= ms::Running;
    }
}
void walk(ServerObject::Monster& m)
{
    if (!(m.status & ms::AlwaysRun)) {
        m.status &= ~ms::Running;
    }
}
/// It casts (monsterCanCast), shoots (monsterCanShoot: an NPC by
/// its weapon, the rest by monster.bin's missile), fights hand to hand (monsterCanMelee).
bool canCast(const ServerObject::Monster& m) { return (m.status & ms::CanCastSpells) != 0; }
bool canShoot(const ServerObject& o)
{
    const auto& m = *o.monster;
    if (o.subclasses & 0x10u) {
        return (m.weaponFlags & 0x47F00FEu) != 0;  // missile weapons and the wand staves
    }
    return m.def && !m.def->missile.empty();
}
bool canMelee(const ServerObject& o)
{
    const auto& m = *o.monster;
    if (canCast(m)) {
        return false;
    }
    if (o.subclasses & 0x10u) {
        return !canShoot(o);
    }
    return m.def && 0.0f < m.def->meleeRange;
}
/// Has health and lives, or has none to lose (checkIsKillable).
bool killable(const ServerObject& t) { return t.health != 0 || t.maxHealth == 0; }
/// An NPC weapon's stamina (weaponGetStaminaByType).
int weaponStamina(std::uint32_t w)
{
    if (w & 0x200u) return 70;  // longsword
    if (w & 0x4000u) return 100;  // war hammer
    if (w & 0x800u) return 50;  // mace
    if (w & 0x100u) return 45;  // sword
    if (w & 0x1000u) return 75;  // axe
    if (w & 0x2000u) return 100;  // ogre axe
    if (w & 0x7FF8000u) return 45;  // the staves
    return (w & 0x400u) ? 75 : 10;  // great sword; anything else 10
}

/// Its health over its most on the x87's stack (FILD, FIDIV: 53 bits), compared from there
/// (monsterCanResumeAttack); 1 with no most.
X87 healthRatio(const ServerObject& o)
{
    return o.maxHealth != 0 ? X87::fromDouble(o.health) / X87::fromDouble(o.maxHealth) : X87(1.0f);
}

/// The squared distance as the exe has it on the x87's stack: each difference, each square and
/// their sum at 53 bits (all exact at the map's sizes), compared or stored from there.
X87 dist2(const ServerObject& a, float x, float y)
{
    const X87 dx = X87(x) - a.x;
    const X87 dy = X87(y) - a.y;
    return dx * dx + dy * dy;
}

/// Its facing within about 25 degrees of `v` (their dot over 0.9). (The facing test: both
/// products and their sum on the x87's stack, 53 bits, compared there; `v` as stored floats.)
bool facing(const ServerObject& o, float vx, float vy)
{
    const auto [fx, fy] = directionVector(o.direction);
    return X87(0.9f) < X87(fy) * vy + X87(fx) * vx;
}

int wrap256(int d)
{
    while (d < 0) {
        d += 0x100;
    }
    while (0xFF < d) {
        d -= 0x100;
    }
    return d;
}

/// The creature behind something: up its owners to one.
ServerObject* creatureOf(World& world, std::uint32_t extent)
{
    ServerObject* o = extent ? world.byExtent(extent) : nullptr;
    while (o && !creature(*o) && o->owner != 0) {
        o = world.byExtent(o->owner);
    }
    return o && creature(*o) ? o : nullptr;
}

/// Each unit whose centre is strictly within `r` of (x, y), in the map grid's order, as it's
/// found (unitsGetInCircle: the callback runs inside the walk, so what it asks of
/// the grid is a search a level deeper). A deleted one too (0x20: ours' pendingDelete), as the
/// exe's grid has it till it's freed; the callbacks that skip it say so.
template <class Fn>
void inCircle(World& world, float x, float y, float r, Fn&& fn)
{
    world.unitsInCircle(x, y, r, [&](ServerObject& t) { fn(&t); });
}

/// The nearest enemy it can reach within `range` (enemyAggro, each by the target test):
/// a creature or generator, not dead; a VILLAIN counts a third as far.
ServerObject* aggroTarget(World& world, ServerObject& o, float range)
{
    float best = range;
    ServerObject* chosen = nullptr;
    inCircle(world, o.x, o.y, range, [&](ServerObject* t) {
        if (t == &o || !(t->classes & 0x20006u) || !isEnemy(world, o, *t) || (t->flags & ObjFlag::Dead) || !canInteract(world, o, *t)) {
            return;
        }
        // (On the x87: dx stored; dy stored, squared from the stack by the stored one; the root
        // stored to a float (a helper's), + 0.001 stored; a third of it and the test on the
        // stack, the best kept stored.)
        const float dx = (X87(t->x) - o.x).f();
        const X87 dyStack = X87(t->y) - o.y;
        const float dy = dyStack.f();
        const float root = util::sqrt(dyStack * dy + X87(dx) * dx).f();
        const float d0 = (X87(root) + 0.001f).f();
        const X87 d = enchanted(*t, Enchant::Villain) ? X87(d0) * 0.33333334f : X87(d0);
        if (d < X87(best)) {
            best = d.f();
            chosen = t;
        }
    });
    return chosen;
}

/// The nearest live enemy creature within 1000; -1 none. (The nearest's squared distance kept
/// stored to a float; its root comes back on the x87's stack, 53 bits.)
X87 nearestEnemy(World& world, ServerObject& o)
{
    float best = 1e8f;
    bool any = false;
    inCircle(world, o.x, o.y, 1000.0f, [&](ServerObject* t) {
        if (!creature(*t) || !isEnemy(world, o, *t) || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || t->pendingDelete) { // (0x8020)
            return;
        }
        const X87 d = dist2(o, t->x, t->y);
        if (d < X87(best)) {
            best = d.f();
            any = true;
        }
    });
    return any ? util::sqrt(X87(best)) : X87(-1.0f);
}

/// The nearest food it can reach within `range` (mobSearchEdible, each by
/// mobSearchEdible2): not subclass 4; a cure (0x80) only when poisoned; a 0x8 one only a 0x10 one in
/// a network game, by an NPC.
ServerObject* edible(World& world, ServerObject& o, float range)
{
    float best = 1e7f;
    ServerObject* chosen = nullptr;
    inCircle(world, o.x, o.y, range, [&](ServerObject* t) {
        if (!(t->classes & oc::Food) || (t->subclasses & 4u) || ((t->subclasses & 0x80u) && o.poison == 0)) {  // not a JUG; a MUSHROOM only when poisoned
            return;
        }
        if ((t->subclasses & 8u) && !((t->subclasses & 0x10u) && world.network() && (o.subclasses & 0x10u))) {  // a POTION only if a HEALTH_POTION, in a network game, for an NPC
            return;
        }
        if (!canInteract(world, o, *t)) {
            return;
        }
        const X87 d = dist2(o, t->x, t->y); // (compared from the stack, kept stored)
        if (d < X87(best)) {
            best = d.f();
            chosen = t;
        }
    });
    return chosen;
}

/// Something about (x, y) it had better keep out of (unitIsDangerous within 50):
/// fire (not to the fireproof), a toxic cloud (not to the poison-proof), anything DANGEROUS.
bool safeAt(World& world, const ServerObject& o, float x, float y)
{
    bool safe = true;
    inCircle(world, x, y, 50.0f, [&](ServerObject* t) {
        if (t->classes & oc::Fire) {
            if (!(o.subclasses & 0x400u)) {  // IMMUNE_FIRE
                safe = false;
            }
        } else if (t->typeName == NameId::ToxicCloud || t->typeName == NameId::SmallToxicCloud) {
            if (!(o.subclasses & 0x200u)) {  // IMMUNE_POISON
                safe = false;
            }
        } else if (t->classes & oc::Dangerous) {
            safe = false;
        }
    });
    return safe;
}

} // namespace

namespace MonsterAction {

const char* name(int action)
{
    if (0 <= action && action < 39) {
        return kActionNames[action];
    }
    if (dep::Or <= action && action <= dep::NotMoved) {
        return kDependencyNames[action - dep::Or];
    }
    return "ACTION_INVALID";
}

int byName(std::string_view n)
{
    for (int i = 0; i < 39; ++i) {
        const std::string_view a = kActionNames[i];
        if (util::equalsNoCase(a, n)) {
            return i;
        }
    }
    return Invalid;
}

} // namespace MonsterAction

void createMonster(ServerObject& o, const things::MonsterDef* def, std::uint32_t gameFlags, const things::ThingDef* type)
{
    auto& m = o.monster.emplace();
    m.def = def;
    // Its type's animation table (the MonsterDraw states: frames a way, delay, Loop / LoopAndFade).
    if (type && type->draw) {
        try {
            const auto spec = things::decodeDraw(*type->draw);
            for (std::size_t s = 0; s < spec.monsterStates.size(); ++s) {
                if (const auto& st = spec.monsterStates[s]) {
                    m.anims[s] = {static_cast<std::uint8_t>(st->animation.directions[0].size()), st->animation.frameDelay,
                                  st->animation.kind == "Loop" || st->animation.kind == "LoopAndFade"};
                    m.hasAnims = true;
                }
            }
        } catch (const std::exception&) {
        }
    }
    if (def) {
        o.maxHealth = o.health = static_cast<std::uint16_t>(def->health);
        o.speedBase = o.speedCur = static_cast<float>(def->speed) * 0.03125f;  // monster.bin's speed / 32
        m.retreatLevel = def->retreatRatio;
        m.resumeLevel = def->resumeRatio;
        m.status = def->status;
        m.fleeRange = def->fleeRange;
    }
    typeSpells(o, gameFlags);
    // A female NPC's colours, all six 0xD2AE79 (set directly: no report owed).
    if (o.subclasses & 0x20u) {
        m.colors.fill({0xD2, 0xAE, 0x79});
    }
}

void typeSpells(ServerObject& o, std::uint32_t gameFlags)
{
    // monsterAutoSpells.
    auto& m = *o.monster;
    auto& s = m.spells;
    constexpr std::uint32_t kCounter = 0x8000000, kSelf = 0x10000000, kAttack = 0x20000000, kHold = 0x40000000, kAway = 0x80000000;
    const std::string_view t = o.typeName;
    // Each spell (by number) marked with the use it's cast for: kCounter against a spell coming,
    // kSelf on itself, kAttack at an enemy, kHold (a hold or summons), kAway to get away.
    // spellDelays: per use in that order, the least and most frames till its next cast (spellAt).
    // So spellDelays[2u] / [2u + 1] are use u's least / most (u: 0 counter, 1 self, 2 attack, 3 hold, 4 away).
    if (t == "UrchinShaman") {
        m.spellPower = 2;
        s[38] = kCounter;
        m.spellDelays[0] = 0;
        m.spellDelays[1] = 15;
        s[12] = kAttack;
        m.spellDelays[4] = 90;
        m.spellDelays[5] = 150;
        m.spellDelays[6] = 0;
        s[58] = kHold;
        s[52] = kHold;
        s[66] = kAway;
        m.spellDelays[7] = 90;
        m.spellDelays[8] = 30;
        m.spellDelays[9] = 90;
    } else if (t == "Wizard" || t == "WizardWhite") {
        m.spellPower = 3;
        s[13] = kCounter;
        s[38] = kCounter;
        s[39] = kSelf;
        s[72] = kAttack;
        s[27] = kHold;
        s[50] = kHold;
        s[43] = kHold;
        s[24] = kHold;
        s[4] = kAway;
    } else if (t == "Beholder") {
        m.spellPower = 3;
        s[24] = kHold;
        if (!(gameFlags & GameFlag::Quest)) {
            s[4] = kAway;
        }
    } else if (t == "Lich") {
        m.spellPower = 3;
        s[13] = kCounter;
        s[38] = kCounter;
        s[71] = kSelf;
        s[72] = kAttack;
        s[33] = kAttack;
        s[39] = kAway;
        s[27] = kHold;
        s[43] = kHold;
        s[24] = kHold;
    } else if (t == "LichLord") {
        m.spellPower = 3;
        s[13] = kCounter;
        s[38] = kCounter;
        s[71] = kSelf;
        m.spellDelays[6] = 90;
        s[31] = kHold;
        s[39] = kAway;
        m.spellDelays[7] = 150;
    } else if (t == "Demon") {
        m.spellPower = 3;
        s[13] = kCounter;
        s[74] = kAttack;
        s[27] = kHold;
        s[10] = kHold;
        s[39] = kAway;
    } else if (t == "WizardGreen") {
        m.spellPower = 3;
        s[38] = kCounter;
        s[12] = kAttack;
        s[58] = kHold;
        s[60] = kHold;
        s[50] = kHold;
        s[4] = kAway;
    } else if (t == "WillOWisp") {
        m.spellPower = 3;
        s[43] = kHold;
    }
    m.typeSpells = 1;
}

void spawnFixup(ServerObject& o, std::uint32_t gameFlags)
{
    // monsterOnSpawnSpellcaster.
    auto& m = *o.monster;
    const things::MonsterDef* def = m.def;
    if (!def) {
        return;
    }
    if (m.healthFromMap == 0) {
        o.health = o.maxHealth = static_cast<std::uint16_t>(def->health);
    }
    if (m.statusFromDef == 1) {
        m.status = def->status;
    } else {
        // Of the first 22, the map keeps only its hold-ground, see-friends, heal and run bits
        // (0x19C40).
        for (int i = 0; i < 0x16; ++i) {
            const std::uint32_t bit = 1u << i;
            if (bit & 0x19C40u) {
                continue;
            }
            if ((def->status & bit) != 0) {
                m.status |= bit;
            } else {
                m.status &= ~bit;
            }
        }
    }
    if (!(m.status & ms::CanCastSpells)) {
        m.status &= ~(ms::CanHealSelf | ms::CanHealOthers);
    }
    if (m.retreatFromDef == 1) {
        m.retreatLevel = def->retreatRatio;
    }
    if (m.resumeFromDef == 1) {
        m.resumeLevel = def->resumeRatio;
    }
    if (m.typeSpells == 1) {
        typeSpells(o, gameFlags);
    }
}

int monsterAnimState(const ServerObject& o)
{
    // mobActionToAnimation.
    const auto& m = *o.monster;
    int s = 8;
    if (0 <= m.top) {
        switch (m.stack[static_cast<std::size_t>(m.top)].action) {
        case MonsterAction::MoveTo:
        case MonsterAction::FarMoveTo:
        case MonsterAction::Roam:
        case MonsterAction::FindObject:
        case MonsterAction::RandomWalk:
        case MonsterAction::Confused:
        case MonsterAction::MoveToHome:
            s = static_cast<int>(((m.status & ms::Running) | 0x30000u) >> 14);  // 12 walking, 13 running (Running is status bit 14)
            break;
        case MonsterAction::Dodge: s = 12; break;
        case MonsterAction::MeleeAttack: s = 1; break;
        case MonsterAction::MissileAttack: s = 3; break;
        case MonsterAction::CastSpellOnObject:
        case MonsterAction::CastSpellOnLocation:
        case MonsterAction::CastDurationSpell: s = 7; break;
        case MonsterAction::BlockAttack:
        case MonsterAction::WeaponBlock: s = 5; break;
        case MonsterAction::BlockFinish: s = 6; break;
        case MonsterAction::Flee: s = 13; break;
        case MonsterAction::Dying: s = 9; break;
        case MonsterAction::Dead: s = 10; break;
        case MonsterAction::MorphIntoChest:
        case MonsterAction::GetUp: s = 14; break;
        case MonsterAction::MorphBackToSelf: s = 15; break;
        default: break;
        }
    }
    if (isMimic(o) && s == 8) {
        return (m.status & ms::Morphed) ? 0 : s;
    }
    if (isPlant(o) && s == 8) {
        return m.enemy == 0 ? 14 : s;
    }
    if (isZombie(o) && s == 9 && (m.status & ms::OnFire)) {
        return 15;
    }
    return s;
}

bool Monsters::has(const ServerObject& o, int action)
{
    const auto& m = *o.monster;
    for (int i = m.top; 0 <= i; --i) {
        if (m.stack[static_cast<std::size_t>(i)].action == action) {
            return true;
        }
    }
    return false;
}

Item* Monsters::push(ServerObject& o, int action)
{
    // monsterPushAction. (The item's arguments are what the slot last held,
    // as in the exe: a push sets the action alone.)
    if (!(o.classes & oc::Monster) || !o.monster) {
        return nullptr;
    }
    auto& m = *o.monster;
    if (m.top == 23) {  // all 24 slots used
        return nullptr;
    }
    if (-1 < m.top) {
        if (m.stack[0].action == MonsterAction::Dead && action != MonsterAction::Dying) {
            return nullptr;
        }
        Item& top = m.stack[static_cast<std::size_t>(m.top)];
        if (top.action != 0 || m.top != 0) {
            if (!isCondition(top.action) && top.started) {
                aside(o, top.action); // (the table's fourth: the action put aside)
            }
        } else {
            m.top = -1; // (a lone IDLE: replaced)
        }
    }
    ++m.top;
    Item& item = m.stack[static_cast<std::size_t>(m.top)];
    item.action = action;
    item.started = false;
    freshTop(m, fight.frame);
    m.stackChanged = true;
    return &item;
}

Item* Monsters::pushFight(ServerObject& o, const ServerObject& at, std::uint32_t frame)
{
    Item* it = push(o, MonsterAction::Fight);
    if (it) {
        it->args[0] = bitsOf(at.x);
        it->args[1] = bitsOf(at.y);
        it->args[2] = frame;
    }
    return it;
}

void Monsters::pop(ServerObject& o)
{
    // monsterPopAction.
    auto& m = *o.monster;
    if (-1 < m.top) {
        Item& top = m.stack[static_cast<std::size_t>(m.top)];
        if (!isCondition(top.action) && top.started) {
            end(o, top.action);
        }
    }
    m.stackChanged = true;
    --m.top;
    // Conditions left on top of what's under it go with it.
    while (-1 < m.top && isCondition(m.stack[static_cast<std::size_t>(m.top)].action)) {
        --m.top;
    }
    freshTop(m, fight.frame);
    if (m.top < 0) {
        m.top = 0;
        m.stack[0].action = Idle;
    }
}

void Monsters::clear(ServerObject& o)
{
    // monsterClearActionStack: popped down to a lone, fresh IDLE.
    auto& m = *o.monster;
    while (!loneIdle(m)) {
        pop(o);
    }
}

void Monsters::init(ServerObject& o)
{
    // MonsterInit.
    auto& m = *o.monster;
    m.initialised = true;
    if (!(o.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
        // A few creatures start their own way: the plant guards, rats walk at random, fish roam,
        // the frog idles alert.
        if (isPlant(o)) {
            clear(o);
            m.startAction = MonsterAction::Guard;
            m.sightRange = (X87(m.def ? m.def->meleeRange : 0.0f) + o.radius + 10.0f).f(); // (one store)
        } else if (isType(o, "Rat")) {
            clear(o);
            push(o, MonsterAction::RandomWalk);
            m.aggression = 0.16f;
            m.startAction = MonsterAction::Invalid;
        } else if (isType(o, "FishBig") || isType(o, "FishSmall")) {
            clear(o);
            if (Item* it = push(o, MonsterAction::Roam)) {
                it->args[0] = 0;
                it->args[2] = 0xFF;
            }
            m.aggression = 0.16f;
            m.startAction = MonsterAction::Invalid;
        } else if (isType(o, "GreenFrog")) {
            clear(o);
            push(o, Idle);
            m.aggression = 0.16f;
            m.status |= ms::Alert;
            m.startAction = MonsterAction::Invalid;
        }
    }
    // Its starting action (the map's; INVALID: none, or already pushed above).
    switch (m.startAction) {
    case MonsterAction::Escort:
        if (Item* it = push(o, MonsterAction::Escort)) {
            it->args = {bitsOf(o.x), bitsOf(o.y), 0, 0};
        }
        break;
    case MonsterAction::Guard:
        if (Item* it = push(o, MonsterAction::Guard)) {
            it->args = {bitsOf(o.x), bitsOf(o.y), static_cast<std::uint32_t>(static_cast<std::int16_t>(o.direction)), 0};
        }
        break;
    case MonsterAction::Roam:
        if (!atWill(m)) {
            if (Item* it = push(o, MonsterAction::Roam)) {
                it->args[0] = 0;
                it->args[2] = m.field333;
            }
        } else {
            push(o, MonsterAction::Hunt);
        }
        break;
    case MonsterAction::Fight:
        if (Item* it = push(o, MonsterAction::Fight)) {
            it->args = {bitsOf(o.x), bitsOf(o.y), fight.frame, 0};
        }
        break;
    case MonsterAction::Invalid:
        break;
    default:
        push(o, Idle);
        break;
    }
    m.startAction = MonsterAction::Invalid;
    m.homeDirection = static_cast<std::int16_t>(o.direction);
    m.homeX = o.x;
    m.homeY = o.y;
    // Unhurt, it starts at field338 (the map record's health fraction) of its full health.
    if (o.health == o.maxHealth) {
        // (FILD, FMUL, __ftol: the product on the x87's stack, truncated.)
        o.health = static_cast<std::uint16_t>(util::ftol(X87::fromDouble(o.maxHealth) * m.field338));
    }
    m.healthGraph.fill(o.health);
    // Speed: 5% either way at random; one drawn as a player (subclass 0x30) from its own number.
    if (!(o.subclasses & 0x30u)) {
        o.speedBase = (util::randomFloat(fight.rng, 0.95f, 1.05f) * o.speedBase).f(); // (the draw's stack value, one store)
    } else {
        o.speedBase = (X87(m.field332) * 0.5f + 1.7f).f(); // (one store)
    }
    if (m.status & ms::CanCastSpells) {
        m.fleeRange = 100.0f;
    }
    if (m.status & ms::HoldYourGround) {
        m.fleeRange = 0;
    }
    if (m.status & ms::AlwaysRun) {
        m.status |= ms::Running;
    }
}

namespace {

/// Hearing, each monster's update: of the sounds played the last 2 frames, the
/// loudest it can hear, when newer than what it last heard or louder, becomes what it heard
/// (unitEmitHearEvent), and its HearEnemy handler runs.
void hear(Fight& fight, ServerObject& o)
{
    World& world = fight.world;
    auto& m = *o.monster;
    // Alive (checkIsKillable: health, or none to lose), not a fish, frog or rat.
    if (!world.soundTable || (o.maxHealth != 0 && o.health == 0) || o.typeName == NameId::FishBig || o.typeName == NameId::FishSmall ||
        o.typeName == NameId::GreenFrog || o.typeName == NameId::Rat) {
        return;
    }
    const std::uint32_t now = world.frameNow;
    const World::HeardSound* best = nullptr;
    int loudest = 0;
    // Sounds older than 2 frames are dropped.
    std::erase_if(world.heardSounds, [&](const World::HeardSound& h) { return now < h.frame || 2u < now - h.frame; });
    // (Newest first: the exe's list takes each at its head, so of two as loud the newer wins.)
    for (auto r = world.heardSounds.rbegin(); r != world.heardSounds.rend(); ++r) {
        World::HeardSound& h = *r;
        if (h.source != 0) {
            const ServerObject* s = world.byExtent(h.source);
            if (!s || (s->flags & ObjFlag::Destroyed)) {
                h.source = 0;
            }
        }
        if (h.frame < m.heardAt || now < m.heardAt) {
            continue;
        }
        const SoundInfo& info = (*world.soundTable)[h.id];
        const auto hearing = static_cast<std::uint32_t>(info.hearing);
        // An enemy's (or no one's) sound with any hearing, a friend's only with 0x10;
        // in its polygon group, when the sound's in one.
        ServerObject* by = h.source ? fight.parentPlayer(world.byExtent(h.source)) : nullptr;
        if (!by || isEnemy(world, o, *by)) {
            if (hearing == 0) {
                continue;
            }
        } else if (!(hearing & 0x10u)) {
            continue;
        }
        const SoundEvent e{h.id, h.x, h.y, h.source, 0, 0};
        if (const std::uint8_t g = soundGroup(world, e); g != 0) {
            const std::uint32_t at = world.polygonAt(static_cast<int>(o.x), static_cast<int>(o.y), 0);
            if (at != 0 && world.polygons()[at - 1].group != g) {
                continue;
            }
        }
        // How loud where it is, halved out of sight (ray flags 5); loud enough for its
        // hearing (0x20 89, 0x40 20, else 50) both times.
        const auto enough = [&](int level) {
            return (hearing & 0x20u) ? 89 <= level : (hearing & 0x40u) ? 20 <= level : 50 <= level;
        };
        int level = soundLoudness(info, h.x, h.y, o.x, o.y);
        if (!enough(level)) {
            continue;
        }
        if (!world.traceRay(o.x, o.y, h.x, h.y, Trace::Doors | Trace::ThroughWindows)) {
            level = static_cast<int>(static_cast<float>(level) * 0.5f);
        }
        if (!enough(level) || level <= 0 || level <= loudest) {
            continue;
        }
        loudest = level;
        best = &h;
    }
    if (!best || (best->frame <= m.heardAt && loudest <= m.heardLevel)) {
        return;
    }
    m.heard = best->id;
    m.heardAt = now;
    m.heardLevel = loudest;
    m.heardFrom = best->source;
    m.heardX = best->x;
    m.heardY = best->y;
    if (world.script) {
        ServerObject* by = best->source ? fight.parentPlayer(world.byExtent(best->source)) : nullptr;
        world.script->callBlock(m.onHearEnemy, by ? by->extent : 0, o.extent);
    }
}

} // namespace

/// Its animation (updateNPCAnimData): a step each frame past its delay; at its
/// end a loop starts over, else it's done. An NPC attacking keeps it going (its frames are its
/// attack's, npc.hpp); else its action's player animation (unitNPCActionToAnim:
/// casting CAST_SPELL 0x15, dying 1, dead 2, blocking 0x28, a weapon's block 0x2F or a staff's
/// 0x1E; none for the rest: done at once), never looping.
static void animate(World& world, ServerObject& o)
{
    auto& m = *o.monster;
    const int topAction = 0 <= m.top ? m.stack[static_cast<std::size_t>(m.top)].action : -1;
    if ((o.subclasses & 0x10u) && (topAction == MonsterAction::MeleeAttack || topAction == MonsterAction::MissileAttack)) {
        m.animDone = 0;
    } else if (o.subclasses & 0x10u) {
        if (m.animDone == 0) {
            int animation = -1;
            switch (topAction) {
            case MonsterAction::CastSpellOnObject:
            case MonsterAction::CastSpellOnLocation:
            case MonsterAction::CastDurationSpell: animation = 0x15; break;
            case MonsterAction::Dying: animation = 1; break;
            case MonsterAction::Dead: animation = 2; break;
            case MonsterAction::BlockAttack: animation = 0x28; break;
            case MonsterAction::WeaponBlock: animation = (m.weaponFlags & 0x7FF8000u) ? 0x1E : 0x2F; break;
            default: break;
            }
            std::uint8_t frames = 0;
            std::uint8_t delay = 0;
            if (0 <= animation && world.playerAnimations) {
                const auto [f, d] = (*world.playerAnimations)[static_cast<std::size_t>(animation)];
                frames = static_cast<std::uint8_t>(f);
                delay = static_cast<std::uint8_t>(d);
            }
            bool done = true;
            if (frames != 0) {
                ++m.animDelay;
                if (m.animDelay < delay + 1u) {
                    done = false;
                } else {
                    m.animDelay = 0;
                    ++m.animFrame;
                    if (m.animFrame < frames) {
                        done = false;
                    } else {
                        m.animFrame = static_cast<std::uint8_t>(frames - 1);
                    }
                }
            }
            if (done) {
                m.animDone = 1;
            }
        }
    } else if (m.animDone == 0 && m.hasAnims) {
        // A monster's own animation (its draw states), by the state its action shows.
        const auto& a = m.anims[static_cast<std::size_t>(monsterAnimState(o))];
        m.animFrames = a.frames; // (SkeletonDamage reads it)
        bool done = a.frames == 0;
        if (a.frames != 0) {
            ++m.animDelay;
            if (a.delay + 1u <= m.animDelay) {
                m.animDelay = 0;
                ++m.animFrame;
                if (a.frames <= m.animFrame) {
                    if (a.loop) {
                        m.animFrame = 0;
                    } else {
                        m.animFrame = static_cast<std::uint8_t>(a.frames - 1);
                        done = true;
                    }
                }
            }
        }
        if (done) {
            m.animDone = 1;
        }
    } else if (!m.hasAnims) {
        m.animDone = 1;
    }
}

void Monsters::update(ServerObject& o)
{
    // MonsterUpdate.
    World& world = fight.world;
    auto& m = *o.monster;
    if (!m.initialised) {
        init(o);
    }
    if (m.field523 != 0) {
        --m.field523;
    }
    if (o.spawnedBy != 0) { // (its generator gone: no one's)
        const ServerObject* g = world.byExtent(o.spawnedBy);
        if (!g || g->gone() || (g->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            o.spawnedBy = 0;
        }
    }
    // mobAction: the one it keeps to, the stack's targets kept up (the
    // destroyed forgotten), and the positions of what they follow.
    const auto alive = [&](std::uint32_t extent) -> ServerObject* {
        ServerObject* t = extent ? world.byExtent(extent) : nullptr;
        return t && !t->gone() ? t : nullptr;
    };
    if (m.focus != 0) {
        const ServerObject* f = alive(m.focus);
        if (!f || (f->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            m.focus = 0;
        }
    }
    // Each stack item: object arguments (kind 1) that are gone cleared, followed points kept up.
    for (int i = m.top; 0 <= i; --i) {
        Item& it = m.stack[static_cast<std::size_t>(i)];
        if (it.action < 0 || 72 <= it.action) {
            continue;
        }
        const ArgSpec& spec = kArgs[it.action];
        for (int a = 0; a < spec.count; ++a) {
            auto& arg = it.args[static_cast<std::size_t>(a * 2)];
            if (spec.kinds[a] == 1 && arg != 0 && !alive(arg)) {
                arg = 0;
            }
        }
        const auto follow = [&](const ServerObject* t) {
            if (t) {
                it.args[0] = bitsOf(t->x);
                it.args[1] = bitsOf(t->y);
            }
        };
        switch (it.action) {
        case MonsterAction::Escort:
            follow(alive(it.args[2]));
            break;
        case MonsterAction::MoveTo:
        case MonsterAction::FarMoveTo:
            // One it can't reach dropped (but while escorting).
            if (const ServerObject* t = alive(it.args[2])) {
                if (!canInteract(world, o, *t) && !has(o, MonsterAction::Escort)) {
                    it.args[2] = 0;
                } else {
                    follow(t);
                }
            }
            break;
        case MonsterAction::Fight:
            follow(alive(m.enemy));
            break;
        case MonsterAction::MissileAttack:
            if (const ServerObject* t = alive(it.args[2]); t && canInteract(world, o, *t)) {
                follow(t);
            }
            break;
        default:
            break;
        }
    }
    hear(fight, o);
    if (!(o.flags & ObjFlag::Enabled) || !m.def) {
        return;
    }
    m.stackChanged = false;
    // Hurt this frame: its hurt sound and IsHit script.
    if (!(o.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
        if (m.status & ms::Injured) {
            playCreatureSound(world, o, things::SoundSlot::Injured);
            if (world.script) {
                world.script->callBlock(m.onIsHit, o.hurtBy, o.extent);
            }
        }
        if (m.hurtSoundAt != 0 && 30u <= fight.frame - m.hurtSoundAt) {
            // A second after its blow landed (monsterPlayHurtSound): its taunt,
            // at most every 2-4 s.
            m.hurtSoundAt = 0;
            if (m.tauntSoundAt <= fight.frame) {
                m.tauntSoundAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30 * 2, 30 * 4)(fight.rng));
                playCreatureSound(world, o, things::SoundSlot::Taunt);
            }
        }
        // Its idle sound (mobAction): idle or guarding, no enemy, one within
        // 300, every 20-60 s.
        if (!atWill(m) && m.enemy == 0 && m.nearestPlayer <= 300.0f) {
            const int a = m.stack[static_cast<std::size_t>(m.top)].action;
            if ((a == Idle || a == MonsterAction::Guard) && m.idleSoundAt <= fight.frame) {
                m.idleSoundAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30 * 20, 30 * 60)(fight.rng));
                playCreatureSound(world, o, things::SoundSlot::Idle);
            }
        }
    }
    // A point back every 180 s / its most, once a second unhurt.
    if (o.maxHealth != 0 && !(o.flags & ObjFlag::Dead) && 30u < fight.frame - o.hurtFrame && o.health < o.maxHealth) {
        const std::uint32_t every = (30u * 180u) / o.maxHealth;
        if (every != 0 && fight.frame % every == 0) {
            adjustHealth(world, o, 1);
        }
    }
    look(o);
    think(o);
    checkDependencies(o);
    animate(world, o); // (updateNPCAnimData, or its own animation)
    // The top action: its start once (again for each new top a start leaves), then its update.
    const bool changed = m.stackChanged;
    int action = 0;
    for (;;) {
        Item& top = m.stack[static_cast<std::size_t>(m.top)];
        action = top.action;
        if (top.started) {
            break;
        }
        top.started = true;
        m.stackChanged = false;
        start(o, action);
        if (!m.stackChanged) {
            break;
        }
    }
    if (!m.stackChanged) {
        m.stackChanged = changed;
    }
    act(o, action);
    m.status &= ~ms::Injured;
    // monsterPolygonEnter: moved (or not looked up yet), the polygon it's in (first
    // any, then only those with a script); out of them, the last one's leave script
    // and none; into another, the last one's leave and the new one's enter (not on the first
    // look). Caller: it.
    if (m.polygon == kPolygonUnknown || o.x != o.lastX || o.y != o.lastY) {
        const int px = static_cast<int>(o.x);
        const int py = static_cast<int>(o.y);
        const std::uint32_t now = m.polygon == kPolygonUnknown ? world.polygonAt(px, py, 0) : world.polygonAt(px, py, m.polygon, true);
        const auto run = [&](std::uint32_t index, bool enter) {
            if (index != 0 && index <= world.polygons().size() && world.script) {
                MapPolygon& poly = world.polygons()[index - 1];
                world.script->callBlock(enter ? poly.onEnter : poly.onLeave, o.extent, 0);
            }
        };
        if (now == 0) {
            if (m.polygon != 0 && m.polygon != kPolygonUnknown) {
                run(m.polygon, false);
                m.polygon = 0;
            }
        } else if (m.polygon != now) {
            if (m.polygon != kPolygonUnknown) {
                run(m.polygon, false);
                run(now, true);
            }
            m.polygon = now;
        }
    }
    if (m.stamina < 100) {
        m.stamina = static_cast<std::uint8_t>(m.stamina + 100 / 30);  // 3 a frame (100 / 30), to 100
    }
    if (isMimic(o)) {
        // monsterMimicCheckMorph: idle, or guarding within 8 of its spot, a mimic
        // not a chest, its stack a second unchanged, becomes one; doing anything else (but
        // turning back) a chest turns back. Uninterruptable either way; its sound (0x1CC).
        const Item& top = m.stack[static_cast<std::size_t>(m.top)];
        const auto near = [&] {
            const float dx = floatOf(top.args[0]) - o.x;
            const float dy = floatOf(top.args[1]) - o.y;
            return dx * dx + dy * dy <= 64.0f;
        };
        int morph = -1;
        if (top.action == MonsterAction::Idle || (top.action == MonsterAction::Guard && near())) {
            if (!(m.status & ms::Morphed) && 30u < fight.frame - m.actionFrame) {
                morph = MonsterAction::MorphIntoChest;
            }
        } else if (top.action != MonsterAction::MorphBackToSelf && (m.status & ms::Morphed)) {
            morph = MonsterAction::MorphBackToSelf;
        }
        if (morph >= 0) {
            push(o, dep::Uninterruptable);
            push(o, morph);
            playSound(world, 0x1CC, o);
        }
    }
    if (30u * 3u < fight.frame - o.hurtFrame) {  // three seconds unhurt
        m.status &= ~ms::OnFire;
    }
}

void Monsters::lostSight(ServerObject& o, int index)
{
    // aiLostSight.
    auto& m = *o.monster;
    const std::uint32_t t = m.seen[static_cast<std::size_t>(index)];
    if (fight.world.script) {
        fight.world.script->callBlock(m.onLostEnemy, t, o.extent);
    }
    if (t == m.enemy) {
        m.lastEnemy = m.enemy;
        m.enemy = 0;
    }
    --m.seenCount;
    for (int i = index; i < m.seenCount; ++i) {
        m.seen[static_cast<std::size_t>(i)] = m.seen[static_cast<std::size_t>(i) + 1];
    }
}

void Monsters::sees(ServerObject& o, ServerObject& t)
{
    // monsterVisionSeeEnemy: a full list gives up its farthest for a nearer one.
    World& world = fight.world;
    auto& m = *o.monster;
    if (m.seenCount == 16) {
        // (Each compared from the x87's stack; the farthest kept as its stored float.)
        float far = 0;
        std::uint32_t farthest = 0;
        for (int i = 0; i < 16; ++i) {
            if (const ServerObject* s = world.byExtent(m.seen[static_cast<std::size_t>(i)])) {
                const X87 d = dist2(o, s->x, s->y);
                if (X87(far) < d) {
                    far = d.f();
                    farthest = s->extent;
                }
            }
        }
        if (X87(far) <= dist2(o, t.x, t.y)) {
            return;
        }
        for (int i = 0; i < m.seenCount; ++i) {
            if (m.seen[static_cast<std::size_t>(i)] == farthest) {
                lostSight(o, i);
                break;
            }
        }
    }
    m.seen[m.seenCount++] = t.extent;
    if (m.sightedSoundAt < fight.frame && isEnemy(world, o, t) && (!isZombie(o) || !(o.flags & ObjFlag::Dead))) {
        playCreatureSound(world, o, things::SoundSlot::Recognize); // "enemy sighted"
        m.sightedSoundAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(60, 120)(fight.rng));
    }
    if (world.script) {
        world.script->callBlock(m.onEnemySighted, t.extent, o.extent);
    }
}

void Monsters::chooseEnemy(ServerObject& o)
{
    // The one it keeps to while it sees it, else the nearest killable enemy seen.
    World& world = fight.world;
    auto& m = *o.monster;
    m.enemy = 0;
    float best = 1e8f;
    for (int i = 0; i < m.seenCount; ++i) {
        const std::uint32_t e = m.seen[static_cast<std::size_t>(i)];
        if (e == m.focus) {
            m.enemy = m.focus;
            return;
        }
        const ServerObject* t = world.byExtent(e);
        if (t && isEnemy(world, o, *t) && killable(*t)) {
            const X87 d = dist2(o, t->x, t->y); // (compared from the stack, kept stored)
            if (d < X87(best)) {
                m.enemy = e;
                best = d.f();
            }
        }
    }
}

void Monsters::look(ServerObject& o)
{
    // unitUpdateSightMB. Its reach: its sight, at least 250 (a quest's 640); it
    // keeps what it sees to 30 past that.
    World& world = fight.world;
    auto& m = *o.monster;
    if ((o.flags & ObjFlag::Dead) && !isZombie(o)) {
        return;
    }
    float r = world.quest() ? 640.0f : 250.0f;
    if (r < m.sightRange) {
        r = m.sightRange;
    }
    // (r + 30 squared on the x87's stack, stored to a float.)
    const X87 keep = X87(r) + 30.0f;
    const float keepSq = (keep * keep).f();
    // (Its periods by the game's frame rate, gameFPS: a constant 30 in the game as in ours.)
    constexpr std::uint32_t kTwoSeconds = static_cast<std::uint32_t>(kFrameRate) * 2;
    const bool recheck = kTwoSeconds < fight.frame - m.reachCheckedAt;
    if (recheck) {
        m.reachCheckedAt = fight.frame;
    }
    // What it saw it loses: gone or dead, out of its sight or reach, or when it was itself
    // moved far (over sqrt 1000) in a frame; and every 2 s, what it can't reach.
    bool changed = false;
    const X87 mx = X87(o.x) - o.lastX;
    const X87 my = X87(o.y) - o.lastY;
    const bool jumped = X87(1000.0f) < mx * mx + my * my;
    for (int i = 0; i < m.seenCount; ++i) {
        ServerObject* t = world.byExtent(m.seen[static_cast<std::size_t>(i)]);
        bool lost = !t || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || !canSee(world, o, *t, 0);
        if (!lost) {
            lost = X87(keepSq) < dist2(o, t->x, t->y) || jumped || (recheck && !canInteract(world, o, *t));
        }
        if (lost) {
            lostSight(o, i);
            --i;
            changed = true;
        }
    }
    if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr; e && enchanted(*e, Enchant::Charming)) {
        changed = true;
    }
    // A fresh look about: with no enemy or every 2 s, when it's due (or a monster went to FIGHT
    // this frame). Creatures in its reach, not dead: enemies (any, with CAN_SEE_FRIENDS) it
    // doesn't see yet, in front of it (the cosine at least 0.5) unless ALERT, it can reach.
    bool choose = changed;
    if ((m.enemy == 0 || kTwoSeconds < fight.frame - m.lookedAt) &&(m.nextLook <= fight.frame || fight.frame == world.monstersLookAt)) {
        inCircle(world, o.x, o.y, r, [&](ServerObject* t) {
            if (t == &o || !creature(*t) || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || t->pendingDelete) { // (0x8020)
                return;
            }
            if (!(m.status & ms::CanSeeFriends) && !isEnemy(world, o, *t)) {
                return;
            }
            if (std::find(m.seen.begin(), m.seen.begin() + m.seenCount, t->extent) != m.seen.begin() + m.seenCount) {
                return;
            }
            if (!(m.status & ms::Alert) && !world.monstersAlert) { // (noxcpp, a mode's: all about)
                // (On the x87: dx kept on the stack; dy stored, squared from the stack by the
                // stored one; the root + 0.001 stored; the cosine on the stack.)
                const X87 dx = X87(t->x) - o.x;
                const X87 dyStack = X87(t->y) - o.y;
                const float dy = dyStack.f();
                const float l = (util::sqrt(dyStack * dy + dx * dx) + 0.001f).f();
                const auto [fx, fy] = directionVector(o.direction);
                if (dx / l * fx + X87(dy) / l * fy < X87(0.5f)) {
                    return;
                }
            }
            if (canInteract(world, o, *t)) {
                sees(o, *t);
            }
        });
        m.lookedAt = fight.frame;
        m.reachCheckedAt = fight.frame;
        choose = true;
    }
    if (choose) {
        const std::uint32_t before = m.enemy;
        chooseEnemy(o);
        if (m.enemy != 0 && before != 0 && before != m.enemy) {
            m.lastEnemy = before;
        }
    }
    // When it next looks: soon (5-10 frames) with an enemy about; else by the nearest enemy
    // creature within 1000 (outside a network game): 5 s with none, sooner the nearer.
    if (m.lookedAt == fight.frame) {
        if (!(m.status & ms::CanSeeFriends) && !world.network() && m.enemy == 0) {
            constexpr int kFiveSeconds = kFrameRate * 5; // (gameFPS x 5)
            const float d = nearestEnemy(world, o).f(); // (stored)
            m.nearestPlayer = d;
            if (d < 0.0f) {
                m.nextLook = fight.frame + kFiveSeconds;
            } else if (d <= r) {
                m.nextLook = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(5, 10)(fight.rng));
            } else {
                // (On the x87: x gameFPS x 5 by FIMUL, / (1000 - r), through __ftol.)
                const X87 wait = (X87(d) - r) * X87::fromDouble(kFiveSeconds) / (X87(1000.0f) - r);
                m.nextLook = fight.frame + static_cast<std::uint32_t>(util::ftol(wait)) + 10;
            }
        } else {
            m.nextLook = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(5, 10)(fight.rng));
        }
    }
}

void Monsters::popAttacks(ServerObject& o)
{
    // monsterPopAttackActions: the attacks and turns on top.
    auto& m = *o.monster;
    for (;;) {
        const int a = m.stack[static_cast<std::size_t>(m.top)].action;
        if ((0x10 <= a && a <= 0x14) || (0x19 <= a && a <= 0x1C)) {  // melee, missile and the casts; the face / set-angle turns
            pop(o);
        } else {
            return;
        }
    }
}

void Monsters::think(ServerObject& o)
{
    // monsterMainAIFn: idle or guarding, every 16th frame (by its code).
    World& world = fight.world;
    auto& m = *o.monster;
    const int top = m.stack[static_cast<std::size_t>(m.top)].action;
    if ((top == Idle || top == MonsterAction::Guard) && ((o.extent - m.actionFrame + fight.frame) & 0xFu) != 0) {
        return;
    }
    // Nothing while an UNINTERRUPTABLE is under the top.
    for (int i = m.top - 1; 0 <= i; --i) {
        if (m.stack[static_cast<std::size_t>(i)].action == dep::Uninterruptable) {
            return;
        }
    }
    if (o.flags & ObjFlag::Dead) {
        return;
    }
    // In a solo game, not in a widescreen (the host's client's), one that talks
    // (xStatus 0x10), not waiting already (action 2), with the host's (updating) cursor within
    // 10 of it and on it (findObjectAtCursor): it stops and waits a second, then
    // while under the cursor, facing the host; its Listen sound unless the host is talking or
    // trading. Nothing else this time.
    if (ServerObject* host = (world.gameFlags & GameFlag::Solo) && (o.xStatus & 0x10u) ? hostPlayer(world) : nullptr;
        host && !(world.hostClient && world.hostClient->wideScreenOn()) && !(host->flags & ObjFlag::NoUpdate) &&
        host->player && !has(o, MonsterAction::WaitRelative)) {
        // (its cursor, ints, FILD less the position, squared, against 100.0f)
        const double dx = static_cast<double>(static_cast<int>(host->player->cursorX)) - static_cast<double>(o.x);
        const double dy = static_cast<double>(static_cast<int>(host->player->cursorY)) - static_cast<double>(o.y);
        if (dx * dx + dy * dy < 100.0 && objectAtCursor(world, *host) == &o) {
            o.lastX = o.x;
            o.lastY = o.y;
            o.vx = o.vy = 0;
            o.forceX = o.forceY = 0;
            o.pushX = o.pushY = 0;
            // (The last pushed runs first: it faces the host, waits while under the cursor, then a
            // second more while it isn't moved.)
            push(o, dep::NotMoved);
            if (Item* it = push(o, MonsterAction::WaitRelative)) {
                it->args[0] = static_cast<std::uint32_t>(kFrameRate); // (gameFPS)
            }
            push(o, dep::UnderCursor);
            if (Item* it = push(o, MonsterAction::WaitRelative)) {
                it->args[0] = 999999;
            }
            if (Item* it = push(o, MonsterAction::FaceObject)) {
                it->args[0] = host->extent;
            }
            if (host->player->dialogWith == 0 && !trading(*host)) { // (not talking nor trading)
                playCreatureSound(world, o, things::SoundSlot::Listen); // (its sound set's)
            }
            return;
        }
    }
    if (enchanted(o, Enchant::Confused) && !has(o, MonsterAction::Confused)) {
        if (Item* it = push(o, dep::IsEnchanted)) {
            it->args[0] = Enchant::Confused;
        }
        push(o, MonsterAction::Confused);
    }
    // A caster with a spell coming at it counters it (mobCastInversion).
    if (!enchanted(o, Enchant::AntiMagic) && counterSpell(o)) {
        return;
    }
    if (enchanted(o, Enchant::Afraid) && canMove(o) && !has(o, MonsterAction::Flee)) {
        // Afraid: it flees while it lasts.
        if (Item* it = push(o, dep::IsEnchanted)) {
            it->args[0] = Enchant::Afraid;
        }
        if (Item* it = push(o, MonsterAction::Flee)) {
            it->args[0] = bitsOf(o.x);
            it->args[1] = bitsOf(o.y);
            it->args[2] = 0;
        }
        playCreatureSound(world, o, things::SoundSlot::Flee);
        return;
    }
    // Guarding but off its post, not fighting: an enemy in its reach within 100 makes it as
    // if hurt by its enemy.
    if ((fight.frame & 0xFu) == 0 && (defends(m) || atWill(m)) && has(o, MonsterAction::Guard) && top != MonsterAction::Guard &&
        !has(o, MonsterAction::Fight)) {
        if (const ServerObject* a = aggroTarget(world, o, 100.0f); a && creature(*a)) {
            m.status |= ms::Injured;
            o.hurtBy = m.enemy;
            o.hurtType = 0xB;
            o.hurtFrame = fight.frame;
        }
    }
    // An enemy within its flee range: it turns about and flees (out of 30 past the range, or
    // cornered); a caster with Blink (spell 4), within half of it, blinks away instead.
    ServerObject* enemy = m.enemy ? world.byExtent(m.enemy) : nullptr;
    if (!cowardly(m) && canMove(o) && !casting(m) && !enchanted(o, Enchant::Confused) && !cornered(m, fight.frame) && enemy) {
        const float d = edgeDistance(o, *enemy);
        if (d < m.fleeRange && (m.status & ms::CanCastSpells) && m.spells[4] != 0 && !enchanted(o, Enchant::AntiMagic) &&
            m.spellAt[4] <= fight.frame && d < m.fleeRange * 0.5f) {
            castAs(o, 4, &o, o.x, o.y);
            m.spellAt[4] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[8], m.spellDelays[9])(fight.rng));
            return;
        }
        if (d < m.fleeRange && !has(o, MonsterAction::Flee) && m.fleeRange != 0.0f) {
            if (Item* it = push(o, MonsterAction::SetAngle)) {
                it->args[0] = static_cast<std::uint32_t>(static_cast<std::int16_t>(o.direction) + 0x80);
            }
            push(o, dep::NotCornered);
            if (Item* it = push(o, dep::EnemyCloserThan)) {
                it->args[0] = bitsOf(m.fleeRange + 30.0f);
            }
            if (Item* it = push(o, MonsterAction::Flee)) {
                it->args[0] = bitsOf(enemy->x);
                it->args[1] = bitsOf(enemy->y);
                it->args[2] = 0;
            }
            if (nox::util::uniform_int_distribution<int>(0, 1)(fight.rng) != 0) { // (half the time)
                playCreatureSound(world, o, things::SoundSlot::Flee);
            }
            return;
        }
    }
    // Hurt to its retreat level (or a caster under ANTI_MAGIC): the attacks dropped, it retreats
    // (a summoned one in the solo game to its master).
    if (o.maxHealth != 0 && canMove(o) && !cornered(m, fight.frame) && !has(o, MonsterAction::Flee) && !has(o, MonsterAction::Retreat) &&
        !has(o, MonsterAction::RetreatToMaster)) {
        const bool spent = (m.status & ms::CanCastSpells) && enchanted(o, Enchant::AntiMagic);
        // (The main AI's ratio: FILD, FIDIV, stored to a float, then compared: unlike
        // monsterCanResumeAttack's, which compares the stack's.)
        if (healthRatio(o).f() <= m.retreatLevel || spent) {
            popAttacks(o);
            push(o, dep::NotCornered);
            const bool toMaster = ((m.status & ms::Summoned) || (o.subclasses & 0x80u)) && (world.gameFlags & GameFlag::Solo);
            push(o, toMaster ? MonsterAction::RetreatToMaster : MonsterAction::Retreat);
            playCreatureSound(world, o, things::SoundSlot::Retreat);
            if (world.script) {
                world.script->callBlock(m.onRetreat, 0, o.extent);
            }
            return;
        }
    }
    // A missile coming at it (monsterTestBlockShield), not while attacking: an
    // NPC with a weapon that blocks (0x400) waits a second; one with a shield (CAN_BLOCK; an
    // NPC's worn shield) blocks; in the solo game one that can dodge (monster.bin's CAN_DODGE),
    // not timid or confused, steps aside.
    const bool attacking = top == MonsterAction::MeleeAttack || top == MonsterAction::MissileAttack;
    if ((o.subclasses & 0x10u) && (m.weaponFlags & 0x400u) && !attacking && incoming(o)) {
        if (top != MonsterAction::Wait && top != MonsterAction::WeaponBlock) {
            popAttacks(o);
            if (Item* it = push(o, MonsterAction::Wait)) {
                it->args[0] = fight.frame + 30;
            }
        }
        return;
    }
    // (monsterHasShield: an NPC's worn shield bits)
    const bool shield = (o.subclasses & 0x10u) ? (m.armorFlags & 0x3000000u) != 0 : (m.status & ms::CanBlock) != 0;
    if (shield && !attacking && incoming(o)) {
        if (!has(o, MonsterAction::BlockAttack)) {
            popAttacks(o);
            if (Item* it = push(o, MonsterAction::BlockAttack)) {
                it->args[0] = fight.frame + 15;
            }
        }
        return;
    }
    if ((world.gameFlags & GameFlag::Solo) && !cowardly(m) && !enchanted(o, Enchant::Confused) && m.def && (m.def->status & ms::CanDodge) &&
        !has(o, MonsterAction::Dodge) && incoming(o) && dodge(o)) {
        return;
    }
    // Stuck while moving: within 15 of where it was for half a second it's FRUSTRATED (a
    // fleeing one cornered too); not fighting, it waits a while (or, one time in three, steps
    // aside if it can); fighting, it steps aside if it can.
    if (top == MonsterAction::MoveTo || top == MonsterAction::MoveToHome || top == MonsterAction::FarMoveTo || top == MonsterAction::Roam ||
        top == MonsterAction::Flee) {
        if (225.0f < dist2(o, m.stuckX, m.stuckY)) {
            m.field1f0 = fight.frame;
            m.stuckX = o.x;
            m.stuckY = o.y;
        } else if (15u < fight.frame - m.field1f0) {
            m.status |= ms::Frustrated;
            if (has(o, MonsterAction::Retreat) || has(o, MonsterAction::RetreatToMaster) || has(o, MonsterAction::Flee)) {
                m.movedAwayAt = fight.frame;
            }
            if (!has(o, MonsterAction::Fight)) {
                if (32 < nox::util::uniform_int_distribution<int>(0, 100)(fight.rng) || !dodge(o)) {
                    if (Item* it = push(o, MonsterAction::Wait)) {
                        it->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(15, 60)(fight.rng));
                    }
                }
            } else {
                dodge(o);
            }
            m.field1f0 = fight.frame;
            m.stuckX = o.x;
            m.stuckY = o.y;
            return;
        }
    }
    // Hurt and not cowardly, every 16th frame it takes food within 75 (Obj_TryPickup: its
    // PICKUP, food eaten as it's taken), a health potion or mushroom used (Obj_UseItem) as well.
    // (A BOT, monster status 0x20000, morphed into its player record (mobMorphToPlayer) picks
    // up what finds within 75: not here, none of the campaign's NPCs is one.)
    if (!cowardly(m) && o.maxHealth != 0 && o.health < o.maxHealth && (fight.frame & 0xFu) == 0 && fight.things) {
        if (ServerObject* food = edible(world, o, 75.0f)) {
            Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
            items.pickUp(o, *food, true);
            if (food->subclasses & 0x90u) {  // a health potion or mushroom
                items.use(o, *food);
            }
        }
    }
}

void Monsters::checkDependencies(ServerObject& o)
{
    // From the top down, each action's conditions under it; one failing pops
    // the actions down to it.
    World& world = fight.world;
    auto& m = *o.monster;
    const auto object = [&](std::uint32_t extent) -> ServerObject* {
        ServerObject* t = extent ? world.byExtent(extent) : nullptr;
        return t && !t->gone() ? t : nullptr;
    };
    // An OR makes the conditions between it and the next action alternatives: any one
    // holding keeps the group.
    bool orGroup = false;
    int orHolds = 0;
    for (int i = m.top; 0 <= i; --i) {
        Item& it = m.stack[static_cast<std::size_t>(i)];
        if (!isCondition(it.action)) {
            orGroup = false;
            continue;
        }
        int holds = 1;
        ServerObject* enemy = object(m.enemy);
        switch (it.action) {
        case dep::Or:
            orGroup = true;
            orHolds = 0;
            continue;
        case dep::Uninterruptable:  // the rest of the stack is left alone this frame
            return;
        case dep::Time:
            if (it.args[0] <= fight.frame) {  // args[0]: the frame it lasts till
                holds = 0;
            }
            break;
        case dep::Alive: {
            const ServerObject* t = object(it.args[0]);
            if (!t || !creature(*t) || (t->health == 0 && t->maxHealth != 0)) {
                holds = 0;
                // Its target gone or dead: what it heard is forgotten, and hear() skips every sound for a
                // second (it ignores sounds while the frame is before heardAt).
                m.heard = 0;
                m.heardAt = fight.frame + 30;
            }
            break;
        }
        case dep::UnderAttack:
            // Each new blow from a creature restarts the count; 10 s (300 frames) without one and it fails.
            if ((m.status & ms::Injured) && creatureOf(world, o.hurtBy)) {
                it.args[0] = fight.frame;
            }
            if (300u < fight.frame - it.args[0]) {
                holds = 0;
            }
            break;
        case dep::NotUnderAttack:
            if ((m.status & ms::Injured) && creatureOf(world, o.hurtBy)) {
                holds = 0;
            }
            break;
        case dep::CanSee: {
            const ServerObject* t = object(it.args[0]);
            if (!t || !canInteract(world, o, *t)) {
                holds = 0;
            }
            break;
        }
        case dep::CannotSee: {
            // (in reach and in front, the cosine over 0.5)
            const ServerObject* t = object(it.args[0]);
            if (!t) {
                holds = 0;
            } else if (canInteract(world, o, *t)) {
                // (On the x87: dx kept on the stack; dy stored, squared from the stack by the
                // stored one; the root + 0.001 stored; the cosine on the stack.)
                const X87 dx = X87(t->x) - o.x;
                const X87 dyStack = X87(t->y) - o.y;
                const float dy = dyStack.f();
                const float l = (util::sqrt(dyStack * dy + dx * dx) + 0.001f).f();
                const auto [fx, fy] = directionVector(o.direction);
                if (X87(0.5f) < dx / l * fx + X87(dy) / l * fy) {
                    holds = 0;
                }
            }
            break;
        }
        case dep::BlockedLineOfFire: {
            // Something of its own side or a fixed obstacle across the line to it
            // (mapTraceObstacles).
            const ServerObject* t = object(it.args[0]);
            if (!t || lineClear(world, o, o.x, o.y, t->x, t->y)) {
                holds = 0;
            }
            break;
        }
        case dep::ObjectAtVisibleLocation: {
            // It must see the object while it sees where it was.
            bool seen = false;
            if (const ServerObject* t = object(it.args[2]); t && canInteract(world, o, *t)) {
                seen = true;
                it.args[0] = bitsOf(t->x);
                it.args[1] = bitsOf(t->y);
            }
            if (world.traceRay(o.x, o.y, floatOf(it.args[0]), floatOf(it.args[1]), 1) && !seen) {  // ray flags 1: closed doors block
                holds = 0;
            }
            break;
        }
        case dep::ObjectFartherThan: {
            const ServerObject* t = object(it.args[2]);
            if (!t || edgeDistance53(o, *t) <= X87(floatOf(it.args[0]))) {
                holds = 0;
            }
            break;
        }
        case dep::ObjectCloserThan: {
            const ServerObject* t = object(it.args[2]);
            if (!t || X87(floatOf(it.args[0])) < edgeDistance53(o, *t)) {
                holds = 0;
            }
            break;
        }
        case dep::LocationFartherThan:
            if (util::sqrt(dist2(o, floatOf(it.args[2]), floatOf(it.args[3]))) < X87(floatOf(it.args[0]))) {
                holds = 0;
            }
            break;
        case dep::LocationCloserThan:
            if (X87(floatOf(it.args[0])) < util::sqrt(dist2(o, floatOf(it.args[2]), floatOf(it.args[3])))) {
                holds = 0;
            }
            break;
        case dep::VisibleEnemy:
            if (!enemy) {
                holds = 0;
            }
            break;
        case dep::NoVisibleEnemy:
            if (enemy) {
                holds = 0;
            }
            break;
        case dep::NoVisibleFood:
            if (edible(world, o, world.quest() ? 640.0f : 250.0f)) {
                holds = 0;
            }
            break;
        case dep::NoInterestingSound:
            if (m.heard != 0 && fight.frame - m.heardAt < 90u) {  // heard within 3 s
                holds = 0;
            }
            break;
        case dep::NoNewEnemy: {
            const ServerObject* t = object(it.args[0]);
            // Fails once its enemy is nearer than the one it was after (a new, nearer enemy).
            if (!t || (enemy && dist2(o, enemy->x, enemy->y) < dist2(o, t->x, t->y))) {
                holds = 0;
            }
            break;
        }
        case dep::IsEnchanted:
            holds = enchanted(o, static_cast<int>(it.args[0])) ? 1 : 0;
            break;
        case dep::EnemyCloserThan:
            if (!enemy || X87(floatOf(it.args[0])) < edgeDistance53(o, *enemy)) {
                holds = 0;
            }
            break;
        case dep::NotHealthy: {
            if (X87(m.resumeLevel) <= healthRatio(o)) {
                holds = 0;
            }
            break;
        }
        case dep::WaitForStamina:
            if (weaponStamina(m.weaponFlags) <= static_cast<int>(m.stamina)) {
                holds = 0;
            }
            break;
        case dep::EnemyFartherThan:
            if (enemy && edgeDistance53(o, *enemy) < X87(floatOf(it.args[0]))) {
                holds = 0;
            }
            break;
        case dep::UnderCursor: { // (the host's own unit there, and its cursor on this one)
            ServerObject* host = hostPlayer(world);
            if (!host || objectAtCursor(world, *host) != &o) {
                holds = 0;
            }
            break;
        }
        case dep::NotCornered:
            if (cornered(m, fight.frame)) {
                holds = 0;
            }
            break;
        case dep::LocationIsSafe:
            if (!safeAt(world, o, floatOf(it.args[0]), floatOf(it.args[1]))) {
                holds = 0;
            }
            break;
        case dep::NotFrustrated:
            if (m.status & ms::Frustrated) {
                holds = 0;
                m.status &= ~ms::Frustrated;
            }
            break;
        case dep::NotMoved:
            if (o.x != o.lastX || o.y != o.lastY) {
                holds = 0;
            }
            break;
        default: // (VISIBLE_FRIEND, VISIBLE_FOOD, NO_VISIBLE_FRIEND: always)
            break;
        }
        // In an OR group the verdict waits for the group's last condition.
        if (orGroup) {
            if (holds == 1) {
                orHolds = 1;
            }
            holds = orHolds;
            if (i != 0 && isCondition(m.stack[static_cast<std::size_t>(i) - 1].action)) {
                continue;
            }
        }
        // Failed: actions popped till the stack is below this condition.
        if (holds == 0) {
            do {
                pop(o);
            } while (i <= m.top && !loneIdle(m));
            i = m.top;
            freshTop(m, fight.frame);
        }
    }
}

bool Monsters::lookAtDamager(ServerObject& o)
{
    // monsterLookAtDamager: hurt this frame, it faces where the blow came from.
    if (!(o.monster->status & ms::Injured)) {
        return false;
    }
    if (Item* it = push(o, MonsterAction::FaceLocation)) {
        it->args[0] = bitsOf(o.hurtFromX);
        it->args[1] = bitsOf(o.hurtFromY);
    }
    return true;
}

bool Monsters::reactToHurt(ServerObject& o)
{
    // A blow within 10 s it hasn't answered. From a creature, an enemy: it fights
    // it (while under attack; a plant only while it's within its sight, if it can reach it now),
    // going where it is when it can't reach it. From nothing it knows: it roams looking (while
    // no enemy is seen, for "5 s", an original bug).
    World& world = fight.world;
    auto& m = *o.monster;
    const std::uint32_t when = o.hurtFrame;
    if (when <= m.hurtHandledAt || 300u <= fight.frame - when) {  // answered already, or over 10 s ago
        return false;
    }
    m.hurtHandledAt = when;
    if (ServerObject* b = creatureOf(world, o.hurtBy)) {
        if (!isEnemy(world, o, *b)) {
            return false;
        }
        const bool reach = canInteract(world, o, *b);
        if (!isPlant(o)) {
            if (Item* it = push(o, dep::UnderAttack)) {
                it->args[0] = fight.frame;
            }
        } else {
            if (!reach) {
                return false;
            }
            if (Item* it = push(o, dep::EnemyCloserThan)) {
                it->args[0] = bitsOf(m.sightRange * 1.05f);
            }
        }
        pushFight(o, *b, fight.frame);
        if (!reach) {
            push(o, dep::NoVisibleEnemy);
            if (atWill(m)) {
                push(o, dep::NoInterestingSound);
            }
            if (Item* it = push(o, MonsterAction::MoveTo)) {
                it->args[0] = bitsOf(b->x);
                it->args[1] = bitsOf(b->y);
                it->args[2] = 0;
            }
        }
        return true;
    }
    if (!has(o, MonsterAction::Roam)) {
        if (Item* it = push(o, dep::Time)) {
            it->args[0] = 30u * 5u; // (a length where a frame belongs)
        }
        push(o, dep::NoVisibleEnemy);
        if (atWill(m)) {
            push(o, dep::NoInterestingSound);
        }
        if (Item* it = push(o, MonsterAction::Roam)) {
            it->args[0] = 0;
            it->args[2] = (it->args[2] & ~0xFFu) | 0x80u;  // roaming waypoints (link flag 0x80)
        }
    }
    return false;
}

bool Monsters::chaseSound(ServerObject& o)
{
    // A sound heard within 3 s. (Going to it, the path finder's check and the
    // hearing itself: with the hearing.)
    auto& m = *o.monster;
    if (m.heard == 0 || 90u <= fight.frame - m.heardAt) {
        return false;
    }
    m.heard = 0;
    return true;
}

bool Monsters::faceTowards(ServerObject& o, float x, float y)
{
    // 8 of 256 a frame towards the point, the shorter way; done (popped) once
    // its facing is within the cosine 0.9 of it.
    // (On the x87: dx kept on the stack; dy stored, squared from the stack by the stored one; the
    // root + 0.001 stored; x's share stored, y's stored but its stack value in the cross product.)
    auto& m = *o.monster;
    const X87 dx = X87(x) - o.x;
    const X87 dyStack = X87(y) - o.y;
    const float dy = dyStack.f();
    const float l = (util::sqrt(dyStack * dy + dx * dx) + 0.001f).f();
    const float ux = (dx / l).f();
    const X87 uyStack = X87(dy) / l;
    const float uy = uyStack.f();
    const auto [fx, fy] = directionVector(o.direction);
    int d = m.turnTo >= 0 ? m.turnTo : o.direction;
    d += X87(0.0f) <= uyStack * fx - X87(ux) * fy ? 8 : -8;  // the cross product's sign picks the shorter way
    m.turnTo = wrap256(d);
    if (facing(o, ux, uy)) {
        pop(o);
        return true;
    }
    return false;
}

void Monsters::idle(ServerObject& o)
{
    // Its LookingForEnemy script at its moment; enabled and not timid, an enemy seen
    // brings a FIGHT (or a sound heard, a look); a blow answered; else it faces a blow, looks
    // back where it was pushed from, or heals (with the spells).
    World& world = fight.world;
    auto& m = *o.monster;
    const Item& self = m.stack[static_cast<std::size_t>(m.top)];
    if (fight.frame - self.args[0] == static_cast<std::uint32_t>(static_cast<int>(m.lookDelay)) && world.script) {
        world.script->callBlock(m.onLookingForEnemy, 0, o.extent);
    }
    if (o.flags & ObjFlag::Enabled) {
        if (defends(m) || atWill(m)) {
            if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
                pushFight(o, *e, fight.frame);
                return;
            }
            if (chaseSound(o)) {
                return;
            }
        }
        if (!cowardly(m) && reactToHurt(o)) {
            return;
        }
    }
    if (!isMimic(o)) {
        if (lookAtDamager(o)) {
            return;
        }
        if (15u < fight.frame - m.actionFrame && (o.x != o.lastX || o.y != o.lastY)) {
            if (Item* it = push(o, MonsterAction::FaceLocation)) {
                it->args[0] = bitsOf(o.lastX);
                it->args[1] = bitsOf(o.lastY);
            }
            return;
        }
    }
    if (!enchanted(o, Enchant::AntiMagic)) {
        healSomeone(o);
    }
}

void Monsters::guard(ServerObject& o)
{
    // mobActionGuard: its post (args 0-1) and facing (arg 2).
    World& world = fight.world;
    auto& m = *o.monster;
    const int self = m.top;
    const auto post = [&]() -> const Item& { return m.stack[static_cast<std::size_t>(self)]; };
    if (!cowardly(m) && reactToHurt(o)) {
        return;
    }
    if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
        if (atWill(m)) {
            pushFight(o, *e, fight.frame);
            return;
        }
        // Defending: an enemy near its post, within its sight; it fights while under attack or
        // within 1.5 times that of its post (a plant while the enemy's within reach).
        const float px = floatOf(post().args[0]);
        const float py = floatOf(post().args[1]);
        // (Both squares on the x87's stack, compared there.)
        const X87 ex = X87(px) - e->x;
        const X87 ey = X87(py) - e->y;
        if (defends(m) && ex * ex + ey * ey < X87(m.sightRange) * m.sightRange) {
            if (!isPlant(o)) {
                if (Item* it = push(o, dep::UnderAttack)) {
                    it->args[0] = 0;
                }
                if (Item* it = push(o, dep::LocationCloserThan)) {
                    it->args[0] = bitsOf(m.sightRange * 1.5f);
                    it->args[2] = bitsOf(px);
                    it->args[3] = bitsOf(py);
                }
                push(o, dep::Or);  // either condition keeps the FIGHT going
            } else if (Item* it = push(o, dep::EnemyCloserThan)) {
                it->args[0] = bitsOf(m.sightRange * 1.05f);
            }
            pushFight(o, *e, fight.frame);
        }
    }
    if (!isMimic(o) && lookAtDamager(o)) {
        return;
    }
    if (!atWill(m)) {
        if (!isMimic(o)) {
            // A sound heard within 3 s: it looks that way a while.
            if (m.heard != 0 && fight.frame - m.heardAt < 90u) {
                push(o, dep::NoInterestingSound);
                push(o, dep::NoVisibleEnemy);
                if (Item* it = push(o, MonsterAction::Wait)) {
                    it->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30, 60)(fight.rng));
                }
                if (Item* it = push(o, MonsterAction::FaceLocation)) {
                    it->args[0] = bitsOf(m.heardX);
                    it->args[1] = bitsOf(m.heardY);
                }
                m.heard = 0;
                return;
            }
        }
    } else if (chaseSound(o)) {
        return;
    }
    // Every 16th frame (by its code): off its post (by over 8) it goes back (while not under
    // attack); on it, it faces its way, or what it sees (its enemy, else the first player
    // seen, else the first seen).
    if (((o.extent + fight.frame) & 0xFu) == 0) {
        const float px = floatOf(post().args[0]);
        const float py = floatOf(post().args[1]);
        if (64.0f < dist2(o, px, py)) {
            if (atWill(m)) {
                push(o, dep::NoVisibleEnemy);
                push(o, dep::NoInterestingSound);
            }
            push(o, dep::NotUnderAttack);
            if (Item* it = push(o, MonsterAction::MoveTo)) {
                it->args[0] = bitsOf(px);
                it->args[1] = bitsOf(py);
                it->args[2] = 0;
            }
            return;
        }
        const int way = static_cast<int>(post().args[2]);
        if (!isMimic(o) && m.seenCount != 0) {
            const ServerObject* t = m.enemy ? world.byExtent(m.enemy) : nullptr;
            if (!t) {
                t = world.byExtent(m.seen[0]);
                for (int i = 0; i < m.seenCount; ++i) {
                    if (const ServerObject* s = world.byExtent(m.seen[static_cast<std::size_t>(i)]); s && (s->classes & oc::Player)) {
                        t = s;
                        break;
                    }
                }
            }
            if (t) {
                // (On the x87: dx stored; dy kept on the stack, squared there; the root + 0.001
                // kept; each share from those, stored.)
                const float dx = (X87(t->x) - o.x).f();
                const X87 dy = X87(t->y) - o.y;
                const X87 l = util::sqrt(dy * dy + X87(dx) * dx) + 0.001f;
                if (!facing(o, (X87(dx) / l).f(), (dy / l).f())) {
                    if (Item* it = push(o, MonsterAction::FaceObject)) {
                        it->args[0] = t->extent;
                    }
                }
            }
        } else {
            const auto [vx, vy] = directionVector(way);
            if (!facing(o, vx, vy)) {
                if (!isMimic(o)) {
                    push(o, dep::NoVisibleEnemy);
                }
                if (Item* it = push(o, MonsterAction::FaceAngle)) {
                    it->args[0] = static_cast<std::uint32_t>(way);
                }
            }
        }
    }
    if (!isMimic(o) && 15u < fight.frame - m.actionFrame && (o.x != o.lastX || o.y != o.lastY)) {
        if (Item* it = push(o, MonsterAction::FaceLocation)) {
            it->args[0] = bitsOf(o.lastX);
            it->args[1] = bitsOf(o.lastY);
        }
    } else if (!enchanted(o, Enchant::AntiMagic)) {
        healSomeone(o);
    }
}

void Monsters::escort(ServerObject& o)
{
    // mobActionEscort: whom it escorts (args 2: found once by name), kept within
    // its escort distance + 30.
    World& world = fight.world;
    auto& m = *o.monster;
    const int self = m.top;
    const auto me = [&]() -> Item& { return m.stack[static_cast<std::size_t>(self)]; };
    if (me().args[2] == 0) {
        // monsterGetObjEscortName: "**PLAYER**" a player at random, "**OWNER**"
        // its owner, else the object of that name; the name then forgotten.
        ServerObject* t = nullptr;
        if (m.escortName == "**PLAYER**") {
            std::vector<ServerObject*> players;
            for (auto& [e, p] : world.objects()) {
                if (p.player && !p.gone()) {
                    players.push_back(&p);
                }
            }
            const int pick = nox::util::uniform_int_distribution<int>(0, static_cast<int>(players.size()) - 1)(fight.rng);
            t = 0 <= pick && pick < static_cast<int>(players.size()) ? players[static_cast<std::size_t>(pick)] : nullptr;
        } else if (m.escortName == "**OWNER**") {
            t = o.owner ? world.byExtent(o.owner) : nullptr;
        } else if (!m.escortName.empty()) {
            t = world.byScriptName(m.escortName);
        }
        m.escortName.clear();
        me().args[2] = t ? t->extent : 0;
        if (!t) {
            pop(o);
            return;
        }
        me().args[0] = bitsOf(t->x);
        me().args[1] = bitsOf(t->y);
    }
    // An enemy or a blow first: a defender answers blows; at will it fights any enemy it sees.
    if (!atWill(m)) {
        if (defends(m) && (reactToHurt(o) || lookAtDamager(o))) {
            return;
        }
    } else {
        if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
            pushFight(o, *e, fight.frame);
            return;
        }
        if (lookAtDamager(o)) {
            return;
        }
    }
    // Near enough (escort distance + 30) it stays, healing; too far, it walks to whom it escorts
    // till within the escort distance (dropped when attacked, or at will when an enemy shows).
    const ServerObject* t = world.byExtent(me().args[2]);
    if (!t) {
        pop(o);
        return;
    }
    const X87 lim = X87(m.field329) + 30.0f; // (on the stack, squared there)
    if (dist2(o, t->x, t->y) <= lim * lim) {
        if ((!atWill(m) || !chaseSound(o)) && !enchanted(o, Enchant::AntiMagic)) {
            healSomeone(o);
        }
        return;
    }
    if (defends(m) || atWill(m)) {
        push(o, dep::NotUnderAttack);
    }
    if (atWill(m)) {
        push(o, dep::NoVisibleEnemy);
    }
    const std::uint32_t whom = me().args[2];
    if (Item* it = push(o, dep::ObjectFartherThan)) {
        it->args[0] = bitsOf(m.field329);
        it->args[2] = whom;
    }
    if (Item* it = push(o, MonsterAction::MoveTo)) {
        it->args[0] = bitsOf(t->x);
        it->args[1] = bitsOf(t->y);
        it->args[2] = whom;
    }
}

void Monsters::engage(ServerObject& o, ServerObject& e)
{
    // As the exe's caller of the melee attack does: a summons or hold first, not under ANTI_MAGIC.
    auto& m = *o.monster;
    if (!enchanted(o, Enchant::AntiMagic) && castHold(o, e)) {
        return;
    }
    const auto approach = [&](bool melee) {
        // monsterAction (melee): up to it while it's the nearest
        // enemy and alive; then, seen (and within 0.6 of the missile range for a shooter),
        // faced and within melee range (an NPC: or waiting for stamina), the blow.
        if (Item* it = push(o, dep::NoNewEnemy)) {
            it->args[0] = e.extent;
        }
        if (Item* it = push(o, dep::Alive)) {
            it->args[0] = e.extent;
        }
        if (melee) {
            if (canShoot(o)) {
                if (Item* it = push(o, dep::ObjectCloserThan)) {
                    it->args[2] = e.extent;
                    it->args[0] = bitsOf((m.def ? m.def->missileRange : 0.0f) * 0.6f);
                }
            }
            if (Item* it = push(o, dep::CanSee)) {
                it->args[0] = e.extent;
            }
            push(o, MonsterAction::MeleeAttack);
            if (Item* it = push(o, MonsterAction::FaceObject)) {
                it->args[0] = e.extent;
            }
            if (Item* it = push(o, dep::ObjectFartherThan)) {
                it->args[2] = e.extent;
                it->args[0] = bitsOf(m.def ? m.def->meleeRange : 0.0f);
            }
            if (o.subclasses & 0x10u) {
                push(o, dep::WaitForStamina);
                push(o, dep::Or);  // it keeps closing while out of reach or short of stamina
            }
        }
        if (Item* it = push(o, MonsterAction::MoveTo)) {
            it->args[0] = bitsOf(e.x);
            it->args[1] = bitsOf(e.y);
            it->args[2] = e.extent;
        }
    };
    // No missile: melee if it can, else just walk up (unless it's a caster); a shooter with melee
    // closes in when nearer than half its missile range.
    if (!canShoot(o)) {
        if (canMelee(o)) {
            approach(true);
        } else if (!canCast(m)) {
            approach(false);
        }
        return;
    }
    if (canMelee(o) && edgeDistance(o, e) < (m.def ? m.def->missileRange : 0.0f) * 0.5f) {
        approach(true);
        return;
    }
    // A shot, while it's the nearest enemy and seen; facing it; when it doesn't
    // hold its ground, closing to the missile range while the line of fire's blocked or it's
    // farther (stage C: the missiles).
    if (Item* it = push(o, dep::NoNewEnemy)) {
        it->args[0] = e.extent;
    }
    if (Item* it = push(o, dep::CanSee)) {
        it->args[0] = e.extent;
    }
    if (Item* it = push(o, MonsterAction::MissileAttack)) {
        it->args[0] = bitsOf(e.x);
        it->args[1] = bitsOf(e.y);
        it->args[2] = e.extent;
    }
    if (Item* it = push(o, MonsterAction::FaceObject)) {
        it->args[0] = e.extent;
    }
    if (!(m.status & ms::HoldYourGround) && m.stack[static_cast<std::size_t>(m.top)].action != MonsterAction::Guard) {
        if (Item* it = push(o, dep::BlockedLineOfFire)) {
            it->args[0] = e.extent;
        }
        if (Item* it = push(o, dep::ObjectFartherThan)) {
            it->args[2] = e.extent;
            it->args[0] = bitsOf(m.def ? m.def->missileRange : 0.0f);
        }
        push(o, dep::Or);  // it keeps closing while the line of fire is blocked or it's out of range
        if (Item* it = push(o, MonsterAction::MoveTo)) {
            it->args[0] = bitsOf(e.x);
            it->args[1] = bitsOf(e.y);
            it->args[2] = e.extent;
        }
    }
}

void Monsters::fightUpdate(ServerObject& o)
{
    // mobActionFight: its enemy's last place and the last frame it saw it (args
    // 0-1, 2); 10 s unseen and it gives up.
    World& world = fight.world;
    auto& m = *o.monster;
    Item& self = m.stack[static_cast<std::size_t>(m.top)];
    if (fight.frame - self.args[2] <= 300u) {
        if (ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
            self.args[2] = fight.frame;
            if (killable(*e)) {
                // Not under ANTI_MAGIC: a spell on itself, or at its enemy, first.
                if (!enchanted(o, Enchant::AntiMagic) && (buffSelf(o) || castOffensive(o, *e))) {
                    return;
                }
                engage(o, *e);
                return;
            }
        } else if (!(m.status & ms::HoldYourGround) && m.stack[static_cast<std::size_t>(m.top)].action != MonsterAction::Guard) {
            // Its enemy's body where it was last seen (its code within 30, dead): it's done
            // (and forgets that one's sound). Else it goes to look there.
            const float lx = floatOf(self.args[0]);
            const float ly = floatOf(self.args[1]);
            bool killed = false;
            inCircle(world, lx, ly, 30.0f, [&](const ServerObject* t) {
                if (t->extent == m.lastEnemy && (t->flags & ObjFlag::Dead)) {
                    killed = true;
                }
            });
            if (killed) {
                pop(o);
                if (m.heardFrom == m.lastEnemy) {
                    m.heard = 0;
                }
                return;
            }
            if (64.0f <= dist2(o, lx, ly)) {
                push(o, dep::NoVisibleEnemy);
                if (Item* it = push(o, MonsterAction::MoveTo)) {
                    it->args[0] = bitsOf(lx);
                    it->args[1] = bitsOf(ly);
                    it->args[2] = 0;
                }
                return;
            }
        }
    }
    pop(o);
}

void Monsters::detailedPath(ServerObject& o, float x, float y)
{
    // creatureSetDetailedPath: one at most every 10 frames (sooner: none, cut
    // short).
    auto& m = *o.monster;
    m.pathIndex = 0;
    if (fight.frame - m.pathAt < 10u) {
        m.pathCount = 0;
        m.pathStatus = 1;
        return;
    }
    m.pathToX = x;
    m.pathToY = y;
    PathSearch s = pathTo(fight.world, o, x, y, fight.rng, fight.frame);
    m.transits = std::move(s.transits);
    m.pathCount = static_cast<int>(s.points.size());
    std::copy(s.points.begin(), s.points.end(), m.path.begin());
    m.pathStatus = static_cast<std::uint8_t>(s.status);
    m.pathAt = fight.frame;
}

void Monsters::waypointPath(ServerObject& o, float x, float y)
{
    auto& m = *o.monster;
    m.waypointsToX = x;
    m.waypointsToY = y;
    const auto route = waypointPathTo(fight.world, o, x, y, fight.rng);
    std::copy(route.begin(), route.end(), m.waypoints.begin());
    m.waypointCount = static_cast<int>(route.size());
    m.waypointIndex = 0;
}

bool Monsters::walkPath(ServerObject& o)
{
    // creatureActuallyMove: from the point it's on, the nearest it can see that's
    // over 8 away (or the last within 8: at the path's end, it's there); it faces along the
    // path there and walks at it.
    World& world = fight.world;
    auto& m = *o.monster;
    if (m.pathCount == 0) {
        return false;
    }
    int chosen = 0;
    bool far = false;
    float best = 1e7f;
    bool near = false;
    int nearIndex = 0;
    for (int i = m.pathIndex; i < m.pathCount; ++i) {
        const auto [px, py] = m.path[static_cast<std::size_t>(i)];
        if (!world.traceRay(o.x, o.y, px, py, 0x84)) {  // ThroughWindows | PassSecret
            continue;
        }
        // (Squared and summed on the x87's stack, 53 bits, compared from there; the best kept
        // stored to a float.)
        const X87 dx = X87(px) - o.x;
        const X87 dy = X87(py) - o.y;
        const X87 d = dy * dy + dx * dx;
        if (X87(64.0f) < d) {
            if (!far || d < X87(best)) {
                chosen = i;
                far = true;
                best = d.f();
            }
        } else {
            near = true;
            nearIndex = i;
            if (i == m.pathCount - 1) {
                m.pathCount = 0;
                return true;
            }
        }
    }
    if (!far && !near) {
        m.pathCount = 0;
        return false;
    }
    if (!far) {
        chosen = nearIndex;
    }
    m.pathIndex = chosen;
    const auto [px, py] = m.path[static_cast<std::size_t>(chosen)];
    // (On the x87: vx stored; vy stored too but squared from the stack, FST then x the stored
    // one; the root + 0.01 stored.)
    const float vx = (X87(px) - o.x).f();
    const X87 vyStack = X87(py) - o.y;
    const float vy = vyStack.f();
    const float l = (util::sqrt(vyStack * vy + X87(vx) * vx) + 0.01f).f();
    // It faces along the path's leg that leads to that point (each difference stored).
    const auto& a = m.path[static_cast<std::size_t>(chosen < 1 ? 0 : chosen - 1)];
    const auto& b = m.path[static_cast<std::size_t>(chosen < 1 ? 1 : chosen)];
    const int way = directionOf((X87(b.first) - a.first).f(), (X87(b.second) - a.second).f());
    if (o.direction != way) {
        o.direction = static_cast<std::uint16_t>(way);
        o.touch();
    }
    m.turnTo = -1;
    const X87 speed = walkSpeedOnStack(o);
    o.forceX = (speed * vx / l).f();
    o.forceY = (speed * vy / l).f();
    return false;
}

bool Monsters::followWaypoints(ServerObject& o)
{
    // With no path, one to the waypoint it's making for (the next once within 8
    // of it; the last: there), ending on it exactly; then along it.
    World& world = fight.world;
    auto& m = *o.monster;
    if (m.waypointCount != 0 && m.pathCount == 0) {
        int i = m.waypointIndex;
        const auto at = [&](int k) -> const ServerWaypoint* {
            const std::uint32_t w = m.waypoints[static_cast<std::size_t>(k)];
            return w != 0 && w <= world.waypoints().size() ? &world.waypoints()[w - 1] : nullptr;
        };
        const ServerWaypoint* wp = at(i);
        if (wp && dist2(o, wp->x, wp->y) < 64.0f) {
            if (i == m.waypointCount - 1) {
                m.waypointCount = 0;
                return true;
            }
            m.waypointIndex = i + 1;
            wp = at(i + 1);
        }
        if (wp) {
            detailedPath(o, wp->x, wp->y);
            if (m.pathStatus == 0 && m.pathCount != 0) {
                m.path[static_cast<std::size_t>(m.pathCount - 1)] = {wp->x, wp->y};
            }
        }
    }
    return walkPath(o) && m.pathStatus == 2;
}

bool Monsters::moveAlong(ServerObject& o)
{
    // creatureSetMovePath: to the top's point; there within 8. Out of sight of it
    // (the wall ray; through windows when airborne): a waypoint route when it has none (or its
    // end's moved 100 off, 10 frames on), else a path; in sight: a path when it has none (or
    // its end's moved 50 off).
    World& world = fight.world;
    auto& m = *o.monster;
    const Item& top = m.stack[static_cast<std::size_t>(m.top)];
    const float tx = floatOf(top.args[0]);
    const float ty = floatOf(top.args[1]);
    if (util::sqrt(dist2(o, tx, ty)) + 0.01f <= X87(8.0f)) { // (+ the double 0.01f, on the stack)
        return true;
    }
    const auto off = [&](float x, float y) {
        const X87 dx = X87(x) - tx;
        const X87 dy = X87(y) - ty;
        return dx * dx + dy * dy;
    };
    bool follow = false;
    if (!world.traceRay(o.x, o.y, tx, ty, (o.flags & ObjFlag::Airborne) ? 4 : 0)) {
        if (m.pathCount == 0) {
            if (m.waypointCount == 0 || (10u < fight.frame - m.pathAt && 10000.0f < off(m.waypointsToX, m.waypointsToY))) {
                waypointPath(o, tx, ty);
            }
            if (m.waypointCount != 0) {
                follow = true;
            } else {
                detailedPath(o, tx, ty); // ("Waypoint path failed, doing detailed")
            }
        }
    } else if (m.pathCount == 0 || (10u < fight.frame - m.pathAt && 2500.0f < off(m.pathToX, m.pathToY))) {
        detailedPath(o, tx, ty);
    }
    if (!follow && m.waypointCount == 0) {
        if (m.pathCount == 0) {
            return false;
        }
        if (walkPath(o)) {
            m.pathCount = 0;
            return true;
        }
        return false;
    }
    if (followWaypoints(o)) {
        m.pathCount = 0;
        m.waypointCount = 0;
        return true;
    }
    return false;
}

void Monsters::moveTo(ServerObject& o)
{
    // mobActionMoveTo: escorting, it walks within 3 times its escort distance and
    // runs beyond 30 past it. There: a plain point (not an object's), reached, faces it and
    // ends; cut short twice within 5 s or no further, it wanders a while (2-4 s, FRUSTRATED);
    // any short of it waits a moment.
    World& world = fight.world;
    auto& m = *o.monster;
    if (!canMove(o)) {
        pop(o);
        return;
    }
    const int self = m.top;
    int under = MonsterAction::Invalid; // (the action under it)
    for (int i = m.top - 1; 0 <= i; --i) {
        if (!isCondition(m.stack[static_cast<std::size_t>(i)].action)) {
            under = m.stack[static_cast<std::size_t>(i)].action;
            break;
        }
    }
    if (under == MonsterAction::Escort) {
        // (x 3 stored as the limit, its stack value + 30 kept; the stored one squared on the
        // stack for the walk, the + 30 one for the run.)
        const X87 limStack = X87(m.field329) * 3.0f;
        const float lim = limStack.f();
        const X87 far = limStack + 30.0f;
        const X87 d = dist2(o, floatOf(m.stack[static_cast<std::size_t>(self)].args[0]), floatOf(m.stack[static_cast<std::size_t>(self)].args[1]));
        if (d < X87(lim) * lim) {
            walk(m);
        } else if (far * far < d) {
            run(m);
        }
    }
    if (moveAlong(o)) {
        const int st = m.pathStatus;
        const bool stuck = st == 2 || (st == 1 && fight.frame - m.blockedAt < 150u);
        if (st == 1) {
            m.blockedAt = fight.frame;
        }
        if (st == 0) {
            const int wps = world.waypointPathStatus; // (read and cleared)
            world.waypointPathStatus = 0;
            const Item& it = m.stack[static_cast<std::size_t>(self)];
            if (wps == 0 && it.args[2] == 0) {
                m.turnTo = directionOf(floatOf(it.args[0]) - o.x, floatOf(it.args[1]) - o.y); // (mobCalcDir)
                pop(o);
            }
        }
        if (stuck) {
            if (Item* t = push(o, dep::Time)) {
                t->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(60, 120)(fight.rng));
            }
            push(o, MonsterAction::RandomWalk);
            m.status |= ms::Frustrated;
        }
        if (m.pathStatus != 0) {
            if (Item* w = push(o, MonsterAction::Wait)) {
                w->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(15, 30)(fight.rng));
            }
        }
    }
    stepSound(world, fight.frame, o); // (monsterMoveAudio)
}

void Monsters::randomWalk(ServerObject& o)
{
    // mobActionRandomWalk: up to 20 of 256 either way of its facing (a quarter
    // turn more when water's 30 ahead, but to a swimmer), at its speed.
    int d = nox::util::uniform_int_distribution<int>(-20, 20)(fight.rng) + static_cast<std::int16_t>(o.direction);
    d = wrap256(d);
    if (!(o.subclasses & 0x400u)) {
        const auto [vx, vy] = directionVector(d);
        if (fight.world.floorAt((X87(vx) * 30.0f + o.x).f(), (X87(vy) * 30.0f + o.y).f()) == 6) {
            d = wrap256(d + 0x40);
        }
    }
    if (o.direction != d) {
        o.direction = static_cast<std::uint16_t>(d);
        o.touch();
    }
    o.monster->turnTo = -1;
    const X87 speed = walkSpeedOnStack(o); // (each product stored from the stack)
    const auto [vx, vy] = directionVector(d);
    o.forceX = (speed * vx).f();
    o.forceY = (speed * vy).f();
    stepSound(fight.world, fight.frame, o); // (monsterMoveAudio)
}

namespace {

/// A ROAM's history: the waypoint at the next of 16 places,
/// any other place holding it emptied.
void remember(ServerObject::Monster& m, std::uint32_t wp)
{
    if (15 < ++m.waypointIndex) {
        m.waypointIndex = 0;
    }
    m.waypoints[static_cast<std::size_t>(m.waypointIndex)] = wp;
    for (int i = 0; i < 16; ++i) {
        if (m.waypoints[static_cast<std::size_t>(i)] == wp && i != m.waypointIndex) {
            m.waypoints[static_cast<std::size_t>(i)] = 0;
        }
    }
}

} // namespace

bool Monsters::roamOn(ServerObject& o, std::uint32_t wp)
{
    // monsterRoamDeadEnd: on from a waypoint reached: a link it
    // hasn't been to lately, at random; else, of those it has, the one it was at longest ago;
    // none: a dead end.
    World& world = fight.world;
    auto& m = *o.monster;
    Item& self = m.stack[static_cast<std::size_t>(m.top)];
    const auto flags = static_cast<std::uint8_t>(self.args[2]);
    m.pathCount = 0;
    const ServerWaypoint* at = wp != 0 && wp <= world.waypoints().size() ? &world.waypoints()[wp - 1] : nullptr;
    if (at && !at->links.empty()) {
        const auto indexOf = [&](std::int32_t id) -> std::uint32_t {
            const ServerWaypoint* l = world.waypoint(id);
            return l ? static_cast<std::uint32_t>(l - world.waypoints().data()) + 1 : 0;
        };
        std::vector<std::uint32_t> fresh;
        for (const std::int32_t id : at->links) {
            const std::uint32_t li = indexOf(id);
            if (waypointUsable(world, li, flags) &&
                std::find(m.waypoints.begin(), m.waypoints.begin() + 16, li) == m.waypoints.begin() + 16) {
                fresh.push_back(li);
            }
        }
        std::uint32_t next = 0;
        if (!fresh.empty()) {
            next = fresh[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(fresh.size()) - 1)(fight.rng))];
        } else {
            // The history from its oldest entry: the first one linked from here.
            for (int i = 0; i < 16 && next == 0; ++i) {
                int k = m.waypointIndex + 1 + i;
                if (15 < k) {
                    k -= 16;
                }
                const std::uint32_t w = m.waypoints[static_cast<std::size_t>(k)];
                if (w == 0) {
                    continue;
                }
                const bool linked = std::any_of(at->links.begin(), at->links.end(), [&](std::int32_t id) { return indexOf(id) == w; });
                if (linked && waypointUsable(world, w, flags)) {
                    next = w;
                }
            }
        }
        self.args[0] = next;
        if (next != 0) {
            remember(m, next);
            return true;
        }
    }
    pop(o); // ("Reached dead end")
    return false;
}

void Monsters::roam(ServerObject& o)
{
    // mobActionRoam: from waypoint to waypoint (those with the flags of arg 2),
    // a blow answered, an enemy fought at will. (A bot morphed invisible sometimes waits.)
    World& world = fight.world;
    auto& m = *o.monster;
    if ((defends(m) || atWill(m)) && reactToHurt(o)) {
        return;
    }
    // A bot invisible with no enemy now and then (every 32 frames, about 1 in 10) waits 3-10 s,
    // while it stays invisible and no enemy comes within 150.
    if ((m.status & ms::Bot) && m.enemy == 0 && enchanted(o, Enchant::Invisible) && (fight.frame & 0x1Fu) == 0 &&
        nox::util::uniform_int_distribution<int>(0, 100)(fight.rng) <= 9) {
        if (Item* it = push(o, dep::EnemyFartherThan)) {
            it->args[0] = bitsOf(150.0f);
        }
        if (Item* it = push(o, dep::IsEnchanted)) {
            it->args[0] = Enchant::Invisible;
        }
        if (Item* it = push(o, MonsterAction::Wait)) {
            it->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(3, 10)(fight.rng)) * 30u;
        }
        return;
    }
    if (atWill(m)) {
        if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
            pushFight(o, *e, fight.frame);
            return;
        }
        if (chaseSound(o)) {
            return;
        }
    }
    const int self = m.top;
    const auto me = [&]() -> Item& { return m.stack[static_cast<std::size_t>(self)]; };
    const auto flags = static_cast<std::uint8_t>(me().args[2]);  // waypoint link flags; 0x80 the roaming ones
    if (!waypointUsable(world, me().args[0], flags)) {
        me().args[0] = 0;
    }
    // No waypoint yet: the nearest usable one it sees, else (roaming) a walk over the tiles to one.
    if (me().args[0] == 0) {
        me().args[0] = nearestWaypoint(world, o.x, o.y, flags, true);
        if (me().args[0] == 0 && flags == 0x80) {
            me().args[0] = waypointFrom(world, o, o.x, o.y, fight.rng);
        }
        if (me().args[0] == 0) {
            pop(o); // ("Cannot find any waypoint")
            if (Item* w = push(o, MonsterAction::Wait)) {
                w->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30, 60)(fight.rng));
            }
            return;
        }
        remember(m, me().args[0]);
    }
    const auto at = [&](std::uint32_t w) { return &world.waypoints()[w - 1]; };
    // Reached (within 8): on to a linked waypoint, the path walked afresh.
    if (dist2(o, at(me().args[0])->x, at(me().args[0])->y) <= 64.0f) {
        if (!roamOn(o, me().args[0])) {
            return;
        }
        m.pathAt = 0;
    }
    if (m.pathCount == 0) {
        detailedPath(o, at(me().args[0])->x, at(me().args[0])->y);
    }
    if (m.pathStatus == 2) {
        // ("Cannot compute path"): back to the latest waypoint it was at that's still usable.
        std::uint32_t back = 0;
        for (int i = 1; i < 16 && back == 0; ++i) {
            int k = m.waypointIndex - i;
            if (k < 0) {
                k += 16;
            }
            const std::uint32_t w = m.waypoints[static_cast<std::size_t>(k)];
            if (w != 0 && waypointUsable(world, w, flags)) {
                back = w;
            }
        }
        if (back == 0) {
            pop(o); // ("No previous waypoint")
            return;
        }
        if (!roamOn(o, back)) {
            return;
        }
    }
    if (walkPath(o)) {
        m.pathCount = 0;
    }
    stepSound(world, fight.frame, o);
}

ServerObject* Monsters::meleeTarget(ServerObject& o, bool any)
{
    // monsterPickMeleeTarget: in the box of its reach + its radius
    // + 30 about it, a creature (or a thing with health, not BELOW / flag 0x10), an enemy (or
    // anything with `any`), in front of it (the cosine over 0.5), the nearest edge to edge
    // within its reach.
    World& world = fight.world;
    auto& m = *o.monster;
    const float reach = m.def ? m.def->meleeRange : 0.0f;
    const X87 r = X87(reach) + o.radius + 30.0f; // (on the x87's stack; each edge stored)
    float best = reach;
    ServerObject* chosen = nullptr;
    const auto [fx, fy] = directionVector(o.direction);
    // (getUnitsInRect over the box, by the grid)
    world.unitsInRect((X87(o.x) - r).f(), (X87(o.y) - r).f(), (r + o.x).f(), (r + o.y).f(), [&](ServerObject& t) {
        if (&t == &o) { // (no 0x20 test)
            return;
        }
        if ((t.flags & 0x11u) && !creature(t)) {
            return;
        }
        if (!creature(t) && t.maxHealth == 0) {
            return;
        }
        if (!isEnemy(world, o, t) && !any) {
            return;
        }
        // (On the x87: dx kept on the stack, dy stored and squared as stored; the root + 0.001
        // stored; the cosine and the gap on the stack, the best kept stored.)
        const X87 dx = X87(t.x) - o.x;
        const float dy = (X87(t.y) - o.y).f();
        const float d = (util::sqrt(X87(dy) * dy + dx * dx) + 0.001f).f();
        if (dx / d * fx + X87(dy) / d * fy <= X87(0.5f)) {
            return;
        }
        const X87 gap = X87(d) - (X87(t.radius) + o.radius);
        if (gap < X87(best)) {
            best = gap.f();
            chosen = &t;
        }
    });
    return chosen;
}

bool Monsters::strike(ServerObject& o)
{
    // monster.bin's STRIKE_FUNCTION.
    World& world = fight.world;
    auto& m = *o.monster;
    const things::MonsterDef& def = *m.def;
    const std::string& fn = def.strikeFn;
    const auto tell = [&](const ServerObject& t, std::string_view id) {
        if (t.player) { // (SrvNet_SendInformString)
            world.broadcasts().push_back({informStringBytes(id), true, t.extent});
        }
    };
    // Its poison: its chance in 100, then the poison itself.
    const auto poisons = [&](ServerObject& t) {
        return def.poisonChance != 0 && nox::util::uniform_int_distribution<int>(1, 100)(fight.rng) <= def.poisonChance &&
               fight.applyPoison(t, def.poisonStrength, def.poisonMax);
    };
    const auto blow = [&](ServerObject& t) {
        fight.damage(t, &o, o.extent, def.meleeDamage, def.meleeDamageType);
    };
    const auto knock = [&](ServerObject& t) {
        if (0.0f < def.meleeImpact) {
            applyForce(world, t, o.x, o.y, def.meleeImpact);
        }
    };
    if (fn == "OgreStrike" || fn == "StoneGolemStrike" || fn == "MechGolemStrike") {
        // A sweep: everything about it in reach. The ogre's (strikeOgre):
        // edge to edge within its reach, in front (the cosine over 0.4), in sight.
        // The golems' (sendEquakeAfterGolem / monsterAttackAreaDamage): on its front side,
        // within its reach (calcDistance), in sight; then the ground shakes.
        const float r = (X87(def.meleeRange) + o.radius + 30.0f).f();
        const bool ogre = fn == "OgreStrike";
        bool hit = false;
        const auto [fx, fy] = directionVector(o.direction);
        // (unitsGetInCircle by the grid, each struck as it's found)
        world.unitsInCircle(o.x, o.y, r, [&](ServerObject& unit) {
            ServerObject* t = &unit;
            if (t == &o) { // (no 0x20 test)
                return;
            }
            if (ogre) {
                // (On the x87: dx kept on the stack; dy stored, squared from the stack by the
                // stored one; the root + 0.01 stored; the gap and the cosine on the stack.)
                const X87 dx = X87(t->x) - o.x;
                const X87 dyStack = X87(t->y) - o.y;
                const float dy = dyStack.f();
                const float d = (util::sqrt(dyStack * dy + dx * dx) + 0.01f).f();
                if (X87(d) - (X87(t->radius) + o.radius) > X87(def.meleeRange) || dx / d * fx + X87(dy) / d * fy <= X87(0.4f)) {
                    return;
                }
                if (!world.traceRay(o.x, o.y, t->x, t->y, 5)) {  // ray flags 5: Doors | ThroughWindows
                    return;
                }
                blow(*t);
                applyForce(world, *t, o.x, o.y, def.meleeImpact);
                hit = true;
            } else {
                if (!(facingSides(o.x, o.y, o.direction, t->x, t->y) & 1) || X87(def.meleeRange) < edgeDistance53(o, *t) ||
                    !world.traceRay(o.x, o.y, t->x, t->y, 5)) {
                    return;
                }
                blow(*t);
                if (creature(*t)) {
                    hit = true;
                }
                knock(*t);
            }
        });
        if (!ogre) {
            // earthquakeSend: MSG_FX_JIGGLE 30 to each player within 300.
            jiggle(world, o.x, o.y, 30);
        }
        return hit;
    }
    if (fn == "BomberStrike") { // (strikeBomber)
        return true;
    }
    // The rest: the nearest enemy in front, in sight (the wall ray, flags 5).
    ServerObject* t = meleeTarget(o, false);
    const bool spiders = fn == "WaspStrike" || fn == "SpiderStrike" || fn == "SpittingSpiderStrike";
    if (!t) {
        return !spiders; // (MonsterStrike, ScorpionStrike, VileZombieStrike, GhostStrike: 1)
    }
    if (!world.traceRay(o.x, o.y, t->x, t->y, 5)) {
        return false;
    }
    blow(*t);
    if (fn == "WaspStrike") { // (AI_StrikeWasp: its poison before the push)
        if (poisons(*t)) {
            tell(*t, "aifunc.c:PoisonedByWasp");
        }
        knock(*t);
        return true;
    }
    knock(*t);
    if (fn == "ScorpionStrike") { // (strikeScorpion)
        if (poisons(*t)) {
            tell(*t, "aifunc.c:PoisonedByScorpion");
        }
    } else if (fn == "VileZombieStrike") { // (AI_StrikeVileZombie)
        if (poisons(*t)) {
            tell(*t, "aifunc.c:PoisonedByZombie");
        }
    } else if (fn == "SpiderStrike" || fn == "SpittingSpiderStrike") { // (AI_StrikeSpider / AI_StrikeSpittingSpider)
        if (poisons(*t)) {
            tell(*t, "aifunc.c:Poisoned");
        }
    } else if (fn == "GhostStrike") {
        // strikeGhost: it holds it 2 s (HELD, power 3), faces it, and flees it
        // for 2-4 s.
        enchant(world, *t, Enchant::Held, 60, 3);
        if (Item* it = this->push(o, MonsterAction::FaceLocation)) {
            it->args[0] = bitsOf(t->x);
            it->args[1] = bitsOf(t->y);
        }
        if (Item* it = this->push(o, dep::Time)) {
            it->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(60, 120)(fight.rng));
        }
        if (Item* it = this->push(o, MonsterAction::Flee)) {
            it->args[0] = bitsOf(t->x);
            it->args[1] = bitsOf(t->y);
            it->args[2] = 0;
        }
    }
    return true; // (MonsterStrike, strikeMonsterDefault: the blow alone)
}

ServerObject* Monsters::incoming(ServerObject& o)
{
    return incomingMissile(fight.world, o);
}

ServerObject* incomingMissile(World& world, ServerObject& o)
{
    // monsterTestBlockShield (a monster's, and Plr_UpdateState's
    // shield): of the missiles within 100, one that would hit it, seen on its front
    // side from where it was (its PrevPos: ours' lastX / lastY, the walk's), flying at it
    // (against its facing, within 20 of its line, the cosine to it over 0.7), that it can see
    // (unitCanInteractWith): the nearest.
    ServerObject* chosen = nullptr;
    float best = 1e9f;
    const auto [fx, fy] = directionVector(o.direction);
    world.missilesInCircle(o.x, o.y, 100.0f, [&](ServerObject& t) { // (getMissilesInCircle, by the grid)
        if (!(t.classes & oc::Missile) || t.pendingDelete || t.holder != 0 || 10000.0f < dist2(o, t.x, t.y)) { // (0x20: ours' pendingDelete)
            return;
        }
        // Whether it would hit (with the sides, then not a friend's unless gameplay
        // flag 1).
        if ((o.classes & oc::Missile) || o.pendingDelete || !t.collides || !o.collides || (o.flags & ObjFlag::NoCollide)) {
            return;
        }
        if (!(o.flags & 0x80u)) {  // not MISSILE_HIT
            // Something on the ground (BELOW or flag 0x10) and something airborne don't meet.
            if (((t.flags & 0x11u) && (o.flags & ObjFlag::Airborne)) || ((o.flags & 0x11u) && (t.flags & ObjFlag::Airborne))) {
                return;
            }
            // NO_COLLIDE_OWNER (0x400) on either: its own side's don't meet.
            if (((t.flags & 0x400u) || (o.flags & 0x400u)) && sameSide(world, o, t)) {
                return;
            }
            // A monster's missile that isn't MAGIC (subclass 2) spares a monster it isn't hostile to.
            const ServerObject* owner = t.owner ? world.byExtent(t.owner) : nullptr;
            if (owner && !(t.subclasses & 2u) && (owner->classes & oc::Monster) && (o.classes & oc::Monster) &&
                (!isEnemy(world, *owner, o) || sameSide(world, o, *owner))) {
                return;
            }
        }
        if (sameSide(world, o, t) && !(world.gameplayFlags & 1u)) {
            return;
        }
        // Seen on its front side where it last was, and moving against its facing.
        // (On the x87, as the game computes it: the facing's products and sums on the stack; dx, dy
        // stored; the speed's root kept, x's share of it stored, y's kept; the distance stored;
        // the shares of it kept, negated, the cosine summed there. A test that fails on a NaN
        // (a missile exactly on it: 0 / 0) skips it, as FCOMP's flags do.)
        if (!(facingSides(o.x, o.y, o.direction, t.lastX, t.lastY) & 1) || !(X87(t.vx) * fx + X87(t.vy) * fy < X87(0.0f))) {
            return;
        }
        const float dx = (X87(t.x) - o.x).f();
        const float dy = (X87(t.y) - o.y).f();
        // Within 20 of the line it faces along.
        if (!(util::fabs(X87(fx) * dy + X87(-fy) * dx) < X87(20.0f))) {
            return;
        }
        const X87 v = util::sqrt(X87(t.vx) * t.vx + X87(t.vy) * t.vy);
        const float ux = (X87(t.vx) / v).f();
        const X87 uy = X87(t.vy) / v;
        const float d = util::sqrt(X87(dy) * dy + X87(dx) * dx).f();
        // Flying at it: the cosine over 0.7.
        if (!(X87(0.7f) < uy * -(X87(dy) / d) + X87(ux) * -(X87(dx) / d))) {
            return;
        }
        if (canInteract(world, o, t) && d < best) {
            chosen = &t;
            best = d;
        }
    });
    return chosen;
}

bool Monsters::dodge(ServerObject& o)
{
    // monsterCheckDodgeables: up to 5 tries at a spot 2-3 times its radius (at
    // most 15) to one side, in sight, clear of bodies and not water: its attacks dropped, it
    // steps there (for at most a second).
    World& world = fight.world;
    const auto [fx, fy] = directionVector(o.direction);
    for (int tries = 0; tries < 5; ++tries) {
        // (On the x87: randomFloat's stack value times the radius, stored, the cap tested from
        // the stack; each side's product and sum on the stack, one store.)
        const X87 reach = util::randomFloat(fight.rng, 2.0f, 3.0f) * o.radius;
        float d = reach.f();
        if (X87(15.0f) < reach) {
            d = 15.0f;
        }
        if (nox::util::uniform_int_distribution<int>(0, 100)(fight.rng) < 50) {
            d = -d;
        }
        const float tx = (X87(d) * -fy + o.x).f();  // to one side, across its facing
        const float ty = (X87(d) * fx + o.y).f();
        if (world.traceRay(o.x, o.y, tx, ty, 1) && lineClear(world, o, o.x, o.y, tx, ty) && world.floorAt(tx, ty) != 6) {
            popAttacks(o);
            if (Item* it = push(o, dep::Time)) {
                it->args[0] = fight.frame + 30;
            }
            if (Item* it = push(o, MonsterAction::Dodge)) {
                it->args[0] = bitsOf(tx);
                it->args[1] = bitsOf(ty);
                it->args[2] = 0;
            }
            return true;
        }
    }
    return false;
}

void Monsters::sidestep(ServerObject& o)
{
    // mobActionDodge: at a run to its spot (not while confused, held or
    // charmed); within 8, done.
    auto& m = *o.monster;
    if (!canMove(o)) {
        pop(o);
        return;
    }
    if (enchanted(o, Enchant::Confused) || enchanted(o, Enchant::Held) || enchanted(o, Enchant::Charming)) {
        return;
    }
    const Item& self = m.stack[static_cast<std::size_t>(m.top)];
    // (On the x87: dx kept on the stack; dy stored, squared from the stack by the stored one; the
    // root + 0.0001 stored, compared from the stack; the run speed stored, its stack value
    // times dx for the x push, the stored speed times dy for the y.)
    const X87 vx = X87(floatOf(self.args[0])) - o.x;
    const X87 vyStack = X87(floatOf(self.args[1])) - o.y;
    const float vy = vyStack.f();
    const X87 lStack = util::sqrt(vyStack * vy + vx * vx) + 0.0001f;
    const float l = lStack.f();
    if (lStack < X87(8.0f)) {
        pop(o);
        return;
    }
    const X87 speed = X87(m.def ? m.def->runMultiplier : 1.0f) * o.speedCur;
    o.speedCur = speed.f();
    o.forceX = (speed * vx / l).f();
    o.forceY = (X87(vy) * o.speedCur / l).f();
}

void Monsters::shoot(ServerObject& o)
{
    // mobActionMissileAtt: at monster.bin's missile frame, its missile from its
    // radius + 4 ahead, at where the target will be (its velocity times the flight:
    // projAddVelocitySmth) or the point, if the wall ray (flags 5) to a frame
    // past it is clear. Its animation over: done. An NPC's: the player's attack (playerAttack,
    // npc.hpp), done when that's over.
    World& world = fight.world;
    auto& m = *o.monster;
    if (o.subclasses & 0x10u) {
        if (!npcAttack(fight, o)) {
            pop(o);
        }
        return;
    }
    const Item& self = m.stack[static_cast<std::size_t>(m.top)];
    if (m.def && m.animFrame == static_cast<std::uint8_t>(m.def->missileFrame) && m.animDelay == 0 && fight.things) {
        if (fight.things->findThing(m.def->missile)) {
            ServerObject& shot = world.spawn(*fight.things, m.def->missile, o.x, o.y);
            const float speed = shot.speedCur;
            float px = floatOf(self.args[0]);
            float py = floatOf(self.args[1]);
            if (const ServerObject* t = self.args[2] ? world.byExtent(self.args[2]) : nullptr) {
                // (projAddVelocitySmth: the flight's time on the x87's stack, each place stored.)
                const X87 time = util::sqrt(dist2(o, t->x, t->y)) / speed;
                px = (time * t->vx + t->x).f();
                py = (time * t->vy + t->y).f();
            }
            // (On the x87: dx kept on the stack; dy stored, squared from the stack by the stored
            // one; the root + 0.1, the reach, the velocity, the start and the ray's end stored.)
            const X87 dx = X87(px) - o.x;
            const X87 dyStack = X87(py) - o.y;
            const float dy = dyStack.f();
            const float l = (util::sqrt(dyStack * dy + dx * dx) + 0.1f).f();
            const float r = (X87(o.radius) + 4.0f).f();
            const float vx = (dx * speed / l).f();
            const float vy = (X87(dy) * speed / l).f();
            const auto [fx, fy] = directionVector(o.direction);
            const float sx = (X87(r) * fx + o.x).f();
            const float sy = (X87(r) * fy + o.y).f();
            if (world.traceRay(o.x, o.y, (X87(sx) + vx).f(), (X87(sy) + vy).f(), 5)) {
                // (createAt: in the world, its owner the monster)
                shot.x = shot.frameStartX = shot.newX = shot.lastX = shot.homeX = sx;
                shot.y = shot.frameStartY = shot.newY = shot.lastY = shot.homeY = sy;
                shot.flags = (shot.flags & 0x35E9FEDBu) | ObjFlag::Active;
                shot.born = shot.timer = fight.frame;
                shot.owner = o.extent;
                shot.vx = vx;
                shot.vy = vy;
                shot.direction = o.direction;
            } else {
                shot.pendingDelete = true; // (delayedDeleteObject)
                shot.flags |= ObjFlag::Destroyed;
            }
        }
        playCreatureSound(world, o, things::SoundSlot::MissileAttack);
    }
    if (m.animDone != 0) {
        pop(o);
    }
}

void Monsters::retreat(ServerObject& o)
{
    // mobActionRetreat: healthy again (at its resume level; a caster not under
    // ANTI_MAGIC), done. With an enemy it flees it (3-4 s at most); without, it looks for food
    // (mobRetreatCheckEdibles: to the nearest seen and picks it up; none: it
    // roams while it's unwell and sees no enemy or food). (A caster's spells first: stage D.)
    World& world = fight.world;
    auto& m = *o.monster;
    const bool resumes = X87(m.resumeLevel) <= healthRatio(o); // (monsterCanResumeAttack)
    const bool spent = (m.status & ms::CanCastSpells) && enchanted(o, Enchant::AntiMagic); // (monsterCanCast2)
    if (resumes && !spent) {
        pop(o);
        return;
    }
    if (const ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
        if (!enchanted(o, Enchant::AntiMagic) && castAway(o)) {
            return;
        }
        if (Item* it = push(o, dep::Time)) {
            it->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(120, 180)(fight.rng));
        }
        if (Item* it = push(o, MonsterAction::Flee)) {
            it->args[0] = bitsOf(e->x);
            it->args[1] = bitsOf(e->y);
            it->args[2] = 0;
        }
        return;
    }
    if (resumes) {
        return;
    }
    if (ServerObject* food = edible(world, o, world.quest() ? 640.0f : 250.0f)) {
        push(o, dep::NotHealthy);
        push(o, dep::NoVisibleEnemy);
        if (Item* it = push(o, dep::ObjectAtVisibleLocation)) {
            it->args[0] = bitsOf(food->x);
            it->args[1] = bitsOf(food->y);
            it->args[2] = food->extent;
        }
        if (Item* it = push(o, MonsterAction::PickupObject)) { // (its pickup: with the non-players' pickups)
            it->args[0] = food->extent;
        }
        if (Item* it = push(o, MonsterAction::MoveTo)) {
            it->args[0] = bitsOf(food->x);
            it->args[1] = bitsOf(food->y);
            it->args[2] = food->extent;
        }
    } else {
        push(o, dep::NotHealthy);
        push(o, dep::NoVisibleEnemy);
        push(o, dep::NoVisibleFood);
        if (Item* it = push(o, MonsterAction::Roam)) {
            it->args[0] = 0;
            it->args[2] = (it->args[2] & ~0xFFu) | 0x80u;  // roaming waypoints (link flag 0x80)
        }
    }
}

void Monsters::flee(ServerObject& o)
{
    // mobActionFlee: from its enemy's place (kept up), a path away
    // (generateRetreatPath) made afresh once the enemy's within its flee range
    // (after half a second), every 2 s, and when it runs out; a path of under 2 points: it
    // stays. (A caster's spells on the way: stage D.)
    World& world = fight.world;
    auto& m = *o.monster;
    if (!canMove(o)) {
        pop(o);
        return;
    }
    const int self = m.top;
    const auto me = [&]() -> Item& { return m.stack[static_cast<std::size_t>(self)]; };
    if (ServerObject* e = m.enemy ? world.byExtent(m.enemy) : nullptr) {
        me().args[0] = bitsOf(e->x);
        me().args[1] = bitsOf(e->y);
        if (dist2(o, e->x, e->y) < X87(m.fleeRange) * m.fleeRange && 15u < fight.frame - m.pathAt) {
            m.pathCount = 0;
        }
        // A caster (not timid or cowardly, not under ANTI_MAGIC) casts on the way: retreating, to
        // get away, else on itself; failing that, at its enemy when it can reach it.
        const bool timid = m.aggression < 0.33f && 0.08f < m.aggression;
        if ((m.status & ms::CanCastSpells) && !enchanted(o, Enchant::AntiMagic) && !timid && !cowardly(m)) {
            const bool cast = has(o, MonsterAction::Retreat) ? castAway(o) : buffSelf(o);
            if (!cast && canInteract(world, o, *e)) {
                castOffensive(o, *e);
            }
            // (Its flight goes on this frame under the cast's actions, as in the exe.)
        }
    }
    if (m.pathCount != 0 && 60u < fight.frame - m.pathAt) {
        m.pathCount = 0;
    }
    if (m.pathCount == 0 && 10u < fight.frame - m.pathAt) {
        PathSearch s = retreatPath(world, o, floatOf(me().args[0]), floatOf(me().args[1]), fight.rng, fight.frame);
        m.transits = std::move(s.transits);
        m.pathCount = static_cast<int>(s.points.size());
        std::copy(s.points.begin(), s.points.end(), m.path.begin());
        m.pathAt = fight.frame;
        m.pathIndex = 0;
        if (m.pathCount < 2) {
            me().args[0] = bitsOf(o.x);
            me().args[1] = bitsOf(o.y);
            return;
        }
    }
    if (walkPath(o)) {
        m.pathCount = 0;
    }
    stepSound(world, fight.frame, o);
}

void Monsters::retreatToMaster(ServerObject& o)
{
    // While it has a master and is unwell, back within its escort distance + 30.
    World& world = fight.world;
    auto& m = *o.monster;
    const ServerObject* master = o.owner ? world.byExtent(o.owner) : nullptr;
    const bool well = X87(m.resumeLevel) <= healthRatio(o) &&!((m.status & ms::CanCastSpells) && enchanted(o, Enchant::AntiMagic));
    if (!master || well) {
        pop(o);
        return;
    }
    const X87 lim = X87(m.field329) + 30.0f; // (on the stack, squared there)
    if (dist2(o, master->x, master->y) <= lim * lim) {
        return;
    }
    if (Item* it = push(o, dep::ObjectFartherThan)) {
        it->args[0] = bitsOf(m.field329);
        it->args[2] = master->extent;
    }
    if (Item* it = push(o, MonsterAction::MoveTo)) {
        it->args[0] = bitsOf(master->x);
        it->args[1] = bitsOf(master->y);
        it->args[2] = master->extent;
    }
}

void Monsters::castAs(ServerObject& o, std::uint32_t spell, ServerObject* target, float x, float y)
{
    // mobCast: it turns to the point (mobCalcDir), then
    // castSpellByUser: at its power; a hostile spell shows it (invisibility,
    // invulnerability, an oval shield end); one that flies goes as a projectile at another;
    // the rest go off (spellAccept). (A bot morphed as a player: with the bots.)
    if (!fight.things) {
        return;
    }
    World& world = fight.world;
    o.monster->turnTo = directionOf(x - o.x, y - o.y);
    Spells spells{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    const int power = spells.power(o, spell);
    const things::SpellDef* def = fight.things->findSpell(spell);
    const std::uint32_t flags = def ? def->flags : 0;
    if (flags & 0x20u) {
        disenchant(world, o, Enchant::Invisible);
        disenchant(world, o, Enchant::Invulnerable);
        cancelLastingSpells(world, 0x43, o);
    }
    if ((flags & 4u) && target != &o) {
        spells.flySpell(o, target, spell);
        return;
    }
    spells.cast(spell, o, target, x, y, power);
}

void Monsters::castAt(ServerObject& o, std::uint32_t spell, ServerObject& target)
{
    // monsterCast: uninterruptable, the cast (a lasting spell for 0.5-2 s); at
    // another (but fleeing), first up to it within its missile range and in a clear line, facing
    // it (not for a spell that flies).
    auto& m = *o.monster;
    const things::SpellDef* def = fight.things ? fight.things->findSpell(spell) : nullptr;
    const std::uint32_t flags = def ? def->flags : 0;
    push(o, dep::Uninterruptable);
    Item* it = nullptr;
    if (flags & 2u) {  // a lasting spell
        if (Item* t = push(o, dep::Time)) {
            t->args[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(15, 60)(fight.rng));
        }
        it = push(o, MonsterAction::CastDurationSpell);
    } else {
        it = push(o, MonsterAction::CastSpellOnObject);
    }
    if (it) {
        it->args[0] = spell;
        it->args[2] = target.extent;
    }
    bool fleeing = false; // (monsterIsActionScheduled: under the top)
    for (int i = m.top - 1; 0 <= i; --i) {
        fleeing = fleeing || m.stack[static_cast<std::size_t>(i)].action == MonsterAction::Flee;
    }
    if (&target != &o && !fleeing) {
        if (!(flags & 4u)) {
            if (Item* f = push(o, MonsterAction::FaceObject)) {
                f->args[0] = target.extent;
            }
            if (Item* b = push(o, dep::BlockedLineOfFire)) {
                b->args[0] = target.extent;
            }
        }
        if (Item* far = push(o, dep::ObjectFartherThan)) {
            far->args[0] = bitsOf(m.def ? m.def->missileRange : 0.0f);
            far->args[2] = target.extent;
        }
        push(o, dep::Or);
        if (Item* mv = push(o, MonsterAction::MoveTo)) {
            mv->args[0] = bitsOf(target.x);
            mv->args[1] = bitsOf(target.y);
            mv->args[2] = target.extent;
        }
    }
}

namespace {

/// Its spells of a use (and spell flag 0x10).
std::vector<std::uint32_t> spellsFor(const ServerObject& o, std::uint32_t use, const things::ThingDb* things)
{
    std::vector<std::uint32_t> list;
    for (std::uint32_t id = 1; id < 0x89; ++id) {
        if (!(o.monster->spells[id] & use)) {
            continue;
        }
        const things::SpellDef* def = things ? things->findSpell(id) : nullptr;
        if (def && (def->flags & 0x10u)) {
            list.push_back(id);
        }
    }
    return list;
}

/// One of them already on it (the enchantment the spell gives).
bool alreadyOn(const ServerObject& o, const std::vector<std::uint32_t>& list)
{
    for (const std::uint32_t s : list) {
        for (int e = 0; e < 32; ++e) {
            if (enchantmentSpell(e) == s) {
                if (enchanted(o, e)) {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

/// It may cast now: enabled, a caster, past `at`, not casting already.
bool mayCast(const ServerObject& o, std::uint32_t at, std::uint32_t frame)
{
    return (o.flags & ObjFlag::Enabled) && (o.monster->status & ms::CanCastSpells) && at <= frame && !casting(*o.monster);
}

} // namespace

bool Monsters::counterSpell(ServerObject& o)
{
    // mobCastInversion: a spell projectile seeking it within half the
    // InversionRange: one of its counters on itself.
    World& world = fight.world;
    auto& m = *o.monster;
    if (!mayCast(o, m.spellAt[0], fight.frame)) {
        return false;
    }
    const float r = world.number("InversionRange") * 0.5f;
    bool coming = false;
    world.missilesInCircle(o.x, o.y, r, [&](const ServerObject& t) { // (getMissilesInCircle, by the grid)
        if ((t.subclasses & 2u) && t.seekTarget == o.extent) { // (no 0x20 test)
            coming = true; // (unitIsMagicMissile)
        }
    });
    if (!coming) {
        return false;
    }
    const auto list = spellsFor(o, 0x8000000u, fight.things);
    if (list.empty()) {
        return false;
    }
    castAt(o, list[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(list.size()) - 1)(fight.rng))], o);
    m.spellAt[0] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[0], m.spellDelays[1])(fight.rng));
    return true;
}

bool Monsters::buffSelf(ServerObject& o)
{
    // monsterBuffSelf: none of them on it yet: one on itself.
    auto& m = *o.monster;
    if (!mayCast(o, m.spellAt[1], fight.frame)) {
        return false;
    }
    const auto list = spellsFor(o, 0x10000000u, fight.things);
    if (list.empty() || alreadyOn(o, list)) {
        return false;
    }
    castAt(o, list[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(list.size()) - 1)(fight.rng))], o);
    m.spellAt[1] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[2], m.spellDelays[3])(fight.rng));
    return true;
}

bool Monsters::castOffensive(ServerObject& o, ServerObject& enemy)
{
    // monsterCastOffensive.
    auto& m = *o.monster;
    if (!mayCast(o, m.spellAt[2], fight.frame)) {
        return false;
    }
    const auto list = spellsFor(o, 0x20000000u, fight.things);
    if (list.empty()) {
        return false;
    }
    castAt(o, list[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(list.size()) - 1)(fight.rng))], enemy);
    m.spellAt[2] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[4], m.spellDelays[5])(fight.rng));
    return true;
}

bool Monsters::castHold(ServerObject& o, ServerObject& enemy)
{
    // mobCastRelated2: one at random; a summons (spells 0x4B-0x72) only while it
    // isn't summoning already and has room (checkSummonedCreaturesLimit: with the
    // summons; none for now), else another draw (none at all when every one's a summons).
    auto& m = *o.monster;
    if (!mayCast(o, m.spellAt[3], fight.frame)) {
        return false;
    }
    const auto list = spellsFor(o, 0x40000000u, fight.things);
    if (list.empty()) {
        return false;
    }
    const auto summons = [](std::uint32_t s) { return 0x4A < s && s < 0x73; };
    const bool all = std::all_of(list.begin(), list.end(), summons);
    for (;;) {
        const std::uint32_t s = list[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(list.size()) - 1)(fight.rng))];
        if (!summons(s)) {
            castAt(o, s, enemy);
            m.spellAt[3] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[6], m.spellDelays[7])(fight.rng));
            return true;
        }
        if (all) {
            return false;
        }
    }
}

bool Monsters::castAway(ServerObject& o)
{
    // mobCastRelated: none of them on it yet: one on itself.
    auto& m = *o.monster;
    if (!mayCast(o, m.spellAt[4], fight.frame)) {
        return false;
    }
    const auto list = spellsFor(o, 0x80000000u, fight.things);
    if (list.empty() || alreadyOn(o, list)) {
        return false;
    }
    castAt(o, list[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(list.size()) - 1)(fight.rng))], o);
    m.spellAt[4] = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.spellDelays[8], m.spellDelays[9])(fight.rng));
    return true;
}

bool Monsters::healSomeone(ServerObject& o)
{
    // mobHealSomeone: every 32nd frame; under half its health (CAN_HEAL_SELF),
    // Lesser Heal on itself; else (CAN_HEAL_OTHERS) on one of its side it can reach under half
    // theirs within 250 (a quest's 640: mobMayHealThis, the last found).
    World& world = fight.world;
    auto& m = *o.monster;
    if (!(o.flags & ObjFlag::Enabled) || !(m.status & ms::CanCastSpells) || (fight.frame & 0x1Fu) != 0 || casting(m)) {
        return false;
    }
    if ((m.status & ms::CanHealSelf) && o.maxHealth != 0 && o.health < (o.maxHealth >> 1)) {
        castAt(o, 0x29, o);  // Lesser Heal
        return false;
    }
    if (m.status & ms::CanHealOthers) {
        const float r = world.quest() ? 640.0f : 250.0f;
        ServerObject* chosen = nullptr;
        world.unitsInRect(o.x - r, o.y - r, r + o.x, r + o.y, [&](ServerObject& t) { // (getUnitsInRect, by the grid)
            if (&t == &o) { // (no 0x20 test)
                return;
            }
            if (!isEnemy(world, o, t) && t.maxHealth != 0 && !(t.flags & ObjFlag::Dead) && canInteract(world, o, t) && t.health < (t.maxHealth >> 1)) {
                chosen = &t;
            }
        });
        if (chosen) {
            castAt(o, 0x29, *chosen);  // Lesser Heal
            return true;
        }
    }
    return false;
}

void Monsters::castAction(ServerObject& o, int mode)
{
    // mobActionCast: at monster.bin's missile frame (its first moment there): at
    // the object (mode 0: where it'll be, off by its inaccuracy, mobCastRandomRecoil)
    // or the point (1), not under ANTI_MAGIC. Its first moment at frame 1 instead (not the
    // missile frame), its SPELL_INIT_SOUND.
    auto& m = *o.monster;
    if (m.animDelay != 0 || !m.def) {
        return;
    }
    if (m.animFrame != static_cast<std::uint8_t>(m.def->missileFrame)) {
        if (m.animFrame == 1) {
            playCreatureSound(fight.world, o, things::SoundSlot::SpellInit);
        }
        return;
    }
    const Item& self = m.stack[static_cast<std::size_t>(m.top)];
    ServerObject* target = nullptr;
    float x = 0;
    float y = 0;
    if (mode == 0) {
        target = self.args[2] ? fight.world.byExtent(self.args[2]) : nullptr;
        if (!target) {
            return;
        }
        // Its aim: off the target by its velocity and a random spread, the wider the less
        // accurate it is (field330).
        // (mobCastRandomRecoil on the x87: 1 - field330 stored, + 1 stored for randomFloat's
        // top; its draw kept on the stack for both lead products, each place stored; the spread
        // f x 0.8 + 0.2 stored; each later draw times it plus the place, one store.)
        const float f = (X87(1.0f) - m.field330).f();
        const X87 r = util::randomFloat(fight.rng, f, (X87(f) + 1.0f).f());
        x = (X87(target->x) - r * target->vx * 6.0f).f();
        y = (X87(target->y) - r * target->vy * 6.0f).f();
        const float spread = (X87(f) * 0.8f + 0.2f).f();
        x = (util::randomFloat(fight.rng, -60.0f, 60.0f) * spread + x).f();
        y = (util::randomFloat(fight.rng, -60.0f, 60.0f) * spread + y).f();
    } else {
        x = floatOf(self.args[2]);
        y = floatOf(self.args[3]);
    }
    if (!enchanted(o, Enchant::AntiMagic)) {
        castAs(o, self.args[0], target, x, y);
    }
}

void Monsters::deathFunction(ServerObject& o, const std::string& name)
{
    // monster.bin's DIE_FUNCTION (DYING's start) / DEAD_FUNCTION (DEAD's), the exe's table.
    World& world = fight.world;
    if (name.empty() || !fight.things) {
        return;
    }
    const things::ThingDb& things = *fight.things;
    Missiles missiles{world, things, fight.modifiers, fight.rng, fight.frame};
    const auto pointFx = [&](std::uint8_t op) { nox::server::pointFx(world, op, o.x, o.y); }; // (netSendPointFx)
    const auto spark = [&](std::uint8_t size) { // (netSparkExplosionFx)
        const auto ix = static_cast<std::uint32_t>(std::clamp(o.x, 0.0f, 65535.0f));
        const auto iy = static_cast<std::uint32_t>(std::clamp(o.y, 0.0f, 65535.0f));
        world.broadcasts().push_back({{0x93, lo(ix), hi(ix), lo(iy), hi(iy), size}, false, 0});
    };
    const auto rnd = [&](float a, float b) { return util::randomFloat(fight.rng, a, b).f(); }; // (randomFloat, each stored as it comes)
    const auto irnd = [&](int a, int b) { return nox::util::uniform_int_distribution<int>(a, b)(fight.rng); };
    const bool solo = (world.gameFlags & GameFlag::Solo) != 0;
    // A piece thrown up from it (bounding: FALLING with a bounce), gone in a while. Its draws in
    // the game's order: the spot (clearPointNear's angle), the height (`height`, a draw or not),
    // the fall, the push (when `pushHi` isn't 0: randomFloat(pushLo, pushHi)), then its seconds
    // (`seconds`, a draw).
    const auto piece = [&](std::string_view type, float r, float bounce, const auto& height, float pushLo, float pushHi,
                           const auto& seconds) -> ServerObject* {
        if (!things.findThing(type)) {
            return nullptr;
        }
        const auto [px, py] = clearPointNear(world, fight.rng, o.x, o.y, r);
        ServerObject* p = world.createAt(things, type, px, py, fight.frame);
        if (!p) {
            return nullptr;
        }
        p->raise(height()); // (unitRaise)
        p->zVel = rnd(-2.0f, 0.0f);
        p->bounce = bounce;
        p->flags |= ObjFlag::Bouncing;
        if (pushHi != 0.0f) {
            applyForce(world, *p, o.x, o.y, rnd(pushLo, pushHi));
        }
        p->decayAt = fight.frame + static_cast<std::uint32_t>(seconds()) * 30u; // (unitSetDecayTime: x gameFPS)
        p->touch();
        return p;
    };
    if (name == "EmberDemonDead" || name == "DemonDead") {
        // The body bursts: a push and fire about it, a blast (0x2A).
        const bool ember = name == "EmberDemonDead";
        const float reach = ember ? 96.0f : 150.0f;
        missiles.pushFrom(o.x, o.y, reach, 10.0f, ember ? 100.0f : 150.0f);
        missiles.damageAt(o.x, o.y, reach, 10.0f, ember ? (solo ? 30 : 96) : 148, 7, &o, nullptr, false);
        spark(ember ? 0x80 : 0xFF);
        playSound(world, 0x2A, o);
        o.pendingDelete = true;
    } else if (name == "ImpDead" || name == "SpiderDead") {
        pointFx(0x81);
    } else if (name == "MechGolemDead") {
        // Its parts fly (0x1EE first).
        playSound(world, 0x1EE, o);
        pointFx(0x8A);
        for (const char* part : {"MechGear", "MechHead", "MechArm", "MechGear", "MechLeg", "MechLeg", "MechGear", "MechGear"}) {
            if (!things.findThing(part)) {
                break;
            }
            piece(part, 30.0f, 2.0f, [&] { return rnd(10.0f, 70.0f); }, 0.0f, 0.0f, [&] { return irnd(10, 20); });
        }
    } else if (name == "GolemDead") {
        // It crumbles to rocks (6; the solo game's 20-30), 0x1E7 first.
        playSound(world, 0x1E7, o);
        pointFx(0x8A);
        static constexpr const char* kRocks[12] = {"BigRock", "MediumRock", "SmallRock", "MediumRock", "SmallRock", "SmallRock",
                                                   "MediumRock", "SmallRock", "SmallRock", "SmallRock", "SmallRock", "SmallRock"};
        static constexpr std::uint8_t kBounce[12] = {1, 3, 5, 3, 5, 5, 3, 5, 5, 5, 5, 5};
        const int count = solo ? irnd(20, 30) : 6;
        for (int i = 0; i < count; ++i) {
            const std::uint32_t k = world.golemRock;
            if (!things.findThing(kRocks[k])) {
                break;
            }
            piece(kRocks[k], 30.0f, static_cast<float>(kBounce[k]), [&] { return rnd(10.0f, 70.0f); }, 5.0f, 20.0f,
                  [&] { return solo ? irnd(10, 20) : irnd(5, 10); });
            world.golemRock = (world.golemRock + 1) % 12;
        }
    } else if (name == "SkeletonDead" || name == "SkeletonLordDead") {
        // Its bones; in the solo game, a sword (21
        // in 100) or its shield (49 in 100) dropped.
        const int roll = irnd(0, 100);
        playSound(world, 0x164, o); // (the bones' first)
        pointFx(0x8A);
        const auto boneSeconds = [&] { return solo ? irnd(10, 20) : irnd(2, 5); };
        if (piece("Skull", 20.0f, 4.0f, [] { return 40.0f; }, 5.0f, 25.0f, boneSeconds)) {
            const int bones = solo ? irnd(10, 20) : irnd(5, 10);
            static constexpr const char* kBones[2] = {"ArmBone", "LegBone"};
            for (int i = 0; i < bones; ++i) {
                if (!piece(kBones[world.nextBone], 20.0f, 4.0f, [&] { return rnd(10.0f, 35.0f); }, 5.0f, 25.0f, boneSeconds)) {
                    break;
                }
                world.nextBone = (world.nextBone + 1) % 2;
            }
        }
        if (roll >= 21 && solo) {
            const bool sword = roll < 51;
            const char* type = sword ? "Sword" : (name == "SkeletonDead" ? "WoodenShield" : "SteelShield");
            if (things.findThing(type)) {
                const auto [px, py] = clearPointNear(world, fight.rng, o.x, o.y, 50.0f);
                if (ServerObject* item = world.createAt(things, type, px, py, fight.frame)) {
                    if (fight.modifiers && (item->classes & 0x13001000u)) {
                        const auto id = [&](const char* n) -> std::uint8_t {
                            const things::ModifierDef* d = n ? fight.modifiers->findModifier(n) : nullptr;
                            return d ? static_cast<std::uint8_t>(d->id) : 0xFF;
                        };
                        item->modifiers = {id(sword ? "WeaponPower1" : nullptr), id("Material2"), 0xFF, 0xFF};
                    }
                    if (sword) {
                        weaponDropSound(world, *item);
                    } else {
                        armorDropSound(world, *item);
                    }
                }
            }
        }
    } else if (name == "TrollDead") {
        // monsterDeadTroll: a small toxic cloud, its own, and 0x284.
        if (ServerObject* cloud = world.createAt(things, "SmallToxicCloud", o.x, o.y, fight.frame)) {
            playSound(world, 0x284, o);
            cloud->owner = o.extent;
            cloud->value = static_cast<int>(world.number("SmallToxicCloudLifetime") * 30.0f);
        }
    } else if (name == "BomberDead") {
        // bomberDead: what it carries first (its glyph) out, its owner's still, at it,
        // facing as it did, goes off at what it ran into (BombCollide's) or the
        // nearest; with nothing, a small blast. Its sound, 0x170.
        pointFx(0x85);
        playSound(world, 0x170, o);
        ServerObject* item = o.inventory.empty() ? nullptr : world.byExtent(o.inventory.front());
        if (item) {
            std::erase(o.inventory, item->extent);
            item->holder = 0;
            item->flags |= ObjFlag::Active;
            if (item->trap) {
                item->trap->x = o.x;
                item->trap->y = o.y;
                item->trap->target = 0;
            }
            item->direction = o.direction;
            item->x = item->newX = o.x;
            item->y = item->newY = o.y;
            world.noteNew(*item); // (createAt's new list)
            ServerObject* by = o.monster->bombTarget ? world.byExtent(o.monster->bombTarget) : nullptr;
            Spells{world, things, fight.modifiers, fight.rng, fight.frame}.glyphGoesOff(*item, by);
            return;
        }
        missiles.damageAt(o.x, o.y, 50.0f, 30.0f, 10, 7, &o, nullptr, false);
        missiles.pushFrom(o.x, o.y, 50.0f, 30.0f, 30.0f);
    }
}

void Monsters::raiseZombie(ServerObject& o)
{
    // mobRaiseZombie: dead, it gets up (uninterruptable), whole again, a body no
    // more; its sound, 0x1D5.
    auto& m = *o.monster;
    if (!isZombie(o) || m.stack[static_cast<std::size_t>(m.top)].action != MonsterAction::Dead) {
        return;
    }
    pop(o);
    push(o, dep::Uninterruptable);
    push(o, MonsterAction::GetUp);
    playSound(fight.world, 0x1D5, o);
    o.health = o.maxHealth; // (unitHPsetOnMax, its owner told)
    informOwnerHealth(fight.world, o);
    o.flags &= 0xFFFF7FA7u;  // DEAD, NO_COLLIDE, ALLOW_OVERLAP and 0x10 off
    o.updates = true;
    o.touch();
}

void Monsters::burnZombie(ServerObject& o)
{
    // zombieBurnDelete: a player's loses it, a scorch, gone. (The solo game's kill
    // award: with the solo game.)
    loseCreature(fight.world, o);
    if (fight.things) {
        makeScorch(fight.world, *fight.things, o.x, o.y, 1, fight.rng, fight.frame);
    }
    o.pendingDelete = true;
}

// An action's start, once, as it first comes to the top (the action table's first).
void Monsters::start(ServerObject& o, int action)
{
    World& world = fight.world;
    auto& m = *o.monster;
    Item& top = m.stack[static_cast<std::size_t>(m.top)];
    switch (action) {
    case Idle: // (when it began)
        top.args[0] = fight.frame;
        break;
    case MonsterAction::Roam: // (its history begun afresh)
        m.waypoints[0] = 0;
        m.waypointIndex = 0;
        break;
    case MonsterAction::MoveToHome: // (mobActionReturnToHome)
    case MonsterAction::Flee:
    case MonsterAction::RetreatToMaster:
        run(m);
        break;
    case MonsterAction::MissileAttack: {
        if (fight.frame < m.nextMelee) {
            // Not ready: it waits for its next shot while the enemy keeps within 1.2 times its range.
            const std::uint32_t enemy = m.enemy;
            if (Item* it = push(o, dep::ObjectCloserThan)) {
                it->args[0] = bitsOf((m.def ? m.def->missileRange : 0.0f) * 1.2f);
                it->args[2] = enemy;
            }
            if (Item* it = push(o, MonsterAction::Wait)) {
                it->args[0] = m.nextMelee;
            }
            break;
        }
        disenchant(world, o, Enchant::Invisible);
        disenchant(world, o, Enchant::Invulnerable);
        o.timer = fight.frame;
        if (m.def) {
            m.nextMelee = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.def->missileMinDelay, m.def->missileMaxDelay)(fight.rng));
        }
        playCreatureSound(world, o, things::SoundSlot::MissileAttackInit);
        break;
    }
    case MonsterAction::Fight: { // mobActionFightStart
        playCreatureSound(world, o, things::SoundSlot::Engage);
        if (world.script) {
            world.script->callBlock(m.onChangeFocus, m.enemy, o.extent);
        }
        m.status |= ms::Alert;
        world.monstersLookAt = fight.frame;
        look(o);
        run(m);
        break;
    }
    case MonsterAction::MeleeAttack: { // mobActionMelee1
        if (fight.frame < m.nextMelee) {
            // Not ready: it waits for its next blow while the enemy keeps within 1.2 times its reach.
            const std::uint32_t enemy = m.enemy;
            if (Item* it = push(o, dep::ObjectCloserThan)) {
                it->args[0] = bitsOf((m.def ? m.def->meleeRange : 0.0f) * 1.2f);
                it->args[2] = enemy;
            }
            if (Item* it = push(o, MonsterAction::Wait)) {
                it->args[0] = m.nextMelee;
            }
            break;
        }
        if (o.subclasses & 0x10u) {
            const int cost = weaponStamina(m.weaponFlags);
            m.stamina = static_cast<int>(m.stamina) < cost ? static_cast<std::uint8_t>(m.stamina - cost) : 0;  // kept as the exe has it (an original bug)
        }
        // The nearest creature, not dead, in the box of 50 about it and in front of it
        // (its cosine over 0.5): one that isn't its enemy, before an NPC holding a war hammer
        // (its weapon's subclass 0x4000), makes it step aside instead: the blow dropped, it faces
        // its own way, a quarter to half a second on (DEPENDENCY_TIME), backing off from the friend
        // (FLEE from its place).
        {
            ServerObject* ahead = nullptr;
            float best = 50.0f + 1.0f;
            const auto [fx, fy] = directionVector(o.direction);
            world.unitsInRect(o.x - 50.0f, o.y - 50.0f, 50.0f + o.x, 50.0f + o.y, [&](ServerObject& t) { // (getUnitsInRect, by the grid)
                if (!(t.classes & (oc::Monster | oc::Player)) || (t.flags & ObjFlag::Dead)) { // (no 0x20 test)
                    return;
                }
                const float dx = t.x - o.x;
                const float dy = t.y - o.y;
                const float d = std::sqrt(dx * dx + dy * dy) + 0.0001f;
                if (d < best && 0.5f < dx / d * fx + dy / d * fy) {
                    ahead = &t;
                    best = d;
                }
            });
            const ServerObject* held = m.weapon ? world.byExtent(m.weapon) : nullptr;
            if (ahead && !isEnemy(world, o, *ahead) && (o.subclasses & 0x10u) && held && (held->subclasses & 0x4000u)) {
                const float ax = ahead->x;
                const float ay = ahead->y;
                pop(o);
                if (Item* it = push(o, MonsterAction::FaceAngle)) {
                    it->args[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(o.direction)));
                }
                if (Item* it = push(o, dep::Time)) {
                    it->args[0] = static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30 >> 2, 30 >> 1)(fight.rng)) + fight.frame;
                }
                if (Item* it = push(o, MonsterAction::Flee)) {
                    it->args[0] = bitsOf(ax);
                    it->args[1] = bitsOf(ay);
                    it->args[2] = 0;
                }
                break;
            }
        }
        disenchant(world, o, Enchant::Invisible);
        disenchant(world, o, Enchant::Invulnerable);
        o.timer = fight.frame;
        if (m.def) {
            m.nextMelee = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(m.def->meleeMinDelay, m.def->meleeMaxDelay)(fight.rng));
        }
        playCreatureSound(world, o, things::SoundSlot::MeleeAttackInit);
        break;
    }
    case MonsterAction::Dying: { // mobGenericDeath
        playCreatureSound(world, o, things::SoundSlot::Die);
        if (world.script) {
            world.script->callBlock(m.onDeath, 0, o.extent);
        }
        if (m.def) {
            deathFunction(o, m.def->dieFn);
        }
        break;
    }
    case MonsterAction::Dead: // mobActionDead1
        // Undead (subclass 0x10000) killed by type 0xE leave a released soul
        // (createReleasedSoul).
        if (o.hurtType == 0xE && (o.subclasses & 0x10000u) && fight.things) {
            if (ServerObject* soul = world.createAt(*fight.things, "ReleasedSoul", o.x, o.y, fight.frame)) {
                soul->direction = o.direction;
            }
        }
        o.vx = o.vy = 0;
        o.forceX = o.forceY = 0;
        o.pushX = o.pushY = 0;
        if (m.def) {
            deathFunction(o, m.def->deadFn);
        }
        if (isZombie(o)) {
            // A zombie lies dead ZombieDeadDuration (its two numbers: the least and most).
            m.deadFrames = static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(static_cast<int>(world.number("ZombieDeadDuration", 0)),
                                                                                      static_cast<int>(world.number("ZombieDeadDuration", 1)))(fight.rng));
            o.flags |= 0x10u;
        } else {
            o.flags |= 0x18u;  // flag 0x10 and ALLOW_OVERLAP
        }
        break;
    default:
        break;
    }
}

// An action's update, each frame it's on top (the action table's second).
void Monsters::act(ServerObject& o, int action)
{
    auto& m = *o.monster;
    Item& top = m.stack[static_cast<std::size_t>(m.top)];
    switch (action) {
    case Idle:
        idle(o);
        break;
    case MonsterAction::Wait: // (till its frame)
        if (top.args[0] <= fight.frame) {
            pop(o);
        }
        break;
    case MonsterAction::WaitRelative: // (its frames after the stack last changed)
        if (top.args[0] + m.actionFrame < fight.frame) {
            pop(o);
        }
        break;
    case MonsterAction::Escort:
        escort(o);
        break;
    case MonsterAction::Guard:
        guard(o);
        break;
    case MonsterAction::MoveTo:
    case MonsterAction::MoveToHome:
        moveTo(o);
        break;
    case MonsterAction::FarMoveTo: // mobActionMoveToFar: a blow answered, an enemy fought at will, on
        if ((defends(m) || atWill(m)) && reactToHurt(o)) {
            break;
        }
        if (atWill(m)) {
            if (const ServerObject* e = m.enemy ? fight.world.byExtent(m.enemy) : nullptr) {
                pushFight(o, *e, fight.frame);
            }
        }
        moveTo(o); // (whatever's on top now: a FIGHT just pushed's point too)
        break;
    case MonsterAction::Roam:
        roam(o);
        break;
    case MonsterAction::RandomWalk:
        randomWalk(o);
        break;
    case MonsterAction::MissileAttack:
        shoot(o);
        break;
    case MonsterAction::Retreat:
        retreat(o);
        break;
    case MonsterAction::Flee:
        flee(o);
        break;
    case MonsterAction::RetreatToMaster:
        retreatToMaster(o);
        break;
    case MonsterAction::Dodge:
        sidestep(o);
        break;
    case MonsterAction::BlockAttack: // monsterShieldBlockStart: half a second past the last missile coming
        if (incoming(o)) {
            top.args[0] = fight.frame + 15;
        }
        if (top.args[0] < fight.frame) {
            pop(o);
            if (!(o.subclasses & 0x10u)) {
                push(o, MonsterAction::BlockFinish);
            }
        }
        break;
    case MonsterAction::CastSpellOnObject: // while its target's about
        if (top.args[2] != 0) {
            castAction(o, 0);
            if (m.animDone == 0) {
                break;
            }
        }
        pop(o);
        break;
    case MonsterAction::CastSpellOnLocation:
        castAction(o, 1);
        if (m.animDone != 0) {
            pop(o);
        }
        break;
    case MonsterAction::CastDurationSpell: // (till its TIME runs out)
        if (top.args[2] == 0) {
            pop(o);
        } else {
            castAction(o, 0);
        }
        break;
    case MonsterAction::BlockFinish: // (monsterShieldBlockStop)
    case MonsterAction::WeaponBlock:
        if (m.animDone != 0) {
            pop(o);
        }
        break;
    case MonsterAction::Hunt: // mobActionHunt: it turns aggressive and roams
        m.aggression = 0.83f;
        if (Item* it = push(o, MonsterAction::Roam)) {
            it->args[0] = 0;
            it->args[2] = (it->args[2] & ~0xFFu) | 0x80u;  // roaming waypoints (link flag 0x80)
        }
        break;
    case MonsterAction::Fight:
        fightUpdate(o);
        break;
    case MonsterAction::MeleeAttack: // mobActionMeleeAtt
        if (!(o.subclasses & 0x10u)) {
            if (!m.def || m.def->strikeFn.empty()) {
                pop(o);
                break;
            }
            // At its strike frame (monster.bin's MELEE_ATTACK_FRAME, its first moment there):
            // the STRIKE_FUNCTION, then its hit (MELEE_ATTACK_HITTING) or miss sound.
            if (m.animFrame == static_cast<std::uint8_t>(m.def->meleeFrame) && m.animDelay == 0) {
                const bool hit = strike(o);
                playCreatureSound(fight.world, o, hit ? things::SoundSlot::MeleeAttackHitting : things::SoundSlot::MeleeAttackMissing);
            }
            if (m.animDone != 0) {
                pop(o);
            }
        } else {
            // An NPC's: the player's attack (playerAttack, npc.hpp), done when that's over. (A BOT's,
            // monster status 0x20000, morphs into its player record for it, mobMorphToPlayer: none of the
            // campaign's NPCs is one.)
            if (!npcAttack(fight, o)) {
                pop(o);
            }
        }
        break;
    case MonsterAction::FaceLocation:
        faceTowards(o, floatOf(top.args[0]), floatOf(top.args[1]));
        break;
    case MonsterAction::FaceObject: {
        const ServerObject* t = top.args[0] ? fight.world.byExtent(top.args[0]) : nullptr;
        if (!t) {
            pop(o);
        } else {
            faceTowards(o, t->x, t->y);
        }
        break;
    }
    case MonsterAction::FaceAngle: { // 8 a frame towards the direction
        const int way = static_cast<int>(top.args[0]);
        const auto [wx, wy] = directionVector(way);
        const auto [fx, fy] = directionVector(o.direction);
        int d = m.turnTo >= 0 ? m.turnTo : o.direction;
        d += X87(0.0f) <= X87(wy) * fx - X87(fy) * wx ? 8 : -8; // (both products on the x87's stack)
        m.turnTo = wrap256(d);
        if (facing(o, wx, wy)) {
            pop(o);
        }
        break;
    }
    case MonsterAction::SetAngle: // at once
        m.turnTo = -1;
        o.direction = static_cast<std::uint16_t>(wrap256(static_cast<std::int16_t>(top.args[0])));
        o.touch();
        pop(o);
        break;
    case MonsterAction::Dying: // (till its animation's done)
        if (m.animDone != 0) {
            pop(o);
        }
        break;
    case MonsterAction::Dead: // mobActionDead2
        if (!isZombie(o)) {
            fight.world.removeUpdatable(o); // (unitRemoveFromUpdatable, at once: a body)
            o.updates = false; // (Update NULL: it updates no more)
            // Its routes, what it saw and its enemy forgotten.
            m.field523 = 0;
            m.waypoints.fill(0);
            m.waypointCount = 0;
            m.waypointIndex = 0;
            m.seen.fill(0);
            m.seenCount = 0;
            m.enemy = 0;
            if (m.def && (m.def->status & ms::DestroyWhenDead)) {
                o.pendingDelete = true;
            }
        } else if (m.status & ms::OnFire) {
            // A burning zombie's done for (netSparkExplosionFx, 100).
            const auto ix = static_cast<std::uint32_t>(std::clamp(o.x, 0.0f, 65535.0f));
            const auto iy = static_cast<std::uint32_t>(std::clamp(o.y, 0.0f, 65535.0f));
            fight.world.broadcasts().push_back({{0x93, static_cast<std::uint8_t>(ix), static_cast<std::uint8_t>(ix >> 8), static_cast<std::uint8_t>(iy),
                                                  static_cast<std::uint8_t>(iy >> 8), 100},
                                                 false, 0});
            burnZombie(o);
        } else if (m.deadFrames < fight.frame - m.actionFrame && !(m.status & ms::StayDead) && m.enemy != 0) {
            raiseZombie(o); // (its time dead over, an enemy in sight)
        }
        break;
    case MonsterAction::GetUp: // mobActionGetUp: its animation over
        if (m.animDone != 0) {
            pop(o);
        }
        break;
    case MonsterAction::Report: // done, its end-of-waypoint script
        pop(o);
        if (fight.world.script) {
            fight.world.script->callBlock(m.onEndOfWaypoint, 0, o.extent);
        }
        break;
    case MonsterAction::PickupObject: { // mobActionPickupObject
        // Within 75 and reachable, taken (its PICKUP), a health potion used too; done either way.
        ServerObject* t = top.args[0] ? fight.world.byExtent(top.args[0]) : nullptr;
        if (t && dist2(o, t->x, t->y) < 5625.0f && canInteract(fight.world, o, *t) && fight.things) {  // within 75
            Items items{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
            items.pickUp(o, *t, true);
            if (t->subclasses & 0x10u) {
                items.use(o, *t);
            }
        }
        pop(o);
        break;
    }
    case MonsterAction::MorphIntoChest: // its animation over, a chest
    case MonsterAction::MorphBackToSelf: // a mimic again
        if (m.animDone != 0) {
            pop(o);
            if (action == MonsterAction::MorphIntoChest) {
                m.status |= ms::Morphed;
            } else {
                m.status &= ~ms::Morphed;
            }
            o.touch(); // (monsterMarkUpdate)
        }
        break;
    default:
        break; // (MOVE_TO and the paths: B2; the rest: stages C-E)
    }
}

void Monsters::end(ServerObject& o, int action)
{
    // The actions' ends (the table's third, on a pop).
    auto& m = *o.monster;
    switch (action) {
    case MonsterAction::Escort:
        walk(m);
        break;
    case MonsterAction::Fight:
        m.status &= ~ms::Alert;
        walk(m);
        break;
    case MonsterAction::MoveToHome:
    case MonsterAction::Flee:
    case MonsterAction::RetreatToMaster:
        walk(m);
        break;
    case MonsterAction::CastDurationSpell: // (the lasting spell ends)
        cancelLastingSpells(fight.world, m.stack[static_cast<std::size_t>(m.top)].args[0], o);
        break;
    case MonsterAction::Dying: // (zombieBurnDeleteCheck: a zombie dying burning is gone)
        if (isZombie(o) && (m.status & ms::OnFire)) {
            burnZombie(o);
        }
        break;
    default:
        break;
    }
}

void Monsters::aside(ServerObject& o, int action)
{
    // The table's fourth, the action put aside by a push: a wait, an attack, a block or a turn
    // just ends (ai_action_pop).
    switch (action) {
    case MonsterAction::Wait:
    case MonsterAction::WaitRelative:
    case MonsterAction::MeleeAttack:
    case MonsterAction::MissileAttack:
    case MonsterAction::BlockAttack:
    case MonsterAction::BlockFinish:
    case MonsterAction::WeaponBlock:
    case MonsterAction::FaceLocation:
    case MonsterAction::FaceObject:
    case MonsterAction::FaceAngle:
    case MonsterAction::SetAngle:
        pop(o);
        break;
    case MonsterAction::Escort:
    case MonsterAction::MoveToHome:
        walk(*o.monster);
        break;
    case MonsterAction::Roam: // (the waypoint it was making for forgotten)
        o.monster->stack[static_cast<std::size_t>(o.monster->top)].args[0] = 0;
        break;
    case MonsterAction::CastDurationSpell: // (the lasting spell ends, and the cast)
        cancelLastingSpells(fight.world, o.monster->stack[static_cast<std::size_t>(o.monster->top)].args[0], o);
        pop(o);
        break;
    default:
        break;
    }
}

void Monsters::dies(ServerObject& o)
{
    // monsterCallDieFn.
    World& world = fight.world;
    if (!o.monster) {
        return;
    }
    // (A quest's tally: with the quests.) Those watching it watch it no more. (The players' units
    // in the objects' order: what a walk of them all finds.)
    for (ServerObject* p : world.playerUnits()) {
        if (p->player && possessed(world, *p) == &o) {
            observeClear(fight, *p);
        }
    }
    clear(o);
    push(o, MonsterAction::Dead);
    push(o, MonsterAction::Dying);
    if (isZombie(o)) {
        return;
    }
    o.flags &= ~0x80u;
    setEnchantments(o, 0);
    clearEnchantments(o); // (unitClearBuffs)
    if (o.monster->status & ms::Summoned) {
        o.decayAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(10, 20)(fight.rng)) * 30u;
    } else if (world.quest()) {
        o.decayAt = fight.frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(5, 8)(fight.rng)) * 30u;
    }
    // Its player told it's lost; what it owned passed to its owner
    // (unitTransferSlaves); no one's.
    loseCreature(world, o);
    o.subclasses &= ~0x100u;  // MIGRATE off
    transferSlaves(world, o);
    clearOwner(world, o);
    if (!(o.subclasses & 0x2000u) && fight.things) {  // not a BOMBER: its glyph is for its death
        Items{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.dropAll(o);
    }
    // Out of a solo game, its last blow put down to damage type 2 (a Berserker Charge's): the
    // charge is ready again for the warrior behind it.
    if (!(world.gameFlags & GameFlag::Solo) && o.monster->killKind == 2 && o.monster->killType == 2 && o.hurtBy != 0) {
        if (ServerObject* k = fight.parentPlayer(world.byExtent(o.hurtBy)); k && (k->classes & oc::Player)) {
            resetAbility(world, *k, 1);
        }
    }
    // (The solo game's kill bookkeeping: with it.)
    if (world.quest()) {
        unlinkSpawn(world, o); // (not a zombie's)
        // The kill to the player behind what last hurt it.
        if (ServerObject* by = o.hurtBy ? fight.parentPlayer(world.byExtent(o.hurtBy)) : nullptr; by && by->player) {
            questKill(*by);
        }
    }
}

namespace {

/// The creature guide.
constexpr const char* kGuide[41] = {
    nullptr, "Bat", "BlackBear", "Bear", "Beholder", "Bomber", "CarnivorousPlant", "AlbinoSpider", "SmallAlbinoSpider",
    "EvilCherub", "EmberDemon", "Ghost", "GiantLeech", "Imp", "FlyingGolem", "MechanicalGolem", "Mimic", "GruntAxe",
    "OgreBrute", "OgreWarlord", "Scorpion", "Shade", "Skeleton", "SkeletonLord", "Spider", "SmallSpider", "SpittingSpider",
    "StoneGolem", "Troll", "Urchin", "Wasp", "WillOWisp", "Wolf", "BlackWolf", "WhiteWolf", "Zombie", "VileZombie",
    "Demon", "Lich", "WizardGreen", "UrchinShaman",
};

// The guide's size of a creature: SMALL_MONSTER 1, MEDIUM_MONSTER 2, else 4.
int sizeOfSubclasses(std::uint32_t subclasses)
{
    if (subclasses & 1u) {
        return 1;
    }
    return (subclasses & 2u) ? 2 : 4;
}

} // namespace

const char* guideName(int index)
{
    return 0 < index && index < 41 ? kGuide[index] : nullptr;
}

int guideIndex(std::string_view type)
{
    for (int i = 1; i < 41; ++i) {
        if (type == kGuide[i]) {
            return type == "Bomber" ? 0 : i; // (the Bomber's type left out of the guide's)
        }
    }
    return 0;
}

int guideSize(const things::ThingDb& things, int index)
{
    const char* name = guideName(index);
    const things::ThingDef* def = name ? things.findThing(name) : nullptr;
    return def ? sizeOfSubclasses(things::thingSubclasses(*def)) : 4;
}

int creatureSize(const ServerObject& creature)
{
    return sizeOfSubclasses(creature.subclasses);
}

bool controls(const World& world, const ServerObject& owner, const ServerObject& creature)
{
    // creatureIsMonitored.
    if ((!(creature.classes & oc::Monster) || (creature.flags & ObjFlag::Dead)) && !isZombie(creature)) {
        return false;
    }
    if (!creature.monster || !(creature.monster->status & ms::Summoned)) {
        return false;
    }
    const auto find = [&](std::uint32_t e) -> const ServerObject* {
        const auto it = world.objects().find(e);
        return e != 0 && it != world.objects().end() ? &it->second : nullptr;
    };
    for (const ServerObject* o = find(creature.owner); o; o = find(o->owner)) {
        if (o == &owner) { // (unitHasThatParent)
            return true;
        }
    }
    return false;
}

bool roomFor(const World& world, const things::ThingDb& things, const ServerObject& owner, int index)
{
    int used = 0;
    for (const auto& [e, o] : world.objects()) {
        if (o.owner == owner.extent && !o.gone() && controls(world, owner, o)) {
            used += creatureSize(o);
        }
    }
    return guideSize(things, index) + used < 5;  // 5 in all at most
}

ServerObject* summonAt(Fight& fight, std::string_view type, float x, float y, ServerObject* owner, int direction)
{
    // unitDoSummonAt.
    World& world = fight.world;
    if (!fight.things) {
        return nullptr;
    }
    ServerObject* c = world.createAt(*fight.things, type, x, y, fight.frame);
    if (!c || !c->monster) {
        return c;
    }
    // (createAt's unitSetOwner: the owner, its owned list, the enemy forgotten, and monsterMarkUpdate:
    // its owner's client, and its teammates', told it's their friend, MSG_OBJECT_FRIEND_ADD: the dot
    // over our summons and their blue hover ring.)
    if (owner) {
        setOwner(world, owner, *c);
    }
    c->direction = static_cast<std::uint16_t>(direction & 0xFF);
    c->monster->status |= ms::Summoned;
    if (owner) {
        if (owner->player) {
            orderCreatures(fight, *owner, c, owner->player->creatureOrder);
            c->monster->startAction = MonsterAction::Invalid;
            c->subclasses |= 0x80u;  // MONITOR: its player watches it
            // MSG_REPORT_ACQUIRE_CREATURE (0x6C: its code, its type) to its player, then its
            // health and its minimap mark (reportAcquired), then its record straight away
            // (SrvNet_SendSimpleObjReliable).
            reportAcquired(world, *owner, *c);
            simpleObjTo(world, *owner, *c);
            if (owner->team != 0) {
                addTeamMember(world, owner->team, *c, true, false, fight.rng, fight.frame);
            }
            c->subclasses |= 0x100u;  // MIGRATE
            return c;
        }
        orderCreatures(fight, *owner, c, 4);
        c->monster->startAction = MonsterAction::Invalid;
    }
    return c;
}

void orderCreatures(Fight& fight, ServerObject& owner, ServerObject* creature, int order)
{
    // AI_OrderCreatures.
    World& world = fight.world;
    Monsters mons{fight};
    const auto enact = [&](ServerObject& unit) {
        // enactUnitOrder: a live monster (a dead zombie to be banished).
        if (!(unit.classes & oc::Monster) || !unit.monster || !((isZombie(unit) && order == 0) || !(unit.flags & ObjFlag::Dead))) {
            return;
        }
        auto& m = *unit.monster;
        if (!m.def) {
            return;
        }
        // Its line to its player (SrvNet_SendUnitSpeechStringId: MSG_TEXT_MESSAGE 0xA8 with flag 8,
        // a string id: the unit's code, its place chopped, the id's length with its end, the
        // duration 0, the id) and its RECOGNIZE sound.
        const auto speech = [&](std::string_view id) {
            if (owner.classes & oc::Player) {
                const std::uint16_t code = netCodeOf(unit);
                const auto x = static_cast<std::uint16_t>(static_cast<int>(unit.x)); // (__ftol)
                const auto y = static_cast<std::uint16_t>(static_cast<int>(unit.y));
                std::vector<std::uint8_t> msg = {0xA8, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), 8,
                                                 static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8), static_cast<std::uint8_t>(y),
                                                 static_cast<std::uint8_t>(y >> 8), static_cast<std::uint8_t>(id.size() + 1), 0, 0};
                msg.insert(msg.end(), id.begin(), id.end());
                msg.push_back(0);
                world.broadcasts().push_back({std::move(msg), true, owner.extent});
            }
            playCreatureSound(world, unit, things::SoundSlot::Recognize);
        };
        switch (order) {
        case 0: // Banish (banishUnit): its own; its glyphs and it gone.
            if (unit.owner == owner.extent) {
                for (const std::uint32_t e : std::vector<std::uint32_t>(unit.inventory)) {
                    if (ServerObject* g = world.byExtent(e); g && g->typeName == "Glyph") {
                        deleteObject(world, *g); // (delayedDeleteObject)
                    }
                }
                pointFx(world, 0x81, unit.x, unit.y);
                unit.pendingDelete = true;
            }
            break;
        case 1: // A player watches it (playerObserveMonster: modes.hpp).
            if (owner.player) {
                observeCreature(fight, owner, unit);
            }
            break;
        case 2: // Idle.
            speech("MonUtil.c:idle");
            m.aggression = 0.5f;
            m.status &= ~ms::HoldYourGround;
            mons.clear(unit);
            mons.push(unit, Idle);
            break;
        case 3: // Guard where it stands (shooters hold their ground), its sight 250.
            if (canMove(unit)) {
                speech("MonUtil.c:guarding");
                m.aggression = 0.5f;
                if (canShoot(unit)) {
                    m.status |= ms::HoldYourGround;
                }
                m.sightRange = 250.0f;
                mons.clear(unit);
                if (Item* it = mons.push(unit, MonsterAction::Guard)) {
                    it->args[0] = bitsOf(unit.x);
                    it->args[1] = bitsOf(unit.y);
                    it->args[2] = static_cast<std::uint32_t>(static_cast<std::int16_t>(unit.direction));
                }
            }
            break;
        case 4: // Escort its master.
            if (canMove(unit)) {
                speech("MonUtil.c:escorting");
                m.aggression = 0.83f;
                m.status &= ~ms::HoldYourGround;
                mons.clear(unit);
                if (Item* it = mons.push(unit, MonsterAction::Escort)) {
                    it->args[0] = bitsOf(owner.x);
                    it->args[1] = bitsOf(owner.y);
                    it->args[2] = owner.extent;
                }
            }
            break;
        case 5: // Hunt.
            if (canMove(unit)) {
                speech("MonUtil.c:Hunting");
                m.aggression = 0.83f;
                m.status &= ~ms::HoldYourGround;
                mons.clear(unit);
                mons.push(unit, MonsterAction::Hunt);
            }
            break;
        default:
            break;
        }
    };
    if (creature) {
        enact(*creature);
        return;
    }
    if (owner.player && (order == 3 || order == 4 || order == 5)) {
        // orderUnitLocal: kept, and MSG_REPORT_CREATURE_CMD (0xED) to its client.
        owner.player->creatureOrder = static_cast<std::uint8_t>(order);
        world.broadcasts().push_back({{0xED, static_cast<std::uint8_t>(order)}, true, owner.extent});
    }
    // Its list from the head (newest first), each it commands (SUMMONED) in turn.
    for (ServerObject* o : ownedCreatures(world, owner)) {
        if (!o->gone()) {
            enact(*o);
        }
    }
}

void reportAcquired(World& world, ServerObject& player, ServerObject& c)
{
    // SrvNet_SendReportAcquireCreature: MSG_REPORT_ACQUIRE_CREATURE (0x6C: its code,
    // its type; bit 15 of the type while a map's being saved or loaded, game flag 0x8000000), then SrvNet_SendReportTotalHealth (MSG_REPORT_TOTAL_HEALTH 0xDD: its code, health,
    // most), both reliable.
    const std::uint32_t code = netCodeOf(c);
    const std::uint32_t type = c.type | ((world.gameFlags & 0x8000000u) ? 0x8000u : 0u);
    world.broadcasts().push_back({{0x6C, lo(code), hi(code), lo(type), hi(type)}, true, player.extent});
    if (c.maxHealth != 0) {
        world.broadcasts().push_back({{0xDD, lo(code), hi(code), lo(c.health), hi(c.health), lo(c.maxHealth), hi(c.maxHealth)}, true, player.extent});
    }
    markMinimap(world, player, c, 1); // (each caller's next: netMarkMinimapObject)
}

void simpleObjTo(World& world, const ServerObject& player, const ServerObject& o)
{
    // SrvNet_SendSimpleObjReliable: MSG_SIMPLE_OBJ (0x2F: its code, its type, x, y
    // truncated: Math_FloatToInt), sequenced and reliable.
    const std::uint32_t code = netCodeOf(o);
    const auto x = static_cast<std::uint16_t>(static_cast<int>(o.x));
    const auto y = static_cast<std::uint16_t>(static_cast<int>(o.y));
    world.broadcasts().push_back({{0x2F, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(o.type),
                                   static_cast<std::uint8_t>(o.type >> 8), static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8),
                                   static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8)},
                                  true, player.extent});
}

void ownedNow(World& world, ServerObject& creature)
{
    creature.ownedOrder = ++world.ownedCount; // (unitSetOwner: the list's head)
    if (creature.monster) {
        creature.monster->enemy = 0; // (monsterResetEnemy)
    }
}

void clearOwner(World& world, ServerObject& o)
{
    // unitClearOwner: a player's creature it controls is lost to it (no longer
    // its, MSG_REPORT_LOSE_CREATURE, off its minimap); out of its owner's list; a monster's enemy
    // forgotten (monsterResetEnemy); a creature's or player's friendship to each
    // player marked afresh (monsterMarkUpdate). An owner already gone from the world
    // (ours only: the exe's pointer can't dangle) clears it all the same.
    if (o.owner == 0) {
        return;
    }
    ServerObject* owner = world.byExtent(o.owner);
    if (owner && (owner->classes & oc::Player) && controls(world, *owner, o)) {
        loseCreature(world, o);
    }
    o.owner = 0;
    if (o.monster) {
        o.monster->enemy = 0;
    }
    if (o.classes & (oc::Monster | oc::Player)) {
        markFriends(world, o);
    }
}

void setOwner(World& world, ServerObject* owner, ServerObject& o)
{
    // unitSetOwner: let go of its old owner first; an owner being deleted (flag
    // 0x20: pendingDelete) passes it up to its own owner.
    clearOwner(world, o);
    while (owner && owner->pendingDelete) {
        owner = owner->owner ? world.byExtent(owner->owner) : nullptr;
    }
    o.owner = owner ? owner->extent : 0;
    if (owner) {
        ownedNow(world, o);
    } else if (o.monster) {
        o.monster->enemy = 0;
    }
    if (o.classes & (oc::Monster | oc::Player)) {
        markFriends(world, o);
    }
}

namespace {

/// unitIsHostileMimic: a player's friend (not its enemy), but a quest's ownerless
/// Mimic.
bool friendOf(const World& world, const ServerObject& player, const ServerObject& o)
{
    if ((world.gameFlags & GameFlag::Quest) && o.typeName == "Mimic" && o.owner == 0) {
        return false;
    }
    return !isEnemy(world, player, o);
}

/// Its friendship to the player of bit `bit` as it is now, a change owed.
void setFriendOf(ServerObject& o, std::uint32_t bit, bool now)
{
    if (now != ((o.friendOf & bit) != 0)) {
        o.friendOf ^= bit;
        o.friendOwed |= bit;
    }
}

} // namespace

void markFriends(World& world, ServerObject& o)
{
    // monsterMarkUpdate (each player has its unit here).
    for (const ServerObject* p : world.playerUnits()) {
        if (!p->gone()) {
            setFriendOf(o, 1u << (p->player->index & 31u), friendOf(world, *p, o));
        }
    }
}

void postCreateNotify(World& world, ServerObject& o)
{
    // unitPostCreateNotify: both bit sets cleared, then each player (with a unit) it's a friend
    // of marked and owed (a player its own friend too: never sent to itself).
    o.friendOf = 0;
    o.friendOwed = 0;
    for (const ServerObject* p : world.playerUnits()) {
        if (!p->gone() && friendOf(world, *p, o)) {
            const std::uint32_t bit = 1u << (p->player->index & 31u);
            o.friendOf |= bit;
            o.friendOwed |= bit;
        }
    }
}

void friendsOf(World& world, ServerObject& player)
{
    // The walk the join (SrvNet_OnIncomingClient) and Team_AddMember make: every object's bit for
    // the player cleared, a creature's or player's set afresh, the change owed.
    if (!player.player) {
        return;
    }
    const std::uint32_t bit = 1u << (player.player->index & 31u);
    for (auto& [extent, o] : world.objects()) {
        o.friendOf &= ~bit;
        o.friendOwed &= ~bit;
        if (o.classes & (oc::Monster | oc::Player)) {
            setFriendOf(o, bit, friendOf(world, player, o));
        }
    }
}

void refriend(World& world, ServerObject& player)
{
    // Team_AddMember's end (and another team call's): SrvNet_SendResetFriends (every object's
    // bit for it cleared, a creature's or player's set afresh), the player marked, then its
    // creatures and players (its owned list's 6 classes).
    if (!player.player) {
        return;
    }
    world.broadcasts().push_back({{0x36}, true, player.extent}); // MSG_RESET_FRIENDS
    friendsOf(world, player);
    markFriends(world, player);
    for (auto& [extent, o] : world.objects()) {
        if (o.owner == player.extent && (o.classes & (oc::Monster | oc::Player))) {
            markFriends(world, o);
        }
    }
}

void transferSlaves(World& world, ServerObject& o)
{
    // unitTransferSlaves: its list from the head (newest first), each to the head
    // of its owner's (so their order turns round).
    std::vector<ServerObject*> slaves;
    for (auto& [e, s] : world.objects()) {
        if (s.owner == o.extent && &s != &o) {
            slaves.push_back(&s);
        }
    }
    if (slaves.empty()) {
        return;
    }
    std::ranges::sort(slaves, [](const ServerObject* a, const ServerObject* b) {
        return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
    });
    ServerObject* owner = o.owner ? world.byExtent(o.owner) : nullptr;
    for (ServerObject* s : slaves) {
        setOwner(world, owner, *s);
    }
}

void reportMonitored(World& world, ServerObject& player, ServerObject& c)
{
    // SrvNet_SendReportMonitorCreature: MSG_REPORT_MONITOR_CREATURE (0xDB: its code, its
    // type), then SrvNet_SendReportTotalHealth (MSG_REPORT_TOTAL_HEALTH 0xDD), both reliable.
    const std::uint32_t code = netCodeOf(c);
    world.broadcasts().push_back({{0xDB, lo(code), hi(code), lo(c.type), hi(c.type)}, true, player.extent});
    if (c.maxHealth != 0) {
        world.broadcasts().push_back({{0xDD, lo(code), hi(code), lo(c.health), hi(c.health), lo(c.maxHealth), hi(c.maxHealth)}, true, player.extent});
    }
}

void becomePet(World& world, ServerObject& player, ServerObject& creature)
{
    // unitBecomePet: its (subclass 0x80), its player told to watch its health
    // (SrvNet_SendReportMonitorCreature: MSG_REPORT_MONITOR_CREATURE 0xDB, its code and
    // type, then MSG_REPORT_TOTAL_HEALTH), on its minimap (bit 1), then the player's
    // (unitSetOwner: its old owner's let go first, which for a player that
    // controls it takes subclass 0x80 off again).
    if (!player.player) {
        return;
    }
    creature.subclasses |= 0x80u;
    reportMonitored(world, player, creature);
    markMinimap(world, player, creature, 1);
    setOwner(world, &player, creature);
}

void removeMonitors(World& world, ServerObject& player, ServerObject& creature)
{
    // monsterRemoveMonitors: no longer its (subclass 0x80), its player told
    // (SrvNet_SendReportUnmonitorCreature: MSG_REPORT_UNMONITOR_CREATURE 0xDC, its
    // code), off its minimap, nobody's (unitClearOwner).
    if (!player.player) {
        return;
    }
    creature.subclasses &= ~0x80u;
    const std::uint32_t code = netCodeOf(creature);
    world.broadcasts().push_back({{0xDC, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8)}, true, player.extent});
    unmarkMinimap(world, player, creature, 1);
    clearOwner(world, creature);
}

bool monsterCanMelee(const ServerObject& m) { return m.monster && canMelee(m); }
bool monsterCanShoot(const ServerObject& m) { return m.monster && canShoot(m); }

std::vector<ServerObject*> ownedCreatures(World& world, const ServerObject& owner)
{
    // playerObserverFindGoodSlave2 / playerObserverFindGoodSlave's walk: its list (newest first),
    // the monsters a player commands (status bit 0x80: SUMMONED).
    std::vector<ServerObject*> out;
    for (auto& [e, o] : world.objects()) {
        if (o.owner == owner.extent && (o.classes & oc::Monster) && o.monster && (o.monster->status & ms::Summoned)) {
            out.push_back(&o);
        }
    }
    std::ranges::sort(out, [](const ServerObject* a, const ServerObject* b) {
        return a->ownedOrder != b->ownedOrder ? a->ownedOrder > b->ownedOrder : a->extent > b->extent;
    });
    return out;
}

void scriptMove(Fight& fight, ServerObject& o, const ServerWaypoint& wp)
{
    // server_scriptMoveTo's monster: its stack cleared; REPORT (its end-of-waypoint
    // script) at the bottom, roaming on from the waypoint when it has links (by its roaming
    // flags), and over them a long walk there.
    if (!o.monster || (o.flags & ObjFlag::Dead)) {
        return;
    }
    Monsters ai{fight};
    ai.clear(o);
    if (Item* it = ai.push(o, MonsterAction::Report)) {
        it->args[0] = 8;
    }
    if (!wp.links.empty()) {
        if (Item* it = ai.push(o, MonsterAction::Roam)) {
            it->args[0] = static_cast<std::uint32_t>(&wp - fight.world.waypoints().data()) + 1;
            it->args[2] = (it->args[2] & ~0xFFu) | o.monster->field333;
        }
    }
    if (Item* it = ai.push(o, MonsterAction::FarMoveTo)) {
        it->args[0] = bitsOf(wp.x);
        it->args[1] = bitsOf(wp.y);
        it->args[2] = 0;
    }
}

ServerObject* bomberGlyph(World& world, const things::ThingDb& things, ServerObject& bomber, bool allThree)
{
    if (!bomber.monster) {
        return nullptr;
    }
    return carriedGlyph(world, things, bomber, bomber.monster->bombSpells, allThree);
}

ServerObject* carriedGlyph(World& world, const things::ThingDb& things, ServerObject& bomber, const std::array<std::uint32_t, 3>& spells, bool allThree)
{
    if (!things.findThing("Glyph")) {
        return nullptr;
    }
    const auto n = static_cast<std::uint8_t>(std::ranges::count_if(spells, [](std::uint32_t s) { return s != 0; }));
    ServerObject& g = world.spawn(things, "Glyph", bomber.x, bomber.y);
    if (g.trap) {
        auto& t = *g.trap;
        for (std::size_t i = 0; i < (allThree ? 3u : n); ++i) {
            t.spells[i] = spells[i];
        }
        t.count = n;
        t.target = 0;
        t.x = bomber.x;
        t.y = bomber.y;
    }
    // (inventoryPutImpl: held, and its holder's.)
    g.holder = bomber.extent;
    g.owner = bomber.extent;
    bomber.inventory.push_back(g.extent);
    return &g;
}

void unlinkSpawn(World& world, ServerObject& o)
{
    if (o.spawnedBy == 0) {
        return;
    }
    if (ServerObject* g = world.byExtent(o.spawnedBy); g && g->generator) {
        --g->generator->active; // (a byte, unchecked)
    }
    o.spawnedBy = 0;
}

namespace {

/// Something (not a generator, not xStatus 0x800) holding the point within 15.
bool spotTaken(World& world, float x, float y)
{
    // (getUnitsInRect over the square of 15 about it, by the grid)
    bool taken = false;
    world.unitsInRect(x - 15.0f, y - 15.0f, x + 15.0f, y + 15.0f, [&](const ServerObject& t) {
        if (taken || (t.classes & oc::MonsterGenerator) || (t.xStatus & 0x800u)) { // (no 0x20 test)
            return;
        }
        if (t.shape == ShapeKind::Circle) {
            // (The shape's point test: the squares and sum on the x87's stack against its stored
            // radius squared.)
            const X87 dx = X87(t.newX) - x;
            const X87 dy = X87(t.newY) - y;
            if (dx * dx + dy * dy <= X87((X87(t.radius) * t.radius).f())) {
                taken = true;
            }
        } else if (t.shape == ShapeKind::Box && insideBox(t, t.newX, t.newY, x, y)) {
            taken = true;
        }
    });
    return taken;
}

/// Where a generator's monster comes (mgenSetCreaturePos): one placing by
/// sight (flag 2) with a player, 45 towards the player if clear; else a clear,
/// seen spot 45 off at one of 32 turns from a random start. It faces the player.
bool generatorSpot(Fight& fight, ServerObject& gen, float& px, float& py, const ServerObject* player, ServerObject& tmpl)
{
    World& world = fight.world;
    const std::uint8_t rayFlags = (tmpl.flags & ObjFlag::Airborne) ? 5 : 1;
    bool placed = false;
    if ((gen.generator->flags & 2u) && player) {
        // (On the x87: the differences stored, utilNormalizeVector, each place's product and sum
        // on the stack, one store.)
        float vx = (X87(player->x) - gen.x).f();
        float vy = (X87(player->y) - gen.y).f();
        if (vx == 0.0f) {
            vx += 1.0f;
        }
        if (vy == 0.0f) {
            vy += 1.0f;
        }
        util::normalizeVector(vx, vy);
        const float sx = (X87(vx) * 45.0f + gen.x).f();
        const float sy = (X87(vy) * 45.0f + gen.y).f();
        if (!spotTaken(world, sx, sy) && world.traceRay(gen.x, gen.y, sx, sy, rayFlags) && !world.noTeleportAt(sx, sy)) {
            px = sx;
            py = sy;
            tmpl.direction = static_cast<std::uint16_t>(directionOf(player->x - sx, player->y - sy));
            return true;
        }
    }
    // (On the x87: the draw stored; each turn's sum stored (FST) but its cosine taken from the
    // stack, the sine from the stored angle; x 45 (its float argument) + the place, one store.)
    float angle = util::randomFloat(fight.rng, -3.1415927f, 3.1415927f).f();
    for (int i = 0; i < 32 && !placed; ++i) {
        const X87 turned = X87(angle) + 1.88495576f;  // each try 0.6 pi (108 degrees) on (the float 0x3FF1463B)
        angle = turned.f();
        const float sx = (X87::fromDouble(util::cosNearest(turned.d())) * 45.0f + gen.x).f();
        const float sy = (X87::fromDouble(util::sinNearest(angle)) * 45.0f + gen.y).f();
        if (world.traceRay(gen.x, gen.y, sx, sy, rayFlags) && !spotTaken(world, sx, sy) && !world.noTeleportAt(sx, sy)) {
            px = sx;
            py = sy;
            placed = true;
        }
    }
    if (!placed) {
        return false;
    }
    if (player) {
        tmpl.direction = static_cast<std::uint16_t>(directionOf(player->x - gen.x, player->y - gen.y));
    }
    return true;
}

} // namespace

void generatorInit(World& world, ServerObject& gen)
{
    // MonsterGeneratorInit.
    auto& g = *gen.generator;
    const int diff = std::clamp(world.mapDifficulty, 0, 2);
    g.initialised = true;
    static constexpr const char* kMost[4] = {"GeneratorMaxActiveCreaturesHigh", "GeneratorMaxActiveCreaturesNormal",
                                             "GeneratorMaxActiveCreaturesLow", "GeneratorMaxActiveCreaturesSingular"};
    if (g.mostBy[static_cast<std::size_t>(diff)] < 4) {
        g.most = static_cast<std::uint8_t>(static_cast<int>(world.number(kMost[g.mostBy[static_cast<std::size_t>(diff)]])));
    }
    // Its facing by subclass GENERATOR_NW / NE / SE / SW (1, 2, 4, 8), from a 3 x 3 table.
    static constexpr int kFacing[9] = {0xA0, 0xC0, 0xE0, 0x80, 0, 0, 0x60, 0x40, 0x20};
    int way = -1;
    if (gen.subclasses & 1u) {
        way = 0;
    } else if (gen.subclasses & 2u) {
        way = 2;
    } else if (gen.subclasses & 4u) {
        way = 8;
    } else if (gen.subclasses & 8u) {
        way = 6;
    }
    if (way >= 0) {
        gen.direction = static_cast<std::uint16_t>(kFacing[way]);
    }
}

bool generatorRoomOnScreen(World& world, float x, float y)
{
    // For each player in a quest (questActive), in Plr_GetFirstUnit's
    // order, whose view (its half screen + 100 each way about it) holds the spot
    // (edges in): the monsters in that view's box (getUnitsInRect, by the grid; not
    // DESTROYED, DEAD only a zombie) it sees (ray flags 0x45 from it) counted; at
    // MaxOnscreenMonsterCount or more, no room. (Outside a quest no player is questActive.)
    static const things::GameDataKey kMost{"MaxOnscreenMonsterCount"};
    const int most = static_cast<int>(world.number(kMost)); // (Math_FloatToInt: chopped)
    std::vector<ServerObject*> scratch;
    for (ServerObject* pp : world.playerObjects(scratch)) {
        ServerObject& p = *pp;
        if (p.player->questActive == 0) {
            continue;
        }
        const auto hw = static_cast<float>(p.player->viewHalfWidth);
        const auto hh = static_cast<float>(p.player->viewHalfHeight);
        const float left = (p.x - hw) - 100.0f;
        const float top = (p.y - hh) - 100.0f;
        const float right = hw + p.x + 100.0f;
        const float bottom = hh + p.y + 100.0f;
        if (!(left <= x && x <= right && top <= y && y <= bottom)) {
            continue;
        }
        int seen = 0;
        world.unitsInRect(left, top, right, bottom, [&](ServerObject& t) {
            if (!(t.classes & oc::Monster) || (t.flags & ObjFlag::Destroyed) || t.pendingDelete) {
                return;
            }
            if ((t.flags & ObjFlag::Dead) && !isZombie(t)) {
                return;
            }
            if (world.traceRay(p.x, p.y, t.x, t.y, 0x45)) {  // Trace::Sight
                ++seen;
            }
        });
        if (most <= seen) {
            return false;
        }
    }
    return true;
}

void generatorUpdate(Fight& fight, ServerObject& gen)
{
    World& world = fight.world;
    if (!gen.generator) {
        return;
    }
    auto& g = *gen.generator;
    const int diff = std::clamp(world.mapDifficulty, 0, 2);
    if (!g.initialised) {
        generatorInit(world, gen);
    }
    if ((gen.xStatus & 0x800u) || !(gen.flags & ObjFlag::Enabled) || (gen.flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
        return;
    }
    gen.touch(); // (unitNeedSync)
    const std::uint32_t since = fight.frame - g.lastSpawn;
    static const things::GameDataKey kRates[5] = {
        things::GameDataKey{"SpawnRateHighValue"}, things::GameDataKey{"SpawnRateNormalValue"}, things::GameDataKey{"SpawnRateLowValue"},
        things::GameDataKey{"SpawnRateVeryLowValue"}, things::GameDataKey{"SpawnRateVeryVeryLowValue"}};
    static const things::GameDataKey kHardcoreStage{"QuestHardcoreStage"}, kHardcoreIncrease{"QuestHardcoreSpawnRateIncrease"},
        kHardcoreCap{"QuestHardcoreSpawnCap"};
    const std::uint8_t rk = g.rate[static_cast<std::size_t>(diff)];
    const std::uint32_t rate = rk < 5 ? static_cast<std::uint32_t>(static_cast<int>(world.number(kRates[rk]))) : 0xFFFFFFFFu;
    std::uint32_t wait = rate;
    // Past a quest's hardcore stage: sooner by QuestHardcoreSpawnRateIncrease a stage, but not
    // below QuestHardcoreSpawnCap of the rate.
    const auto hardcore = static_cast<std::uint32_t>(static_cast<int>(world.number(kHardcoreStage)));
    if (hardcore <= static_cast<std::uint32_t>(world.questStage)) {
        const std::uint32_t less = (static_cast<std::uint32_t>(world.questStage) - hardcore + 1) *
                                   static_cast<std::uint32_t>(static_cast<int>(world.number(kHardcoreIncrease)));
        wait = less <= rate ? rate - less : 0;
        // (On the x87: FILD both as 64-bit, the quotient on the stack against the stored cap, a
        // NaN (no rate) taking the cap too; the rate x cap stored, Math_FloatToInt.)
        const float cap = world.number(kHardcoreCap);
        const X87 r = X87::fromDouble(static_cast<double>(rate));
        if (!(X87::fromDouble(static_cast<double>(wait)) / r >= X87(cap))) {
            wait = static_cast<std::uint32_t>(util::floatToInt(r * cap));
        }
    }
    // Due, under its most out, and only on frames with bit 8 set (8 on, 8 off).
    if (!(wait < since && g.active < g.most && (fight.frame & 8u) != 0)) {
        return;
    }
    auto& group = g.groups[static_cast<std::size_t>(diff)];
    if (group.empty()) {
        return;
    }
    auto& tp = group[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(group.size()) - 1)(fight.rng))];
    ServerObject& tmpl = *tp.object;
    // mobGeneratorPick: by any player within 300 (flag 1); or where one, in
    // reach of its view (its half screen's diagonal) and sight (ray flags 0x45), is, at random.
    float px = 0;
    float py = 0;
    bool found = false;
    std::vector<ServerObject*> seers;
    std::vector<ServerObject*> scratch;
    for (ServerObject* pp : world.playerObjects(scratch)) {
        ServerObject& p = *pp;
        if (p.gone() || (p.flags & ObjFlag::Dead) || (p.player->statusFlags & 1u)) {
            continue;
        }
        if (g.flags & 1u) {
            if (edgeDistance53(gen, p) <= X87(300.0f)) {
                found = generatorSpot(fight, gen, px, py, nullptr, tmpl);
                break;
            }
        } else if (g.flags & 2u) {
            const float reach = std::sqrt(static_cast<float>(static_cast<int>(p.player->viewHalfWidth * p.player->viewHalfWidth +
                                                                              p.player->viewHalfHeight * p.player->viewHalfHeight)));
            if (edgeDistance53(gen, p) <= X87(reach) && world.traceRay(gen.x, gen.y, p.x, p.y, 0x45)) {
                seers.push_back(&p);
            }
        } else {
            return;
        }
    }
    if (!found && !seers.empty()) {
        ServerObject* p = seers[static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(seers.size()) - 1)(fight.rng))];
        found = generatorSpot(fight, gen, px, py, p, tmpl);
    }
    if (!found || !fight.things) {
        return;
    }
    // mobGeneratorSpawn; tried or not, its time is taken (updateMonsterGenerator sets it
    // after it whatever it did).
    g.lastSpawn = fight.frame;
    if (!generatorRoomOnScreen(world, px, py)) {
        return;
    }
    ServerObject* m = world.createAt(*fight.things, tmpl.typeName, px, py, fight.frame);
    if (!m) {
        return;
    }
    if (tmpl.monster && m->monster) {
        m->monster = tmpl.monster; // (unitCreatureCopyUC: its record the template's)
    }
    if (m->subclasses & 0x10u) {
        // (unitCreatureCopyUC) An NPC's things: the template's, from its
        // inventory's head (its records each read to the head: the last first), each a new one with
        // its modifiers into the inventory's head (inventoryPutImpl, unreported) and
        // worn (NPCEquipArmor / NPCEquipWeapon) when the template's is. The template's are as its NPCXfer
        // left them: (a two-handed weapon and a shield not both) and each worn again
        // in its inventory's order, whose dequips can take off one not yet reached (a bow's the
        // sword after it, a sword's the quiver). That's npcWearLoaded over the new things in the
        // template's order; the same worn at the end as the exe's wearing the template's worn ones.
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        std::vector<std::uint32_t> made; // (the template's inventory's order, its head first)
        for (auto it = tp.items.rbegin(); it != tp.items.rend(); ++it) {
            const ServerObject& item = **it;
            if (ServerObject* copy = world.createAt(*fight.things, item.typeName, m->x, m->y, fight.frame)) {
                if (copy->classes & 0x13001000u) {
                    copy->modifiers = item.modifiers; // (modifSetItemAttrs)
                }
                copy->flags &= ~(ObjFlag::Active | ObjFlag::Equipped);
                copy->flags |= item.flags & ObjFlag::Equipped;
                items.inventoryPut(*m, *copy, false);
                made.push_back(copy->extent);
            }
        }
        std::vector<std::uint32_t> order = m->inventory; // (each put to the head: the exe's copy's order)
        m->inventory = made;
        npcWearLoaded(world, fight.modifiers, *m);
        m->inventory = std::move(order);
    }
    m->direction = tmpl.direction;
    // Its health: a quest's multiplier (0.2 outside one) times monster.bin's quest health (or its
    // type's), at least 1.
    const float mult = world.questHealth;
    const int full = m->monster && m->monster->def ? m->monster->def->questHealth : static_cast<int>(m->maxHealth);
    m->health = static_cast<std::uint16_t>(std::max(static_cast<int>(mult * static_cast<float>(full)), 1));
    m->maxHealth = static_cast<std::uint16_t>(std::max(static_cast<int>(mult * static_cast<float>(full)), 1));
    if (m->monster && m->typeName == "Beholder") {
        m->monster->spells[4] = 0; // (no Blink)
    }
    // (its generator's; a bomber carrying a glyph of its three spells.)
    m->spawnedBy = gen.extent;
    ++g.active;
    if (m->subclasses & 0x2000u) {
        bomberGlyph(world, *fight.things, *m, true);
    }
    if (world.script) {
        world.script->callBlock(g.onSpawn, m->extent, gen.extent);
    }
    // The clients' effect (0xF0 0x10: from above it to 30 past where it came, 10).
    float vx = px - gen.x;
    float vy = py - gen.y;
    if (const float l = std::sqrt(vx * vx + vy * vy); 0.0f < l) {
        vx /= l;
        vy /= l;
    }
    const auto u16 = [](int v) { return static_cast<std::uint16_t>(v); };
    const std::uint16_t a = u16(static_cast<int>(gen.x));
    const std::uint16_t b = u16(static_cast<int>(gen.y) - 50);
    const std::uint16_t c = u16(static_cast<int>(vx * 30.0f + px));
    const std::uint16_t d = u16(static_cast<int>(vy * 30.0f + py));
    world.broadcasts().push_back({{0xF0, 0x10, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(a >> 8), static_cast<std::uint8_t>(b),
                                   static_cast<std::uint8_t>(b >> 8), static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 8),
                                   static_cast<std::uint8_t>(d), static_cast<std::uint8_t>(d >> 8), 10, 0},
                                  false, 0});
    playSound(world, 0x3EA, *m); // (at the new one)
}

void generatorDies(Fight& fight, ServerObject& gen)
{
    // dieMonsterGen. (A quest's tallies: with the quests.) After its handler, its sound (1000).
    World& world = fight.world;
    if (gen.generator && world.script) {
        world.script->callBlock(gen.generator->onDie, gen.hurtBy, gen.extent);
    }
    playSound(world, 1000, gen);
    const auto ix = static_cast<std::uint16_t>(static_cast<int>(gen.x));
    const auto iy = static_cast<std::uint16_t>(static_cast<int>(gen.y));
    world.broadcasts().push_back({{0xF0, 0x19, static_cast<std::uint8_t>(ix), static_cast<std::uint8_t>(ix >> 8), static_cast<std::uint8_t>(iy),
                                   static_cast<std::uint8_t>(iy >> 8), 0xC8},
                                  false, 0});
    if (fight.things) {
        world.createAt(*fight.things, "DestroyedGenerator", gen.x, gen.y, fight.frame);
    }
    gen.pendingDelete = true;
}

void generatorTouched(Fight& fight, ServerObject& gen, ServerObject* other)
{
    if (other && (other->classes & oc::Player) && gen.generator && fight.world.script) {
        fight.world.script->callBlock(gen.generator->onCollide, other->extent, gen.extent);
    }
}

void loseCreature(World& world, ServerObject& creature)
{
    // SrvNet_SendReportLoseCreature (0x6D: its code, bit 15 while a map's being saved or loaded,
    // game flag 0x8000000: the client's cage then hides it without its sound) to its player; no
    // longer its (subclass 0x80); off its minimap (netUnmarkMinimapObj, bit 1).
    ServerObject* owner = creature.owner ? world.byExtent(creature.owner) : nullptr;
    if (!owner || !owner->player) {
        return;
    }
    creature.subclasses &= ~0x80u;
    const std::uint32_t code = creature.extent | ((world.gameFlags & 0x8000000u) ? 0x8000u : 0u);
    world.broadcasts().push_back({{0x6D, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8)}, true, owner->extent});
    unmarkMinimap(world, *owner, creature, 1);
}

} // namespace nox::server
