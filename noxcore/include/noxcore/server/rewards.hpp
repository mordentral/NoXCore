// The quest's rewards (GAME.EXE's reward generator): what a RewardMarker
// becomes.
//   - server_rewardgen_activateMarker: a marker (a RewardMarkerPlus two stages better)
//     gives by its odds (1-4, a roll of 0-100 at most 75 / 50 / 25 / 5) one of its kinds
//     (weighted by its table: spell book 16, ability book 2, field guide 2, weapon 24, armour 16,
//     gold or a gem 23, potion 16, kind 0x80 1 (gold or a gem too)).
//   - The stage's slot (server_rewardGen_pickRandomSlots: 1 at stage 0, 0x10 from 10;
//     between, one of five bits by fixed odds on a roll of 0-200 halved) picks from the item
//     table (weapons, armour, potions by weight and slots) and the spell and guide
//     tables; a marker's own lists (1 spells, 2 abilities, 4 guides)
//     instead when it has them.
//   - A weapon or armour's modifiers (rewardMakeWeapon / rewardMakeArmor): how
//     many by the slot (2: 0-1, 4: 0-2, 8: 1-3, 0x10: 2-4), which of power or quality, material,
//     enchantment and second enchantment by a roll, each from its table by the slot, its
//     ALLOWED_WEAPONS / ALLOWED_ARMOR and ALLOWED_POSITION;
//     a staff only Replenishment1, when its table entry says so ('#').
//   - Gold or a gem (createGem): a QuestGoldChest or QuestGoldPile with gold by the
//     slot (100-250 ... 1600-4000), from slot 4 one time in ten a Ruby, Emerald or Diamond.
//   - A map's (at a quest map's load): one marker of kind 0x80 made an Ankh;
//     of the plain markers not always placed a share by the players (0.4 for 1-2,
//     0.7 for 3-4, all for 5-6; half on stage 1) chosen, and of the red potions the same share
//     kept; each chosen marker's item made where it was (a bow or crossbow with a
//     Quiver within 30), each chest's markers its items a stage better; the markers gone.
#pragma once

#include "noxcore/server/combat.hpp"

#include <cstdint>

namespace nox::server {

/// server_rewardgen_activateMarker: the item `marker` (a RewardMarker or
/// RewardMarkerPlus) gives at `stage`, made at (x, y); none when it gives nothing.
ServerObject* activateRewardMarker(Fight& fight, const ServerObject& marker, std::uint32_t stage, float x, float y);
/// (a quest map's load): the map's rewards.
void questRewardPass(Fight& fight);
/// A boss's loot (spawnHecubahQuest / spawnNecroQuest): a fresh
/// RewardMarker (every kind, always) activated `count` times at `stage`, each item into `unit`.
void giveRewards(Fight& fight, ServerObject& unit, std::uint32_t stage, int count);

/// a spell a quest death may take (in the reward spell table with slots; not
/// Fireball, Charm, Lesser Heal, 0x22 or 0).
bool questSpellLosable(int spell);
/// a field guide creature a quest death may take (in the reward guide table).
bool questGuideLosable(int creature);

// What a quest's .plr may bring (the checks as cliPlrInfoLoadFromFile's sections
// read a quest player's file: anything else fails the load).
/// spell: a spell the reward tables give (with slots), 0x2E-0x31, 0x7A-0x7D or a
/// summoning (0x4B-0x72).
bool questSpellValid(int spell);
/// a field guide creature the reward tables give, or one of the spiders.
bool questGuideValid(int creature);
/// not a key; food only of subclass 0x1FF78; a book of a spell or creature the reward
/// tables give (an ability book's 1-5); the gems; the starting wand (only Replenishment1) and
/// street clothes (only their colours); else an item of the item table, a weapon's or
/// armour's modifiers each from its table, allowed on it and in its position
/// (a Replenishment only on a '#' staff).
bool questItemValid(const ServerObject& item, const things::ModifierDb* modifiers);
/// A weapon's or armour's modifiers as questItemValid checks them: its effectiveness, material and
/// enchantments each in their table, allowed on its kind (ALLOWED_WEAPONS / ALLOWED_ARMOR, not the
/// row's exclusions), the enchantments in their position (ALLOWED_POSITION); a Replenishment only on
/// a '#' staff. (Also what a mode's items are held to: no item the game never makes.)
bool questModifiersValid(const ServerObject& item, const things::ModifierDb& db);

} // namespace nox::server
