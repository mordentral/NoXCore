// Enchantments on units (the exe's buffs, testUnitBuffs-updateUnitBuffs; the bits in world.hpp's Enchant).
//
//   - A unit has 32: a bit each (Buffs), the frames left (0: for good) and a power (1-5).
//   - Put on (buffApplyTo): not on the dead, destroyed or frozen-out (flags
//     0x8022); not over one that's on for good; anything but invisibility takes invisibility
//     off first. Hecubah never takes ANTI_MAGIC; in a quest she and the Necromancer shrug off
//     confusion; fear-proof monsters (subclass 0x1000) take no fear outside a solo game.
//   - Taken off (spellBuffOff); all of them (unitClearBuffs).
//   - Each frame (updateUnitBuffs, the units update's last pass): every timed
//     one counts down; at 0 it goes, ETHEREAL taking its NO_COLLIDE with it, DEATH killing the
//     unit (no killer, type 0xD). HASTED x1.25 its speed (after SLOWED's x0.5, physics.cpp).
//   - Its clients told (setUnitBuffFlags -> MSG_REPORT_ENCHANTMENT 0x5A, the
//     unit and its bits): a player's (or an always-sent object's, classes 0x20400004) change is
//     owed to every player; anything else's to those who know it, and to the others when they
//     first see it, if it has any. The host sends what's owed with the unit's update.
// Sounds: put on and off (the spell's sound 1 / 2), DEATH's tick (0x1A) and end (0x30B),
// Hecubah's and the Necromancer's refusals (0x246 / 0x253).
// Not yet: the elimination death and the lesson DEATH's end gives a player.
#pragma once

#include "noxcore/server/world.hpp"

#include <cstdint>
#include <string_view>

namespace nox::server {

struct Fight;

/// The enchantments' names by bit (as the saves write them).
inline constexpr std::string_view kEnchantNames[32] = {
    "ENCHANT_INVISIBLE",         "ENCHANT_MOONGLOW",           "ENCHANT_BLINDED",
    "ENCHANT_CONFUSED",          "ENCHANT_SLOWED",             "ENCHANT_HELD",
    "ENCHANT_DETECTING",         "ENCHANT_ETHEREAL",           "ENCHANT_RUN",
    "ENCHANT_HASTED",            "ENCHANT_VILLAIN",            "ENCHANT_AFRAID",
    "ENCHANT_BURNING",           "ENCHANT_VAMPIRISM",          "ENCHANT_ANCHORED",
    "ENCHANT_LIGHT",             "ENCHANT_DEATH",              "ENCHANT_PROTECT_FROM_FIRE",
    "ENCHANT_PROTECT_FROM_POISON", "ENCHANT_PROTECT_FROM_MAGIC", "ENCHANT_PROTECT_FROM_ELECTRICITY",
    "ENCHANT_INFRAVISION",       "ENCHANT_SHOCK",              "ENCHANT_INVULNERABLE",
    "ENCHANT_TELEKINESIS",       "ENCHANT_FREEZE",             "ENCHANT_SHIELD",
    "ENCHANT_REFLECTIVE_SHIELD", "ENCHANT_CHARMING",           "ENCHANT_ANTI_MAGIC",
    "ENCHANT_CROWN",             "ENCHANT_SNEAK"};
/// The enchantments a save keeps, in the order it writes them (a player's and a monster's alike:
/// 0x1D of them, not CHARMING, CROWN or SNEAK).
inline constexpr int kEnchantOrder[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                        16, 17, 18, 19, 20, 21, 22, 24, 25, 27, 29, 26, 23, 0};

inline bool enchanted(const ServerObject& o, int e) noexcept { return (o.buffs >> (e & 31)) & 1u; }
inline int enchantPower(const ServerObject& o, int e) noexcept { return o.buffPower[static_cast<std::size_t>(e & 31)]; }
inline int enchantTime(const ServerObject& o, int e) noexcept { return o.buffTime[static_cast<std::size_t>(e & 31)]; }

/// Its enchantment bits changed (setUnitBuffFlags): the clients owed a report.
void setEnchantments(ServerObject& o, std::uint32_t bits);
/// Puts enchantment `e` on for `frames` (0: for good) at `power` (buffApplyTo),
/// with its spell's "on" sound.
void enchant(World& world, ServerObject& o, int e, int frames, int power);
/// Takes enchantment `e` off (spellBuffOff), with its spell's "off" sound (but
/// Death's and the crown's).
void disenchant(World& world, ServerObject& o, int e);
/// Takes them all off (unitClearBuffs).
void clearEnchantments(ServerObject& o);
/// The frame's count-down (updateUnitBuffs).
void stepEnchantments(ServerObject& o, Fight& fight);

} // namespace nox::server
