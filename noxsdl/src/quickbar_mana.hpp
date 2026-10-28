// The spell bar's "short of mana" darkening (quickBarWarriorDraw): a slot's top is darkened by
// (cost - mana) x 30 / cost of its 30-pixel height while the client's mana (cliGetMana: the
// server's last MSG_REPORT_MANA) is below the cost and the cost isn't 0.
//
// The cost is spellManaCost's ordinary mode: the spell's byte in thing.bin's spell list. The
// summonings (the creature guide's spells) are 0 there, since the server charges each from its
// own per-creature price table, so in GAME.EXE a summon's slot never darkens for want of mana
// (OB-223). Ours reads that same per-creature table (things::summonManaCost, the one the server
// charges from) for them, so a summon's slot darkens and refills like any other spell's (VD-34).
// The trap tray takes each slot's ordinary cost too, from what's left once the slots before it
// are paid at their trap prices (spellManaCost's trap mode: the EnergyBoltTrapCost,
// LightningTrapCost and ManaBombTrapCost gamedata values for those three). Of the summonings only
// Summon Bomber lacks thing.bin's no-trap flag (0x400000, client::canTrap); in a trap the server
// charges it its summon price as well, and the tray reads that same price for it (VD-34).
// Nothing here needs SDL.
#pragma once

#include "noxcore/things/game_data.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include <cstdint>

namespace noxsdl {

/// The cost a spell-bar slot is darkened by: its thing.bin cost (spellManaCost's ordinary mode),
/// a summoning's per-creature price (VD-34), 0 for an unknown spell.
inline int slotManaCost(const nox::things::ThingDb& things, std::uint32_t spell)
{
    if (nox::things::isSummonSpell(spell)) {
        return nox::things::summonManaCost(spell);
    }
    const nox::things::SpellDef* def = things.findSpell(spell);
    return def ? def->manaCost : 0;
}

/// What a spell costs in a trap (spellManaCost's trap mode): Energy Bolt (0x18), Lightning
/// (0x2B) and Mana Bomb (0x38) their gamedata trap prices, the rest their ordinary cost. The
/// prices as a game of `gameFlags` reads gamedata.bin (its SOLO lines in a solo game, ARENA ones
/// online: the game's own figures).
inline int trapManaCost(const nox::things::ThingDb& things, const nox::things::GameData* data, std::uint32_t spell, std::uint32_t gameFlags)
{
    const char* table = spell == 0x18 ? "EnergyBoltTrapCost" : spell == 0x2B ? "LightningTrapCost"
                      : spell == 0x38 ? "ManaBombTrapCost"
                                      : nullptr;
    if (table && data) {
        return static_cast<int>(nox::things::gameNumber(data, table, gameFlags));
    }
    return slotManaCost(things, spell);
}

/// A warrior ability's recharge time in frames (gamedata.bin's BerserkerChargeDelay ...
/// EyeOfTheWolfDelay, abilities 1-5), as a game of `gameFlags` reads it; 0 for another.
inline int abilityDelayFrames(const nox::things::GameData* data, std::uint32_t ability, std::uint32_t gameFlags)
{
    static constexpr const char* kDelays[] = {"", "BerserkerChargeDelay", "WarcryDelay", "HarpoonDelay", "TreadLightlyDelay", "EyeOfTheWolfDelay"};
    if (!data || ability == 0 || ability >= std::size(kDelays)) {
        return 0;
    }
    return static_cast<int>(nox::things::gameNumber(data, kDelays[ability], gameFlags));
}

/// The height of a slot's darkened top for `cost` at `mana`: 0 while it's affordable (or free).
inline int manaShadeHeight(int cost, int mana)
{
    if (mana < cost && cost != 0) {
        return (cost - mana) * 30 / cost;
    }
    return 0;
}

} // namespace noxsdl
