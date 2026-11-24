// A creature's numbers a mode sets (mod_creatures.hpp).
#include "mod_creatures.hpp"

#include "toolbox.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include "lua.h"
#include "lualib.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nox::modes::script {

namespace {

namespace oc = things::ObjectClass;

/// A creature's (one with an AI) at `idx`.
server::ServerObject& creatureArg(lua_State* L, int idx)
{
    server::ServerObject& o = toolbox::objectArg(L, idx);
    luaL_argcheck(L, (o.classes & oc::Monster) != 0 && o.monster.has_value(), idx, "a creature");
    return o;
}

using toolbox::coordArg;
using toolbox::floatArg; // (a finite number from lo to hi, as the nearest float)

// ---- the record ------------------------------------------------------------------------------

enum class KeyKind { Int, Float, Status, DamageType, Missile, Strike, Die, Dead };

struct RecordKey {
    const char* name;
    KeyKind kind;
};

// monster.bin's keys (monster_db.hpp), as a record names them.
constexpr RecordKey kKeys[] = {
    {"EXPERIENCE", KeyKind::Int},
    {"HEALTH", KeyKind::Int},
    {"QUESTHEALTH", KeyKind::Int},
    {"SPEED", KeyKind::Int},
    {"RETREAT_RATIO", KeyKind::Float},
    {"RESUME_RATIO", KeyKind::Float},
    {"FLEE_RANGE", KeyKind::Float},
    {"STATUS", KeyKind::Status},
    {"RUN_MULTIPLIER", KeyKind::Float},
    {"MOVE_SOUND_FRAME_A", KeyKind::Int},
    {"MOVE_SOUND_FRAME_B", KeyKind::Int},
    {"MELEE_ATTACK_FRAME", KeyKind::Int},
    {"MELEE_ATTACK_RANGE", KeyKind::Float},
    {"MELEE_ATTACK_DAMAGE", KeyKind::Int},
    {"MELEE_ATTACK_IMPACT", KeyKind::Float},
    {"MELEE_ATTACK_DAMAGE_TYPE", KeyKind::DamageType},
    {"MELEE_ATTACK_MIN_DELAY", KeyKind::Int},
    {"MELEE_ATTACK_MAX_DELAY", KeyKind::Int},
    {"MELEE_ATTACK_POISON_CHANCE", KeyKind::Int},
    {"MELEE_ATTACK_POISON_STRENGTH", KeyKind::Int},
    {"MELEE_ATTACK_POISON_MAX", KeyKind::Int},
    {"MISSILE_NAME", KeyKind::Missile},
    {"MISSILE_ATTACK_RANGE", KeyKind::Float},
    {"MISSILE_ATTACK_FRAME", KeyKind::Int},
    {"MISSILE_ATTACK_MIN_DELAY", KeyKind::Int},
    {"MISSILE_ATTACK_MAX_DELAY", KeyKind::Int},
    {"MELEE_STRIKE_FUNCTION", KeyKind::Strike},
    {"DIE_FUNCTION", KeyKind::Die},
    {"DEAD_FUNCTION", KeyKind::Dead},
};

const RecordKey* recordKey(std::string_view name)
{
    for (const auto& k : kKeys) {
        if (name == k.name) {
            return &k;
        }
    }
    return nullptr;
}

/// A record's value as its line's text, from the table's value at the top of the stack.
std::string lineValue(lua_State* L, const RecordKey& k)
{
    const auto bad = [&](const char* what) {
        luaL_error(L, "nox.setCreatureProfile: %s %s", k.name, what);
        return std::string();
    };
    switch (k.kind) {
    case KeyKind::Int: {
        if (!lua_isnumber(L, -1)) {
            return bad("takes a whole number");
        }
        const double v = lua_tonumber(L, -1);
        if (v != std::floor(v) || v < -2147483648.0 || v > 2147483647.0) {
            return bad("takes a whole number");
        }
        return std::to_string(static_cast<long long>(v));
    }
    case KeyKind::Float: {
        // (The nearest float, as a map script's literal: toolbox::toFloat; one past a float's range
        // refused.)
        const float f = lua_isnumber(L, -1) ? toolbox::toFloat(lua_tonumber(L, -1)) : std::numeric_limits<float>::quiet_NaN();
        if (!std::isfinite(f)) {
            return bad("takes a number (a float's: finite, at most about 3.4e38 either way)");
        }
        char buf[48];
        // (In hexadecimal: the reader takes it back as exactly that float, whatever any printing's
        // rounding.)
        std::snprintf(buf, sizeof buf, "%a", static_cast<double>(f));
        return buf;
    }
    case KeyKind::Status:
        if (lua_isnumber(L, -1)) {
            const double v = lua_tonumber(L, -1);
            if (v != std::floor(v) || v < 0 || v > 4294967295.0) {
                return bad("takes a number of bits or '+' names");
            }
            return things::monsterStatusText(static_cast<std::uint32_t>(v));
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            return bad("takes a number of bits or '+' names");
        }
        return lua_tostring(L, -1);
    case KeyKind::DamageType:
        if (lua_isnumber(L, -1)) {
            const double v = lua_tonumber(L, -1);
            const std::string_view n = v == std::floor(v) && v >= 0 && v < 64 ? things::monsterDamageTypeName(static_cast<int>(v)) : std::string_view();
            if (n.empty()) {
                return bad("takes a damage type's number (0-17) or name");
            }
            return std::string(n);
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            return bad("takes a damage type's number (0-17) or name");
        }
        return lua_tostring(L, -1);
    default:
        // (A missile's name, a function's; "" or false: none.)
        if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1)) {
            return "NULL";
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            return bad("takes a name (\"\" or false: none)");
        }
        const std::string v = lua_tostring(L, -1);
        return v.empty() ? std::string("NULL") : v;
    }
}

/// The AI's copies a creature takes from its record when it's made (retreat and resume levels,
/// status flags, flee range), taken from m.def again: its flags status.
void aiFromRecord(server::ServerObject::Monster& m, decltype(server::ServerObject::Monster::status) status)
{
    m.retreatLevel = m.def->retreatRatio;
    m.resumeLevel = m.def->resumeRatio;
    m.status = status;
    m.fleeRange = m.def->fleeRange;
}

int noxSetCreatureProfile(lua_State* L)
{
    // nox.setCreatureProfile(id, profile, keepAi?): the creature reads `profile` as its monster.bin
    // record in place of its type's (nil: its type's again), wherever the engine reads a creature's
    // record (its strike and death functions, melee and missile numbers, run multiplier, move sound
    // frames, the STATUS bits it reads live, QUESTHEALTH...). `profile` holds monster.bin's keys with
    // their values (a whole record: a key left out is 0 or none, as in the file); `name`, the
    // record's name (its type's by default). The AI's copies a creature takes from its record when
    // it's made (retreat and resume levels, status flags, flee range) are taken from it again, as for
    // one made with it, unless keepAi is true; its health, experience and speed stay (nox.setHealth,
    // nox.setSpeed). Not kept in a saved map.
    toolbox::refuseInsideAsked(L, "nox.setCreatureProfile");
    server::ServerObject& o = creatureArg(L, 1);
    auto& m = *o.monster;
    const bool keepAi = lua_toboolean(L, 3) != 0;
    if (lua_isnoneornil(L, 2)) {
        m.ownDef.reset();
        const server::World& world = toolbox::worldOf(L);
        m.def = world.monsterDb ? world.monsterDb->find(o.typeName) : nullptr;
        if (!keepAi && m.def) {
            aiFromRecord(m, m.def->status);
        }
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    // The record as monster.bin's lines, read by monster.bin's own reader.
    std::string name = o.typeName.str();
    std::string text;
    std::optional<std::uint32_t> statusBits;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "nox.setCreatureProfile: the profile's keys are monster.bin's (and name)");
        }
        const std::string key = lua_tostring(L, -2);
        if (key == "name") {
            if (lua_type(L, -1) != LUA_TSTRING || std::string_view(lua_tostring(L, -1)).find_first_of(" \t\r\n") != std::string_view::npos ||
                lua_objlen(L, -1) == 0 || lua_objlen(L, -1) > 63) {
                luaL_error(L, "nox.setCreatureProfile: name is a word of 1-63 letters");
            }
            name = lua_tostring(L, -1);
        } else {
            const RecordKey* k = recordKey(key);
            if (!k) {
                luaL_error(L, "nox.setCreatureProfile: %s is no key of monster.bin's", key.c_str());
            }
            const std::string v = lineValue(L, *k);
            if (v.find_first_of(" \t\r\n") != std::string::npos || v.find("//") != std::string::npos) {
                luaL_error(L, "nox.setCreatureProfile: %s's value is one word", k->name);
            }
            if (k->kind == KeyKind::Status && lua_isnumber(L, -1)) {
                statusBits = static_cast<std::uint32_t>(lua_tonumber(L, -1));
            }
            text += std::string(k->name) + " " + v + "\n";
        }
        lua_pop(L, 1);
    }
    text = name + "\n" + text + "END\n";
    things::MonsterDb read = things::parseMonsterDb(text, true);
    if (!read.problems.empty() || read.monsters.size() != 1) {
        luaL_error(L, "nox.setCreatureProfile: %s", read.problems.empty() ? "not one record" : read.problems.front().c_str());
    }
    things::MonsterDef& def = read.monsters.front();
    if (!def.missile.empty() && !toolbox::thingsOf(L).findThing(def.missile)) {
        luaL_error(L, "nox.setCreatureProfile: MISSILE_NAME %s is no thing type", def.missile.c_str());
    }
    m.ownDef = std::make_shared<const things::MonsterDef>(std::move(def));
    m.def = m.ownDef.get();
    if (!keepAi) {
        // (Its flags all as given: the record keeps STATUS's 16 bits, as monster.bin's reader does.)
        aiFromRecord(m, statusBits ? *statusBits : m.def->status);
    }
    return 0;
}

using toolbox::setField;

void setString(lua_State* L, const char* key, const std::string& v)
{
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}

int noxCreatureProfile(lua_State* L)
{
    // nox.creatureProfile(id): the monster.bin record the creature reads (its own, else its type's),
    // as nox.setCreatureProfile takes one: its keys, STATUS and MELEE_ATTACK_DAMAGE_TYPE as numbers,
    // the names "" for none, and `name`; nil when it has none.
    const server::ServerObject& o = creatureArg(L, 1);
    const things::MonsterDef* d = o.monster->def;
    if (!d) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 30);
    setString(L, "name", d->name);
    setField(L, "EXPERIENCE", d->experience);
    setField(L, "HEALTH", d->health);
    setField(L, "QUESTHEALTH", d->questHealth);
    setField(L, "SPEED", d->speed);
    setField(L, "RETREAT_RATIO", d->retreatRatio);
    setField(L, "RESUME_RATIO", d->resumeRatio);
    setField(L, "FLEE_RANGE", d->fleeRange);
    setField(L, "STATUS", d->status);
    setField(L, "RUN_MULTIPLIER", d->runMultiplier);
    setField(L, "MOVE_SOUND_FRAME_A", d->moveSoundFrameA);
    setField(L, "MOVE_SOUND_FRAME_B", d->moveSoundFrameB);
    setField(L, "MELEE_ATTACK_FRAME", d->meleeFrame);
    setField(L, "MELEE_ATTACK_RANGE", d->meleeRange);
    setField(L, "MELEE_ATTACK_DAMAGE", d->meleeDamage);
    setField(L, "MELEE_ATTACK_IMPACT", d->meleeImpact);
    setField(L, "MELEE_ATTACK_DAMAGE_TYPE", d->meleeDamageType);
    setField(L, "MELEE_ATTACK_MIN_DELAY", d->meleeMinDelay);
    setField(L, "MELEE_ATTACK_MAX_DELAY", d->meleeMaxDelay);
    setField(L, "MELEE_ATTACK_POISON_CHANCE", d->poisonChance);
    setField(L, "MELEE_ATTACK_POISON_STRENGTH", d->poisonStrength);
    setField(L, "MELEE_ATTACK_POISON_MAX", d->poisonMax);
    setString(L, "MISSILE_NAME", d->missile);
    setField(L, "MISSILE_ATTACK_RANGE", d->missileRange);
    setField(L, "MISSILE_ATTACK_FRAME", d->missileFrame);
    setField(L, "MISSILE_ATTACK_MIN_DELAY", d->missileMinDelay);
    setField(L, "MISSILE_ATTACK_MAX_DELAY", d->missileMaxDelay);
    setString(L, "MELEE_STRIKE_FUNCTION", d->strikeFn);
    setString(L, "DIE_FUNCTION", d->dieFn);
    setString(L, "DEAD_FUNCTION", d->deadFn);
    return 1;
}

// ---- speed, mass, immunities, sound set, colours ----------------------------------------------

int noxSpeed(lua_State* L)
{
    // nox.speed(id): its base speed (a creature's monster.bin SPEED / 32 as it's made, a player's
    // from its class and level) and this frame's.
    const server::ServerObject& o = toolbox::objectArg(L, 1);
    lua_pushnumber(L, o.speedBase);
    lua_pushnumber(L, o.speedCur);
    return 2;
}

int noxSetSpeed(lua_State* L)
{
    // nox.setSpeed(id, base, current?): its base speed (what its speed each frame is worked out
    // from, as the engine does: slowed, hasted, running), and this frame's when given. The engine
    // sets a base again where it always does (a player's level, a creature's spawn).
    toolbox::refuseInsideAsked(L, "nox.setSpeed");
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, (o.classes & (oc::Monster | oc::Player)) != 0, 1, "a creature or player");
    o.speedBase = floatArg(L, 2, 0, 100, "a speed 0-100");
    if (!lua_isnoneornil(L, 3)) {
        o.speedCur = floatArg(L, 3, 0, 100, "a speed 0-100");
    }
    return 0;
}

int noxSetMass(lua_State* L)
{
    // nox.setMass(id, mass): its mass (how hard it is to push: thing.bin's MASS at first), above 0.
    toolbox::refuseInsideAsked(L, "nox.setMass");
    server::ServerObject& o = toolbox::objectArg(L, 1);
    o.mass = floatArg(L, 2, 0.001, 1e6, "a mass above 0, at most 1000000");
    return 0;
}

int noxImmune(lua_State* L)
{
    // nox.immune(id, kind, on?): whether a creature is immune to "poison", "fire" or "electricity"
    // (its SUBCLASS's IMMUNE_POISON, IMMUNE_FIRE, IMMUNE_ELECTRICITY, which the engine reads as a
    // hit lands); given on, sets it so first.
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, (o.classes & oc::Monster) != 0, 1, "a creature");
    const std::string_view kind = luaL_checkstring(L, 2);
    std::uint32_t bit = 0;
    if (kind == "poison") {
        bit = 0x200u;
    } else if (kind == "fire") {
        bit = 0x400u;
    } else if (kind == "electricity") {
        bit = 0x800u;
    }
    luaL_argcheck(L, bit != 0, 2, "\"poison\", \"fire\" or \"electricity\"");
    if (!lua_isnoneornil(L, 3)) {
        toolbox::refuseInsideAsked(L, "nox.immune");
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        o.subclasses = lua_toboolean(L, 3) ? (o.subclasses | bit) : (o.subclasses & ~bit);
    }
    lua_pushboolean(L, (o.subclasses & bit) != 0);
    return 1;
}

int noxSetSoundSet(lua_State* L)
{
    // nox.setSoundSet(id, name): the sound set a creature's cries are played from (soundset.bin's,
    // by its exact name), as an NPC's voice is set; nil: its type's again.
    toolbox::refuseInsideAsked(L, "nox.setSoundSet");
    server::ServerObject& o = creatureArg(L, 1);
    const server::World& world = toolbox::worldOf(L);
    if (lua_isnoneornil(L, 2)) {
        o.monster->soundSet.reset();
        return 0;
    }
    const std::string_view name = luaL_checkstring(L, 2);
    luaL_argcheck(L, !world.soundSets || world.soundSets->find(name), 2, "a sound set of soundset.bin's");
    server::setNpcVoice(world, o, name);
    return 0;
}

int noxSetNpcColors(lua_State* L)
{
    // nox.setNpcColors(id, colors): an NPC's colours (a creature of subclass NPC or FEMALE_NPC, as
    // the clients draw its clothes, hair and skin): colors[1..6] each { r, g, b } (nil: kept), set as
    // the engine sets one, each player who knows it told.
    toolbox::refuseInsideAsked(L, "nox.setNpcColors");
    server::ServerObject& o = creatureArg(L, 1);
    luaL_argcheck(L, (o.subclasses & 0x30u) != 0, 1, "an NPC (subclass NPC or FEMALE_NPC)");
    luaL_checktype(L, 2, LUA_TTABLE);
    for (int i = 1; i <= 6; ++i) {
        lua_rawgeti(L, 2, i);
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                luaL_error(L, "nox.setNpcColors: colour %d is { r, g, b }", i);
            }
            std::array<std::uint8_t, 3> rgb{};
            for (int c = 0; c < 3; ++c) {
                lua_rawgeti(L, -1, c + 1);
                const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
                if (v < 0 || v > 255 || v != std::floor(v)) {
                    luaL_error(L, "nox.setNpcColors: colour %d is { r, g, b }, each 0-255", i);
                }
                rgb[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(v);
                lua_pop(L, 1);
            }
            server::setNpcColor(o, static_cast<std::uint8_t>(i - 1), rgb);
        }
        lua_pop(L, 1);
    }
    return 0;
}

// ---- casting ---------------------------------------------------------------------------------

std::uint32_t spellOf(lua_State* L, int idx)
{
    if (lua_isnumber(L, idx)) {
        const double n = lua_tonumber(L, idx);
        luaL_argcheck(L, n >= 0 && n < 137 && n == std::floor(n), idx, "a spell's number (0-136) or name");
        return static_cast<std::uint32_t>(n);
    }
    const auto id = things::spellId(luaL_checkstring(L, idx));
    luaL_argcheck(L, id.has_value() && *id < 137, idx, "a spell's number (0-136) or name (SPELL_...)");
    return *id;
}

int noxCreatureCast(lua_State* L)
{
    // nox.creatureCast(id, spell, x, y): the creature's AI told to cast a spell (its number 0-136,
    // or name) at a point, as its own CAST_SPELL_ON_LOCATION: pushed on its action stack, cast with
    // its animation and its power; whether it was pushed (a full stack, a dead one: not).
    toolbox::refuseInsideAsked(L, "nox.creatureCast");
    server::ServerObject& o = creatureArg(L, 1);
    const std::uint32_t spell = spellOf(L, 2);
    // (A point on the map, as every point a script gives: one off it would reach the spell's
    // effects, the things it makes and the clients' messages.)
    const float x = coordArg(L, 3);
    const float y = coordArg(L, 4);
    server::Fight fight = toolbox::fightOf(L);
    server::ServerObject::Monster::Item* it = server::Monsters{fight}.push(o, server::MonsterAction::CastSpellOnLocation);
    if (it) {
        it->args[0] = spell;
        it->args[2] = std::bit_cast<std::uint32_t>(x);
        it->args[3] = std::bit_cast<std::uint32_t>(y);
    }
    lua_pushboolean(L, it != nullptr);
    return 1;
}

// The ways a creature casts a spell (its spellbook's bits, the delays' order).
constexpr std::pair<const char*, std::uint32_t> kUses[] = {
    {"reaction", 0x08000000u}, {"defensive", 0x10000000u}, {"disabling", 0x20000000u}, {"offensive", 0x40000000u}, {"escape", 0x80000000u},
};

std::uint32_t useBits(lua_State* L, int idx)
{
    const auto one = [&](std::string_view n) -> std::uint32_t {
        for (const auto& [name, bit] : kUses) {
            if (n == name) {
                return bit;
            }
        }
        luaL_error(L, "nox.setCaster: a spell's ways are \"reaction\", \"defensive\", \"disabling\", \"offensive\", \"escape\"");
        return 0;
    };
    if (lua_type(L, idx) == LUA_TBOOLEAN && !lua_toboolean(L, idx)) {
        return 0;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        return one(lua_tostring(L, idx));
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    std::uint32_t bits = 0;
    for (int i = 1;; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        if (lua_type(L, -1) != LUA_TSTRING) {
            luaL_error(L, "nox.setCaster: a spell's ways are names");
        }
        bits |= one(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return bits;
}

int noxSetCaster(lua_State* L)
{
    // nox.setCaster(id, { spells =, delays =, power =, aim = }): a creature's spellbook and casting
    // rhythm, as its type's own are kept (each field left out kept): spells[name or number] = the
    // ways it casts it ("reaction" against a spell coming at it, "defensive" on itself, "disabling"
    // and "offensive" at an enemy, "escape" to get away; one, a list, or false: not cast);
    // delays[way] = { least, most } frames between such casts (0-65535); power, its spells' level
    // (0-5); aim, its missiles' accuracy (0-1: the lower, the wider).
    toolbox::refuseInsideAsked(L, "nox.setCaster");
    server::ServerObject& o = creatureArg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto& m = *o.monster;
    lua_getfield(L, 2, "spells");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        const int t = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, t) != 0) {
            const std::uint32_t spell = spellOf(L, -2);
            m.spells[spell] = useBits(L, lua_gettop(L));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "delays");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        for (std::size_t u = 0; u < std::size(kUses); ++u) {
            lua_getfield(L, -1, kUses[u].first);
            if (!lua_isnil(L, -1)) {
                if (!lua_istable(L, -1)) {
                    luaL_error(L, "nox.setCaster: delays.%s is { least, most }", kUses[u].first);
                }
                for (int k = 0; k < 2; ++k) {
                    lua_rawgeti(L, -1, k + 1);
                    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
                    if (v < 0 || v > 65535 || v != std::floor(v)) {
                        luaL_error(L, "nox.setCaster: delays.%s is { least, most }, each 0-65535 frames", kUses[u].first);
                    }
                    m.spellDelays[2 * u + static_cast<std::size_t>(k)] = static_cast<std::uint16_t>(v);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "power");
    if (!lua_isnil(L, -1)) {
        const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        if (v < 0 || v > 5 || v != std::floor(v)) {
            luaL_error(L, "nox.setCaster: power is 0-5");
        }
        m.spellPower = static_cast<int>(v);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "aim");
    if (!lua_isnil(L, -1)) {
        const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        if (!(v >= 0 && v <= 1)) {
            luaL_error(L, "nox.setCaster: aim is 0-1");
        }
        m.field330 = toolbox::toFloat(v);
    }
    lua_pop(L, 1);
    return 0;
}

// ---- items ----------------------------------------------------------------------------------

int noxSetDroppable(lua_State* L)
{
    // nox.setDroppable(id, droppable): false: a creature or a player holding it can't let it go
    // (every drop of it by one fails: by hand, a Fumble, its holder's death, a deleted holder's
    // drop), as an item whose DROP does nothing; a chest or a thrown chakram in flight lets it out
    // as any item. true or nil: as the game has it.
    toolbox::refuseInsideAsked(L, "nox.setDroppable");
    server::ServerObject& o = toolbox::objectArg(L, 1);
    luaL_argcheck(L, lua_isnoneornil(L, 2) || lua_isboolean(L, 2), 2, "true, false or nil");
    const bool kept = !lua_isnoneornil(L, 2) && !lua_toboolean(L, 2);
    o.modeMarks = kept ? (o.modeMarks | server::ModeMark::KeptByHolder) : (o.modeMarks & ~server::ModeMark::KeptByHolder);
    return 0;
}

} // namespace

void addCreaturesToolbox(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"setCreatureProfile", noxSetCreatureProfile},
        {"creatureProfile", noxCreatureProfile},
        {"speed", noxSpeed},
        {"setSpeed", noxSetSpeed},
        {"setMass", noxSetMass},
        {"immune", noxImmune},
        {"setSoundSet", noxSetSoundSet},
        {"setNpcColors", noxSetNpcColors},
        {"creatureCast", noxCreatureCast},
        {"setCaster", noxSetCaster},
        {"setDroppable", noxSetDroppable},
    };
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::modes::script
