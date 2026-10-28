// The campaign's NPCs on the server: the NPC object (thing NPC, subclass 0x10,
// NPCXfer) and the female NPCs' colours and voices (subclass 0x20, MonsterXfer).
//
//   - Its colours (six RGB): setNPCColor sets one, owes the
//     players who know it MSG_REPORT_NPC and sets the report's state for the rest (oweNpcReport
//     with the report-kind test's 0x400: a monster of subclass 0x30). NPCXfer reads all six through it,
//     MonsterXfer a female NPC's; a map change by an exit sends an escort's again.
//   - Its voice (the sound set): readNPCVoiceSet by name
//     (getDefaultSoundSet; none of that name: none).
//   - Its worn things (only a monster of subclass 0x10): NPCEquipWeapon /
//     NPCEquipArmor and their dequips equipWeaponNPC and the armour's
//     keep the worn bits (weapon and armour, npcSetItemEquipFlags: the
//     report owed), the weapon held, its armour value
//     (recalculateArmorVal) and the modifiers' engage / disengage effects. A
//     quiver comes off for any weapon but a bow or crossbow, the weapon worn for
//     any but a quiver; a two-handed weapon (0x7FFE40C) takes the shield off, a
//     shield the two-handed weapons; armour of the same subclass comes off.
//     NPCXfer's load wears what the map has worn (ObjFlags 0x100, host only).
//   - MSG_REPORT_NPC (SrvNet_SendReportNPC, 21 bytes): the NPC's code (bit 15 when
//     poisoned), the six colours; then each worn thing's SrvNet_SendReportEquipClass to the same
//     player. Sent from netUpdateObjectSpecial (GameHost's update) when owed.
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/modifier.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace nox::server {

/// The report-kind test with 0x400: a monster of subclass NPC or FEMALE_NPC (0x30).
bool npcReported(const ServerObject& o);

/// The report's state (0x4000000, by the 0x400 kind test) for every player; owed to
/// those who know it; the others' owed cleared unless the state is set.
void oweNpcReport(ServerObject& o, bool state);

/// setNPCColor: colour `index` (0-5) of `o` (needs sync; the report owed).
void setNpcColor(ServerObject& o, std::uint8_t index, const std::array<std::uint8_t, 3>& rgb);

/// readNPCVoiceSet's read: its sound set by name (setNPCVoiceSet:
/// only a monster's).
void setNpcVoice(const World& world, ServerObject& o, std::string_view name);

/// readNPCVoiceSet's write: its sound set's name (monsterGetSoundSet:
/// a monster's; none: "").
std::string npcVoiceName(const World& world, const ServerObject& o);

/// npcSetItemEquipFlags: `item`'s equip bits on (`on`) or off in the NPC's worn
/// weapon or armour bits; the report owed.
void npcSetItemEquipFlags(World& world, ServerObject& npc, const ServerObject& item, bool on);

/// recalculateArmorVal for a monster: its worn armour's values (itemApplyDefendEffect) summed,
/// at most 1, into its armour value.
void npcRecalculateArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc);

/// NPCEquipWeapon: wears a weapon (or wand) it carries; false when it can't.
bool npcEquipWeapon(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item);
/// NPCEquipArmor: wears a piece of armour it carries.
bool npcEquipArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item);
/// equipWeaponNPC: takes a worn weapon off.
bool npcDequipWeapon(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item);
/// The armour's dequip: takes worn armour off.
bool npcDequipArmor(World& world, const things::ModifierDb* modifiers, ServerObject& npc, ServerObject& item);

/// NPCXfer's end on the host (XFerNPC): each thing it carries marked worn (ObjFlags 0x100) has
/// the mark cleared and is worn again (a weapon or wand by NPCEquipWeapon, else armour by NPCEquipArmor),
/// in its inventory's order. first: of the worn ones, a two-handed weapon (0x7FFE40C)
/// and a shield (armour subclass 2) don't go together (the later of them unmarked).
void npcWearLoaded(World& world, const things::ModifierDb* modifiers, ServerObject& npc);

/// playerAttack for an NPC (its melee and missile attacks: mobActionMeleeAtt / mobActionMissileAtt):
/// a frame of the blow by what it holds (its worn bits, the weapon held), its strength, the player's
/// animation of it (World::playerAnimations), from the frame it began (ServerObject::timer);
/// its animation frame kept. Bare-handed a jab either hand (the hand drawn once); a sword,
/// longsword, great sword, war hammer, mace or axe lands at its animation's middle; a bow or
/// crossbow looses its arrow (Missiles::shoot); a staff strikes, a wand is used at the second frame
/// (its USE) and taken off when spent; the Oblivion staves strike. False once it's over.
bool npcAttack(Fight& fight, ServerObject& npc);

/// SrvNet_SendReportNPC's message (21 bytes, MSG_REPORT_NPC 0x69).
std::vector<std::uint8_t> npcReport(const ServerObject& npc);

/// SrvNet_SendReportEquipClass's message (SrvNet_SendReportEquipClass) for a held weapon or armour: 0x50 / 0x51 a
/// weapon (0x11001000), 0x4F / 0x52 armour, its holder's code (bit 15 for a player), its equip
/// bits, its modifiers when it has any; empty for anything else.
std::vector<std::uint8_t> equipClassReport(const ServerObject& holder, const ServerObject& item);

} // namespace nox::server
