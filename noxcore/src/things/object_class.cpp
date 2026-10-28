// Name tables ported from OpenNox libs `object` (MIT, Copyright (c) 2022 NoX World); see
// THIRD_PARTY_NOTICES.txt. Order verified against the GAME.EXE tables named in the header.
#include "noxcore/things/object_class.hpp"

#include "noxcore/things/thing_db.hpp"
#include "thing_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <span>

namespace nox::things {

// Each table's index is the bit number: name i sets 1 << i.
const std::string_view kClassNames[32] = {
    "MISSILE",        "MONSTER",       "PLAYER",        "OBSTACLE",       "FOOD",
    "EXIT",           "KEY",           "DOOR",          "INFO_BOOK",      "TRIGGER",
    "TRANSPORTER",    "HOLE",          "WAND",          "FIRE",           "ELEVATOR",
    "ELEVATOR_SHAFT", "DANGEROUS",     "MONSTERGENERATOR", "READABLE",    "LIGHT",
    "SIMPLE",         "COMPLEX",       "IMMOBILE",      "VISIBLE_ENABLE", "WEAPON",
    "ARMOR",          "NOT_STACKABLE", "TREASURE",      "FLAG",           "CLIENT_PERSIST",
    "CLIENT_PREDICT", "PICKUP",
};

namespace {

constexpr std::string_view kArmorNames[] = {
    "HELMET", "SHIELD", "BREASTPLATE", "ARM_ARMOR", "PANTS", "BOOTS", "SHIRT", "LEG_ARMOR", "BACK",
};

constexpr std::string_view kWeaponNames[] = {
    "FLAG",
    "QUIVER",
    "BOW",
    "CROSSBOW",
    "ARROW",
    "BOLT",
    "CHAKRAM",
    "SHURIKEN",
    "SWORD",
    "LONG_SWORD",
    "GREAT_SWORD",
    "MACE",
    "AXE",
    "OGRE_AXE",
    "HAMMER",
    "STAFF",
    "STAFF_SULPHOROUS_FLARE",
    "STAFF_SULPHOROUS_SHOWER",
    "STAFF_LIGHTNING",
    "STAFF_FIREBALL",
    "STAFF_TRIPLE_FIREBALL",
    "STAFF_FORCE_OF_NATURE",
    "STAFF_DEATH_RAY",
    "STAFF_OBLIVION_HALBERD",
    "STAFF_OBLIVION_HEART",
    "STAFF_OBLIVION_WIERDLING",
    "STAFF_OBLIVION_ORB",
};

constexpr std::string_view kMonsterNames[] = {
    "SMALL_MONSTER",   "MEDIUM_MONSTER", "LARGE_MONSTER", "SHOPKEEPER",     "NPC",
    "FEMALE_NPC",      "UNDEAD",         "MONITOR",       "MIGRATE",        "IMMUNE_POISON",
    "IMMUNE_FIRE",     "IMMUNE_ELECTRICITY", "IMMUNE_FEAR", "BOMBER",       "NO_TARGET",
    "NO_SPELL_TARGET", "HAS_SOUL",       "WARCRY_STUN",   "LOOK_AROUND",    "WOUNDED_NPC",
};

constexpr std::string_view kFoodNames[] = {
    "SIMPLE",
    "APPLE",
    "JUG",
    "POTION",
    "HEALTH_POTION",
    "MANA_POTION",
    "CURE_POISON_POTION",
    "MUSHROOM",
    "HASTE_POTION",
    "INVISIBILITY_POTION",
    "SHIELD_POTION",
    "FIRE_PROTECT_POTION",
    "SHOCK_PROTECT_POTION",
    "POISON_PROTECT_POTION",
    "INVULNERABILITY_POTION",
    "INFRAVISION_POTION",
    "VAMPIRISM_POTION",
};

constexpr std::string_view kOtherNames[] = {
    "HEAVY",    "LAVA",     "GATE",     "VISIBLE_OBELISK", "INVISIBLE_OBELISK", "TECH",      "LOTD",
    "USEABLE",  "CHEST_NW", "CHEST_NE", "CHEST_SE",        "CHEST_SW",          "STONE_DOOR",
};

constexpr std::string_view kBookNames[] = {"SPELL_BOOK", "FIELD_GUIDE", "ABILITY_BOOK"};
constexpr std::string_view kMissileNames[] = {"MISSILE_COUNTERSPELL", "MAGIC"};
constexpr std::string_view kGeneratorNames[] = {"GENERATOR_NW", "GENERATOR_NE", "GENERATOR_SE", "GENERATOR_SW"};
constexpr std::string_view kExitNames[] = {"QUEST_EXIT", "QUEST_WARP_EXIT"};

} // namespace

// Searched in this order, as the game does.
const std::span<const std::string_view> kSubclassTables[9] = {
    kArmorNames, kWeaponNames, kMonsterNames, kFoodNames,      kOtherNames,
    kBookNames,  kMissileNames, kGeneratorNames, kExitNames,
};

namespace {

using detail::equalsNoCase;
using detail::trim;

/// Calls f(token) for each '+'-separated name, unless the value starts with "NULL".
template <class F>
void forEachName(std::string_view value, F f)
{
    value = trim(value);
    if (value.size() >= 4 && equalsNoCase(value.substr(0, 4), "NULL")) {
        return;
    }
    while (!value.empty()) {
        const auto plus = value.find('+');
        const std::string_view token = trim(value.substr(0, plus));
        if (!token.empty()) {
            f(token);
        }
        if (plus == std::string_view::npos) {
            break;
        }
        value.remove_prefix(plus + 1);
    }
}

// Sets the bit of `name` in `table`; false when the table doesn't have it.
bool setBit(std::string_view name, std::span<const std::string_view> table, std::uint32_t& bits)
{
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (equalsNoCase(table[i], name)) {
            bits |= 1u << i;
            return true;
        }
    }
    return false;
}

} // namespace

std::uint32_t parseClassSet(std::string_view value)
{
    std::uint32_t bits = 0;
    forEachName(value, [&](std::string_view name) { setBit(name, kClassNames, bits); });
    return bits;
}

// The object flags' names (ObjFlag), a bit each.
const std::string_view kFlagNames[32] = {
    "BELOW", "NO_UPDATE", "ACTIVE", "ALLOW_OVERLAP", "SHORT", "DESTROYED", "NO_COLLIDE", "MISSILE_HIT",
    "EQUIPPED", "PARTITIONED", "NO_COLLIDE_OWNER", "OWNER_VISIBLE", "EDIT_VISIBLE", "NO_PUSH_CHARACTERS", "AIRBORNE", "DEAD",
    "SHADOW", "FALLING", "IN_HOLE", "RESPAWN", "ON_OBJECT", "SIGHT_DESTROY", "TRANSIENT", "BOUNCY",
    "ENABLED", "PENDING", "TRANSLUCENT", "STILL", "NO_AUTO_DROP", "FLICKER", "SELECTED", "MARKED"};

namespace {

std::uint32_t readClasses(const ThingDef& thing)
{
    const auto v = thing.attribute("CLASS");
    return v ? parseClassSet(*v) : 0;
}

std::uint32_t readSubclasses(const ThingDef& thing)
{
    const auto v = thing.attribute("SUBCLASS");
    return v ? parseSubclassSet(*v) : 0;
}

std::uint32_t readFlags(const ThingDef& thing)
{
    std::uint32_t flags = 0;
    if (const auto f = thing.lastAttribute("FLAGS")) {
        std::string_view rest = *f;
        while (!rest.empty()) {
            const auto end = rest.find_first_of("+ \t");
            const std::string_view word = rest.substr(0, end);
            for (std::size_t i = 0; i < std::size(kFlagNames); ++i) {
                if (word == kFlagNames[i]) {
                    flags |= 1u << i;
                }
            }
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
    }
    return flags;
}

} // namespace

ThingDef::Bits detail::readBits(const ThingDef& thing)
{
    return {readClasses(thing), readSubclasses(thing), readFlags(thing)};
}

// (From the bits reindex kept, or read from the attributes as they stand.)
std::uint32_t thingClasses(const ThingDef& thing)
{
    return thing.bits ? thing.bits->classes : readClasses(thing);
}

std::uint32_t thingSubclasses(const ThingDef& thing)
{
    return thing.bits ? thing.bits->subclasses : readSubclasses(thing);
}

std::uint32_t thingFlags(const ThingDef& thing)
{
    return thing.bits ? thing.bits->flags : readFlags(thing);
}

std::uint32_t parseSubclassSet(std::string_view value)
{
    std::uint32_t bits = 0;
    forEachName(value, [&](std::string_view name) {
        for (auto table : kSubclassTables) {
            if (setBit(name, table, bits)) {
                break;
            }
        }
    });
    return bits;
}

} // namespace nox::things
