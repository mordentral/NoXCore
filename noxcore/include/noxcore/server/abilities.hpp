// A warrior's abilities (Server/Ability: Abil_TryExecute and its kin).
//
//   - Asked (MSG_TRY_ABILITY): a live warrior's, 1-5; not Berserker Charge carrying a CTF flag
//     (INFORM 2: 5); War Cry not while charging, Berserker Charge and Harpoon not while War
//     Cry's shout is under way (nor Harpoon while charging), and none twice at once (2); not
//     jumping or (out of the campaign) in the air (6); known (a network game but a quest knows
//     them all, unless the book is the mode's (World::modeBook); 3); Berserker Charge not carrying too much (7); not cooling down (2). Then its
//     cool-down (its *Delay frames; MSG_REPORT_ABILITY_STATE 0xCE: not ready), its record for
//     its *Duration frames (if any), its work, its sound. Refused: MSG_INFORM 2 and the "can't"
//     sound.
//   - Each frame (after the spells): the cool-downs count down (ready: 0xCE again);
//     a record whose warrior is dead or gone is dropped; one past its end ends (Berserker
//     Charge: the warrior idle), MSG_REPORT_ACTIVE_ABILITIES (0xCF) off.
//   - Berserker Charge (1, warriorBerserker): channelled spells end, a confused warrior's way wanders
//     (+-8 x 4), the attack state; INVISIBLE and INVULNERABLE off, an oval shield too. The
//     charge (playerAttack): the BERSERKER_CHARGE animation round and round, pushed along its facing
//     at 6 x its base speed, till frozen or held; what it meets (its collide, collide.hpp)
//     ends it.
//   - War Cry (2, warriorWarcry): the attack state; spells being spoken within 300 by the other
//     side cut off (Spell_WarcryInterruptCasting: INFORM 0 15, idle); Nullify (power 5) on each enemy caster in
//     300 in sight (players but warriors; monsters that cast); INVISIBLE and INVULNERABLE off.
//     At the shout (WARCRY's frame 3): the ground shakes (MSG_FX_JIGGLE 15 to those within
//     300), monsters that fear it (subclass 0x20000) within 300 held 3 s, a counterspell
//     about it (power 5); then its flag's done.
//   - Harpoon (3): the throw (state 0x20, then 0x21 (anim 0x36's second half)) and
//     the HarpoonBolt (missiles.hpp).
//   - Tread Lightly (4): a runner stops; SNEAK for TreadLightlyDuration at its level.
//   - Eye of the Wolf (5): INFRAVISION for EyeOfTheWolfDuration at its level.
// Sounds: each ability's sound (start, end), the "can't" (0xE7), the charge's crash
// (0xAB), the harpoon's (999, 0x3E6).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"

#include <cstdint>

namespace nox::server {

/// One of `unit`'s abilities under way (playerIsAbilityActive).
bool abilityActive(const World& world, const ServerObject& unit, int ability);
/// Its record's flag (probablyWarcryCheck: 0 if none).
int abilityFlag(const World& world, const ServerObject& unit, int ability);
/// The ability ends early: Harpoon's rope cut (0x9E 14), Tread Lightly's SNEAK
/// off, (Eye of the Wolf can't); MSG_REPORT_ACTIVE_ABILITIES off; its record gone.
void endAbility(World& world, ServerObject& unit, int ability);
/// All its abilities stop and cool down no more (playerCancelAbils: at death;
/// MSG_RESET_ABILITIES 6).
void cancelAbilities(World& world, ServerObject& unit);
/// a warrior's `ability` ready again at once (MSG_RESET_ABILITIES {ability}), its
/// records of it dropped (told off). A kill by Berserker Charge (the victim's last blow of damage
/// type 2 noted as such: a monster's out of a solo game, a player's in a network game).
void resetAbility(World& world, ServerObject& unit, int ability);
/// Whether `p` may use `ability` (Abil_TryExecute's "known"): in a network game that isn't a
/// quest every one; otherwise, or when the book is the mode's (World::modeBook), only one whose
/// granted level (PlayerControl::abilityLevel) isn't 0. Asked on a use only. An id outside 1-5
/// (PlayerControl::abilityLevels' size) is never known: abilities past the stock five need a
/// table of their own for every player (a warrior's five share the spell book's slots 1-5).
bool abilityKnown(const World& world, const PlayerControl& p, int ability);
/// A harpoon let go: the bolt gone, the ability ended. `broken`: it snapped
/// (harpoonBreakForPlr), with 0x3E6.
void releaseHarpoon(World& world, ServerObject& warrior, bool broken);

struct Abilities {
    Fight& fight; ///< its world, frame and the rest
    const things::ThingDb& things;

    /// MSG_TRY_ABILITY (Abil_TryExecute).
    void tryExecute(ServerObject& unit, int ability);
    /// The frame's cool-downs and endings.
    void update();
    /// A charge's or a shout's frame in the attack state (playerAttack's): whether it goes on;
    /// nullopt when neither is under way.
    std::optional<bool> attackFrame(ServerObject& unit, std::uint8_t previousFrame, int frames, int delay);

private:
    void run(ServerObject& unit, int ability);
    void tell(const ServerObject& unit, std::vector<std::uint8_t> bytes);
};

/// MSG_REPORT_ACTIVE_ABILITIES (0xCF): the ability on or off for its warrior's client.
void reportActiveAbility(World& world, const ServerObject& unit, int ability, bool active);

} // namespace nox::server
