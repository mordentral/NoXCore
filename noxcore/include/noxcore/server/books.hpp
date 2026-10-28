// Books, signs and spell pedestals on the server (the campaign's).
//
//   - Spell books and scrolls (SpellRewardUse): a wizard's or conjurer's whose class may
//     have the spell (playerCheckSpellClass: flagged for everyone, 0x1000000, or
//     its class, 0x2000000 / 0x4000000) learns it (Spell_GrantToPlayer, told; shown, 0x80, in a
//     solo or quest game when it's new) and the book goes; at its most already 0x39D, the book
//     kept; anyone else "use.c:SpellRewardClassFail" and 0x39D.
//   - Ability books (AbilityRewardUse): a warrior learns it (Abil_GrantToPlayer, shown in
//     a solo or quest game when new) and the book goes; known, 0x39D; anyone else
//     "pickup.c:ObjectEquipClassFail" and 0x39D.
//   - Field guides (FieldGuideUse): the creature by its name (Game_GetCreatureGuideIndex:
//     0 unknown) learned (Plr_AwardFieldGuide, told) and the guide gone; known,
//     "objcoll.c:AlreadyHaveGuide"; in a quest only a conjurer's ("pickup.c:ObjectEquipClassFail").
//   - In a solo or quest game a book or guide is used as it's picked up (SpellBookPickup /
//     AbilityBookPickup); used up, that's all; else picked up (0x33A, a spell book's
//     subclass 1; 0x33C the others').
//   - Signs (SignCollide runs the sign's USE for a player touching or working it):
//     ReadUse its text as a sign's line, WarpReadUse a quest's warp sign
//     ("GeneralPrint:WarpClosed", or MSG_INFORM 0x15 with the stage the warp leads to); at most
//     every 3 s a sign, when nothing stands between (mapCheck: sight.hpp's
//     bodiesClear).
//   - Spell pedestals (AwardSpellCollide): what touches one is granted its spell
//     (SpellPagePedestalXfer's, told).
//   - The Oblivion staves (OblivionPickup): picked up by a player as weapons are, then its
//     line ("weapon.c:Pickup...Oblivion") and sound, the level-up moment's OblivionUp (solo.hpp)
//     and worn (Plr_TryEquip).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/world.hpp"

#include <cstdint>
#include <string_view>

namespace nox::server {

/// SpellBookPickup / AbilityBookPickup: the item's PICKUP for a book or guide.
bool bookPickup(Items& items, ServerObject& who, ServerObject& book, bool everyone);
/// The USEs of books, guides and signs (Obj_UseItem's call): true when `item`'s is one of them.
bool useBook(Items& items, ServerObject& user, ServerObject& item);
/// ReadUse / WarpReadUse (a sign's USE, as SignCollide calls it).
void readSign(World& world, std::uint32_t frame, ServerObject& user, ServerObject& sign);
/// OblivionPickup's part after the weapon's pickup.
void oblivionPickedUp(Items& items, ServerObject& who, ServerObject& item);
/// AwardSpellCollide.
void awardSpellCollide(Fight& fight, ServerObject& pedestal, ServerObject* other);
/// Game_GetCreatureGuideIndex: a guide creature by its name (case counts; 0 unknown).
int guideCreatureIndex(std::string_view name);

} // namespace nox::server
