// The field guide: the creatures the spellbook's guide tab describes (loadGuides
// and its name table). Creature n (1-40) is titled "creature:<name>", described by
// "creature_desc:<name>", pictured by the image Spellbook<name> (CreatureCage<name> in the summon
// panel), and summoned by spell n + 74 (SPELL_SUMMON_BAT is 75). Its size, from the thing's
// SUBCLASS, sets where the picture goes. MSG_REPORT_GUIDE_AWARD gives one creature by number.
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace nox::things {

inline constexpr int kGuideCreatures = 40;

/// The guide's thing names, creature n at [n - 1].
inline constexpr std::array<std::string_view, kGuideCreatures> kGuideNames{
    "Bat",          "BlackBear",       "Bear",        "Beholder",       "Bomber",       "CarnivorousPlant",
    "AlbinoSpider", "SmallAlbinoSpider", "EvilCherub", "EmberDemon",    "Ghost",        "GiantLeech",
    "Imp",          "FlyingGolem",     "MechanicalGolem", "Mimic",      "GruntAxe",     "OgreBrute",
    "OgreWarlord",  "Scorpion",        "Shade",       "Skeleton",       "SkeletonLord", "Spider",
    "SmallSpider",  "SpittingSpider",  "StoneGolem",  "Troll",          "Urchin",       "Wasp",
    "WillOWisp",    "Wolf",            "BlackWolf",   "WhiteWolf",      "Zombie",       "VileZombie",
    "Demon",        "Lich",            "WizardGreen", "UrchinShaman"};

/// The summon spell for creature n.
inline constexpr std::uint32_t guideSummonSpell(int creature) { return static_cast<std::uint32_t>(creature) + 74; }

/// Whether creature n is in the guide at all (the exe drops the Bomber: its thing id slot is 0).
inline constexpr bool guideListed(int creature) { return creature >= 1 && creature <= kGuideCreatures && creature != 5; }

/// Whether a game shows creature n (bookCreatureTest): the last four only in quests.
inline constexpr bool guideShown(int creature, bool quest) { return quest || creature < 37; }

/// The creatures learning `creature` also teaches (g_guideCreatureGroups): the spiders.
std::span<const int> guideGroup(int creature);

/// The picture's size class (loadGuides from the thing's SUBCLASS): 1 small, 2 medium, 4 large.
int guideSize(const ThingDb& things, int creature);

} // namespace nox::things
