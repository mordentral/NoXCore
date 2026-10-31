// The classic Quest mode (game flag 0x1000, the multiplayer gauntlet), as the game plays it: its
// start, its map list, the difficulty, exits and warp gates, scores, deaths, ankhs and soul gates.
// quest.hpp says what each function does; the comments here follow the exe's order of steps and
// note where its own quirks are kept. A NoX Core mode may run on top of this quest.
#include "noxcore/server/quest.hpp"

#include "noxcore/server/abilities.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/rewards.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/x87.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

// Low and high byte of a 16-bit value, for little-endian message fields.
using util::hi;
using util::lo;

/// The players' units, in the world's order (Plr_GetFirstUnit's walk).
std::vector<ServerObject*> playerUnits(World& world)
{
    std::vector<ServerObject*> units;
    for (ServerObject* o : world.playerUnits()) {
        if (!o->gone()) {
            units.push_back(o);
        }
    }
    return units;
}

/// The players' units by their index (Plr_GetFirstUnit / Plr_GetNextUnit: the player table's
/// order).
std::vector<ServerObject*> playersByIndex(World& world)
{
    return playersInOrder(world, [](const ServerObject& o) { return !o.gone(); });
}

/// a float's magnitude as a u16, chopped (the FPU's mode).
std::uint16_t chopAbs(float v) { return static_cast<std::uint16_t>(static_cast<int>(std::fabs(v))); }

using util::chopToFloat;

/// Math_FloatToInt (util::chop: chopped; out of range, the integer indefinite 0x80000000), as a word.
std::uint32_t floatToInt(float v) { return static_cast<std::uint32_t>(util::chop(v)); }

/// A thing type's HEALTH (its template's, both its current and its most).
int typeHealth(const World& world, const ServerObject& o)
{
    const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(o.typeName) : nullptr;
    const auto h = t ? t->lastAttribute("HEALTH") : std::nullopt;
    return h ? std::atoi(std::string(*h).c_str()) : 0;
}

/// The map's objects in the exe's list order at its load (World::loadOrder: the file's), those
/// not carried.
std::vector<ServerObject*> mapList(World& world)
{
    std::vector<ServerObject*> out;
    for (const std::uint32_t e : world.loadOrder) {
        if (ServerObject* o = world.byExtent(e); o && o->holder == 0) {
            out.push_back(o);
        }
    }
    return out;
}

/// delayedDeleteObject: gone at the frame's end, off the respawn list.
void removeObject(World& world, ServerObject& o)
{
    deleteObject(world, o);
    forgetRespawn(world, o.extent);
}

/// Case aside, as the exe's compares do (the first `n` letters; all with n < 0).
bool sameName(std::string_view a, std::string_view b, int n = -1)
{
    for (std::size_t i = 0;; ++i) {
        if (0 <= n && static_cast<int>(i) == n) {
            return true;
        }
        const char x = i < a.size() ? a[i] : '\0';
        const char y = i < b.size() ? b[i] : '\0';
        if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

/// The generators' table: a monster's generator type.
const char* generatorTypeOf(std::string_view monster)
{
    static constexpr std::pair<const char*, const char*> kTable[] = {
        {"GruntAxe", "GruntAxeGenerator"},
        {"Scorpion", "ScorpionGenerator"},
        {"GiantLeech", "GiantLeechGenerator"},
        {"Zombie", "ZombieGenerator"},
        {"VileZombie", "VileZombieGenerator"},
        {"Bear", "BearGenerator"},
        {"BlackBear", "BearGenerator"},
        {"Wolf", "WolfGenerator"},
        {"BlackWolf", "WolfGenerator"},
        {"WhiteWolf", "WolfGenerator"},
        {"CarnivorousPlant", "CarnivorousPlantGenerator"},
        {"Mimic", "MimicGenerator"},
        {"StoneGolem", "StoneGolemGenerator"},
        {"MechanicalGolem", "MechanicalGolemGenerator"},
        {"Wasp", "WaspGenerator"},
        {"Swordsman", "SwordsmanGenerator"},
        {"Ghost", "GhostGenerator"},
        {"Bomber", "BomberGenerator"},
        {"BomberGreen", "BomberGenerator"},
        {"BomberBlue", "BomberGenerator"},
        {"BomberYellow", "BomberGenerator"},
        {"SmallSpider", "SmallSpiderGenerator"},
        {"SmallAlbinoSpider", "SmallAlbinoSpiderGenerator"},
        {"Bat", "BatGenerator"},
        {"Rat", "RatGenerator"},
        {"Shade", "ShadeGenerator"},
        {"Urchin", "UrchinGenerator"},
        {"UrchinShaman", "UrchinShamanGenerator"},
        {"Archer", "ArcherGenerator"},
        {"Imp", "ImpGenerator"},
        {"FlyingGolem", "FlyingGolemGenerator"},
        {"EmberDemon", "EmberDemonGenerator"},
        {"MeleeDemon", "EmberDemonGenerator"},
        {"Demon", "DemonGenerator"},
        {"Skeleton", "SkeletonGenerator"},
        {"SkeletonLord", "SkeletonLordGenerator"},
        {"Wizard", "WizardGenerator"},
        {"WizardWhite", "WizardWhiteGenerator"},
        {"Beholder", "BeholderGenerator"},
        {"Lich", "LichGenerator"},
        {"LichLord", "LichGenerator"},
        {"AlbinoSpider", "AlbinoSpiderGenerator"},
        {"SpittingSpider", "SpittingSpiderGenerator"},
        {"NPC", "NPCGenerator"},
        {"Horrendous", "HorrendousGenerator"},
        {"WizardGreen", "WizardGreenGenerator"},
        {"Spider", "SpiderGenerator"},
        {"OgreWarlord", "OgreWarlordGenerator"},
        {"OgreBrute", "OgreBruteGenerator"},
        {"EvilCherub", "EvilCherubGenerator"},
        {"WillOWisp", "WillOWispGenerator"},
        {"Troll", "TrollGenerator"},
        {"Necromancer", "NecromancerGenerator"},
    };
    for (const auto& [m, g] : kTable) {
        if (monster == m) { // (by type id)
            return g;
        }
    }
    return nullptr;
}

/// spawnHecubahQuest / spawnNecroQuest: the boss at (x, y), its
/// health its quest health times the monsters' (at least 1), how it fights set.
std::uint32_t spawnBoss(Fight& fight, bool hecubah, float x, float y)
{
    World& world = fight.world;
    if (!fight.things) {
        return 0;
    }
    ServerObject& o = world.spawn(*fight.things, hecubah ? "Hecubah" : "Necromancer", x, y);
    if (!o.monster) {
        return o.extent;
    }
    auto& m = *o.monster;
    const int base = m.def ? m.def->questHealth : typeHealth(world, o);
    const float mult = world.questHealth < 1.0f ? 1.0f : world.questHealth;
    const util::X87 v = util::X87::fromDouble(base) * mult; // (FILD, FMUL; FST to a float, then __ftol of the stack's)
    o.health = static_cast<std::uint16_t>(util::ftol(v));  // (__ftol's low word)
    o.maxHealth = static_cast<std::uint16_t>(util::floatToInt(v)); // (Math_FloatToInt of the stored float)
    if (o.health == 0) {
        o.health = 1;
    }
    if (o.maxHealth == 0) {
        o.maxHealth = 1;
    }
    o.touch();
    // Its spells' uses (by spell), its start (4), aggression (0.83), power
    // (1, Hecubah's 3). (HecubahQuestSkill is read and dropped.)
    m.spells[0x27] = 0x10000000u;
    m.spells[0x33] = 0x10000000u;
    m.startAction = 4;
    m.aggression = std::bit_cast<float>(0x3F547AE1u);
    m.spellPower = hecubah ? 3 : 1;
    m.spells[0x26] = 0x8000000u;
    m.spells[0x48] = 0x20000000u;
    m.spells[0x2B] = 0x40000000u;
    if (hecubah) {
        m.spells[0x10] = 0x40000000u;
        // Its bits below are the float 0.85.
        m.field330 = std::bit_cast<float>(0x3F59999Au);
    }
    // Its loot: a RewardMarker at stage + 2, Hecubah's four times.
    giveRewards(fight, o, static_cast<std::uint32_t>(world.questStage) + 2u, hecubah ? 4 : 1);
    return o.extent;
}

/// the warp gates on or off. Off from on, the players in the quest waiting in one are
/// put out, then every warp gate (Exit, subclass 2) is turned off.
void setWarpGates(Fight& fight, bool on)
{
    World& world = fight.world;
    if (world.warpGatesOn && !on) {
        for (ServerObject* u : playerUnits(world)) {
            if (u->player->questActive != 0 && u->player->warpGate != 0) {
                questLeaveWarpGate(fight, *u);
            }
        }
        for (auto& [extent, o] : world.objects()) {
            if (o.holder == 0 && (o.classes & oc::Exit) && (o.subclasses & 2u)) {
                setOnOff(o, false);
            }
        }
    }
    world.warpGatesOn = on;
}

/// a player's quest tallies cleared, the stage it joined at.
void clearTallies(const World& world, PlayerControl& p)
{
    p.questTimeUps = p.questTally1230 = p.questDeaths = 0;
    p.questKills = p.questGenerators = p.questSecrets = 0;
    p.questSecretsMap = p.questSecretsCredited = p.questTally124C = 0;
    p.questJoinStage = static_cast<std::uint32_t>(world.questStage);
    p.questFlags = 0x3F;
}

// A u32 appended little-endian.
using util::putU32;

/// SrvNet_SendInformString (MSG_INFORM 0x0F, not a sign, the string's id) to `to` (0: everyone,
/// SrvNet_BroadcastInformString).
void informString(World& world, std::uint32_t to, std::string_view id)
{
    world.broadcasts().push_back({informStringBytes(id), true, to});
}

/// SrvNet_SendInform / SrvNet_BroadcastInform: MSG_INFORM `code` with a
/// player's code.
void inform(World& world, std::uint32_t to, std::uint8_t code, std::uint32_t value)
{
    std::vector<std::uint8_t> m = {0xA9, code};
    putU32(m, value);
    world.broadcasts().push_back({std::move(m), true, to});
}
} // namespace

void QuestMaps::build(const std::vector<std::pair<std::string, std::uint32_t>>& mapList)
{
    maps.clear();
    for (const auto& [name, flags] : mapList) {
        if ((flags & GameFlag::Quest) && maps.size() < 0x80) {
            maps.push_back({name + ".map", 0, 0, 0});
        }
    }
    // The groups: each map without one starts the next, and gives it to the maps after it whose
    // first 6 letters match.
    int next = 1;
    for (std::size_t i = 0; i < maps.size(); ++i) {
        if (maps[i].group != 0) {
            continue;
        }
        maps[i].group = next++;
        for (std::size_t j = i + 1; j < maps.size(); ++j) {
            if (sameName(maps[i].name, maps[j].name, 6)) {
                maps[j].group = maps[i].group;
            }
        }
    }
}

void QuestMaps::clearPlays()
{
    playCount = 1000;
    for (Entry& e : maps) {
        e.plays = 0;
        e.last = 0;
    }
}

void QuestMaps::played(std::string_view map)
{
    for (std::size_t i = 0; i < maps.size(); ++i) {
        if (!sameName(maps[i].name, map)) {
            continue;
        }
        current = static_cast<int>(i);
        ++maps[i].plays;
        maps[i].last = playCount;
        for (std::size_t j = 0; j < maps.size(); ++j) {
            if (maps[j].group == maps[i].group && j != i) {
                ++maps[j].plays;
                maps[j].last = playCount;
            }
        }
        ++playCount;
        return;
    }
}

std::optional<std::string> QuestMaps::pick(std::minstd_rand& rng) const
{
    const int n = static_cast<int>(maps.size());
    const auto draw = [&](int hi) { return nox::util::uniform_int_distribution<int>(0, hi)(rng); };
    if (n == 0) {
        return std::nullopt;
    }
    if (n == 1) {
        return maps[0].name;
    }
    // The most plays of any map; none played yet: any at random.
    int most = 0;
    for (const Entry& e : maps) {
        most = std::max(most, e.plays);
    }
    if (most == 0) {
        return maps[static_cast<std::size_t>(draw(n - 1))].name;
    }
    // Every map played as often (the exe compares each with every map after the first): all may.
    bool even = true;
    for (int i = 0; i < n; ++i) {
        for (int j = 1; j < n; ++j) {
            if (maps[static_cast<std::size_t>(i)].plays != maps[static_cast<std::size_t>(j)].plays) {
                even = false;
            }
        }
    }
    const int below = even ? most + 1 : most;
    const int lastGroup = maps[static_cast<std::size_t>(current)].group;
    // A map that may come next: played fewer times than `below`, not the last map nor of its group,
    // and not played in the last 4 plays.
    const auto may = [&](int i) {
        const Entry& e = maps[static_cast<std::size_t>(i)];
        return e.plays < below && i != current && e.group != lastGroup && 4 < playCount - e.last;
    };
    int count = 0;
    for (int i = 0; i < n; ++i) {
        count += may(i) ? 1 : 0;
    }
    if (count == 0) {
        // randInt(0, -1) draws nothing and gives -1: the exe takes the entry before the list
        // (an original bug). None here.
        return std::nullopt;
    }
    // The k-th of the maps that may.
    const int k = draw(count - 1);
    int seen = 0;
    for (int i = 0; i < n; ++i) {
        if (may(i)) {
            if (seen == k) {
                return maps[static_cast<std::size_t>(i)].name;
            }
            ++seen;
        }
    }
    return maps[static_cast<std::size_t>(k)].name;
}

int questPlayers(const World& world)
{
    // (player; the dedicated server's own slot 0x1F aside: no host player here)
    int n = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && o.player->questActive == 1) {
            ++n;
        }
    }
    return n;
}

std::uint32_t questScoreOf(std::uint32_t generators, std::uint32_t secrets, std::uint32_t kills, std::uint32_t stage)
{
    // (The stage to the power 1.9f made a float; the tallies by 10, 35 and 0.1f; the product a
    // float, then an integer: each step chopped.)
    // (The CRT's pow: ours the library's, its last bit possibly not the game's.)
    const float grow = chopToFloat(std::pow(static_cast<double>(stage), static_cast<double>(1.9f)));
    const util::X87 sum = util::X87::fromDouble(generators) * 10.0f + util::X87::fromDouble(secrets) * 35.0f +
                          util::X87::fromDouble(kills) * 0.1f;
    return floatToInt((sum * grow).f());
}

std::uint32_t questScore(World& world, const ServerObject& unit)
{
    const PlayerControl& p = *unit.player;
    if (p.questActive == 0) {
        return 0;
    }
    std::uint32_t score = 0;
    if (questPlayers(world) == 1) {
        score = questScoreOf(p.questGenerators, p.questSecrets, p.questKills, p.questJoinStage);
    } else {
        // Shared out: its own score times the group's (all generators and secrets, no kills, at the
        // last player's stage) over the best of anyone's (its generators and secrets, no kills).
        // (The exe asks the asked player's quest flag in the walk, not each one's: every player
        // counts, those out of the quest too: an original bug.)
        std::uint32_t best = 0;
        std::uint32_t generators = 0;
        std::uint32_t secrets = 0;
        std::uint32_t stage = 1;
        for (const ServerObject* u : playersByIndex(world)) {
            const PlayerControl& q = *u->player;
            best = std::max(best, questScoreOf(q.questGenerators, q.questSecrets, 0, q.questJoinStage));
            stage = q.questJoinStage;
            generators += q.questGenerators;
            secrets += q.questSecrets;
        }
        const std::uint32_t group = questScoreOf(generators, secrets, 0, stage);
        float share = 1.0f;
        if (best != 0) {
            share = (util::X87::fromDouble(group) / util::X87::fromDouble(best)).f(); // (FILD, FILD, FDIVP, FSTP)
        }
        const std::uint32_t own = questScoreOf(p.questGenerators, p.questSecrets, p.questKills, p.questJoinStage);
        score = floatToInt((util::X87::fromDouble(own) * share).f()); // (FILD, FMUL, FSTP)
    }
    return std::min(score, 999999999u);
}

std::vector<std::uint8_t> questLevelWonMessage(World& world, const ServerObject& to)
{
    // 0x5A bytes: 0xF0, 0x0C, u16 secrets at 2, u16 stage at 4, then from 6 up to six 14-byte
    // entries {u16 code, u16 generators, u16 secrets, u16 credited, u16 kills, u32 score}.
    std::vector<std::uint8_t> m(0x5A, 0);
    m[0] = 0xF0;
    m[1] = 0x0C;
    const auto put16 = [&](std::size_t at, std::uint32_t v) {
        m[at] = lo(v);
        m[at + 1] = hi(v);
    };
    put16(2, static_cast<std::uint32_t>(world.questSecretsLast));
    std::size_t at = 6;
    int n = 0;
    for (const ServerObject* u : playersByIndex(world)) {
        const PlayerControl& q = *u->player;
        if (q.questActive != 1 || 6 <= n) {
            continue;
        }
        put16(at, u->extent);
        put16(at + 2, q.questGenerators);
        put16(at + 4, q.questSecrets);
        put16(at + 6, q.questSecretsCredited);
        put16(at + 8, q.questKills);
        const std::uint32_t score = questScore(world, *u);
        for (int i = 0; i < 4; ++i) {
            m[at + 10 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(score >> (8 * i));
        }
        put16(4, to.player->questJoinStage); // (written with each entry: none, 0)
        at += 14;
        ++n;
    }
    return m;
}

std::vector<std::uint8_t> questBriefingMessage(const World& world, std::uint8_t sub, bool lockScreen)
{
    // 0x45 bytes: 0xF0, sub, u16 stage, u8 flags, the picture's name at 5, the text id at 37.
    std::vector<std::uint8_t> m(0x45, 0);
    m[0] = 0xF0;
    m[1] = sub;
    m[2] = lo(static_cast<std::uint32_t>(world.questStage));
    m[3] = hi(static_cast<std::uint32_t>(world.questStage));
    m[4] = static_cast<std::uint8_t>((lockScreen ? 1 : 0) | (world.questBosses ? 2 : 0));
    // Its picture at 5, its text at 37 (strcpy'd: the text's copy writes over a 32-letter
    // picture's end).
    const auto copy = [&](std::size_t at, const std::string& s) {
        const std::size_t n = std::min(s.size() + 1, m.size() - at);
        std::copy_n(s.c_str(), n, m.begin() + static_cast<std::ptrdiff_t>(at));
    };
    copy(5, world.questGraphic);
    copy(37, world.questIntro);
    return m;
}

void questMapStart(Fight& fight)
{
    World& world = fight.world;
    if (!world.questScreens) {
        // (noxcpp: no screens; the saves still sent but where they're off, the map faded in as any
        // other's.)
        for (ServerObject* u : playersByIndex(world)) {
            if (u->player->questActive == 1 && !u->player->questAwaitingSave && world.questSendSave && world.questSaves && world.questStage != 1) {
                world.questSendSave(fight, *u);
            }
        }
        world.questWarped = false;
        world.broadcasts().push_back({{0xE4, 1, 1}, true, 0});
    } else if (world.questStage == 1) {
        world.broadcasts().push_back({questBriefingMessage(world, 0x0E, false), true, 0}); // (SrvNet_SendGauntletQuestStart)
    } else if (world.questWarped) {
        // (a saved quest resumed would have the quest start's briefing: not kept.)
        world.broadcasts().push_back({questBriefingMessage(world, 0x0D, true), true, 0});
        world.questWarped = false; // (the warp flag cleared)
    } else {
        // The level won, to each player (each one in the quest and not waiting for its own save
        // first saved and the save sent to its client: the host's questSendSave, Plr_SaveServerPlayerData,
        // MSG_GAUNTLET 0x1C, SrvNet_SendSaveDataToClient), then the briefing.
        for (ServerObject* u : playersByIndex(world)) {
            if (u->player->questActive == 1 && !u->player->questAwaitingSave && world.questSendSave && world.questSaves) {
                world.questSendSave(fight, *u);
            }
            world.broadcasts().push_back({questLevelWonMessage(world, *u), true, u->extent});
        }
        world.broadcasts().push_back({questBriefingMessage(world, 0x0D, false), true, 0});
    }
    // (A flag set, 1: only read with a saved quest's stage.) Every player's tallies cleared.
    for (ServerObject* u : playerUnits(world)) {
        clearTallies(world, *u->player);
    }
}

void questTimeUpTally(ServerObject& unit)
{
    // (not one being deleted; in the quest): a map's time run out with it in.
    if (!(unit.flags & 0x20u) && unit.player && unit.player->questActive == 1) {
        ++unit.player->questTimeUps;
        unit.player->questFlags |= 1u;
    }
}

void questKill(ServerObject& unit)
{
    // (not one being deleted).
    if (unit.player && !(unit.flags & 0x20u)) {
        ++unit.player->questKills;
        unit.player->questFlags |= 4u;
    }
}

void questSecretFound(ServerObject& unit)
{
    // (not one being deleted).
    if (unit.player && !(unit.flags & 0x20u)) {
        ++unit.player->questSecrets;
        ++unit.player->questSecretsMap;
        unit.player->questFlags |= 0x10u;
    }
}

void questSetDifficulty(World& world)
{
    const float delta = world.number("PlayerDifficultyDelta");
    // (FILD the stage; FILD the players less one, FMUL, FADD 1, FMULP, FSTP)
    world.questDifficulty =
        ((util::X87::fromDouble(questPlayers(world) - 1) * delta + 1.0f) * util::X87::fromDouble(world.questStage)).f();
}

void questRescale(World& world, std::span<const std::uint32_t> notYet)
{
    // The damage players take and the monsters' health (at most
    // their caps).
    // (FLD the difficulty, FSUB 1, FMUL the coefficient, FADD the start, FSTP to the setter's
    // argument, capped there.)
    const util::X87 d = util::X87(world.questDifficulty) - 1.0f;
    world.questDamage = std::min((d * world.number("PlayerDamageDiffCoeff") + world.number("PlayerDamageDiffInit")).f(), world.number("PlayerDamageCap"));
    world.questHealth = std::min((d * world.number("SystemHealthDiffCoeff") + world.number("SystemHealthDiffInit")).f(), world.number("SystemHealthCap"));
    const auto most = static_cast<std::uint16_t>(static_cast<int>(world.number("GeneratorMaxHealth")));
    for (auto& [extent, o] : world.objects()) {
        if (o.gone() || (o.flags & ObjFlag::Dead) || o.maxHealth == 0 || o.health == 0 || o.health != o.maxHealth ||
            std::ranges::find(notYet, o.extent) != notYet.end()) {
            continue;
        }
        if (o.classes & oc::MonsterGenerator) {
            // At most GeneratorMaxHealth, at least 1.
            const int base = typeHealth(world, o);
            // (FLD the health multiplier, FIMUL, FSTP)
            const float scaled = (util::X87(world.questHealth) * util::X87::fromDouble(base)).f();
            std::uint16_t now = std::max<std::uint16_t>(chopAbs(scaled), 1);
            std::uint16_t top = std::max<std::uint16_t>(chopAbs(scaled), 1);
            o.health = std::min(now, most);
            o.maxHealth = std::min(top, most);
            o.touch();
        } else if ((o.classes & oc::Monster) && o.monster) {
            // Its monster.bin quest health, else its type's; not one a player summoned (MonsterStatus
            // bit 0x80, SUMMONED): its health as each player last saw it the new health (so no
            // delta is reported for the change).
            const int base = o.monster->def ? o.monster->def->questHealth : typeHealth(world, o);
            if (o.monster->status & things::MonsterStatus::Summoned) {
                continue;
            }
            const std::uint16_t v = std::max<std::uint16_t>(chopAbs((util::X87(world.questHealth) * static_cast<float>(base)).f()), 1);
            o.health = v;
            o.maxHealth = v;
            o.touch();
            o.monster->healthSeen.fill(o.health);
        }
    }
}

void questDifficultyTick(Fight& fight)
{
    if (fight.frame % (30u * 5u) != 0) {  // every 5 s (30 frames a second)
        return;
    }
    World& world = fight.world;
    const float was = world.questDifficulty;
    questSetDifficulty(world);
    if (world.questDifficulty != was) {
        questRescale(world);
    }
}

void sendStatMultipliers(World& world, const ServerObject& unit)
{
    if (!unit.player || 2 < unit.player->classId) {
        return;
    }
    const std::size_t k = unit.player->classId;
    const std::size_t manaClass = unit.player->multiclass ? 2 : k; // (noxcpp's multiclass: a conjurer's mana)
    // 0xEF, then four little-endian floats: health, mana, strength, speed.
    std::vector<std::uint8_t> m{0xEF};
    for (const float f : {world.healthMultiplier[k], world.manaMultiplier[manaClass], world.strengthMultiplier[k], world.speedMultiplier[k]}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        for (int i = 0; i < 4; ++i) {
            m.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
    }
    world.broadcasts().push_back({std::move(m), true, unit.extent});
}

void endQuestStats(World& world, bool tell)
{
    world.healthMultiplier = world.multipliersBeforeQuest[0];
    world.manaMultiplier = world.multipliersBeforeQuest[1];
    world.strengthMultiplier = world.multipliersBeforeQuest[2];
    world.speedMultiplier = world.multipliersBeforeQuest[3];
    for (ServerObject* u : playerUnits(world)) {
        readVals(world, *u, false);
        if (tell) {
            sendStatMultipliers(world, *u);
        }
    }
}

bool questOnceOnlySpell(int spell)
{
    static constexpr int kOnce[] = {0x22, 0x86, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x13};
    return std::ranges::find(kOnce, spell) != std::end(kOnce);
}

bool grantSpell(World& world, const things::ThingDb& things, ServerObject& unit, int spell, bool notify, bool show, int setLevel)
{
    if (!unit.player) {
        return false;
    }
    if (spell < 1 || 0x88 < spell) {
        world.textTo(unit, "plyrspel.c:AwardSpellError");
        return false;
    }
    PlayerControl& p = *unit.player;
    auto& levels = p.spellLevels;
    const bool quest = world.quest();
    // At the most already (3 in a solo or quest game, 5 anyway); a quest's once-only spells once.
    const int now = levels[static_cast<std::size_t>(spell)];
    if (((world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && now == 3) || now == 5 ||
        (quest && questOnceOnlySpell(spell) && now != 0)) {
        world.textTo(unit, "plyrspel.c:MaxSpellLevel");
        return false;
    }
    const auto set = [&](std::size_t i, int v) { levels[i] = static_cast<std::uint8_t>(v); };
    set(static_cast<std::size_t>(spell), std::min(now + 1, 5));
    if (quest && 3 < levels[static_cast<std::size_t>(spell)]) {
        set(static_cast<std::size_t>(spell), 3);
    }
    if (setLevel != 0) {
        set(static_cast<std::size_t>(spell), setLevel);
    }
    // Its group (spell flag 0x1000 / 0x4000 / 0x10000: those flagged 0x2000 / 0x8000 / 0x20000),
    // each up a level or to `setLevel`, at most 5; a quest's cap of 3 checks the granted spell, not
    // the member (an original bug).
    const things::SpellDef* def = things.findSpell(static_cast<std::uint32_t>(spell));
    std::uint32_t group = 0;
    if (def && (def->flags & 0x1000u)) {
        group = 0x2000u;
    } else if (def && (def->flags & 0x4000u)) {
        group = 0x8000u;
    } else if (def && (def->flags & 0x10000u)) {
        group = 0x20000u;
    }
    if (group != 0) {
        // Every spell of the group (1-0x88).
        for (int i = 1; i < 0x89; ++i) {
            const things::SpellDef* d = things.findSpell(static_cast<std::uint32_t>(i));
            if (!d || !(d->flags & group)) { // (spellIsValid: one it has)
                continue;
            }
            set(static_cast<std::size_t>(i), std::min(setLevel != 0 ? setLevel : levels[static_cast<std::size_t>(i)] + 1, 5));
            if (quest && 3 < levels[static_cast<std::size_t>(spell)]) {
                set(static_cast<std::size_t>(spell), 3);
            }
        }
    }
    if (notify) {
        playSound(world, 0xE2, unit);
        // Told to it and the others (MSG_GAUNTLET 0x1E), but a solo game's Fireball and the spells
        // flagged 0x15000; a quest's only for a player in it. (A player in transfer:
        // none here.)
        bool told = true;
        if ((world.gameFlags & GameFlag::Solo) && (spell == 0x22 || (def && (def->flags & 0x15000u)))) {
            told = false;
        }
        if ((!quest || p.questActive != 0) && told) {
            for (ServerObject* to : playerUnits(world)) {
                world.broadcasts().push_back({{0xF0, 0x1E, static_cast<std::uint8_t>(spell), lo(unit.extent), hi(unit.extent)}, true, to->extent});
            }
        }
    }
    // A solo game's shop shut on a book's award told and shown (Trade_ExitShop).
    if ((world.gameFlags & GameFlag::Solo) && notify && show && p.trade != 0) {
        if (TradeSession* s = tradeSession(world, p.trade)) {
            std::minstd_rand rng; // (the exit draws nothing)
            Fight f{world, rng, world.frameNow, &things, nullptr};
            exitShop(f, *s);
        }
    }
    world.broadcasts().push_back({{0x6F, static_cast<std::uint8_t>(spell), levels[static_cast<std::size_t>(spell)],
                                   static_cast<std::uint8_t>((notify ? 1 : 0) | (show ? 0x80 : 0))},
                                  true,
                                  unit.extent});
    return true;
}

std::optional<std::uint8_t> forcedClass(const World& world)
{
    if (!world.forceClass || !world.quest() || world.questSaves || 2 < *world.forceClass) {
        return std::nullopt;
    }
    return world.forceClass;
}

void forceClass(Fight& fight, ServerObject& unit, std::uint8_t classId)
{
    if (!unit.player) {
        return;
    }
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    p.info[0x42] = classId; // (PlayerInfo + 0x42: what its MSG_NEW_PLAYER tells; its class)
    if (p.classId == classId) {
        return;
    }
    p.classId = classId;
    // Its book the class's (another class's spells and a warrior's abilities, spells 1-5's levels
    // too, mean nothing to it): a quest's start's (startQuest's, which a run's start runs after
    // this anyway), or a network game's every spell at 3 (makePlayer's).
    p.fieldGuide.fill(false);
    p.spellLevels.fill(0);
    if (world.quest()) {
        if (fight.things) {
            if (classId == 1) {
                grantSpell(world, *fight.things, unit, 0x1B, false, false, 1);
            } else if (classId == 2) {
                grantSpell(world, *fight.things, unit, 0x09, false, false, 1);
                grantSpell(world, *fight.things, unit, 0x29, false, false, 1);
            }
        }
    } else if (world.network() && classId != 0) {
        std::fill(p.spellLevels.begin() + 1, p.spellLevels.end(), std::uint8_t{3});
    }
    readVals(world, unit, true);
    sendStatMultipliers(world, unit);
    // What the class can't use: taken off, kept (nothing it carries is lost).
    if (fight.things) {
        Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            ServerObject* o = world.byExtent(e);
            if (o && (o->flags & ObjFlag::Equipped) && (o->classes & (oc::Weapon | oc::Wand | oc::Armor)) && !items.classMay(unit, *o)) {
                items.takeOff(unit, *o, true, true);
            }
        }
    }
}

void startQuest(Fight& fight)
{
    World& world = fight.world;
    world.questStage = 0; // (a saved quest's stage kept; not yet)
    // The quest maps' plays cleared; engine flag 0x10 (every spell known) and game
    // flag 0x10000 off.
    if (world.questMaps) {
        world.questMaps->clearPlays();
    }
    world.gameFlags &= ~0x10000u;
    world.broadcasts().push_back({{0xF0, 0x00}, true, 0}); // (SrvNet_SendGauntletLevelStart)
    const auto units = playerUnits(world);
    for (ServerObject* u : units) {
        u->player->questActive = 0;
        // noxcpp: a forced class (a mode's conjurers) taken as the run begins, before the
        // class's start below (its spells and its item: a conjurer's Bow).
        if (const auto forced = forcedClass(world)) {
            forceClass(fight, *u, *forced);
        }
    }
    for (ServerObject* u : units) {
        PlayerControl& p = *u->player;
        clearTallies(world, p);
        // In while five or fewer are; told to everyone. (noxcpp: a stock player a NoX Core-only
        // game holds out, heldOut, stays out: no place in the party.)
        int in = 0;
        for (const ServerObject* v : units) {
            in += v->player->questActive != 0 ? 1 : 0;
        }
        p.questActive = in <= 5 && !p.heldOut ? 1 : 0;
        if (p.questActive == 1) {
            world.broadcasts().push_back({{0xF0, 0x01, lo(u->extent), hi(u->extent)}, true, 0});
        }
        // unitInitPlayer: its level (a network game's 10, else by its experience:),
        // its guides and spells none (spellAwardAll1 / 2, engine flag 0x10 off)
        // but a quest's own, its numbers, a warrior's abilities none (spellAwardAll3), its
        // starting lives, its outfit. (Its gold gone: no gold yet.)
        if (world.network()) {
            p.level = 10;
        } else {
            // Its level by the XP table: the first entry over its experience, less one.
            int level = 0;
            while (level < 0xB && world.number("XPTable", static_cast<std::size_t>(level)) <= u->experience) {
                ++level;
            }
            p.level = static_cast<std::uint8_t>(level - 1);
        }
        p.fieldGuide.fill(false);
        p.spellLevels.fill(0);
        p.abilityLevels.fill(0); // (noxcpp's multiclass book of abilities: none either)
        if (fight.things) {
            if (p.classId == 1) {
                grantSpell(world, *fight.things, *u, 0x1B, true, true, 1);
            } else if (p.classId == 2) {
                grantSpell(world, *fight.things, *u, 0x09, true, true, 1);
                grantSpell(world, *fight.things, *u, 0x29, true, true, 1);
            }
        }
        readVals(world, *u, false);
        if (p.classId == 0) {
            std::fill(p.spellLevels.begin() + 1, p.spellLevels.begin() + 6, std::uint8_t{0});
        }
        // Its lives anew.
        p.questLives = static_cast<std::uint32_t>(static_cast<int>(world.number("QuestGameStartingExtraLives")));
        if (fight.things) {
            Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
            items.defaultItems(*u);
            // Its armour but the 0x405 kinds and its weapons of subclass 0x8200 (long swords,
            // staffs) gone.
            for (const std::uint32_t e : keysOf(u->inventory)) {
                ServerObject* i = world.byExtent(e);
                if (!i) {
                    continue;
                }
                // Subclass 0x8200: STAFF and LONG_SWORD; armour kinds 0x405: the street
                // clothes (sneakers, pants, shirt).
                const bool goes = (i->classes & oc::Weapon) ? (i->subclasses & 0x8200u) != 0
                                                             : (i->classes & oc::Armor) && !(things::armorBitOf(i->typeName) & 0x405u);
                if (goes) {
                    items.removeFromInventory(*u, *i);
                    i->pendingDelete = true;
                }
            }
            // Its class's item (playerRespawnItem(.., 1, 1): worn as given, told to
            // every client, SrvNet_SendReportEquip's broadcastClass).
            if (p.classId == 0) {
                items.give(*u, "Sword", {0xFF, 0xFF, 0xFF, 0xFF}, true);
            } else if (p.classId == 1) {
                std::array<std::uint8_t, 4> mods{0xFF, 0xFF, 0xFF, 0xFF};
                if (const things::ModifierDef* m = fight.modifiers ? fight.modifiers->findModifier("Replenishment1") : nullptr) {
                    mods[2] = static_cast<std::uint8_t>(m->id);
                }
                items.give(*u, "SulphorousFlareWand", mods, true);
            } else if (p.classId == 2) {
                items.give(*u, "Bow", {0xFF, 0xFF, 0xFF, 0xFF}, true);
            }
        }
        if (p.questActive == 0) {
            goObserver(fight, *u, false, false);
        }
    }
    // Team play, one team of colour 9 with every player in. (Team_RemoveAll clears the team play
    // flag just set: a quest plays with one team but without team play, an original bug.)
    world.gameplayFlags |= 4u;
    removeAllTeams(world, true);
    if (ServerTeam* team = createTeam(world, 0)) {
        team->color = 9;
        setTeamName(*team, world.text("modifier.db:MaterialTeamOrangeDesc"), true);
        world.broadcasts().push_back({teamMessage(world, *team), true, 0});
        for (ServerObject* u : units) {
            if (u->player->questActive == 1) {
                addTeamMember(world, team->id, *u, true, false, fight.rng, fight.frame);
            }
        }
    }
    // The stat multipliers kept, the quest's.
    world.multipliersBeforeQuest = {world.healthMultiplier, world.manaMultiplier, world.strengthMultiplier, world.speedMultiplier};
    world.healthMultiplier = {3.0f, 3.0f, 3.0f};
    world.manaMultiplier = {1.0f, 3.0f, 3.0f};
    world.strengthMultiplier = {1.0f, 1.0f, 1.0f};
    world.speedMultiplier = {1.0f, 1.0f, 1.0f};
    for (ServerObject* u : units) {
        readVals(world, *u, false);
        sendStatMultipliers(world, *u);
    }
}

std::vector<std::uint32_t> questGenerators(Fight& fight, int difficulty)
{
    World& world = fight.world;
    std::vector<std::uint32_t> made;
    const auto stage = static_cast<std::uint32_t>(world.questStage);
    const auto hardcore = static_cast<std::uint32_t>(static_cast<int>(world.number("QuestHardcoreStage")));
    const auto d = static_cast<std::size_t>(std::clamp(difficulty, 0, 2));
    static constexpr const char* kMost[4] = {"GeneratorMaxActiveCreaturesHigh", "GeneratorMaxActiveCreaturesNormal",
                                             "GeneratorMaxActiveCreaturesLow", "GeneratorMaxActiveCreaturesSingular"};
    int exits = 0;
    int hecubahs = 0;
    // Each generator on the ground: deleted with no monsters for this difficulty; its most alive at a
    // time from the gamedata value mostBy names (High, Normal, Low, Singular), doubled from
    // QuestHardcoreStage (not a Singular one's); its type the generator of its first monster.
    for (auto& [extent, o] : world.objects()) {
        if (o.holder != 0) {
            continue; // (the world's list: not what's carried)
        }
        if ((o.classes & oc::Exit) && (o.subclasses & 1u)) {
            ++exits;
        } else if (o.typeName == "HecubahMarker") {
            ++hecubahs;
        }
        if (!(o.classes & oc::MonsterGenerator) || !o.generator) {
            continue;
        }
        auto& g = *o.generator;
        if (g.groups[d].empty()) {
            removeObject(world, o);
            continue;
        }
        const std::uint8_t by = g.mostBy[d];
        if (by < 4) {
            g.most = static_cast<std::uint8_t>(util::ftol(util::X87(world.number(kMost[by])))); // (__ftol)
        }
        if (hardcore <= stage && by != 3) {
            g.most = static_cast<std::uint8_t>(g.most << 1);
        }
        if (const char* type = generatorTypeOf(g.groups[d][0].object->typeName)) {
            o.typeName = type;
        }
    }
    // One quest exit kept, at random.
    if (1 < exits) {
        const int keep = nox::util::uniform_int_distribution<int>(0, exits - 1)(fight.rng);
        int i = 0;
        for (ServerObject* o : mapList(world)) {
            if ((o->classes & oc::Exit) && (o->subclasses & 1u)) {
                if (i != keep) {
                    removeObject(world, *o);
                }
                ++i;
            }
        }
    }
    // The bosses: from stage 5, on stage 5, from MinionsAlwaysStage, or on an odd stage one time in
    // two (a draw of 1-100 over 49); Hecubah at a random marker, a Necromancer at each of its
    // markers one time in two. (With no Hecubah marker, no Necromancers either.)
    world.questBosses = false;
    const int s = static_cast<int>(stage);
    if (4 < s) {
        const int always = static_cast<int>(world.number("MinionsAlwaysStage"));
        const auto odds = [&] { return nox::util::uniform_int_distribution<int>(1, 100)(fight.rng); };
        if (s == 5 || always <= s || ((stage & 1u) && 0x31 < odds())) {
            world.questBosses = true;
            if (hecubahs != 0) {
                // (Made in the walk, as the exe: what's made goes on the new objects' list, not
                // walked.)
                const int at = nox::util::uniform_int_distribution<int>(1, hecubahs)(fight.rng);
                int i = 0;
                for (ServerObject* o : mapList(world)) {
                    const float x = o->x;
                    const float y = o->y;
                    if (o->typeName == "HecubahMarker" && ++i == at) {
                        made.push_back(spawnBoss(fight, true, x, y));
                    }
                    if (o->typeName == "NecromancerMarker" && 0x31 < odds()) {
                        made.push_back(spawnBoss(fight, false, x, y));
                    }
                }
            }
        }
    }
    for (auto& [extent, o] : world.objects()) {
        if (o.typeName == "HecubahMarker" || o.typeName == "NecromancerMarker") {
            removeObject(world, o);
        }
    }
    return made;
}

void questMapLoaded(Fight& fight, std::string_view mapName)
{
    World& world = fight.world;
    // MonsterGeneratorInit ran as the map's objects were added, before the map's difficulty is
    // drawn: by the last map's.
    for (auto& [extent, o] : world.objects()) {
        if (o.generator && !o.generator->initialised) {
            generatorInit(world, o);
        }
    }
    world.mapDifficulty = nox::util::uniform_int_distribution<int>(0, 2)(fight.rng);
    ++world.questStage;
    const auto made = questGenerators(fight, world.mapDifficulty);
    questSetDifficulty(world);
    questRescale(world, made); // (the bosses not yet on the exe's list)
    questRewardPass(fight);
    if (world.questMaps) {
        world.questMaps->played(mapName);
    }
    setWarpGates(fight, true);
    if (static_cast<std::uint32_t>(static_cast<std::int64_t>(world.number("WarpGateCutoffStage"))) <= static_cast<std::uint32_t>(world.questStage)) {
        setWarpGates(fight, false);
    }
    world.questRejoinUntil = fight.frame + 30u; // (gameFrame + gameFPS)
    world.questWarpState = 0;
    if (world.questWarpGui) {
        world.questWarpGui = false; // (SrvNet_SendGauntletWarpGui to all)
        world.broadcasts().push_back({{0xF0, 0x18, 0}, true, 0});
    }
}
int questNextStageThreshold(int stage)
{
    if (stage < 5) {
        return 5;
    }
    if (stage < 10) {
        return 10;
    }
    if (stage < 15) {
        return 15;
    }
    return std::max(stage, 20);
}

bool questMaybeWarp(const World& world)
{
    const auto threshold = static_cast<std::uint32_t>(questNextStageThreshold(world.questStage));
    int in = 0;
    bool reached = false;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (!o.player || o.gone() || o.player->questActive == 0) {
            continue;
        }
        ++in;
        if (o.player->warpGate == 0) {
            return false;
        }
        if (threshold <= o.player->questHighestStage) {
            reached = true;
        }
    }
    return in != 0 && reached;
}

bool questAllExited(const World& world)
{
    int in = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (!o.player || o.gone() || o.player->questActive == 0) {
            continue;
        }
        ++in;
        if (o.player->exitGate == 0) {
            return false;
        }
    }
    return in != 0;
}

void questExitCountdown(Fight& fight)
{
    World& world = fight.world;
    int total = static_cast<int>(world.number("QuestExitTimerStart")); // (Math_FloatToInt)
    if (world.timerRunning) {
        // What's left of the timer, in seconds (the timer's ms left / 1000).
        const auto ms = (static_cast<std::int64_t>(world.timerEnd) - static_cast<std::int64_t>(fight.frame)) * 1000 / 30;
        total = static_cast<int>(static_cast<std::int32_t>(ms) / 1000);
    }
    int in = 0;
    int out = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && o.player->questActive == 1) {
            ++in;
            out += o.player->exitGate != 0 ? 1 : 0;
        }
    }
    if (in == 0) {
        setTimerRunning(world, false, fight.frame);
        return;
    }
    // The full time less the share of the players already out.
    // (FILD, FIDIV, FIMUL, FSTP, Math_FloatToInt)
    const int left = total - util::floatToInt(util::X87::fromDouble(out) / util::X87::fromDouble(in) * util::X87::fromDouble(total));
    int seconds = total;
    if (total <= left) {
        if (world.countdown) {
            return; // (one under way already)
        }
    } else {
        seconds = left;
    }
    // servStartCountdown, then MSG_GAUNTLET 0x14 (the HUD) to everyone.
    world.timerEnd = fight.frame + static_cast<std::uint32_t>(seconds) * 30u;
    setTimerRunning(world, true, fight.frame);
    informString(world, 0, "objcoll.c:ExitCountdown");
    world.countdown = true;
    world.broadcasts().push_back({{0xF0, 0x14}, true, 0});
}

void questLeaveWarpGate(Fight& fight, ServerObject& unit)
{
    if (!unit.player || unit.player->warpGate == 0) {
        return;
    }
    PlayerControl& p = *unit.player;
    const ServerObject* gate = fight.world.byExtent(p.warpGate);
    leaveObserver(fight, unit);
    p.cameraFollow = 0; // (playerCameraUnlock)
    p.warpGate = 0;
    const float x = gate ? gate->exitX : unit.x;
    const float y = gate ? gate->exitY : unit.y;
    setPosition(fight.world, unit, x, y, fight.frame);
    playSound(fight.world, 0x138, unit, 2, unit.extent);
    pointFx(fight.world, 0x81, x, y);
}

void questExitCollide(Fight& fight, ServerObject& exit, ServerObject& unit)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    // The next quest map (getQuestMapFile), its abilities ended.
    const std::optional<std::string> next = world.questMaps ? world.questMaps->pick(fight.rng) : std::nullopt;
    for (int a = 1; a < 6; ++a) {
        if (abilityActive(world, unit, a)) {
            endAbility(world, unit, a);
        }
    }
    bool done = true; // (an exit neither kind: the map at once)
    if (exit.subclasses & 1u) {
        // A quest exit: the map to its tallies (as a time run out), the
        // stage it reached, out as an observer, everyone told; everyone out?
        const auto reached = static_cast<std::uint32_t>(world.questStage) + 1u;
        questTimeUpTally(unit);
        if (p.questHighestStage < reached) {
            p.questHighestStage = reached;
            world.broadcasts().push_back({{0xF0, 0x1D, static_cast<std::uint8_t>(reached), static_cast<std::uint8_t>(reached >> 8)}, true, unit.extent});
        }
        p.exitGate = exit.extent;
        p.warpGate = 0;
        setPlayerState(world, unit, 0xD, fight.frame);
        goObserver(fight, unit, false, false);
        inform(world, 0, 0x12, unit.extent);
        done = questAllExited(world);
    } else if (exit.subclasses & 2u) {
        // A warp gate: waiting in it as an observer; everyone waiting? (The exe calls
        // SrvNet_SendInform(0x13, its code) for each other player, but that function sends only
        // codes 0-2, 0xC, 0xD, 0x10, 0x11, 0x14 and 0x15: the others are never told,
        // an original bug.)
        p.exitGate = 0;
        p.warpGate = exit.extent;
        world.questWarpEnterFrame = fight.frame;
        setPlayerState(world, unit, 0xD, fight.frame);
        goObserver(fight, unit, false, false);
        informString(world, unit.extent, "objcoll.c:PlayerEntersWarp");
        done = questMaybeWarp(world);
    }
    playSound(world, 0x3EB, exit);
    if (!(exit.subclasses & 2u) && !done) {
        questExitCountdown(fight);
    }
    // The map kept for the time limit (the exe copies the pick's text: none crashes it there).
    if (next) {
        world.questNextMap = *next;
    }
    if (done && next && !next->empty()) {
        if (exit.subclasses & 2u) {
            // A warp: to the threshold's stage (the load adds the 1), the tallies anew.
            world.questStage = questNextStageThreshold(world.questStage) - 1;
            world.questWarped = true;
            for (ServerObject* o : playerUnits(world)) {
                clearTallies(world, *o->player);
            }
        }
        world.exitTo = *next; // (mapLoad)
    }
}

void questFrame(Fight& fight)
{
    World& world = fight.world;
    // after a quest map's load (and its second), those observing but ready, in the
    // quest and in no exit, back in play.
    // statusFlags 1: observing; 0x10: ready.
    const auto rejoins = [](const PlayerControl& p) {
        // (noxcpp: heldOut, a NoX Core-only game's stock player: never)
        return (p.statusFlags & 1u) && p.questActive == 1 && p.exitGate == 0 && p.warpGate == 0 && (p.statusFlags & 0x10u) &&
               !p.heldOut;
    };
    // (Looked for first: the units gathered only when one is to come back.)
    const auto anyRejoins = [&] {
        return std::ranges::any_of(world.playerUnits(), [&](const ServerObject* u) { return !u->gone() && rejoins(*u->player); });
    };
    if (world.questRejoinUntil != 0 && world.questRejoinUntil < fight.frame && anyRejoins()) {
        for (ServerObject* u : playerUnits(world)) {
            if (rejoins(*u->player)) {
                joinTeam(fight, *u);
                leaveObserver(fight, *u);
                u->player->cameraFollow = 0;
            }
        }
    }
    // 5 minutes from the first soul gate taken, with a checkpoint among them and more
    // than one in the quest, the warp window (MSG_GAUNTLET 0x18 on) to everyone.
    if (world.questWarpState != 0 && 8999u < fight.frame - static_cast<std::uint32_t>(world.questWarpState)) {
        bool checkpoint = false;
        for (const ServerObject* u : world.playerUnits()) {
            checkpoint = checkpoint || (!u->gone() && u->player->soulGate != 0);
        }
        if (checkpoint && 1 < questPlayers(world)) {
            world.questWarpState = 0;
            if (!world.questWarpGui) {
                world.questWarpGui = true;
                world.broadcasts().push_back({{0xF0, 0x18, 1}, true, 0});
            }
        }
    }    // Game_QuestCheckWarpGate: 30 frames on from the last in, everyone in the quest waiting in a
    // gate: the warp, or each put out and told why.
    const int in = questPlayers(world);
    if (29u < fight.frame - world.questWarpEnterFrame && in != 0) {
        int waiting = 0;
        for (const ServerObject* u : world.playerUnits()) {
            waiting += !u->gone() && u->player->questActive != 0 && u->player->warpGate != 0 ? 1 : 0;
        }
        if (in == waiting && !questMaybeWarp(world)) {
            for (ServerObject* u : playerUnits(world)) {
                if (u->player->questActive != 0 && u->player->warpGate != 0) {
                    questLeaveWarpGate(fight, *u);
                    informString(world, u->extent, in < 2 ? "Gauntlet.c:WarpRestrictedSolo" : "Gauntlet.c:WarpRestrictedMulti");
                }
            }
        }
    }
    // (the save transfers' end, not yet.) The slots not in the quest 30 s after a
    // client came in forgotten.
    questForgetSlots(fight);
}

void questTimeUp(Fight& fight)
{
    World& world = fight.world;
    if (!world.questNextMap.empty()) {
        world.exitTo = world.questNextMap; // (the kept next map, mapLoad)
    }
    informString(world, 0, "chklimit.c:AutoExitToNextMap");
    // Those still in (out by no quest exit) tallied.
    for (ServerObject* u : playersByIndex(world)) {
        if (u->player->exitGate == 0 && u->player->questActive == 1) {
            questTimeUpTally(*u);
        }
    }
    setTimerRunning(world, false, fight.frame);
}

void questPlayerLeaves(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    if (!world.quest() || !unit.player || unit.player->questActive == 0) {
        return;
    }
    playSound(world, 0x3F3, unit);
    unit.player->questActive = 0;
    if (questPlayers(world) == 0) {
        // Nobody left in the quest: stage 0 and the next quest map (the load makes it 1). (No quest
        // map: the exe loads from a null name; ours keeps the map and the stage.)
        const std::optional<std::string> next = world.questMaps ? world.questMaps->pick(fight.rng) : std::nullopt;
        if (next && !next->empty()) {
            world.questStage = 0;
            world.exitTo = *next; // (mapLoad)
        }
    }
    // Its keys to the others (Plr_GiveAwayKeysOnLeave), then everything it holds gone.
    questGiveAwayKeys(fight, unit);
    for (const std::uint32_t e : keysOf(unit.inventory)) {
        if (ServerObject* i = world.byExtent(e)) {
            deleteObject(world, *i); // (delayedDeleteObject: out of it at once)
        }
    }
}

ServerObject* questSharedSilverKey(World& world)
{
    // the last player (by index) holding a SilverKey, its first one.
    const auto isSilver = [&](std::uint32_t e) {
        const ServerObject* i = world.byExtent(e);
        return i && i->typeName == "SilverKey";
    };
    ServerObject* holder = nullptr;
    for (ServerObject* u : playersByIndex(world)) {
        if (std::ranges::any_of(u->inventory, isSilver)) {
            holder = u;
        }
    }
    if (!holder) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(holder->inventory, isSilver);
    return it != holder->inventory.end() ? world.byExtent(*it) : nullptr;
}

void questOpenDoor(World& world, ServerObject& door)
{
    // the door's marker cleared, everyone told (SrvNet_SendGauntletClearMarker).
    door.doorQuestOpened = true;
    world.broadcasts().push_back({{0xF0, 0x0F, lo(door.extent), hi(door.extent)}, true, 0});
}

void questGiveAwayKeys(Fight& fight, ServerObject& unit)
{
    // Plr_GiveAwayKeysOnLeave: with anyone left in the quest, each key to the one holding
    // fewest (by index, the last of the fewest), "GeneralPrint:GainedKey" and 0x334;
    // then the keys left on the ground about it (within 60).
    World& world = fight.world;
    if (!fight.things) {
        return;
    }
    Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    const auto isKey = [&](const ServerObject* i) { return i && (i->classes & oc::Key); };
    if (questPlayers(world) != 0) {
        for (const std::uint32_t e : keysOf(unit.inventory)) {
            ServerObject* key = world.byExtent(e);
            if (!isKey(key)) {
                continue;
            }
            ServerObject* to = nullptr;
            int fewest = 99999;
            for (ServerObject* u : playersByIndex(world)) {
                if (u->player->questActive != 1) {
                    continue;
                }
                const int n = static_cast<int>(std::ranges::count_if(u->inventory, [&](std::uint32_t k) { return isKey(world.byExtent(k)); }));
                if (n <= fewest) {
                    to = u;
                    fewest = n;
                }
            }
            if (to) {
                items.removeFromInventory(unit, *key);
                items.receive(*to, *key);
                informString(world, to->extent, "GeneralPrint:GainedKey");
                playSound(world, 0x334, *to);
            }
        }
    }
    for (const std::uint32_t e : keysOf(unit.inventory)) {
        if (ServerObject* key = world.byExtent(e); isKey(key)) {
            items.removeFromInventory(unit, *key);
            const auto [x, y] = clearPointNear(world, fight.rng, unit.x, unit.y, 60.0f);
            items.place(*key, x, y);
        }
    }
}

void questAfterLeave(Fight& fight)
{
    World& world = fight.world;
    // Everyone left in the quest out by a quest exit: the kept next map.
    if (questAllExited(world)) {
        if (!world.questNextMap.empty()) {
            world.exitTo = world.questNextMap;
        }
        return;
    }
    // Everyone left waiting in a warp gate, one past the threshold: the warp (the tallies anew,
    // the warp flag set; the stage the threshold's less one).
    if (questMaybeWarp(world)) {
        for (ServerObject* u : playerUnits(world)) {
            clearTallies(world, *u->player);
        }
        world.questWarped = true;
        world.questStage = questNextStageThreshold(world.questStage) - 1;
        if (!world.questNextMap.empty()) {
            world.exitTo = world.questNextMap;
        }
        return;
    }
    // Someone out by a quest exit: the exit's countdown anew.
    for (const ServerObject* u : playersByIndex(world)) {
        if (!u->pendingDelete && u->player->exitGate != 0) {
            questExitCountdown(fight);
            return;
        }
    }
}

void questClearTallies(const World& world, ServerObject& unit)
{
    if (unit.player) {
        clearTallies(world, *unit.player);
    }
}

std::vector<std::uint8_t> questTeamHealthMessage(const ServerObject& unit)
{
    // (SrvNet_SendTeamHealthPercent: MSG_TEAM_MSG sub 0x0C, its code, health x 100 / most)
    const std::uint32_t share = unit.maxHealth != 0 ? static_cast<std::uint32_t>(unit.health) * 100u / static_cast<std::uint32_t>(unit.maxHealth) : 0u;
    return {0xC4, 0x0C, lo(unit.extent), hi(unit.extent), static_cast<std::uint8_t>(share)};
}

bool questRoomFor(const World& world)
{
    // (the host's own slot 0x1F aside, with engine flag 0x40000: no host player here)
    int in = 0;
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && !o.pendingDelete && o.player->questActive != 0) {
            ++in;
        }
    }
    return in <= 5;
}

void questForgetSlot(World& world, std::uint8_t index)
{
    for (ServerObject* pu : world.playerUnits()) {
        ServerObject& o = *pu;
        if (o.player) {
            o.player->livesSent[index & 31u] = 0;
            o.player->silverKeySent[index & 31u] = 0;
            o.player->goldKeySent[index & 31u] = 0;
        }
    }
}

void questPlayerJoined(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    world.broadcasts().push_back({{0xF0, 0x01, lo(unit.extent), hi(unit.extent)}, true, 0}); // (SrvNet_SendGauntletPlayerJoined)
    clearTallies(world, *unit.player);
    questForgetSlot(world, unit.player->index);
}

void questForgetSlots(Fight& fight)
{
    World& world = fight.world;
    // (Each slot's unit: the first player of that index in the objects' order, all found in one
    // walk before any slot is forgotten: whether it's in the quest.)
    std::array<bool, 32> found{};
    std::array<bool, 32> inQuest{};
    for (const ServerObject* pu : world.playerUnits()) {
        const ServerObject& o = *pu;
        if (o.player && !o.gone() && !o.pendingDelete && o.player->index < 32 && !found[o.player->index]) {
            found[o.player->index] = true;
            inQuest[o.player->index] = o.player->questActive == 1;
        }
    }
    for (std::uint8_t i = 0; i < 32; ++i) {
        const bool in = inQuest[i];
        std::uint32_t& seen = world.questSlotSeenAt[i];
        if (in) {
            seen = 0;
        } else if (seen != 0 && 900u < fight.frame - seen) { // (gameFPS x 30)
            questForgetSlot(world, i);
            seen = 0;
        }
    }
}

void questDeath(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    if (p.questLives == 0) {
        // Game over: MSG_GAUNTLET 2 (its generators, secrets, kills, the stage it joined at), its
        // tallies cleared, the death's losses, its lives anew (its own client not told them).
        p.questGameOverAt = fight.frame;
        const auto u16 = [](std::vector<std::uint8_t>& m, std::uint32_t v) {
            m.push_back(static_cast<std::uint8_t>(v));
            m.push_back(static_cast<std::uint8_t>(v >> 8));
        };
        std::vector<std::uint8_t> m = {0xF0, 0x02};
        u16(m, p.questGenerators);
        u16(m, p.questSecrets);
        u16(m, p.questKills);
        u16(m, p.questJoinStage);
        m.insert(m.end(), 4, std::uint8_t{0});
        world.broadcasts().push_back({std::move(m), true, unit.extent});
        clearTallies(world, p);
        questDeathLosses(fight, unit);
        p.questLives = static_cast<std::uint32_t>(static_cast<int>(world.number("QuestGameStartingExtraLives")));
        p.livesSent[p.index & 31u] = static_cast<std::uint8_t>(p.questLives);
        return;
    }
    --p.questLives;
    // (not flag 0x20) a death counted, flag 2.
    if (!(unit.flags & 0x20u)) {
        ++p.questDeaths;
        p.questFlags |= 2u;
    }
}

void questDeathLosses(Fight& fight, ServerObject& unit)
{
    World& world = fight.world;
    PlayerControl& p = *unit.player;
    // The exe's randInt: an empty range (hi = lo - 1) draws nothing and gives hi.
    const auto randInt = [&](int lo, int hi) {
        return hi - lo + 1 == 0 ? hi : nox::util::uniform_int_distribution<int>(lo, hi)(fight.rng); // (randInt)
    };
    const auto held = [&] {
        std::vector<ServerObject*> out;
        for (const std::uint32_t e : unit.inventory) {
            if (ServerObject* o = world.byExtent(e); o && !o->pendingDelete) {
                out.push_back(o);
            }
        }
        return out;
    };
    std::optional<Items> items;
    if (fight.things) {
        items.emplace(world, *fight.things, fight.modifiers, fight.rng, fight.frame);
    }
    const auto destroy = [&](ServerObject& o) {
        if (items) {
            items->removeFromInventory(unit, o);
        }
        o.pendingDelete = true; // (delayedDeleteObject)
    };
    // Half its gold.
    subGold(unit, p.gold >> 1);
    // of each of Diamond, Emerald, Ruby, an odd one sold for half its price
    // (shopGetItemCost(1, no shop): its worth by its health, at least 1), half of
    // the rest gone.
    {
        static constexpr std::string_view kGems[3] = {"Diamond", "Emerald", "Ruby"};
        std::array<int, 3> count{};
        for (const ServerObject* o : held()) {
            for (std::size_t k = 0; k < 3; ++k) {
                count[k] += o->typeName == kGems[k] ? 1 : 0;
            }
        }
        std::array<bool, 3> sell{};
        std::array<int, 3> lose{};
        for (std::size_t k = 0; k < 3; ++k) {
            sell[k] = (count[k] & 1) != 0;
            lose[k] = (count[k] - (sell[k] ? 1 : 0)) / 2;
        }
        for (ServerObject* o : held()) {
            for (std::size_t k = 0; k < 3; ++k) {
                if (o->typeName != kGems[k]) {
                    continue;
                }
                if (sell[k]) {
                    const int worth = itemCost(world, fight.modifiers, 1, nullptr, *o); // (no shop's multipliers)
                    destroy(*o);
                    addGold(unit, worth / 2);
                    sell[k] = false;
                } else if (0 < lose[k]) {
                    destroy(*o);
                    --lose[k];
                }
            }
        }
    }
    // its weapon in hand (not a quiver), when it has another its class may use; its
    // class's own (a SulphorousFlare staff, a plain sword or bow) kept.
    if (items) {
        ServerObject* inHand = nullptr;
        for (ServerObject* o : held()) {
            // Subclass 2: a quiver.
            if ((o->flags & ObjFlag::Equipped) && (o->classes & (oc::Weapon | oc::Wand)) && !(o->subclasses & 2u)) {
                inHand = o;
                break;
            }
        }
        // Plain: a sword or bow (subclass 0x104) with no modifiers. Subclass 0x10000: the
        // SulphorousFlare staff.
        const bool plain = inHand && (inHand->subclasses & 0x104u) &&
                           std::ranges::all_of(inHand->modifiers, [](std::uint8_t m) { return m == 0xFF; });
        if (inHand && !(inHand->subclasses & 0x10000u) && !plain) {
            const bool spare = std::ranges::any_of(held(), [&](const ServerObject* o) {
                return (o->classes & (oc::Weapon | oc::Wand)) && !(o->flags & ObjFlag::Equipped) && items->classMay(unit, *o);
            });
            if (spare) {
                destroy(*inHand);
            }
        }
    }
    // an armour piece worn at random (not the 0x405 kinds), twice for a warrior.
    const auto loseArmour = [&] {
        std::vector<ServerObject*> worn;
        for (ServerObject* o : held()) {
            if ((o->flags & ObjFlag::Equipped) && (o->classes & oc::Armor) && !(things::armorBitOf(o->typeName) & 0x405u)) {
                worn.push_back(o);
            }
        }
        if (!worn.empty()) {
            destroy(*worn[static_cast<std::size_t>(randInt(0, static_cast<int>(worn.size()) - 1))]);
        }
    };
    loseArmour();
    if (p.classId == 0) {
        loseArmour();
    }
    // Plr_QuestDeathLoseRandomSpell (a caster's, twice), ...Guide (a conjurer's, twice),
    // ...Ability (a warrior's): one of those it has and may lose, told (MSG_GAUNTLET 0x11 / 0x13 /
    // 0x12 with its number).
    const auto revoke = [&](std::uint8_t sub, int what) {
        world.broadcasts().push_back({{0xF0, sub, static_cast<std::uint8_t>(what), static_cast<std::uint8_t>(what >> 8)}, true, unit.extent});  // 0xF0: MSG_GAUNTLET
    };
    // (noxcpp's multiclass player, a mode's: each loss as it knows that kind.)
    const auto loseSpell = [&] {
        // (A wizard's or a conjurer's: class 1 or 2, not "not a warrior".)
        if (p.classId != 1 && p.classId != 2 && !p.multiclass) {
            return;
        }
        int n = 0;
        for (int i = 0; i < 0x89; ++i) {  // 0x89 = 137: every spell id
            n += p.spellLevels[static_cast<std::size_t>(i)] != 0 && questSpellLosable(i) ? 1 : 0;
        }
        // None to lose: k is -1 and nothing matches.
        const int k = randInt(0, n - 1);
        int seen = 0;
        for (int i = 0; i < 0x89; ++i) {
            if (p.spellLevels[static_cast<std::size_t>(i)] != 0 && questSpellLosable(i)) {
                if (seen == k) {
                    p.spellLevels[static_cast<std::size_t>(i)] = 0;
                    revoke(0x11, i);
                    return;
                }
                ++seen;
            }
        }
    };
    const auto loseGuide = [&] {
        if (p.classId != 2 && !p.multiclass) {
            return;
        }
        int n = 0;
        for (int i = 0; i < 0x29; ++i) {  // 0x29 = 41: every field guide entry
            n += p.fieldGuide[static_cast<std::size_t>(i)] && questGuideLosable(i) ? 1 : 0;
        }
        const int k = randInt(0, n - 1);
        int seen = 0;
        for (int i = 0; i < 0x29; ++i) {
            if (p.fieldGuide[static_cast<std::size_t>(i)] && questGuideLosable(i)) {
                if (seen == k) {
                    p.fieldGuide[static_cast<std::size_t>(i)] = false;
                    revoke(0x13, i);
                    return;
                }
                ++seen;
            }
        }
    };
    loseSpell();
    loseSpell();
    loseGuide();
    loseGuide();
    if (p.usesAbilities()) {
        int n = 0;
        for (int a = 1; a < 6; ++a) {
            n += p.abilityLevel(static_cast<std::size_t>(a)) != 0 ? 1 : 0;
        }
        const int k = randInt(1, n); // (none: 0, no draw, nothing taken)
        int seen = 1;
        // Pick the k-th held ability (counting from 1).
        for (int a = 1; a < 6; ++a) {
            if (p.abilityLevel(static_cast<std::size_t>(a)) != 0) {
                if (seen == k) {
                    p.abilityLevel(static_cast<std::size_t>(a)) = 0;
                    revoke(0x12, a);
                    break;
                }
                ++seen;
            }
        }
    }
}

void questAnkhCollide(Fight& fight, ServerObject& ankh, ServerObject* other)
{
    World& world = fight.world;
    if (!other || !(other->classes & oc::Player) || !other->player) {
        return;
    }
    PlayerControl& p = *other->player;
    auto& memory = ankh.ankh ? *ankh.ankh : ankh.ankh.emplace();
    const auto refuse = [&](std::string_view id) {
        // (Not within 1.5 s, 0x5DD ms, of the last: the exe's one timer, qword with the
        // doors' and chests' locked notices: World::lockNoticeAt.)
        if (world.lockNoticeAt && fight.frame - *world.lockNoticeAt <= 45u) {
            return;
        }
        informString(world, other->extent, id);
        playSound(world, 0x39D, *other);
        world.lockNoticeAt = fight.frame;
    };
    const auto noteTaken = [&] {
        for (ObjectRef& t : p.ankhsTaken) {
            if (t == 0) {
                t = ankh.extent;
                break;
            }
        }
    };
    // Refused when this ankh is in the player's own taken list, or the ankh remembers the same
    // character (class, name and serial) from the last 240 s.
    const bool already = std::ranges::find(p.ankhsTaken, ankh.extent) != p.ankhsTaken.end();
    bool known = false;
    for (auto& t : memory.takers) {
        if (30u * 240u < fight.frame - t.frame) {
            t = {}; // (forgotten after 240 s)
        }
        if (t.classId == p.classId && t.name == p.name && t.serial == p.serial) {
            known = true;
            noteTaken();
            break;
        }
    }
    if (known || already) {
        refuse("objcoll.c:ExtraLifeAlreadyAwarded");
        return;
    }
    // Or when the player already holds the most extra lives the gamedata allows.
    if (static_cast<int>(world.number("MaxExtraLives")) <= static_cast<int>(p.questLives)) {
        refuse("pickup.c:MaxTradableAnkhsReached");
        return;
    }
    // An AnkhTradable made and picked up (AnkhTradablePickup: the life, the ankh gone, 0x3EC at the
    // player).
    if (fight.things && fight.things->findThing("AnkhTradable")) {
        world.spawn(*fight.things, "AnkhTradable", other->x, other->y).pendingDelete = true;
    }
    ++p.questLives;
    playSound(world, 0x3EC, *other);
    playSound(world, 0x3EC, ankh);
    pointFx(world, 0x82, ankh.x, ankh.y);
    informString(world, other->extent, "objcoll.c:AwardExtraLife");
    noteTaken();
    // Remembered in the ankh's ring of its last 64 takers.
    auto& slot = memory.takers[memory.next];
    slot.name = p.name;
    slot.classId = p.classId;
    slot.serial = p.serial;
    slot.frame = fight.frame;
    memory.next = static_cast<std::uint8_t>((memory.next + 1) % 64);
}
void questSoulGateCollide(Fight& fight, ServerObject& gate, ServerObject* other)
{
    World& world = fight.world;
    if (!world.quest() || !other || !(other->classes & oc::Player) || !other->player) {
        return;
    }
    setWarpGates(fight, false);
    bool first = true;
    for (ServerObject* u : playerUnits(world)) {
        if (u->player->questActive == 1 && u->player->soulGate != 0) {
            first = false;
        }
    }
    if (first) {
        world.questWarpState = static_cast<int>(fight.frame); // (the warp window's start)
    }
    PlayerControl& p = *other->player;
    if (p.soulGate != gate.extent || 30u < fight.frame - gate.soulGateAt) { // (gameFPS)
        playSound(world, 0x3ED, gate);
        pointFx(world, 0x82, gate.x, gate.y);
        informString(world, other->extent, "objcoll.c:SoulGateCollide");
    }
    p.soulGate = gate.extent;
    gate.soulGateAt = fight.frame;
}

std::pair<float, float> nearSoulGate(Fight& fight, const ServerObject& gate)
{
    std::pair<float, float> at{gate.x, gate.y};
    for (int i = 0; i < 32; ++i) {
        at = clearPointNear(fight.world, fight.rng, gate.x, gate.y, 60.0f);
        if (!fight.world.noTeleportAt(at.first, at.second)) { // (mapTileAllowTeleport)
            break;
        }
    }
    return at;
}

bool joinAtSoulGate(Fight& fight, ServerObject& unit, float& x, float& y)
{
    World& world = fight.world;
    const ServerObject* best = nullptr;
    std::uint32_t bestAt = 0;
    for (ServerObject* u : playerUnits(world)) {
        const ServerObject* gate = u->player->soulGate ? world.byExtent(u->player->soulGate) : nullptr;
        if (gate && bestAt < gate->soulGateAt) {
            bestAt = gate->soulGateAt;
            best = gate;
        }
    }
    if (!best || !unit.player) {
        return false;
    }
    unit.player->soulGate = best->extent;
    std::tie(x, y) = clearPointNear(world, fight.rng, best->x, best->y, 60.0f);
    return true;
}
} // namespace nox::server
