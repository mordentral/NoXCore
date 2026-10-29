// The player's unit each frame: class numbers and levels, the state machine behind walking,
// running, jumping, attacking and the emotes, the control events from MSG_PLAYER_INPUT, XP and
// level ups, the camper alarm, minimap marks, field guide and journal awards. player.hpp has
// the overview; the per-state notes below follow Plr_UpdateState's switch.
#include "noxcore/server/player.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/draw.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace nox::server {

namespace {

constexpr float kRunDistance2 = 10000; // (100 units)

/// Plr_UpdateState's run test: the cursor (FILD) less the place, each squared and summed on the
/// x87 (53 bits) and compared with the float 10000; past it, a run.
bool pastRunDistance(const PlayerControl& p, float x, float y)
{
    const util::X87 dx = util::X87(p.cursorX) - x;
    const util::X87 dy = util::X87(p.cursorY) - y;
    return util::X87(kRunDistance2) < dx * dx + dy * dy;
}

/// A class's numbers at level 10 (gamedata's, x the server's multipliers, 1).
struct ClassNumbers {
    const char* health;
    const char* mana;
    const char* strength;
    const char* speed;
};
constexpr ClassNumbers kClasses[3] = {
    {"WarriorMaxHealth", "WarriorMaxMana", "WarriorMaxStrength", "WarriorMaxSpeed"},
    {"WizardMaxHealth", "WizardMaxMana", "WizardMaxStrength", "WizardMaxSpeed"},
    {"ConjurerMaxHealth", "ConjurerMaxMana", "ConjurerMaxStrength", "ConjurerMaxSpeed"},
};
/// Everyone's at level 1.
constexpr ClassNumbers kBase = {"BaseHealth", "BaseMana", "BaseStrength", "BaseSpeed"};

/// Math_FloatToInt in the game's chop mode: towards zero.
using util::chop;
using util::X87;

/// playerCanMove: not frozen nor held, not trading in a quest, not in the middle
/// of a swing of a weapon of subclass 8.
bool canMove(const ServerObject& unit, World& world)
{
    if (enchanted(unit, Enchant::Freeze) || enchanted(unit, Enchant::Held)) {
        return false;
    }
    const PlayerControl& p = *unit.player;
    if (world.quest() && p.trade != 0) {
        return false;
    }
    if (p.state == 1) {
        const ServerObject* w = p.currentWeapon ? world.byExtent(p.currentWeapon) : nullptr;
        if (w && (w->classes & things::ObjectClass::Weapon) && (w->subclasses & 8u)) {
            return false;
        }
    }
    return true;
}

/// Where a confused player goes (playerConfusedGetDirection): its facing swung
/// back and forth over 40 frames, by (power + 3) a frame's worth.
int confusedDirection(const ServerObject& unit, std::uint32_t frame)
{
    // Fold the frame into a 40-frame cycle, then into a 0-20-0 triangle: (swing - 10) sways the
    // facing to one side and back.
    int swing = static_cast<int>((unit.extent + frame) % 40u);
    if (20 < swing) {
        swing = 40 - swing;
    }
    const int d = static_cast<std::int16_t>(unit.direction) + (swing - 10) * (enchantPower(unit, Enchant::Confused) + 3);
    return d & 0xFF;
}


/// The shields' armour bits (the round and kite shields).
constexpr std::uint32_t kShields = 0x3000000;

/// A missile coming at its front: monsterTestBlockShield, the monsters' own
/// (a would-hit test, its owner chains and gameplay flag 1, the side from the
/// missile's PrevPos, unitCanInteractWith).
bool threatened(ServerObject& unit, World& world)
{
    return incomingMissile(world, unit) != nullptr;
}

} // namespace

bool setPlayerState(World& world, ServerObject& unit, std::uint8_t state, std::uint32_t frame)
{
    // Plr_SetState.
    PlayerControl& p = *unit.player;
    if ((unit.flags & ObjFlag::Dead) && state != 3 && state != 4) {
        return false;
    }
    if (!(world.gameFlags & GameFlag::Solo) && (unit.flags & ObjFlag::Airborne) && state == 0x1E) {
        return false; // (no recoil in the air)
    }
    if (0x18 <= state && state <= 0x1D && (abilityActive(world, unit, 1) || p.state == 0xC)) {
        return false; // (no emote charging or jumping)
    }
    if (p.state == 1 && state != 1 && abilityFlag(world, unit, 2) != 0 && state != 4 && state != 3) {
        return false; // (War Cry's shout holds the attack)
    }
    if (state == 1) {
        // An attack not before the last one's done. (The punch is drawn as it begins: combat's
        // punching(); the bare hand's showing, the input's.)
        if (frame < p.attackReady) {
            return false;
        }
        p.attackReady = 0;
    } else {
        p.punch = 0;
        // Taunt, laugh and the point's middle speak as they begin.
        const std::uint32_t sound = state == 0x19 ? 0x12Du : state == 0x1A ? 300u : state == 0x1C ? 0x12Eu : 0u;
        if (sound != 0 && p.state != state) {
            playSound(world, sound, unit);
        }
        // (Dying or dead clear two exe player fields, and a recoil sets the second to the frame: nothing
        // here reads them.)
    }
    if (p.state != state) {
        p.previousState = p.state;
        p.state = state;
        p.stateFrame = frame;
        p.animFrame = 0;
    }
    return true;
}

void makeDefItems(Fight& fight, ServerObject& unit, bool heal, bool keep)
{
    // playerMakeDefItems. (Not kept here: the custom waypoints,
    // the clients' known objects: the host's.)
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    if (heal) {
        fight.removePoison(unit);
        unit.health = unit.maxHealth; // (unitHPsetOnMax)
        p.mana = p.maxMana;           // (playerManaRefresh)
    }
    cancelAbilities(world, unit);
    camperReset(world, unit, fight.frame); // (the camper's watch begun anew)
    p.exitGate = p.warpGate = 0;
    unit.flags &= 0xFFEB3FE7u;  // clears AllowOverlap, 0x10, Airborne, Dead, InHole, OnObject
    setPlayerState(world, unit, 0xD, fight.frame);
    clearEnchantments(unit); // (unitClearBuffs)
    p.castStart = 0;
    p.trapSpells = {};
    p.trapCount = 0;
    p.harpoonBolt = p.harpoonTarget = 0;
    unit.hurtBy = 0;
    // Its whole health and mana told it (MSG_REPORT_TOTAL_HEALTH / _MANA: the host's report); its
    // outfit, or (keeping what it has) MSG_PLAYER_RESPAWN's flag 0.
    p.healthSent = -1;
    p.manaSent = -1;
    if (!keep && fight.things) {
        Items{world, *fight.things, fight.modifiers, fight.rng, fight.frame}.defaultItems(unit);
    } else {
        // MSG_PLAYER_RESPAWN 0xE9: u16 its code, u32 the frame, 0xFF, then 0 to keep its outfit.
        const std::uint32_t code = unit.extent;
        const std::uint32_t f = fight.frame;
        world.broadcasts().push_back({{0xE9, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(f),
                                       static_cast<std::uint8_t>(f >> 8), static_cast<std::uint8_t>(f >> 16), static_cast<std::uint8_t>(f >> 24),
                                       0xFF, static_cast<std::uint8_t>(keep ? 0 : 1)},
                                      true});
    }
}

void mapChangePlayer(Fight& fight, ServerObject& unit, bool keep)
{
    // (The map switch's start), for each player: its trade cancelled (trade.hpp; the map
    // switch's own cancel left none), its control buffer emptied
    // (playerResetControlBuffer), a quest's secrets of the map credited; Plr_SetState
    // 0xD and (refused, or not a co-op game) its outfit anew (playerMakeDefItems:
    // healed but going to the solo game, keeping what it has when the next map is a quest's); an
    // observer told to leave (status 0x20) out of it, its camera its own; stateFrame the frame.
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    cancelTrade(fight, unit);
    p.controls.clear();
    if (world.quest()) {
        p.questSecretsCredited = p.questSecretsMap;
        p.questSecretsMap = 0;
    }
    if (!setPlayerState(world, unit, 0xD, fight.frame) || !(world.gameFlags & GameFlag::Coop)) {
        makeDefItems(fight, unit, true, keep); // (gameIsSwitchToSolo: never here)
    }
    if (p.statusFlags & 0x20u) {  // status 0x20: eliminated
        leaveObserver(fight, unit);
        p.cameraFollow = 0;
    }
    p.stateFrame = fight.frame;
}

void social(World& world, ServerObject& unit, std::uint8_t gesture, std::uint32_t frame)
{
    // SrvNet_OnPacket's MSG_SOCIAL (0xE3): the player's unit, whatever it's doing (Plr_SetState
    // decides).
    if (!unit.player) {
        return;
    }
    switch (gesture) {
    case 1:
        setPlayerState(world, unit, 0x1A, frame); // laugh
        break;
    case 2:
        setPlayerState(world, unit, 0x19, frame); // taunt
        break;
    case 4:
        setPlayerState(world, unit, 0x1B, frame); // point
        break;
    default:
        break;
    }
}

namespace {

/// The warrior's abilities by level (index: level - 1; value: ability number): Berserker at 2,
/// War Cry 3, Tread Lightly 4, Eye of the Wolf 5, Harpoon 6.
constexpr int kLevelAbilities[10] = {0, 1, 2, 4, 5, 3, 0, 0, 0, 0};

/// Abil_GrantToPlayer: known already, "use.c:HadAbility"; else at level 5, its
/// player told (MSG_REPORT_ABILITY_AWARD 0xCD: the ability, its level, 0x80 to show it); in a
/// quest the reward notice (SrvNet_SendRewardNotify: MSG_GAUNTLET 0x20, the ability, its net
/// code) to it and, unless its .plr is being applied, to the other players.
bool grantAbility(World* world, ServerObject& unit, int ability, bool notify)
{
    PlayerControl& p = *unit.player;
    if (p.abilityLevel(static_cast<std::size_t>(ability)) != 0) {
        if (world && !p.transfer) { // (SrvNet_SendInformString: none while in transfer)
            // MSG_INFORM 0xA9 kind 0x0F: a text id the client looks up in its string table.
            world->broadcasts().push_back({informStringBytes("use.c:HadAbility"), true, unit.extent});
        }
        return false;
    }
    p.abilityLevel(static_cast<std::size_t>(ability)) = 5; // (spells 1-5's levels; a multiclass player's own book)
    if (world) {
        // MSG_REPORT_ABILITY_AWARD 0xCD: the ability, then its level (5) with 0x80 to show it.
        world->broadcasts().push_back({{0xCD, static_cast<std::uint8_t>(ability), static_cast<std::uint8_t>(5 | (notify ? 0x80 : 0))}, true, unit.extent});
        if (world->quest()) {
            const std::uint32_t code = netCodeOf(unit);
            const std::vector<std::uint8_t> m = {0xF0, 0x20, static_cast<std::uint8_t>(ability), static_cast<std::uint8_t>(code),
                                                 static_cast<std::uint8_t>(code >> 8)};
            world->broadcasts().push_back({m, true, unit.extent});
            if (!p.transfer) {
                for (const auto& [e, o] : world->objects()) {
                    if (o.player && !o.gone() && &o != &unit) {
                        world->broadcasts().push_back({m, true, o.extent});
                    }
                }
            }
        }
    }
    return true;
}

/// abilGivePlayerAll: those of the levels up to `level`; in a quest (or while a
/// quest's being set up) cleared instead, the book's slot by the
/// level (an original bug).
void giveLevelAbilities(World* world, std::uint32_t gameFlags, ServerObject& unit, int level, bool notify)
{
    // Index i is level i + 1: every level reached so far grants its ability.
    for (int i = 0; i < level && i < 10; ++i) {
        if (kLevelAbilities[i] == 0) {
            continue;
        }
        if (!(gameFlags & GameFlag::Quest)) {
            grantAbility(world, unit, kLevelAbilities[i], notify);
        } else {
            unit.player->spellLevels[static_cast<std::size_t>(i)] = 0;
        }
    }
}

/// plrReadVals's numbers: the class's maxima are gamedata's times the world's
/// multipliers (1 without a world).
void applyStats(ServerObject& unit, const things::GameData* gameData, std::uint32_t gameFlags, const World* world = nullptr)
{
    PlayerControl& p = *unit.player;
    const std::uint8_t classId = p.classId;
    const std::uint8_t level = p.level;
    const ClassNumbers& c = kClasses[classId < 3 ? classId : 0];
    const auto n = [&](const char* name) { return things::gameNumber(gameData, name, gameFlags); };
    // A network game's at the class's best; a solo game's a ninth of the way from the base to it
    // a level past the first, rounded (+ 0.5).
    const std::size_t k = classId < 3 ? classId : 0;
    // (The class's table, each gamedata number x its multiplier stored to a float: on the x87,
    // chopped.)
    const auto scaled = util::chopMul;
    float health = scaled(n(c.health), world ? world->healthMultiplier[k] : 1.0f);
    // (noxcpp: a multiclass player, a mode's, a conjurer's mana and a warrior's strength
    // whatever its class: any class's spells, and any class's weapons, a war hammer's strength
    // wanted (WeaponEquipStrengthFail) and its damage over it.)
    const std::size_t manaClass = p.multiclass ? 2 : k;
    float mana = scaled(n(kClasses[manaClass].mana), world ? world->manaMultiplier[manaClass] : 1.0f);
    const std::size_t strengthClass = p.multiclass ? 0 : k;
    float strength = scaled(n(kClasses[strengthClass].strength), world ? world->strengthMultiplier[strengthClass] : 1.0f);
    float speed = scaled(n(c.speed), world ? world->speedMultiplier[k] : 1.0f);
    if (gameFlags & GameFlag::Network) {
        health = static_cast<float>(chop(health));
        mana = static_cast<float>(chop(mana));
        strength = static_cast<float>(chop(strength));
        p.speed = static_cast<std::uint32_t>(chop(speed)); // (the info's speed)
        unit.speedBase = (X87(speed) * 1e-4f).f(); // (its SpeedBonus, worn speed's, kept)
    } else {
        // f: levels past the first (0-9). Each figure grows on the x87's stack, + 0.5 there, and
        // is stored to Math_FloatToInt's float argument (chopped); mana's is held to the best on
        // the stack first, speed's x 0.0001 stored as SpeedBase from the stack too.
        const auto f = static_cast<float>(std::min<int>(static_cast<std::int8_t>(level), 10) - 1);
        const auto grown = [&](float best, const char* base) {
            const float b = n(base);
            return (X87(best) - b) * 0.11111111f * f + b;
        };
        health = static_cast<float>(util::floatToInt(grown(health, kBase.health) + 0.5f));
        const X87 m = grown(mana, kBase.mana);
        mana = static_cast<float>(util::floatToInt((m <= X87(mana) ? m : X87(mana)) + 0.5f));
        strength = static_cast<float>(util::floatToInt(grown(strength, kBase.strength) + 0.5f));
        const X87 s = grown(speed, kBase.speed);
        unit.speedBase = (s * 1e-4f).f(); // (its SpeedBonus, worn speed's, kept)
        p.speed = static_cast<std::uint32_t>(util::floatToInt(s + 0.5f));
    }
    unit.maxHealth = unit.health = static_cast<std::uint16_t>(health);
    p.maxMana = p.mana = static_cast<std::uint16_t>(std::max(mana, 0.0f));
    p.strength = static_cast<std::uint32_t>(strength);
    unit.speedCur = unit.speedBase;
    // Mass and carrying capacity from strength (FILD), on the stack: the mass stored, the
    // capacity x 1.5 (a double) through __ftol.
    const float best = n(kClasses[0].strength);
    const X87 share = X87::fromDouble(static_cast<double>(p.strength)) / best;
    unit.mass = (share * 20.0f + 10.0f).f();
    unit.carryCapacity = static_cast<std::uint16_t>(util::ftol((share * 1250.0f + 750.0f) * X87::fromDouble(1.5)));
}

} // namespace

void makePlayer(ServerObject& unit, std::uint8_t classId, const things::GameData* gameData, std::uint32_t gameFlags, std::uint8_t level)
{
    unit.player = PlayerControl{};
    PlayerControl& p = *unit.player;
    p.classId = classId;
    p.level = level;
    unit.updates = true; // (PlayerUpdate)
    unit.collides = true;
    unit.speedBonus = 0; // (a new unit's)
    applyStats(unit, gameData, gameFlags); // (plrReadVals)
    // A solo game's warrior: its abilities by its level (none told: no client yet). (A network
    // game's: every one known, abilities.cpp.)
    if (classId == 0 && !(gameFlags & GameFlag::Network)) {
        giveLevelAbilities(nullptr, gameFlags, unit, std::min<int>(static_cast<std::int8_t>(level), 10), false);
    }
    // Its spells (spellAwardAll2): a network game but a quest's all at 3. Its field guide
    // (spellAwardAll1, under the same engine flag, which the character screen sets for a network
    // game and a quest's start clears): every creature, 1-0x28, nothing told.
    if ((gameFlags & GameFlag::Network) && !(gameFlags & GameFlag::Quest)) {
        std::fill(p.spellLevels.begin() + 1, p.spellLevels.end(), std::uint8_t{3});
        std::fill(p.fieldGuide.begin() + 1, p.fieldGuide.begin() + 0x29, true);
    } else if (gameFlags & GameFlag::Quest) {
        if (classId == 1) {
            p.spellLevels[0x1B] = 1; // Fireball
        } else if (classId == 2) {
            p.spellLevels[0x09] = 1; // Charm
            p.spellLevels[0x29] = 1; // Lesser Heal
        }
    }
}

void updatePolygon(World& world, ServerObject& unit)
{
    // Map_UpdatePlayerPolygon: moved (or not looked up yet), the polygon it's in; on
    // a change the old one's leave script and the new one's enter script (caller: it); a quest's
    // secret area found the first time ("GeneralPrint:SecretFound", MSG_INFORM 0x14 with its code
    // to the others, the area's flag spent; first the secret to its tallies). (The
    // host's own observer, index 0x1F: no host player here.)
    if (!unit.player) {
        return;
    }
    PlayerControl& p = *unit.player;
    if (p.polygon != kPolygonUnknown && unit.x == unit.lastX && unit.y == unit.lastY) {
        return;
    }
    // Out of every polygon it stays in the last it was in (the lookup's none falls back to it:
    // so the exe's own "none, group 1" branch is never reached for a player in one).
    const std::uint32_t now = world.polygonAt(static_cast<int>(unit.x), static_cast<int>(unit.y), p.polygon);
    if (now == 0 || p.polygon == now) {
        return;
    }
    const auto leave = [&](std::uint32_t index) {
        if (index != 0 && index <= world.polygons().size() && world.script) {
            world.script->callBlock(world.polygons()[index - 1].onLeave, unit.extent, 0);
        }
    };
    MapPolygon& poly = world.polygons()[now - 1];
    if (p.polygon != kPolygonUnknown) {
        leave(p.polygon);
        // visited: one bit per player slot; the secret flag (1) is spent by the first finder.
        const std::uint32_t bit = 1u << (p.index & 31);
        if (!(poly.visited & bit) && (poly.flags & 1u) && world.quest()) {
            questSecretFound(unit);
            world.broadcasts().push_back({informStringBytes("GeneralPrint:SecretFound"), true, unit.extent});
            playSound(world, 0x388, unit);
            const std::uint32_t code = unit.extent;
            for (const auto& [e, o] : world.objects()) {
                if (o.player && &o != &unit) {
                    // MSG_INFORM 0x14 to everyone else, carrying the finder's code.
                    world.broadcasts().push_back({{0xA9, 0x14, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), 0, 0}, true, o.extent});
                }
            }
            poly.flags &= ~1u;
        }
        poly.visited |= bit;
        if (world.script) {
            world.script->callBlock(poly.onEnter, unit.extent, 0);
        }
    }
    p.polygon = now;
    p.polygonGroup = poly.group;
}

void markMinimap(World& world, ServerObject& player, ServerObject& o, std::uint32_t bits)
{
    (void)world;
    if (!player.player) {
        return;
    }
    auto& marks = player.player->minimapMarks;
    for (auto& m : marks) {
        if (m.object == o.extent) {
            m.bits |= bits;
            return;
        }
    }
    marks.insert(marks.begin(), {o.extent, bits});
    setXStatus(o, 1);
}

void unmarkMinimap(World& world, ServerObject& player, ServerObject& o, std::uint32_t bits)
{
    (void)world;
    if (!player.player) {
        return;
    }
    auto& marks = player.player->minimapMarks;
    const auto it = std::ranges::find(marks, o.extent, &PlayerControl::MinimapMark::object);
    if (it == marks.end()) {
        return;
    }
    it->bits &= ~bits;
    if (it->bits == 0) {
        marks.erase(it);
        o.xStatus &= ~1u; // (as the exe: no word to the clients)
    }
}

void markMinimapForAll(World& world, ServerObject& o, std::uint32_t bits)
{
    std::vector<ServerObject*> players;
    for (ServerObject* p : world.playerUnits()) {
        if (!p->gone()) {
            players.push_back(p);
        }
    }
    for (ServerObject* p : players) {
        markMinimap(world, *p, o, bits);
    }
}

void unmarkMinimapForAll(World& world, ServerObject& o, std::uint32_t bits)
{
    for (ServerObject* p : world.playerUnits()) {
        if (p->player && !p->gone()) {
            unmarkMinimap(world, *p, o, bits);
        }
    }
}

void readVals(World& world, ServerObject& unit, bool notify)
{
    // plrReadVals: its numbers by its class and level (health and mana full),
    // a warrior's abilities (a solo game's by its level, told as `notify` says; a network game's
    // all but in a quest, untold), its client sent its health, mana and stats again
    // (reportStats).
    if (!unit.player) {
        return;
    }
    PlayerControl& p = *unit.player;
    applyStats(unit, world.gameData, world.gameFlags, &world);
    if (p.classId == 0) {
        if (!world.network()) {
            giveLevelAbilities(&world, world.gameFlags, unit, std::min<int>(static_cast<std::int8_t>(p.level), 10), notify);
        } else if (!world.quest() && !world.modeBook()) { // (noxcpp: a mode's book, World::modeBook, has only what it grants)
            giveLevelAbilities(&world, world.gameFlags, unit, 10, false);
        }
    }
    p.healthSent = -1;
    p.manaSent = -1;
    p.reportStats = true;
    unit.touch();
}

void checkLevelUp(Fight& fight, ServerObject& unit)
{
    // Plr_CheckLevelUp: past XPTable's next level, a level up, its numbers anew; a
    // solo game's moment, else "explevel.c:LevelUP". (A save pending: not kept.)
    World& world = fight.world;
    if (!unit.player) {
        return;
    }
    PlayerControl& p = *unit.player;
    if (unit.experience < world.number("XPTable", static_cast<std::size_t>(static_cast<std::int8_t>(p.level) + 1))) {
        return;
    }
    ++p.level;
    readVals(world, unit, true);
    if (world.gameFlags & GameFlag::Solo) {
        levelUpMoment(fight, &unit, 0); // (solo.hpp)
        return;
    }
    world.textTo(unit, "explevel.c:LevelUP");
    playSound(world, 0x386, unit, 2, netCodeOf(unit));
}

void reportExperience(World& world, const ServerObject& unit)
{
    if (!unit.player) {
        return;
    }
    const auto xp = static_cast<std::uint32_t>(static_cast<std::int32_t>(unit.experience));
    world.broadcasts().push_back({{0x6E, static_cast<std::uint8_t>(xp), static_cast<std::uint8_t>(xp >> 8), static_cast<std::uint8_t>(xp >> 16),
                                   static_cast<std::uint8_t>(xp >> 24)},
                                  true, unit.extent});
}

float giveXP(Fight& fight, ServerObject& unit, float worth)
{
    // unitGiveXP: worth no more than it has, nothing; else a hundredth of the
    // difference and one. (On the x87: the gain stored as what it gives back, its stack value
    // added to the experience.)
    if (worth <= unit.experience) {
        return 0;
    }
    const X87 g = (X87(worth) - unit.experience) * 0.01f + 1.0f;
    const float gain = g.f();
    unit.experience = (g + unit.experience).f();
    reportExperience(fight.world, unit);
    checkLevelUp(fight, unit);
    return gain;
}

void giveExperience(Fight& fight, ServerObject& unit, float xp)
{
    // Plr_GiveExperience (the script's GiveXp): all of it, "health.c:gainpoints".
    unit.experience = (X87(xp) + unit.experience).f();
    reportExperience(fight.world, unit);
    std::u16string n;
    for (const char c : std::to_string(static_cast<int>(xp))) {
        n.push_back(static_cast<char16_t>(c));
    }
    fight.world.textTo(unit, "health.c:gainpoints", n);
    checkLevelUp(fight, unit);
}

void awardKillXP(Fight& fight, ServerObject& victim)
{
    // Obj_AwardSoloKillXP, a solo game's: whoever last hurt it, up to its player (a
    // creature's only when the player controls it, creatureIsMonitored), gets
    // its worth (giveXP), "health.c:gainpoints" with the gain.
    World& world = fight.world;
    if (!(world.gameFlags & GameFlag::Solo) || victim.hurtBy == 0) {
        return;
    }
    ServerObject* by = world.byExtent(victim.hurtBy);
    if (!by) {
        return;
    }
    const ServerObject* top = fight.parentPlayer(by);
    ServerObject* player = top ? world.byExtent(top->extent) : nullptr;
    if (!player || !(player->classes & things::ObjectClass::Player)) {
        return;
    }
    bool monitored = true;
    for (ServerObject* u = by; u && u != player; u = u->owner ? world.byExtent(u->owner) : nullptr) {
        if (u->classes & things::ObjectClass::Monster) {
            monitored = controls(world, *player, *u);
            break;
        }
    }
    if (!monitored) {
        return;
    }
    const float gain = giveXP(fight, *player, victim.experience);
    if (0.0f < gain) {
        std::u16string n;
        for (const char c : std::to_string(static_cast<int>(gain))) {
            n.push_back(static_cast<char16_t>(c));
        }
        world.textTo(*player, "health.c:gainpoints", n);
    }
    // (noxcpp: a mode's share of it for others: the co-op campaign's party.)
    if (world.modeHooks) {
        world.modeHooks->killExperienceGiven(fight, victim, *player);
    }
}

void addControls(ServerObject& unit, std::span<const net::ControlEntry> entries)
{
    if (!unit.player) {
        return;
    }
    auto& buffer = unit.player->controls;
    if (buffer.size() + entries.size() >= 128) {
        return; // (the whole batch, as SrvNet_OnPlayerInput)
    }
    for (const auto& e : entries) {
        buffer.push_back({static_cast<std::uint8_t>(e.code), e.data, true});
    }
    // Only the newest of each move stands (SrvNet_DedupPlayerCtrlEvents; MoveBackward isn't one of them).
    bool forward = false;
    bool left = false;
    bool right = false;
    for (auto it = buffer.rbegin(); it != buffer.rend(); ++it) {
        if (!it->active) {
            continue;
        }
        // Codes 2, 4 and 5: MoveForward, MoveLeft, MoveRight.
        bool* seen = it->code == 2 ? &forward : it->code == 4 ? &left : it->code == 5 ? &right : nullptr;
        if (seen) {
            if (*seen) {
                it->active = false;
            }
            *seen = true;
        }
    }
}

namespace {
bool overloaded(const World& world, const ServerObject& unit);
} // namespace

std::optional<std::pair<float, float>> predictedFrameMove(const World& world, const ServerObject& unit)
{
    // The next frame's own move of a walking, running or standing player. Plr_UpdateState
    // (Plr_UpdateState) runs its switch on the state and facing the last frame left before its control
    // events (after the switch: the event loop), which only set the next frame's
    // state and facing: so the controls that came since don't change this frame's push. A walker
    // or runner (case 0 / 5) free to move and not laden is pushed along its facing (Direction2)
    // at its SpeedCur (the last frame's end's: slowed, hasted), doubled running (state 5, or the
    // cursor past 100 from it) unless treading lightly; one standing isn't pushed. Then
    // the frame's five steps (updateObjectsVelocity).
    if (!unit.player || unit.gone() || (unit.flags & ObjFlag::NoUpdate)) {
        return std::nullopt;
    }
    const PlayerControl& p = *unit.player;
    // (Standing, raising or holding a shield (0xF / 0x10) push it no more than idle does.)
    if ((p.statusFlags & 3u) || (p.state != 0 && p.state != 5 && p.state != 0xD && p.state != 0xF && p.state != 0x10) ||
        enchanted(unit, Enchant::Confused)) {
        return std::nullopt; // (observing, attacking, jumping...; a confused one's facing wanders)
    }
    float vx = unit.vx;
    float vy = unit.vy;
    if (world.quest() && p.trade != 0) {
        vx = vy = 0; // (updatePlayer: trading in a quest, held still)
    }
    float fx = 0;
    float fy = 0;
    // (canMove only looks things up.)
    if ((p.state == 0 || p.state == 5) && canMove(unit, const_cast<World&>(world)) && !overloaded(world, unit)) {
        float speed = unit.speedCur;
        if ((p.state == 5 || pastRunDistance(p, unit.frameStartX, unit.frameStartY)) && !abilityActive(world, unit, 4)) {
            speed += speed;
        }
        const auto [cx, cy] = directionVector(unit.direction);
        fx = (X87(cx) * speed).f();
        fy = (X87(cy) * speed).f();
    }
    // Not pushed and all but still: the units update's test brings it to rest this frame
    // (settleOrQueue: velocity zeroed, no step), so it doesn't move at all. (Foreseen, the last
    // hundredths of a stop no longer put a substep a hair past the frame, which the update stream's
    // whole units made a step back and forth on screen.)
    constexpr float kStill = 0.01f; // (physics.cpp's)
    const auto astir = [](float v) { return v <= -kStill || kStill <= v; };
    if (fx == 0 && fy == 0 && !astir(unit.forceX) && !astir(unit.forceY) && !astir(vx) && !astir(vy) && !astir(unit.z) &&
        !astir(unit.zVel)) {
        return std::pair{0.0f, 0.0f};
    }
    // The frame's five steps (updateObjectsVelocity): v += (force - drag v) / 5, the place by v / 5.
    float x = 0;
    float y = 0;
    for (int i = 0; i < 5; ++i) {
        vx = (fx - unit.drag * vx) * 0.2f + vx;
        vy = (fy - vy * unit.drag) * 0.2f + vy;
        x += 0.2f * vx;
        y += 0.2f * vy;
    }
    return std::pair{x, y};
}

std::vector<std::uint8_t> camperMarker(const ServerObject& unit, bool add)
{
    // SrvNet_BroadcastInterestingIdAdd / Remove: {u16 its code, u16 its
    // type, u8 1 add / 2 take off, u8 2 the camper's kind}, important (reliable).
    const std::uint32_t code = netCodeOf(unit);
    return {0xD2, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(unit.type),
            static_cast<std::uint8_t>(unit.type >> 8), static_cast<std::uint8_t>(add ? 1 : 2), 2};
}

namespace {

/// Plr_GetFirstUnit's order: the players' units by their index.
std::vector<ServerObject*> playerUnits(World& world)
{
    return playersInOrder(world, [](const ServerObject& o) { return (o.classes & things::ObjectClass::Player) != 0; });
}

/// SrvNet_BroadcastInterestingIdAdd / Remove's sending: Net_QueueImportant to each player with a
/// unit (Plr_GetFirstUnit's walk, observers too; onlyJoined 1), one by one, so each is its own: one
/// still loading the map keeps it till its CLIENT_READY (netImportant sends a
/// player none of the queue before its status 0x10: GameHost's pending), not the ready players'
/// broadcast.
void broadcastCamperMarker(World& world, const ServerObject& unit, bool add)
{
    const auto bytes = camperMarker(unit, add);
    for (const ServerObject* to : playerUnits(world)) {
        world.broadcasts().push_back({bytes, true, to->extent});
    }
}

} // namespace

void camperReset(World& world, ServerObject& unit, std::uint32_t frame)
{
    // A player's only: the last look's place (0, 0), found still now, unmarked.
    if (!unit.player || !(unit.classes & things::ObjectClass::Player)) {
        return;
    }
    PlayerControl& p = *unit.player;
    p.campX = 0;
    p.campY = 0;
    p.campStillAt = frame;
    if (p.camping) {
        broadcastCamperMarker(world, unit, false);
    }
    p.camping = false;
}

void camperResetAll(World& world, std::uint32_t frame)
{
    for (ServerObject* u : playerUnits(world)) {
        camperReset(world, *u, frame);
    }
}

void camperWatch(Fight& fight, std::uint32_t& checkedAt)
{
    World& world = fight.world;
    if (!world.serverOption(0x2000u)) { // (always on in CTF and elimination)
        return;
    }
    // (at a hosted session's start: CamperRadiusSq, and CamperStartTime and
    // CamperFadeTime through Math_FloatToInt; the shipped gamedata.bin has them outside the SOLO /
    // ARENA lines, 22500, 150 and 0, so the game's flags since don't change them.)
    static const things::GameDataKey kRadiusSq{"CamperRadiusSq"}, kStartTime{"CamperStartTime"}, kFadeTime{"CamperFadeTime"};
    const float radiusSq = world.number(kRadiusSq);
    const auto startTime = static_cast<std::uint32_t>(chop(world.number(kStartTime)));
    const auto fadeTime = static_cast<std::uint32_t>(chop(world.number(kFadeTime)));
    const std::uint32_t frame = fight.frame;
    // In play (not DESTROYED nor dead) and not observing (status bit 1).
    const auto watched = [](const ServerObject& o) { return !(o.flags & (ObjFlag::Destroyed | ObjFlag::Dead)) && !(o.player->statusFlags & 1u); };
    const auto units = playerUnits(world);
    // A look every CamperStartTime frames: one still within CamperRadiusSq of the last look's
    // place has campStillAt moved up to now; the place is noted for the next look.
    if (startTime < frame - checkedAt) {
        for (ServerObject* u : units) {
            if (!watched(*u)) {
                continue;
            }
            PlayerControl& p = *u->player;
            // (On the x87 at 53 bits, FCOMP against the float: the differences exact, the squares
            // and their sum in double; in floats a sum just under the radius could round to it.)
            const double dy = static_cast<double>(u->y) - p.campY;
            const double dx = static_cast<double>(u->x) - p.campX;
            if (dx * dx + dy * dy < static_cast<double>(radiusSq)) {
                p.campStillAt = frame;
            }
            p.campX = u->x;
            p.campY = u->y;
        }
        checkedAt = frame;
    }
    // Then every frame: found still at the last look (within CamperFadeTime) marks it a camper;
    // otherwise its mark comes off.
    for (ServerObject* u : units) {
        if (!watched(*u)) {
            continue;
        }
        PlayerControl& p = *u->player;
        if (fadeTime < checkedAt - p.campStillAt) {
            if (p.camping) {
                broadcastCamperMarker(world, *u, false);
                p.camping = false;
            }
        } else if (!p.camping) {
            // The alarm (CamperAlarm), in CTF only for one carrying the flag (WeaponEquip bit 1).
            if (!(world.gameFlags & GameFlag::CaptureTheFlag) || (p.weaponEquip & 1u)) {
                playSound(world, 0x306, *u);
            }
            broadcastCamperMarker(world, *u, true);
            p.camping = true;
        }
    }
}

namespace {

/// Carrying more than it can.
bool overloaded(const World& world, const ServerObject& unit)
{
    int carried = 0;
    for (const std::uint32_t e : unit.inventory) {
        if (const ServerObject* o = const_cast<World&>(world).byExtent(e)) {
            carried += o->weight;
        }
    }
    return unit.carryCapacity < carried;
}

/// Too laden to move or jump: a grunt (0x143, a woman's 0x14D) and INFORM 0xD, 3.
void overloadedGrunt(World& world, ServerObject& unit)
{
    playSound(world, unit.typeName == "PlayerFemale" ? 0x14Du : 0x143u, unit);
    world.broadcasts().push_back({{0xA9, 0x0D, 3, 0, 0, 0}, true, unit.extent});
}

/// A footstep on the floor under it (Plr_UpdateState): by the floor's material, walking or
/// running; none on silent floors (2, 0x4000) or none at all.
std::uint32_t footstepSound(const World& world, const ServerObject& unit, bool running)
{
    const int floor = world.floorAt(unit.x, unit.y);
    if (floor < 0 || !world.thingDb || static_cast<std::size_t>(floor) >= world.thingDb->floors.size()) {
        return 0;
    }
    switch (world.thingDb->floors[static_cast<std::size_t>(floor)].material) {
    case 0x80: return running ? 0x116 : 0x110;  // liquid
    case 8: return running ? 0x115 : 0x10F;     // wood
    case 0x40: return running ? 0x114 : 0x10E;  // earth
    case 0x400: return running ? 0x112 : 0x10C; // snow
    case 0x800: return running ? 0x117 : 0x111; // mud
    case 2:
    case 0x4000: return 0;
    default: return running ? 0x113 : 0x10D;    // stone, the rest
    }
}

/// A bare-handed attack's frame (playerAttack without a weapon): the punch
/// lands when its animation's done; false then.
bool punching(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    if (p.punch == 0) { // a jab either hand; a warrior's a hook one time in four
        // (Plr_SetState's draw as the attack begins: past 0x4B, 25 in 101. playerAttack's own,
        // past 0x4A, runs only when none was drawn, which for a player never happens.)
        p.punch = static_cast<std::uint8_t>(nox::util::uniform_int_distribution<int>(0x17, 0x18)(fight.rng));
        if (p.classId == 0 && 0x4B < nox::util::uniform_int_distribution<int>(0, 100)(fight.rng)) {
            p.punch = 0x19;
        }
    }
    const auto [frames, delay] = animations[p.punch];
    if (p.attackReady == 0) {
        p.attackReady = static_cast<std::uint32_t>((delay + 1) * frames) + frame;
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
    if (step < frames) {
        return true;
    }
    // The fist: 0.04 a point of strength, and 5 (a jab) or 10 (the hook) (calcBoltDamage's record).
    Attack a;
    // (calcBoltDamage on the x87: FILD the strength, x 0.04, FIADD the 5 or 10; stored.)
    a.damage = (X87::fromDouble(static_cast<double>(p.strength)) * 0.04f + X87::fromDouble(p.punch == 0x19 ? 10.0 : 5.0)).f();
    a.type = 10;
    a.range = unit.radius + 20;
    a.hitsOverlapping = false;
    a.sides = 1;
    if (!fight.attack(unit, a)) {
        playSound(fight.world, 0x36F, unit); // (a miss)
    }
    return false;
}

/// A melee weapon's swing (playerAttack's branches by Player.WeaponEquip, the first that
/// matches): its animation, damage type, whether it hits ALLOW_OVERLAP things too, and its sound
/// on a miss (the hammer's on every blow).
struct Swing {
    std::uint32_t bits;
    int animation;
    std::uint8_t type;
    bool hitsOverlapping;
    std::uint32_t missSound;
};
constexpr Swing kSwings[] = {
    {0x200, 0x1C, 0, false, 0x370}, // Longsword: LONG_SWORD_STRIKE
    {0x100, 0x1B, 0, false, 0x371}, // Sword: SWORD_STRIKE
    {0x400, 0x25, 0, false, 0x371}, // GreatSword: GREAT_SWORD_STRIKE
    {0x4000, 0x27, 2, true, 0x372}, // WarHammer: HAMMER_STRIKE, round its head
    {0x800, 0x1A, 2, true, 0x374},  // MorningStar: MACE_STRIKE
    {0x3000, 0x23, 0, true, 0x373}, // the axes: AXE_STRIKE
};

const Swing* swingOf(std::uint32_t weaponEquip)
{
    if (weaponEquip & (0x47F8000u | 0x7800000u | 0x40u | 0x80u)) {
        return nullptr; // (staffs and wands, chakrams, the Oblivion staffs: their own)
    }
    for (const Swing& s : kSwings) {
        if (weaponEquip & s.bits) {
            return &s;
        }
    }
    return nullptr; // (bows, crossbows: not yet)
}

/// A weapon's swing frame: it lands at the animation's middle; false when the swing's done.
bool swinging(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    const Swing* s = swingOf(p.weaponEquip);
    const ServerObject* weapon = fight.world.byExtent(p.currentWeapon);
    if (!s || !weapon) {
        return false;
    }
    const auto [frames, delay] = animations[static_cast<std::size_t>(s->animation)];
    if (p.attackReady == 0) {
        p.attackReady = static_cast<std::uint32_t>((delay + 1) * frames) + frame; // (READINESS: only a bow's or crossbow's)
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    const int last = p.animFrame;
    p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
    if (step == frames / 2 && last < step) {
        const things::EquipDef* def = fight.modifiers ? fight.modifiers->findWeapon(weapon->typeName) : nullptr;
        Attack a;
        a.damage = boltDamage(fight.world, def, static_cast<int>(p.strength)); // (calcBoltDamage)
        a.type = s->type;
        a.weapon = weapon->extent;
        a.hitsOverlapping = s->hitsOverlapping;
        a.sides = 1;
        const float reach = def ? def->range : 30.0f;
        if (s->bits == 0x4000) { // the hammer's head, 35 ahead
            const auto [fx, fy] = directionVector(unit.direction);
            a.area = true;
            a.x = (util::X87(fx) * 35.0f + unit.x).f(); // (FMUL, FADD on the x87, one store)
            a.y = (util::X87(fy) * 35.0f + unit.y).f();
            a.range = reach;
        } else {
            a.range = unit.radius + reach;
        }
        const bool hit = fight.attack(unit, a);
        if (s->bits == 0x4000) {
            // The ground shakes about the wielder (earthquakeSend, its strength x
            // 0.1 chopped): MSG_FX_JIGGLE to each player within 300.
            jiggle(fight.world, unit.x, unit.y, util::floatToInt(util::X87::fromDouble(p.strength) * 0.1f)); // (FILD, FMUL, FSTP)
        }
        if (!hit || s->bits == 0x4000) {
            playSound(fight.world, s->missSound, unit);
        }
    }
    return step < frames;
}

/// A bow's or crossbow's attack frame (playerAttack's bits 4 and 8): a bow looses at its
/// SHOOT_BOW animation's end and holds there; a crossbow on its second frame. False when done.
bool shooting(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    ServerObject* weapon = fight.world.byExtent(p.currentWeapon);
    if (!weapon || !fight.things) {
        return false;
    }
    const bool bow = (p.weaponEquip & 4u) != 0;
    const auto [frames, delay] = animations[bow ? 0x21 : 0x22];
    // Its READINESS (itemCheckReadinessEffect) off its wait and on its frame (a
    // byte): a bow's from the start, a crossbow's past its first.
    const int ready = readiness(*weapon, fight.modifiers);
    if (p.attackReady == 0) {
        p.attackReady = static_cast<std::uint32_t>((delay + 1) * frames) + frame - static_cast<std::uint32_t>(ready);
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    const int last = p.animFrame;
    int shown = step;
    Missiles missiles{fight.world, *fight.things, fight.modifiers, fight.rng, frame};
    if (bow) {
        shown = static_cast<std::uint8_t>(step + ready);
        if (frames - 1 <= shown && last < shown) {
            missiles.shoot(unit, *weapon);
            shown = frames;
        }
    } else {
        if (step == 1 && last == 0) {
            missiles.shoot(unit, *weapon);
        }
        if ((step & 0xFF) != 0) {
            shown = static_cast<std::uint8_t>(step + ready);
        }
    }
    p.animFrame = static_cast<std::uint8_t>(shown < frames ? shown : std::max(frames - 1, 0));
    return shown < frames;
}

/// A chakram's throw (playerAttack's bits 0x40 / 0x80): CHAKRAM_STRIKE, the chakram away at its
/// middle (Missiles::throwChakram). False when done.
bool throwing(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    ServerObject* weapon = fight.world.byExtent(p.currentWeapon);
    const auto [frames, delay] = animations[0x2C];
    if (p.attackReady == 0) {
        p.attackReady = static_cast<std::uint32_t>((delay + 1) * frames) + frame;
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    const int last = p.animFrame;
    p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
    if (weapon && fight.things && step == frames / 2 && last < step) {
        Missiles{fight.world, *fight.things, fight.modifiers, fight.rng, frame}.throwChakram(unit, *weapon);
    }
    return step < frames;
}

/// A staff's (or an Oblivion staff's) melee blow at its swing's middle: its damage as a sword's
/// (calcBoltDamage, type 0), its RANGE past the wielder's radius; a miss sounds 0x36F.
void staffBlow(ServerObject& unit, const ServerObject& weapon, Fight& fight)
{
    const things::EquipDef* def = fight.modifiers ? fight.modifiers->findWeapon(weapon.typeName) : nullptr;
    Attack a;
    a.damage = boltDamage(fight.world, def, static_cast<int>(unit.player->strength));
    a.type = 0;
    a.weapon = weapon.extent;
    a.range = unit.radius + (def ? def->range : 30.0f);
    a.sides = 1;
    if (!fight.attack(unit, a)) {
        playSound(fight.world, 0x36F, unit);
    }
}

/// A staff's or wand's attack frame (playerAttack's bits 0x47F8000): a wooden staff, or a spent
/// wand swung as one, strikes (STAFF_STRIKE) at the middle as a melee weapon; a wand's blast
/// (STAFF_SPELL_BLAST, the shot already fired) is only its animation. False when done.
bool staffing(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    ServerObject* staff = fight.world.byExtent(p.currentWeapon);
    if (!staff) {
        return false;
    }
    const bool strike = (p.weaponEquip & 0x8000u) || staff->staffMelee;
    p.staffStrike = strike;
    const auto [frames, delay] = animations[strike ? 0x1D : 0x1F];
    if (p.attackReady == 0) {
        p.attackReady = strike ? static_cast<std::uint32_t>((delay + 1) * frames) + frame : frame;
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    const int last = p.animFrame;
    p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
    if (frames <= step) {
        staff->staffMelee = false;
        return false;
    }
    if (strike && step == frames / 2 && last < step) {
        staffBlow(unit, *staff, fight);
    }
    return true;
}

/// playerMapAnimState giving the staff strike (0x1D): in an attack's state (1,
/// 0xE, 0x16), not shouting nor charging, a staff in hand whose strike bit (its use data's bit 2)
/// is on.
bool staffStriking(Fight& fight, const ServerObject& unit)
{
    const PlayerControl& p = *unit.player;
    if (p.state != 1 && p.state != 0xE && p.state != 0x16) {
        return false;
    }
    if ((abilityActive(fight.world, unit, 2) && abilityFlag(fight.world, unit, 2) != 0) || abilityActive(fight.world, unit, 1)) {
        return false;
    }
    const ServerObject* staff = fight.world.byExtent(p.currentWeapon);
    return (p.weaponEquip & 0x47F0000u) && staff && staff->staffMelee;
}

/// The Oblivion Halberd's, Heart's or Wierdling's attack frame (playerAttack's bits 0x3800000, the
/// animation 0x20): a melee blow at its middle (its damage as a sword's, type 0, the wielder's
/// radius and its RANGE). False when done.
bool oblivionStriking(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    const ServerObject* weapon = fight.world.byExtent(p.currentWeapon);
    if (!weapon) {
        return false;
    }
    const auto [frames, delay] = animations[0x20];
    if (p.attackReady == 0) {
        p.attackReady = static_cast<std::uint32_t>((delay + 1) * frames) + frame;
    }
    const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
    const int last = p.animFrame;
    p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
    if (step == frames / 2 && last < step) {
        staffBlow(unit, *weapon, fight);
    }
    return step < frames;
}

/// weaponGetStaminaByType: what a swing of the weapon costs.
int swingStamina(std::uint32_t bits)
{
    if (bits & 0x200u) return 70;
    if (bits & 0x4000u) return 100;
    if (bits & 0x800u) return 50;
    if (bits & 0x100u) return 45;
    if (bits & 0x1000u) return 75;
    if (bits & 0x2000u) return 100;
    if (bits & 0x7FF8000u) return 45;
    return (bits & 0x400u) ? 75 : 10;
}

/// The state's own work for this frame (Plr_UpdateState's switch). False when the update ends
/// there (dying, dead, a jump in the air): its control events wait.
bool stateWork(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    p.running = false;
    switch (p.state) {
    case 1: { // attacking
        // (playerAttack) A warrior's shout or charge first.
        if (fight.things) {
            const int which = abilityActive(fight.world, unit, 2) && abilityFlag(fight.world, unit, 2) != 0 ? 0x2E
                              : abilityActive(fight.world, unit, 1)                                          ? 0x2D
                                                                                                             : 0;
            if (which != 0) {
                const auto [frames, delay] = animations[static_cast<std::size_t>(which)];
                Abilities abilities{fight, *fight.things};
                if (const auto going = abilities.attackFrame(unit, p.animFrame, frames, delay)) {
                    if (!*going) {
                        setPlayerState(fight.world, unit, (p.weaponEquip & 4u) ? 0xE : 0xD, frame);
                        p.stateFrame = frame;
                    }
                    return true;
                }
            }
        }
        // Otherwise the attack by what is in hand (its WeaponEquip bits), the first that matches.
        const bool going = p.weaponEquip == 0             ? punching(unit, frame, animations, fight)
                           : (p.weaponEquip & 0x47F8000u) ? staffing(unit, frame, animations, fight)
                           : (p.weaponEquip & 0x7800000u) ? oblivionStriking(unit, frame, animations, fight)
                           : (p.weaponEquip & 0xC0u)      ? throwing(unit, frame, animations, fight)
                           : (p.weaponEquip & 0xCu)       ? shooting(unit, frame, animations, fight)
                                                          : swinging(unit, frame, animations, fight);
        if (!going) {
            if (p.weaponEquip & 4u) { // a bow holds its pose a moment (0xE)
                setPlayerState(fight.world, unit, 0xE, frame);
                p.stateFrame = frame;
            } else {
                setPlayerState(fight.world, unit, 0xD, frame);
                p.punch = 0;
            }
        }
        return true;
    }
    case 2: { // casting: CAST_SPELL, held on its last frame till the spell's spoken (spells.hpp)
        const auto [frames, delay] = animations[0x15];
        const int step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        p.animFrame = static_cast<std::uint8_t>(step < frames ? step : std::max(frames - 1, 0));
        return true;
    }
    case 0xE: { // a bow held: for a second (its last frame)
        const auto [frames, delay] = animations[0x21];
        p.animFrame = static_cast<std::uint8_t>(std::max(frames - 1, 0));
        if (30u < frame - p.stateFrame) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 3: // dying: a second, then dead (ALLOW_OVERLAP, SHORT), still
        if (frame - p.stateFrame <= 30) {
            return false;
        }
        setPlayerState(fight.world, unit, 4, frame);
        p.stateFrame = frame;
        unit.flags |= 0x18u;
        unit.vx = unit.vy = 0;
        unit.forceX = unit.forceY = 0;
        unit.pushX = unit.pushY = 0;
        // (the pick-up flags' bit 0x20 is cleared here: nothing in the exe sets it, and we don't keep
        // those flags.) The map's PlayerDeath functions (by the name's first 11 letters, in the
        // script's order), with no caller nor trigger.
        if (fight.world.script && (!fight.world.modeHooks || fight.world.modeHooks->playerDeath(fight, unit, DeathStep::DeathState))) {
            fight.world.script->runEntryPoints("PlayerDeath");
        }
        return false;
    case 4: // dead: from half a second on, out of elimination's lives it goes observing; in a
            // network game (not observing) the Action key brings it back; with the automatic
            // respawn rule (not in a quest) it comes back by itself.
        if (frame - p.stateFrame <= 15) {
            return false;
        }
        if (outOfLives(fight.world, unit)) {
            eliminated(fight, unit);
            return false;
        }
        if (p.respawnHeld) {
            return false; // (noxcpp: a mode holds it dead till it brings it back)
        }
        if ((fight.world.gameFlags & GameFlag::Network) && !(p.statusFlags & 1u)) {
            for (const auto& e : p.controls) {
                if (e.active && e.code == 6) {
                    p.controls.clear();
                    fight.respawn(unit);
                    return false;
                }
            }
        }
        // (The Action key's respawn empties the control buffer: no events. The automatic one
        // goes on to this frame's events.)
        if (fight.world.autoRespawn && !fight.world.quest()) {
            fight.respawn(unit);
            return true;
        }
        return false;
    case 0xD: // idle
        unit.flags &= ~0x4001u;  // Below and Airborne off
        // With a shield, up it goes: a quarter of a second standing, or at once with a missile
        // coming at its front (game flag 0x80 aside).
        if (!(fight.world.gameFlags & 0x80u) && (p.armorEquip & kShields) &&
            (threatened(unit, fight.world) || frame - p.stateFrame > 30 / 4)) {
            setPlayerState(fight.world, unit, 0xF, frame);
        }
        return true;
    case 0xF: { // raising the shield (RAISE_SHIELD forward), then holding it
        const auto [frames, delay] = animations[0x28];
        const int step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        p.animFrame = static_cast<std::uint8_t>(step);
        if (frames <= step) {
            setPlayerState(fight.world, unit, 0x10, frame);
            p.animFrame = static_cast<std::uint8_t>(std::max(frames - 1, 0));
        }
        return true;
    }
    case 0x10: { // the shield up (its last frame) till a move lowers it
        const auto [frames, delay] = animations[0x28];
        p.animFrame = static_cast<std::uint8_t>(std::max(frames - 1, 0));
        return true;
    }
    case 0x11: { // lowering it (RAISE_SHIELD backward), then idle
        const auto [frames, delay] = animations[0x28];
        int step = frames - static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        if (step < frames) {
            if (step < 1) {
                step = 0;
                setPlayerState(fight.world, unit, 0xD, frame);
            }
            p.animFrame = static_cast<std::uint8_t>(step);
        } else {
            p.animFrame = static_cast<std::uint8_t>(std::max(frames - 1, 0));
        }
        return true;
    }
    case 0x12: // a great sword's parries (0x30, 0x31, 0x2F) and a staff's block (0x1E): played
    case 0x13: // through, then idle (a shock's stun, 0x17: with the emotes below)
    case 0x14:
    case 0x15: {
        const std::uint8_t animation = p.state == 0x12 ? 0x30 : p.state == 0x13 ? 0x31 : p.state == 0x14 ? 0x2F : 0x1E;
        const auto [frames, delay] = animations[animation];
        const int step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        p.animFrame = static_cast<std::uint8_t>(step);
        if (frames <= step) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 10: // a spell going off (CAST_SPELL's first frame)
        p.animFrame = 0;
        return true;
    case 0x16: { // a spell held (STAFF_SPELL_BLAST's last frame, 0x1F)
        const auto [frames, delay] = animations[0x1F];
        p.animFrame = static_cast<std::uint8_t>(frames - 1);
        return true;
    }
    case 0x17: // stunned (STUN 0x32) and the emotes, 0x18 (0x13), the taunt (0x19: 0x14) and the
    case 0x18: // laugh (0x1A: 0xF): each animation played through (its frame a byte), then idle
    case 0x19:
    case 0x1A: {
        const std::uint8_t animation = p.state == 0x17 ? 0x32 : p.state == 0x18 ? 0x13 : p.state == 0x19 ? 0x14 : 0xF;
        const auto [frames, delay] = animations[animation];
        p.animFrame = static_cast<std::uint8_t>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        if (frames <= p.animFrame) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 0x1B: { // pointing (0x10) to its animation's middle, then held there (0x1C)
        const auto [frames, delay] = animations[0x10];
        p.animFrame = static_cast<std::uint8_t>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        if (frames / 2 <= p.animFrame) {
            setPlayerState(fight.world, unit, 0x1C, frame);
            p.animFrame = static_cast<std::uint8_t>(frames / 2);
        }
        return true;
    }
    case 0x1C: { // the point held 20 frames, then its second half (0x1D)
        const auto [frames, delay] = animations[0x10];
        p.animFrame = static_cast<std::uint8_t>(frames / 2);
        if (0x14u < frame - p.stateFrame) {
            setPlayerState(fight.world, unit, 0x1D, frame);
            p.animFrame = static_cast<std::uint8_t>(frames / 2);
        }
        return true;
    }
    case 0x1D: { // (as 0x21's) from the middle to the end, then idle
        const auto [frames, delay] = animations[0x10];
        const int at = static_cast<int>(static_cast<std::uint8_t>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1) + frames / 2));
        p.animFrame = static_cast<std::uint8_t>(at);
        if (frames <= at) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 0x20: { // a harpoon thrown: HARPOON's middle held 20 frames, then its second half
        const auto [frames, delay] = animations[0x36];
        p.animFrame = static_cast<std::uint8_t>(frames / 2);
        if (0x14u < frame - p.stateFrame) {
            setPlayerState(fight.world, unit, 0x21, frame);
            p.stateFrame = frame;
            p.animFrame = static_cast<std::uint8_t>(frames / 2);
        }
        return true;
    }
    case 0x21: {
        const auto [frames, delay] = animations[0x36];
        const int at = static_cast<int>(static_cast<std::uint8_t>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1) + frames / 2));
        p.animFrame = static_cast<std::uint8_t>(at);
        if (frames <= at) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 0x1E: { // recoiling: for the RECOIL animation
        const auto [frames, delay] = animations[0x34];
        const auto step = static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1));
        p.animFrame = static_cast<std::uint8_t>(step);
        if (frames <= step) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return true;
    }
    case 0:
    case 5:
        if (canMove(unit, fight.world)) {
            if (overloaded(fight.world, unit)) {
                // Carrying too much: a grunt, told so (INFORM 0xD, 3), no move.
                overloadedGrunt(fight.world, unit);
                return true;
            }
            // (Not treading lightly: then it walks, Plr_UpdateState's ability 4 check.)
            if ((p.state == 5 || pastRunDistance(p, unit.x, unit.y)) && !abilityActive(fight.world, unit, 4)) {
                p.running = true;
                unit.speedCur += unit.speedCur;
                // A step on RUN's frames 2 and 8 (the animation timed by net code and game frame),
                // by the floor; now and then (2 in 100) a pant.
                const auto [frames, delay] = animations[6];
                const std::uint32_t t = netCodeOf(unit) + frame;
                if (0 < frames) {
                    const auto at = static_cast<int>(t / static_cast<std::uint32_t>(delay + 1) % static_cast<std::uint32_t>(frames));
                    const auto was = static_cast<int>((t - 1) / static_cast<std::uint32_t>(delay + 1) % static_cast<std::uint32_t>(frames));
                    if (was < at && (at == 2 || at == 8)) {
                        playSound(fight.world, footstepSound(fight.world, unit, true), unit);
                    }
                }
                if (nox::util::uniform_int_distribution<int>(0, 100)(fight.rng) < 2) {
                    playSound(fight.world, 0x142, unit);
                }
            }
            // (Click-to-move's path: not yet.) A confused one's facing wanders.
            if (enchanted(unit, Enchant::Confused)) {
                unit.direction = static_cast<std::uint16_t>(confusedDirection(unit, frame));
            }
            // (The product and sum on the x87, one store.)
            const auto [cx, cy] = directionVector(unit.direction);
            unit.forceX = (X87(cx) * unit.speedCur + unit.forceX).f();
            unit.forceY = (X87(cy) * unit.speedCur + unit.forceY).f();
            // Walking (state 0, a runner too), not treading lightly: a step on WALK's frames 3 and
            // 9, by the floor.
            if (p.state == 0 && (!abilityActive(fight.world, unit, 4) || p.running)) {
                const auto [frames, delay] = animations[4];
                const std::uint32_t t = frame + netCodeOf(unit);
                if (0 < frames) {
                    const auto at = static_cast<int>(t / static_cast<std::uint32_t>(delay + 1) % static_cast<std::uint32_t>(frames));
                    const auto was = static_cast<int>((t - 1) / static_cast<std::uint32_t>(delay + 1) % static_cast<std::uint32_t>(frames));
                    if (at != was && (at == 3 || at == 9)) {
                        playSound(fight.world, footstepSound(fight.world, unit, false), unit);
                    }
                }
            }
        }
        return true;
    case 0xC: { // jumping: twice its speed along its facing till the JUMP animation's done (no
                // push while a player's harpoon holds it: their harpoon's target)
        const auto [frames, delay] = animations[3];
        const bool harpooned = std::ranges::any_of(fight.world.playerUnits(), [&](const ServerObject* o) {
            return o->player->harpoonTarget == unit.extent;
        });
        if (!harpooned) {
            const X87 push = X87(unit.speedCur) + unit.speedCur; // (on the stack, each product stored)
            const auto [cx, cy] = directionVector(unit.direction);
            unit.forceX = (push * cx).f();
            unit.forceY = (push * cy).f();
        }
        if (static_cast<int>((frame - p.stateFrame) / static_cast<std::uint32_t>(delay + 1)) < frames) {
            return false;
        }
        // The game lands in walk (0), which shows WALK for a frame before the next move makes a
        // runner RUN. Walking with the cursor beyond the run distance already moves at run speed
        // (case 0), so a runner lands in run (5) here: the same motion without the walk frame.
        setPlayerState(fight.world, unit, pastRunDistance(p, unit.x, unit.y) ? 5 : 0, frame);
        unit.flags &= ~ObjFlag::Airborne;
        p.stateFrame = frame;
        return false;
    }
    default:
        return true;
    }
}

/// The Action control: an attack (playerInputAttack), as events takes it.
void attackInput(ServerObject& unit, std::uint32_t frame, Fight& fight)
{
    PlayerControl& p = *unit.player;
    // (playerCanAttack: not frozen, nor in state 0x17.) No attack in a
    // chat game; a flagball it holds is thrown instead (checkWinkFlags).
    if (enchanted(unit, Enchant::Freeze) || p.state == 0x17) {
        return;
    }
    // (noxcpp, a mode's: no bare-handed punch either, World::noBareHands, as a chat
    // game's no attack; after the flagball's throw, as the exe orders them.)
    if ((fight.world.gameFlags & GameFlag::Chat) || throwBall(fight, unit) || (fight.world.noBareHands && p.weaponEquip == 0)) {
        if (p.state == 10) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return;
    }
    // (playerAimsAtEnemy) Out of a quest, not with the cursor on
    // a friend. (Refused, the case still ends with state 10 to idle, as every
    // way out of it does.)
    if (const ServerObject* aim = p.cursorObject ? fight.world.byExtent(p.cursorObject) : nullptr;
        aim && !isEnemy(fight.world, unit, *aim) && !fight.world.quest()) {
        if (p.state == 10) {
            setPlayerState(fight.world, unit, 0xD, frame);
        }
        return;
    }
    // An attack shows it: invisibility and invulnerability end, an oval shield
    // too (Plr_SetState's for a punch begun, the input's for any weapon).
    const auto shown = [&] {
        disenchant(fight.world, unit, Enchant::Invisible);
        disenchant(fight.world, unit, Enchant::Invulnerable);
        cancelLastingSpells(fight.world, 0x43, unit);
    };
    if (p.weaponEquip == 0) { // bare hands: no stamina
        if (p.state != 1 && setPlayerState(fight.world, unit, 1, frame)) {
            p.stateFrame = frame;
            shown();
        }
    } else if (ServerObject* wand = fight.world.byExtent(p.currentWeapon);
               wand && (p.weaponEquip & 0x47F0000u) && !staffStriking(fight, unit)) {
        // A wand: fired at the key (its USE), or swung as a staff once spent.
        // (Its strike bit counts only through the strike's animation: one left
        // on by a strike cut short doesn't keep it from firing again.)
        if (wand->charges == 0 && wand->maxCharges != 0) {
            if (p.stamina >= 45) {
                p.stamina = static_cast<std::uint8_t>(p.stamina - 45);
                p.reportStamina = true;
                wand->staffMelee = true;
                p.staffStrike = true;
                p.stateFrame = frame;
                p.animFrame = 0;
                setPlayerState(fight.world, unit, 1, frame);
            }
        } else {
            p.stateFrame = frame;
            p.animFrame = 0;
            p.staffStrike = false;
            setPlayerState(fight.world, unit, 1, frame);
            if (fight.things && wand->wandSpell != 0) { // (WandCastUse)
                Spells{fight.world, *fight.things, fight.modifiers, fight.rng, frame}.wandCast(unit, *wand);
            } else if (fight.things) { // (WandUse)
                Missiles{fight.world, *fight.things, fight.modifiers, fight.rng, frame}.wandShot(unit, *wand);
            }
        }
    } else if (p.state != 1 && (swingOf(p.weaponEquip) || (p.weaponEquip & (0xCu | 0xC0u | 0x8000u | 0x3800000u | 0x47F0000u)))) {
        const int cost = swingStamina(p.weaponEquip);
        if (p.stamina >= cost) {
            p.stamina = static_cast<std::uint8_t>(p.stamina - cost);
            p.reportStamina = true;
            p.stateFrame = frame;
            p.animFrame = 0;
            if (!setPlayerState(fight.world, unit, 1, frame)) {
                p.stamina = static_cast<std::uint8_t>(p.stamina + cost); // (given back)
            }
        }
    }
    if (p.weaponEquip != 0) {
        shown();
    }
    if (p.state == 10) {
        setPlayerState(fight.world, unit, 0xD, frame);
    }
}

/// Its control events, in order (Plr_UpdateState after the switch). False when a jump ended
/// Plr_UpdateState there (its end's Tread Lightly check skipped).
bool events(ServerObject& unit, std::uint32_t frame, Fight& fight)
{
    PlayerControl& p = *unit.player;
    const bool running = p.running;
    // A frame with events: a walker stops unless a move keeps it going.
    // A frame without any changes nothing: a late packet doesn't stop it.
    if (p.controls.empty()) {
        return true;
    }
    if (p.state == 0 || p.state == 5) {
        setPlayerState(fight.world, unit, 0xD, frame);
        p.stateFrame = frame;
    }
    // Not an observer's (status 1 or 2); charging a Force of Nature (its lasting spell, 0x1F):
    // only Orientation.
    if (p.statusFlags & 3u) {
        return true;
    }
    const bool forceOfNature = std::ranges::any_of(fight.world.durationSpells(), [&](const DurationSpell& s) {
        return s.spell == 0x1F && s.caster == unit.extent && !s.glyph && !s.cancelled;
    });
    for (const auto& e : p.controls) {
        if (!e.active || (forceOfNature && e.code != 1)) {
            continue;
        }
        switch (e.code) {
        case 1: // Orientation (not while frozen, trading in a quest or charging)
            if (!enchanted(unit, Enchant::Freeze) && !(fight.world.quest() && p.trade != 0) && !abilityActive(fight.world, unit, 1)) {
                unit.direction = static_cast<std::uint16_t>(e.data & 0xFFFF);
            }
            break;
        case 2:
        case 3:
        case 4:
        case 5: // the moves (not out of a bare-handed or melee attack, nor charging); its
                // channelled spells end
            if (canMove(unit, fight.world)) {
                cancelChannelledSpells(fight.world, unit);
            }
            // (Out of an attack only a wand's blast, not a staff's strike, gives way to a move.)
            if (canMove(unit, fight.world) && (p.state != 1 || ((p.weaponEquip & 0x47F0000u) && !p.staffStrike)) &&
                !abilityActive(fight.world, unit, 1)) {
                if (p.state == 0x10) {
                    setPlayerState(fight.world, unit, 0x11, frame); // (the shield comes down first)
                } else {
                    setPlayerState(fight.world, unit, running ? 5 : 0, frame);
                    p.stateFrame = frame;
                }
            }
            break;
        case 6: // Action: an attack (playerInputAttack)
            attackInput(unit, frame, fight);
            break;
        case 7: // Jump: 90 stamina; then nothing more this frame (not charging, nor in War
                // Cry's shout)
            if (canMove(unit, fight.world) && !abilityActive(fight.world, unit, 1) && abilityFlag(fight.world, unit, 2) == 0) {
                cancelChannelledSpells(fight.world, unit);
                // Carrying too much: no jump, a grunt, and it's told (INFORM 0xD, 3).
                if (overloaded(fight.world, unit)) {
                    overloadedGrunt(fight.world, unit);
                } else if (p.stamina >= 90) {
                    p.stamina = static_cast<std::uint8_t>(p.stamina - 90);
                    p.reportStamina = true;
                    if (enchanted(unit, Enchant::Confused)) {
                        unit.direction = static_cast<std::uint16_t>(confusedDirection(unit, frame));
                    }
                    unit.flags |= ObjFlag::Airborne;
                    setPlayerState(fight.world, unit, 0xC, frame);
                    p.stateFrame = frame;
                    return false;
                }
            }
            break;
        case 0x14: // the hand's gestures (Up, Down, Left, Right, UpperRight, UpperLeft,
        case 0x15: // LowerRight, LowerLeft): not in a chat game. The first begins a spell
        case 0x16: // (SpellCastStart, the tree's root); each a phoneme down the
        case 0x17: // phoneme tree (1, 7, 3, 5, 2, 0, 8, 6) and its sound to everyone (a
        case 0x18: // man's, 0xBA-0xC1, whoever speaks, an original bug), not heard by
        case 0x19: // itself (hearsOwnPhonemes cleared).
        case 0x1A:
        case 0x1B:
            if (!(fight.world.gameFlags & GameFlag::Chat) && fight.things) {
                static constexpr int kPhonemes[8] = {1, 7, 3, 5, 2, 0, 8, 6};
                static constexpr std::uint32_t kSounds[8] = {0xBA, 0xBE, 0xC0, 0xBC, 0xBB, 0xC1, 0xBD, 0xBF};
                if (p.castStart == 0) {
                    p.phonemeLeaf = std::vector<std::uint8_t>{};
                    p.castStart = frame;
                }
                walkPhoneme(*fight.things, p.phonemeLeaf, kPhonemes[e.code - 0x14]);
                playSound(fight.world, kSounds[e.code - 0x14], unit);
                p.hearsOwnPhonemes = false;
            }
            break;
        case 0x1C: // the pattern's end: idle; the spell spoken cast (Plr_FinishSpellCast), or
                   // with none, the trap's first spell at what the data names
            setPlayerState(fight.world, unit, 0xD, frame);
            if (!(fight.world.gameFlags & GameFlag::Chat) && fight.things) {
                Spells spells{fight.world, *fight.things, fight.modifiers, fight.rng, frame};
                if (p.castStart == 0) {
                    spells.castQueued(unit, fight.world.byNetCode(e.data), false); // (Obj_GetByNetCode)
                } else {
                    spells.finishCast(unit);
                    p.castStart = 0;
                }
            }
            break;
        case 0x1D: // the trap's first (0x1D, idle first) or last (0x1E) spell, at the cursor
        case 0x1E: // and what the data names (a spell being spoken cast first). (The 1.2b
                   // control table gives these codes no data: an original bug.)
            if (e.code == 0x1D) {
                setPlayerState(fight.world, unit, 0xD, frame);
            }
            if (!(fight.world.gameFlags & GameFlag::Chat) && fight.things) {
                Spells spells{fight.world, *fight.things, fight.modifiers, fight.rng, frame};
                if (p.castStart != 0) {
                    spells.finishCast(unit);
                    p.castStart = 0;
                }
                p.castX = p.cursorX;
                p.castY = p.cursorY;
                spells.castQueued(unit, fight.world.byNetCode(e.data), e.code == 0x1E); // (Obj_GetByNetCode)
            }
            break;
        default:
            break;
        }
    }
    return true;
}

} // namespace

void updatePlayer(ServerObject& unit, std::uint32_t frame, const PlayerAnimations& animations, Fight& fight)
{
    PlayerControl& p = *unit.player;
    // (updatePlayer) In a quest, trading, it's held still (its velocity and push nil).
    if (fight.world.quest() && p.trade != 0) {
        unit.vx = unit.vy = 0;
        unit.forceX = unit.forceY = 0;
    }
    // (updatePlayer) A quest's game over left unanswered (no MSG_GAUNTLET 3) more than 30 s: its save
    // to its client and out (the host's questSaveAndKick); nothing else this frame.
    if (fight.world.quest() && p.questGameOverAt != 0 && 900u < frame - p.questGameOverAt) {
        if (fight.world.questSaveAndKick) {
            fight.world.questSaveAndKick(fight, unit);
        }
        return;
    }
    // (updatePlayer) Its camera (camX / camY), where it hears from and its view's middle: what it
    // follows, else its unit, as the frame begins (before this frame's move).
    const ServerObject* followed = p.cameraFollow ? fight.world.byExtent(p.cameraFollow) : nullptr;
    p.camX = followed ? followed->x : unit.x;
    p.camY = followed ? followed->y : unit.y;
    // (updatePlayer: unitNeedSync every frame) Sent to everyone who sees it each frame,
    // moving or not: clients stop drawing a player they've had no word of for 5 frames.
    // (Monsters too: Server::updateWorld, updateUnits.)
    unit.touch();
    // (updatePlayer) A look at its health for a hurt cry, at most every 7 frames: the thousandths lost
    // since the last look.
    int lost = 0;
    if (p.hurtLookWait == 0) {
        if (p.hurtLookHealth != 0) {
            lost = (static_cast<int>(p.hurtLookHealth) - static_cast<int>(unit.health)) * 1000 / static_cast<int>(p.hurtLookHealth);
        }
        p.hurtLookHealth = unit.health;
        if (0 < lost) {
            p.hurtLookWait = 7;
        }
    } else {
        --p.hurtLookWait;
    }
    // (updatePlayer) Its stamina comes back.
    if (!(unit.flags & ObjFlag::Dead) && p.stamina < 100) {
        p.stamina = static_cast<std::uint8_t>(p.stamina + 100 / 30);
    }
    // Health and mana come back, all of each in 300 seconds: a point every 9000 /
    // its most frames; health not within a second of a hurt. (noxcpp: a most over 9000, which
    // only a mode can set, is a point every frame, as from 4501 up; the exe's quotient there is 0,
    // a division by zero.)
    if (!(unit.flags & ObjFlag::Dead)) {
        if (unit.maxHealth != 0 && unit.health < unit.maxHealth && 30u < frame - unit.hurtFrame &&
            frame % std::max(30u * 300u / unit.maxHealth, 1u) == 0) {
            ++unit.health;
            unit.touch();
        }
        if (p.maxMana != 0 && p.mana < p.maxMana && frame % std::max(30u * 300u / p.maxMana, 1u) == 0) {
            ++p.mana;
        }
        // The cry, by how much it lost (under 71, under 451, more) or poison's (type 5), a woman's
        // her own.
        if (0 < lost) {
            const bool female = unit.typeName == "PlayerFemale";
            const std::uint32_t cry = unit.hurtType == 5 ? (female ? 0x14Au : 0x140u)
                                      : lost < 0x47     ? (female ? 0x147u : 0x13Du)
                                      : lost < 0x1C3    ? (female ? 0x148u : 0x13Eu)
                                                        : (female ? 0x149u : 0x13Fu);
            playSound(fight.world, cry, unit);
        }
    }
    // (updatePlayer) A spell begun by hand (not a book's: hearsOwnPhonemes) is cast as it stands half a
    // second on (the frame rate at start, 30, halved).
    if (p.castStart != 0 && !p.hearsOwnPhonemes && 15u < frame - p.castStart && fight.things) {
        Spells{fight.world, *fight.things, fight.modifiers, fight.rng, frame}.finishCast(unit);
        p.castStart = 0;
    }
    // (Plr_UpdateState's end) Walking or running this frame (free to, not laden) and now neither:
    // Tread Lightly ends. Not after a jump: Plr_UpdateState returns there.
    const bool walking = (p.state == 0 || p.state == 5) && canMove(unit, fight.world) && !overloaded(fight.world, unit);
    if (stateWork(unit, frame, animations, fight) && events(unit, frame, fight)) {
        if (walking && p.state != 0 && p.state != 5 && abilityActive(fight.world, unit, 4)) {
            endAbility(fight.world, unit, 4);
        }
    }
    // (updatePlayer, after Plr_UpdateState) Under RUN (buff 8) and not attacking: it runs (state 5,
    // which runs whatever the cursor's distance), along its facing when no move steers it.
    if (enchanted(unit, Enchant::Run) && p.state != 1) {
        setPlayerState(fight.world, unit, 5, frame);
    }
    updatePolygon(fight.world, unit); // (Map_UpdatePlayerPolygon)
    // A harpoon stuck in something pulls it in (HarpoonForce toward the warrior,
    // objectApplyForce); the warrior is its assist.
    if (p.harpoonTarget != 0) {
        ServerObject* t = fight.world.byExtent(p.harpoonTarget);
        if (!t || t->gone()) {
            releaseHarpoon(fight.world, unit, true);
        } else {
            if (t->player) {
                t->player->assistBy = unit.extent;
                t->player->assistFrame = frame;
            }
            if (joinsMotion(*t)) {
                applyForce(fight.world, *t, unit.x, unit.y, -fight.world.number("HarpoonForce")); // (objectApplyForce)
            }
        }
    }
    p.controls.clear(); // (playerResetControlBuffer, after the update)
}

PlayerAnimations PlayerAnimations::load(const things::ThingDb& things)
{
    PlayerAnimations a;
    const things::ThingDef* t = things.findThing("NewPlayer");
    if (!t || !t->draw) {
        return a;
    }
    const auto spec = things::decodeDraw(*t->draw);
    for (std::size_t i = 0; i < spec.playerStates.size() && i < a.ranges.size(); ++i) {
        if (spec.playerStates[i]) {
            a.ranges[i] = {spec.playerStates[i]->frames, spec.playerStates[i]->frameDelay};
        }
    }
    return a;
}

std::uint8_t playerAnimation(const ServerObject& unit, std::uint8_t& frame, const World* world)
{
    const PlayerControl& p = *unit.player;
    // The animated states carry their frame (SrvNet_SendPlayerObj).
    switch (p.state) {
    case 1: case 2: case 10: case 0xE: case 0xF: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
    case 0x20:
        frame = p.animFrame;
        break;
    default:
        frame = 0xFF;
        break;
    }
    if ((p.state == 3 || p.state == 4) && unit.hurtType == 0x10) {
        frame = 0xFF;
        return 0x33; // (electrocuted)
    }
    // A weapon's strike by its bit (the exe's table, bits 2 on).
    constexpr std::uint8_t kStrikes[27] = {0,    0,    0x21, 0x22, 0,    0,    0x2C, 0x2C, 0x1B, 0x1C, 0x25, 0x1A, 0x23, 0x27,
                                           0x27, 0x1D, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x1F};
    switch (p.state) { // (playerMapAnimState)
    case 0:
        return 4;
    case 1:
    case 0xE:
    case 0x16:
        // A warrior's shout (WARCRY) or charge (BERSERKER_CHARGE) first.
        if (world && abilityActive(*world, unit, 2) && abilityFlag(*world, unit, 2) != 0) {
            return 0x2E;
        }
        if (world && abilityActive(*world, unit, 1)) {
            return 0x2D;
        }
        if (p.weaponEquip & 0x47F0000u) { // (a wand: its blast, or struck as a staff)
            return p.staffStrike ? 0x1D : 0x1F;
        }
        if ((p.weaponEquip == 0 || p.weaponEquip == 1) && p.punch != 0) {
            return p.punch; // (bare hands: the punch)
        }
        for (int bit = 2; bit < 27; ++bit) {
            if (p.weaponEquip & (1u << bit)) {
                return kStrikes[bit];
            }
        }
        return 0;
    case 0xD:
        return (p.weaponEquip & 0x400u) ? 0x26 : 0; // (GREAT_SWORD_IDLE)
    case 3:
        return 1;
    case 4:
        return 2;
    case 5:
        return 6;
    case 0xC:
        return 3;
    case 2:
    case 10:
        return 0x15; // (CAST_SPELL)
    case 0xF:
    case 0x10:
    case 0x11:
        return 0x28; // (RAISE_SHIELD)
    case 0x12:
        return 0x30; // (the great sword's parries)
    case 0x13:
        return 0x31;
    case 0x14:
        return 0x2F;
    case 0x15:
        return 0x1E; // (a staff's block)
    case 0x17:
        return 0x32; // (stunned)
    case 0x18:
        return 0x13;
    case 0x19:
        return 0x14;
    case 0x1A:
        return 0xF;
    case 0x1B:
    case 0x1C:
    case 0x1D:
        return 0x10;
    case 0x1E:
        return 0x34;
    case 0x20:
        return 0x36; // (HARPOON; 0x21, its second half, falls to 0 as in the exe)
    default:
        return 0;
    }
}

void addGold(ServerObject& unit, std::int32_t amount)
{
    if (unit.player) {
        unit.player->gold = static_cast<std::uint32_t>(static_cast<std::int32_t>(unit.player->gold) + amount);
    }
}

void subGold(ServerObject& unit, std::uint32_t amount)
{
    if (unit.player) {
        unit.player->gold = unit.player->gold < amount ? 0u : unit.player->gold - amount;
    }
}

bool grantPlayerAbility(World& world, ServerObject& unit, int ability, bool notify)
{
    if (!(unit.classes & things::ObjectClass::Player) || !unit.player) {
        return false;
    }
    if (ability < 1 || 5 < ability) {
        world.textTo(unit, "Ability.c:AwardAbilityError");
        return false;
    }
    return grantAbility(&world, unit, ability, notify);
}

bool awardFieldGuide(World& world, ServerObject& unit, int creature, bool notify)
{
    // Plr_AwardFieldGuide.
    if (!(unit.classes & things::ObjectClass::Player) || !unit.player) {
        return false;
    }
    if (creature < 1 || 0x28 < creature) {
        world.textTo(unit, "PlyrGide.c:AwardGuideError");
        return false;
    }
    PlayerControl& p = *unit.player;
    auto& guide = p.fieldGuide;
    if (guide[static_cast<std::size_t>(creature)]) {
        return false;
    }
    guide[static_cast<std::size_t>(creature)] = true;
    const std::uint32_t code = netCodeOf(unit);
    const auto notice = [&](const ServerObject& to) { // (SrvNet_SendRewardNotify, kind 1: MSG_GAUNTLET 0x1F)
        world.broadcasts().push_back({{0xF0, 0x1F, static_cast<std::uint8_t>(creature), static_cast<std::uint8_t>(code),
                                       static_cast<std::uint8_t>(code >> 8)},
                                      true,
                                      to.extent});
    };
    if (notify) {
        playSound(world, 0xE3u, unit);
        notice(unit);
    }
    // Its group with it (the table: the spiders with the Spider).
    for (const int other : things::guideGroup(creature)) {
        guide[static_cast<std::size_t>(other)] = true;
    }
    if (notify) {
        for (const auto& [e, o] : world.objects()) {
            if (o.player && !o.gone() && &o != &unit) {
                notice(o);
            }
        }
    }
    // SrvNet_SendReportGuideAward: MSG_REPORT_GUIDE_AWARD 0xD1 {creature, `notify`}.
    world.broadcasts().push_back({{0xD1, static_cast<std::uint8_t>(creature), static_cast<std::uint8_t>(notify ? 1 : 0)}, true, unit.extent});
    return true;
}

bool addJournalEntry(World& world, ServerObject& unit, std::string_view name, std::uint16_t type)
{
    // comJournalEntryAdd: Plr_AddJournalEntry (at most 63 letters, at the
    // list's head), then SrvNet_SendJournalAdd (MSG_JOURNAL_MSG 0xD5, 1 'add', the name in
    // 64 bytes, its type).
    if (!unit.player) {
        return false;
    }
    PlayerControl& p = *unit.player;
    const std::string entry(name.substr(0, std::min<std::size_t>(name.find('\0'), 0x3F)));
    p.journal.insert(p.journal.begin(), {entry, type});
    std::vector<std::uint8_t> m(0x44, 0);
    m[0] = 0xD5;
    m[1] = 1;
    std::copy(entry.begin(), entry.end(), m.begin() + 2);
    m[0x42] = static_cast<std::uint8_t>(type);
    m[0x43] = static_cast<std::uint8_t>(type >> 8);
    world.broadcasts().push_back({std::move(m), true, unit.extent});
    return true;
}

void removeJournalEntries(World& world, ServerObject& unit, std::uint16_t types)
{
    // The entries whose type shares a bit with `types` go, untold. (The exe's host,
    // slot 0x1F, reads this same Player record in its journal window: ours is a client, told each
    // by MSG_JOURNAL_MSG 2 as comJournalEntryRemove's are.)
    if (!unit.player) {
        return;
    }
    auto& journal = unit.player->journal;
    for (auto it = journal.begin(); it != journal.end();) {
        if (!(it->type & types)) {
            ++it;
            continue;
        }
        if (unit.player->index == 0x1F) {
            std::vector<std::uint8_t> m(0x44, 0);
            m[0] = 0xD5;
            m[1] = 2;
            std::copy(it->name.begin(), it->name.end(), m.begin() + 2);
            world.broadcasts().push_back({std::move(m), true, unit.extent});
        }
        it = journal.erase(it);
    }
}

} // namespace nox::server
