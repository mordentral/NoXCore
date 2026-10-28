// Items' modifier effects (modifier.bin's ATTACKEFFECT ... DISENGAGEEFFECT: the exe's four effect
// tables, attack, defend, update and engage, things::ModifierEffect), as GAME.EXE's server runs
// them.
//
//   - Worn and taken off (itemApplyEngageEffect / Disengage, from the
//     equip and dequip functions): its enchantments' (slots 2 and 3) ENGAGEEFFECT and
//     DISENGAGEEFFECT. Each sets a bit of its wearer's itemBuffs (taken off: cleared unless
//     another worn thing still gives it, enchantItemTestInventory) and plays its sound at the wearer:
//     Brilliance 8 (0x4B / 0x4C), Speed 0x10 and its number on a player's SpeedBonus, told
//     (MSG_REPORT_STAT_MODIFIER 0x68; 0x3B / 0x3C), FireProtect 1 (0x66 / 0x67), LightningProtect
//     4 (0x6A / 0x6B), PoisonProtect 2 (0x6E / 0x6F), Regeneration 0x20 (0x7B / 0x7C).
//   - The protections (checkFireProtect, checkElectrProtect, getPoisonDmg):
//     the worn things' (and an item's own) FireProtect / LightningProtect / PoisonProtect numbers.
//   - Each frame for each player (the units update's start): its worn things'
//     UPDATEEFFECTs. RegenerationUpdate: a point of health back now and then, a second after its
//     last hurt (a wizard's robe's three times as often); ContinualReplenishmentUpdate: a wand's
//     charge back every so many frames.
//   - A blow (itemApplyAttackEffect: the player's swing, a chakram's or an
//     arrow's hit): the weapon's four slots' ATTACKEFFECT on the blow's damage
//     (DamageMultiplierEffect) or round the attacker (FireRingEffect, BlueFireRingEffect: a
//     Cleansing Flame ring). Then where it lands (playerPreAttackEffects: not a
//     friend, not one INVULNERABLE or with a REFLECTIVE_SHIELD) its enchantments'
//     ATTACKPREHITEFFECT: Stun and Confuse cast at the victim, Fire (damage type 7, a spark
//     burst, 0xE0) and Lightning (type 9, FX 0x81, 0xE1) damage, Recoil pushes it.
//   - The damage it does (DefaultDamage's itemApplyPreDamageEffect, a held
//     weapon's): its four slots' ATTACKPREDAMAGEEFFECT: DrainMana, Vampirism, Poison (not
//     through a raised shield facing the attacker), Sympathy.
//   - A bow's shot (shootApplyEffects): its Recoil enchantment rides on the arrow,
//     its ProjectileSpeed speeds the arrow.
//   - Armour's value (itemApplyDefendEffect): its ARMOR_VALUE by its
//     effectiveness's DEFENDEFFECT.
//   - Spell missiles meeting a player (checkInversionEffect): a worn enchantment's
//     InversionEffect DEFENDCOLLIDEEFFECT turns them as a reflective shield does (none in the
//     shipped modifier.bin).
// The functions that do nothing (Frost, Panic, Readiness's and Replenishment's attack ones,
// Resilience, Breaking, PunctureProne, Parasite, Attraction) aren't here; READINESS is read by
// the attacks (readiness(), items.hpp).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/modifier.hpp"

namespace nox::server {

/// A thing's mana (unitGetOldMana): a player's, a monster's 1000, else none.
int manaOf(const ServerObject& o);
/// A player's mana up (playerManaAdd) or down (playerManaSub): at most its most, not below 0;
/// anything else's, nothing.
void addMana(ServerObject& o, int amount);
/// `item`'s enchantments' ENGAGEEFFECTs, `wearer` putting it on (itemApplyEngageEffect).
void engageEffects(World& world, const things::ModifierDb* modifiers, ServerObject& wearer, const ServerObject& item);
/// `item`'s enchantments' DISENGAGEEFFECTs, `wearer` having taken it off (itemApplyDisengageEffect).
void disengageEffects(World& world, const things::ModifierDb* modifiers, ServerObject& wearer, const ServerObject& item);
/// What `o`'s worn weapons, wands, armour and flags (and its own modifiers, an item's) have of
/// the ENGAGEEFFECT `engage` (FireProtectEngage, LightningProtectEngage, PoisonProtectEngage):
/// their numbers summed (checkFireProtect, checkElectrProtect, getPoisonDmg's first part).
float wornProtection(World& world, const things::ModifierDb* modifiers, const ServerObject& o, things::EffectFn engage);
/// A piece of armour's value (itemApplyDefendEffect): its ARMOR_VALUE, by its effectiveness's DEFENDEFFECT
/// (ArmorMultiplierEffect); 0 for what isn't armour or has no definition.
float armorValueOf(const things::ModifierDb* modifiers, const ServerObject& item);
/// The blow's ATTACKEFFECTs: `weapon`'s four slots', `attacker` dealing `damage`.
void attackEffects(Fight& fight, ServerObject& weapon, ServerObject* attacker, float& damage);
/// The ATTACKPREHITEFFECTs of `weapon`'s enchantments on `victim` (playerPreAttackEffects).
void preHitEffects(Fight& fight, ServerObject& victim, ServerObject* attacker, ServerObject* weapon);
/// The ATTACKPREDAMAGEEFFECTs of `weapon`'s four slots, `damage` from `source` to `victim`
/// (itemApplyPreDamageEffect).
void preDamageEffects(Fight& fight, ServerObject& victim, ServerObject* source, ServerObject& weapon, int damage);
/// A player's worn things' UPDATEEFFECTs for the frame.
void updateEffects(Fight& fight, ServerObject& player);
/// Whether a worn enchantment of `wearer`'s turns spell missiles (checkInversionEffect).
bool inverts(World& world, const things::ModifierDb* modifiers, const ServerObject& wearer);
/// A shot's arrow from `bow` (shootApplyEffects): the bow's RecoilEffect into its last slot, its
/// ProjectileSpeedEffect on its speed.
void shootEffects(const things::ModifierDb* modifiers, const ServerObject& bow, ServerObject& arrow);

} // namespace nox::server
