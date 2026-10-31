// A warrior's five abilities on the server (abilities.hpp): the request checks, cool-downs,
// the list of abilities under way and what each one does.
#include "noxcore/server/abilities.hpp"

#include "noxcore/server/enchant.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <cmath>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
using util::X87;

// gamedata.txt's numbers for each ability, by its id (1-5; 0 unused): the cool-down and how
// long it lasts, in frames.
constexpr const char* kDelay[6] = {"", "BerserkerChargeDelay", "WarcryDelay", "HarpoonDelay", "TreadLightlyDelay", "EyeOfTheWolfDelay"};
constexpr const char* kDuration[6] = {"", "BerserkerChargeDuration", "WarCryDuration", "HarpoonDuration", "TreadLightlyDuration",
                                      "EyeOfTheWolfDuration"};

// (A multiclass player too: a NoX Core mode's, PlayerControl::usesAbilities.)
bool warrior(const ServerObject& o) { return o.player && o.player->usesAbilities(); }

/// servCompareTeams: on the same (real) team.
bool sameTeam(const ServerObject& a, const ServerObject& b) { return a.team != 0 && a.team == b.team; }

} // namespace

// MSG_REPORT_ACTIVE_ABILITIES (0xCF): the ability, then 1 on or 0 off; to its client alone.
void reportActiveAbility(World& world, const ServerObject& unit, int ability, bool active)
{
    if (unit.player) {
        world.broadcasts().push_back(
            {{0xCF, static_cast<std::uint8_t>(ability), static_cast<std::uint8_t>(active ? 1 : 0)}, true, unit.extent});
    }
}

bool abilityActive(const World& world, const ServerObject& unit, int ability)
{
    if (!warrior(unit)) {
        return false;
    }
    for (const ActiveAbility& a : world.activeAbilities()) {
        if (a.unit == unit.extent && a.ability == ability) {
            return true;
        }
    }
    return false;
}

int abilityFlag(const World& world, const ServerObject& unit, int ability)
{
    if (!warrior(unit)) {
        return 0;
    }
    for (const ActiveAbility& a : world.activeAbilities()) {
        if (a.unit == unit.extent && a.ability == ability) {
            return a.flag;
        }
    }
    return 0;
}

void endAbility(World& world, ServerObject& unit, int ability)
{
    if (ability < 1 || 5 < ability) {
        return;
    }
    if (ability == 3) {
        // SrvNet_BroadcastHarpoonBreak (0x9E 14, the warrior and its bolt; the exe sends the
        // buffer unset when either's missing: an original bug).
        const std::uint32_t bolt = unit.player ? unit.player->harpoonBolt : 0;
        if (bolt != 0) {
            // 0x9E 14 (the rope off), a zero byte, then the warrior's and the bolt's extents, 16 bits each.
            world.broadcasts().push_back({{0x9E, 0x0E, 0, static_cast<std::uint8_t>(unit.extent), static_cast<std::uint8_t>(unit.extent >> 8),
                                           static_cast<std::uint8_t>(bolt), static_cast<std::uint8_t>(bolt >> 8)},
                                          true, 0});
        }
    } else if (ability == 4) {
        disenchant(world, unit, Enchant::Sneak);
    } else if (ability == 5) {
        return; // (Eye of the Wolf runs its time)
    }
    reportActiveAbility(world, unit, ability, false);
    std::erase_if(world.activeAbilities(), [&](const ActiveAbility& a) { return a.unit == unit.extent && a.ability == ability; });
}

void cancelAbilities(World& world, ServerObject& unit)
{
    if (!warrior(unit)) {
        return;
    }
    unit.player->abilityCooldown.fill(0);
    world.broadcasts().push_back({{0xEB, 6}, true, unit.extent}); // MSG_RESET_ABILITIES: all
    for (auto it = world.activeAbilities().begin(); it != world.activeAbilities().end();) {
        if (it->unit == unit.extent) {
            reportActiveAbility(world, unit, it->ability, false);
            it = world.activeAbilities().erase(it);
        } else {
            ++it;
        }
    }
}

void resetAbility(World& world, ServerObject& unit, int ability)
{
    // a warrior's (class 0) cool-down over, MSG_RESET_ABILITIES {ability}
    // (SrvNet_SendResetAbilities), and its records of it dropped, each told off
    // (MSG_REPORT_ACTIVE_ABILITIES) with nothing else (no sound, no idle).
    if (!(unit.classes & oc::Player) || !warrior(unit) || ability < 0 || 5 < ability) {
        return;
    }
    unit.player->abilityCooldown[static_cast<std::size_t>(ability)] = 0;
    world.broadcasts().push_back({{0xEB, static_cast<std::uint8_t>(ability)}, true, unit.extent});
    for (auto it = world.activeAbilities().begin(); it != world.activeAbilities().end();) {
        if (it->unit == unit.extent && it->ability == ability) {
            reportActiveAbility(world, unit, ability, false);
            it = world.activeAbilities().erase(it);
        } else {
            ++it;
        }
    }
}

void releaseHarpoon(World& world, ServerObject& warrior, bool broken)
{
    // broken, harpoonBreakForPlr then plays 0x3E6 (bolt or not).
    if (broken) {
        playSound(world, 0x3E6, warrior);
    }
    if (!warrior.player || warrior.player->harpoonBolt == 0) {
        return;
    }
    warrior.player->harpoonTarget = 0;
    endAbility(world, warrior, 3);
    if (ServerObject* bolt = world.byExtent(warrior.player->harpoonBolt)) {
        bolt->pendingDelete = true;
    }
    warrior.player->harpoonBolt = 0;
}

bool abilityKnown(const World& world, const PlayerControl& p, int ability)
{
    // A networked game other than a quest knows them all; otherwise its level must be non-zero.
    // (noxcpp: a mode's book, World::modeBook, is asked as a quest's: only what it granted.)
    // (noxcpp: an id outside the abilities' table, 1-5, is never known: a non-multiclass player's
    // levels are the spell book's slots 1-5, so a later skill needs a table of its own.)
    if (ability < 1 || static_cast<std::size_t>(ability) >= p.abilityLevels.size()) {
        return false;
    }
    if (world.network() && !world.quest() && !world.modeBook()) {
        return true;
    }
    return p.abilityLevel(static_cast<std::size_t>(ability)) != 0;
}

void Abilities::tell(const ServerObject& unit, std::vector<std::uint8_t> bytes)
{
    fight.world.broadcasts().push_back({std::move(bytes), true, unit.extent});
}

void Abilities::tryExecute(ServerObject& unit, int ability)
{
    // Abil_TryExecute.
    World& world = fight.world;
    if (ability < 1 || 5 < ability || (unit.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || !warrior(unit)) {
        return;
    }
    PlayerControl& p = *unit.player;
    // Refused: MSG_INFORM (0xA9) kind 2 with the reason code, and the "can't" sound.
    const auto refuse = [&](int why) {
        tell(unit, {0xA9, 2, static_cast<std::uint8_t>(why), 0, 0, 0});
        playSound(world, 0xE7, unit);
    };
    // No Berserker Charge while carrying a CTF flag (reason 5).
    if ((world.gameFlags & GameFlag::CaptureTheFlag) && ability == 1) {
        for (const std::uint32_t e : unit.inventory) {
            const ServerObject* o = world.byExtent(e);
            if (o && (o->classes & oc::Flag)) {
                refuse(5);
                return;
            }
        }
    }
    // The ones that can't go together.
    bool busy = false;
    if (ability == 2) {
        busy = abilityActive(world, unit, 1);
    } else if (ability == 1) {
        busy = abilityFlag(world, unit, 2) != 0;
    } else if (ability == 3) {
        busy = abilityFlag(world, unit, 2) != 0 || abilityActive(world, unit, 1);
    }
    if (busy || abilityActive(world, unit, ability)) {
        refuse(2);
        return;
    }
    // Not while jumping (state 0xC), nor in the air out of a solo game (reason 6).
    if (p.state == 0xC || (!(world.gameFlags & GameFlag::Solo) && (unit.flags & ObjFlag::Airborne))) {
        refuse(6);
        return;
    }
    if (!abilityKnown(world, p, ability)) {
        refuse(3);
        return;
    }
    // Berserker Charge: not while carrying more than its capacity (reason 7).
    if (ability == 1) {
        int carried = 0;
        for (const std::uint32_t e : unit.inventory) {
            if (const ServerObject* o = world.byExtent(e)) {
                carried += o->weight;
            }
        }
        if (unit.carryCapacity < carried) {
            tell(unit, {0xA9, 2, 7, 0, 0, 0}); // (no "can't" sound for this one)
            return;
        }
    }
    if (p.abilityCooldown[static_cast<std::size_t>(ability)] != 0) {
        refuse(2);
        return;
    }
    // Accepted: its cool-down starts, and a record (flag 1) lasts its Duration when it has one.
    const int cooldown = static_cast<int>(world.number(kDelay[ability]));
    p.abilityCooldown[static_cast<std::size_t>(ability)] = cooldown;
    if (cooldown != 0) {
        tell(unit, {0xCE, static_cast<std::uint8_t>(ability), 0}); // MSG_REPORT_ABILITY_STATE: not ready
    }
    const int duration = static_cast<int>(world.number(kDuration[ability]));
    if (0 < duration) {
        world.activeAbilities().push_front({ability, ObjectRef{unit.extent}, fight.frame + static_cast<std::uint32_t>(duration), 1});
    }
    run(unit, ability);
    if (fight.things) {
        playSound(world, abilitySound(*fight.things, ability, 0), unit);
    }
}

// An accepted ability's work (each one as abilities.hpp describes).
void Abilities::run(ServerObject& unit, int ability)
{
    // Abilities: 1 Berserker Charge, 2 War Cry, 3 Harpoon, 4 Tread Lightly, 5 Eye of the Wolf.
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    if (unit.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
        return;
    }
    // Plr_SetState(1): the attack state, if its last attack's done.
    const auto attackState = [&] {
        if (fight.frame < p.attackReady) {
            return;
        }
        p.attackReady = 0;
        fight.setPlayerState(unit, 1);
    };
    // Using an ability shows the warrior: invisibility, invulnerability and its Oval Shield
    // (spell 0x43) end.
    const auto shown = [&] {
        disenchant(world, unit, Enchant::Invisible);
        disenchant(world, unit, Enchant::Invulnerable);
        cancelLastingSpells(world, 0x43, unit);
    };
    const int level = p.abilityLevel(static_cast<std::size_t>(ability));
    switch (ability) {
    case 1: { // warriorBerserker
        cancelChannelledSpells(world, unit);
        // Confused: the charge's heading wanders up to 8 x 4 (of 256) either way.
        if (enchanted(unit, Enchant::Confused)) {
            const int swing = nox::util::uniform_int_distribution<int>(-8, 8)(fight.rng);
            unit.direction = static_cast<std::uint16_t>(static_cast<std::uint8_t>(swing * 4 + static_cast<int>(unit.direction)));
        }
        p.animFrame = 0;
        attackState();
        shown();
        reportActiveAbility(world, unit, 1, true);
        break;
    }
    case 2: { // warriorWarcry
        p.animFrame = 0;
        attackState();
        // Spell_WarcryInterruptCasting: the pending-cast list from its head (newest
        // first; only players' casts are on it, Spell_StartCast's): each not of its team
        // (servCompareTeams), within 300 (+0.1) and clear of bodies and walls
        // (mapCheck) is cut off and leaves the list. (No self-skip: a warrior has
        // no spell being spoken but a multiclass one's, whose own it cuts.)
        auto& pending = world.pendingCasts();
        for (std::size_t i = 0; i < pending.size();) {
            ServerObject* c = world.byExtent(pending[i]);
            if (!c || !c->player || !c->player->chant) {
                ++i; // (its spell gone already: Spell_UpdatePendingCasts drops it)
                continue;
            }
            ServerObject& o = *c;
            const X87 dx = X87(o.x) - unit.x; // (the distance + 0.1 on the stack)
            const X87 dy = X87(o.y) - unit.y;
            if (sameTeam(unit, o) || X87(300.0f) <= util::sqrt(dx * dx + dy * dy) + 0.1f || !bodiesClear(world, unit, o)) {
                ++i;
                continue;
            }
            o.player->chant.reset();
            o.player->castStart = 0;
            o.player->hearsOwnPhonemes = false;
            tell(o, {0xA9, 0, 0x0F, 0, 0, 0}); // "spell.c:SpellCancelledByWarCry"
            playSound(world, 0xE7, o);
            fight.setPlayerState(o, 0xD);
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        }
        // Nullify (power 5) on the casters about it: players but warriors and
        // observers, monsters that cast (MonsterStatus CAN_CAST_SPELLS, 0x20).
        Spells spells{world, things, fight.modifiers, fight.rng, fight.frame};
        // (unitsGetInCircle of 300, by the grid; each as it's found)
        world.unitsInCircle(unit.x, unit.y, 300.0f, [&](ServerObject& o) {
            if (!(o.classes & (oc::Monster | oc::Player)) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || o.pendingDelete) { // (0x8020)
                return;
            }
            if (o.classes & oc::Player) {
                if (!o.player || o.player->classId == 0 || (o.player->statusFlags & 1u)) {
                    return; // (warriors (class 0), observers (status bit 1))
                }
            } else if (!o.monster || !(o.monster->status & things::MonsterStatus::CanCastSpells)) {
                return; // (a monster that doesn't cast: MonsterStatus & 0x20)
            }
            if (!sameTeam(unit, o) && bodiesClear(world, o, unit)) { // (mapCheck(it, the warrior))
                spells.cast(0x37, unit, &o, o.x, o.y, 5);
            }
        });
        shown();
        reportActiveAbility(world, unit, 2, true);
        break;
    }
    case 3: { // the throw, the bolt
        fight.setPlayerState(unit, 0x20);
        // The bolt starts just past the warrior's radius along its facing, at the bolt's own speed.
        if (things.findThing("HarpoonBolt")) {
            // (Its radius + 1 kept on the stack; each coordinate stored.)
            const auto [fx, fy] = directionVector(unit.direction & 0xFF);
            const X87 r = X87(unit.radius) + 1.0f;
            ServerObject& bolt = world.spawn(things, "HarpoonBolt", (r * fx + unit.x).f(), (r * fy + unit.y).f());
            bolt.owner = unit.extent;
            bolt.born = fight.frame;
            bolt.flags |= ObjFlag::Active;
            bolt.vx = (X87(fx) * bolt.speedCur).f();
            bolt.vy = (X87(fy) * bolt.speedCur).f();
            bolt.newX = bolt.x;
            bolt.newY = bolt.y;
            bolt.direction = unit.direction;
            bolt.touch();
            p.harpoonBolt = bolt.extent;
        }
        p.harpoonThrown = 0;
        break;
    }
    case 4: // warriorTreadLightly
        // A running warrior (state 5) stops (idle, 0xD).
        if (p.state == 5) {
            fight.setPlayerState(unit, 0xD);
        }
        enchant(world, unit, Enchant::Sneak, static_cast<int>(world.number(kDuration[4])), level);
        reportActiveAbility(world, unit, 4, true);
        world.monstersLookAt = fight.frame + 1; // (frameCounterSetCopyToNextFrame)
        break;
    case 5: // warriorInfravis
        enchant(world, unit, Enchant::Infravision, static_cast<int>(world.number(kDuration[5])), level);
        reportActiveAbility(world, unit, 5, true);
        break;
    default:
        break;
    }
}

void Abilities::update()
{
    // Each frame: the warriors' cool-downs counted down, then the timed ability records.
    World& world = fight.world;
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (!warrior(o) || o.gone()) {
            continue;
        }
        // Cool-downs count down; at 0 its client hears the ability is ready (0xCE, 1).
        for (int i = 0; i < 6; ++i) {
            int& left = o.player->abilityCooldown[static_cast<std::size_t>(i)];
            if (left != 0 && --left == 0) {
                tell(o, {0xCE, static_cast<std::uint8_t>(i), 1}); // ready
            }
        }
    }
    // Records of a dead or gone warrior are dropped; one past its end finishes (its end sound,
    // told off; Berserker Charge leaves the warrior idle).
    auto& list = world.activeAbilities();
    for (auto it = list.begin(); it != list.end();) {
        ServerObject* unit = world.byExtent(it->unit);
        if (!unit || (unit->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            it = list.erase(it);
            continue;
        }
        if (it->end < fight.frame) {
            if (fight.things) {
                playSound(world, abilitySound(*fight.things, it->ability, 2), *unit); // its end
            }
            reportActiveAbility(world, *unit, it->ability, false);
            if (it->ability == 1) {
                fight.setPlayerState(*unit, 0xD);
            }
            it = list.erase(it);
            continue;
        }
        ++it;
    }
}

std::optional<bool> Abilities::attackFrame(ServerObject& unit, std::uint8_t previousFrame, int frames, int delay)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    // The animation step: frames spent in this state over (delay + 1).
    const auto step = static_cast<int>(((fight.frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1)) & 0xFF);
    // War Cry: flag non-zero until its animation has played out.
    if (abilityActive(world, unit, 2) && abilityFlag(world, unit, 2) != 0) {
        // The shout, as WARCRY reaches its frame 3.
        if (step == 3 && previousFrame == 2) {
            // earthquakeSend: MSG_FX_JIGGLE to each player within 300, strength 15 at the
            // warrior, falling to 0 at 300.
            jiggle(world, unit.x, unit.y, 15);
            // The monsters that fear it held (warcryStunMonsters).
            // (unitsGetInCircle of 300, by the grid)
            world.unitsInCircle(unit.x, unit.y, 300.0f, [&](ServerObject& o) {
                if ((o.classes & oc::Monster) && (o.subclasses & 0x20000u) && !(o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) && !o.pendingDelete) { // (0x8020)
                    // 0x5A frames is 3 seconds.
                    enchant(world, o, Enchant::Held, 0x5A, 3);
                }
            });
            // spell 0x0D: a Counterspell at power 5 about the warrior.
            Spells{world, things, fight.modifiers, fight.rng, fight.frame}.cast(0x0D, unit, nullptr, unit.x, unit.y, 5);
        }
        // Animation done: the shout's flag clears.
        if (frames <= step) {
            for (ActiveAbility& a : world.activeAbilities()) {
                if (a.unit == unit.extent && a.ability == 2) {
                    a.flag = 0;
                    break;
                }
            }
        }
        p.animFrame = static_cast<std::uint8_t>(frames <= step ? frames - 1 : step);
        return step < frames;
    }
    // Berserker Charge: pushed along its facing at 6 x its base speed each frame.
    if (abilityActive(world, unit, 1)) {
        if (enchanted(unit, Enchant::Freeze) || enchanted(unit, Enchant::Held)) {
            return false;
        }
        // (playerAttack: the speed kept on the stack as it's stored.)
        const X87 speed = X87(unit.speedBase) * 6.0f;
        unit.speedCur = speed.f();
        const auto [fx, fy] = directionVector(unit.direction & 0xFF);
        unit.forceX = (speed * fx + unit.forceX).f();
        unit.forceY = (speed * fy + unit.forceY).f();
        // Past its last animation frame it shows frame 0 and goes on; a collide, freeze or hold ends it.
        const int at = frames - 1 <= step ? 0 : step;
        p.animFrame = static_cast<std::uint8_t>(frames <= at ? frames - 1 : at);
        return at < frames;
    }
    return std::nullopt;
}

} // namespace nox::server
