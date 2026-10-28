// The spell bars as the client keeps them (guispell.c): five sets of five slots, the one in use,
// and the conjurer's / wizard's three trap sets of three. A slot holds a spell id (a warrior's: an
// ability id) and a flag, bit 0 of which turns the spell on its caster (the slot's lit nugget,
// MSG_TRY_SPELL's target byte). They live in the .plr's Gui section (plr_rwGUI,
// spells and abilities saved by name, plr_rwBarSlots); the spellbook fills them.
#pragma once

#include "noxcore/player/player_file.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace nox::client {

struct BarSlot {
    std::uint32_t id = 0; ///< spell id (abilities 1-5 for a warrior), 0 if empty
    bool target = false;  ///< cast on ourselves (flag bit 0)

    bool operator==(const BarSlot&) const = default;
};

using BarRow = std::array<BarSlot, 5>;

/// noxcpp's multiclass, a mode's (HeadlessClient::multiclass): a book and bars holding spells
/// and abilities together, an ability's id there carrying this tag (a warrior's bars hold bare
/// ability ids as the game's do; a caster's bare spell ids).
inline constexpr std::uint32_t kAbilityTag = 0x10000;
inline constexpr bool isTaggedAbility(std::uint32_t id) { return (id & kAbilityTag) != 0; }
inline constexpr std::uint32_t untagged(std::uint32_t id) { return id & 0xFFFF; }

/// The ability names, by id; 0 is ABILITY_INVALID.
inline constexpr std::array<const char*, 6> kAbilityNames{"ABILITY_INVALID",       "ABILITY_BERSERKER_CHARGE",
                                                          "ABILITY_WARCRY",        "ABILITY_HARPOON",
                                                          "ABILITY_TREAD_LIGHTLY", "ABILITY_EYE_OF_THE_WOLF"};

/// A spell's SPEL flags (0 if unknown). Bits the bars use: 0x600 a spell that goes on its caster
/// (the slot starts lit: 0x200 the heals and wards, 0x400 those with no creature target, such as
/// the summons, marks and teleports), 0x200400 one that can't be turned (its nugget refuses:
/// the aimed attack spells' bit and the untargeted ones').
std::uint32_t spellFlags(const things::ThingDb& things, std::uint32_t spell);

/// SPELL_GLYPH: the trap (a conjurer's: the bomber) a trap set's spells are cast into.
inline constexpr std::uint32_t kGlyph = 0x22;

/// Whether `spell` may go in a trap (spellCanUseInTrap): not one with flag
/// 0x400000, and in a quest not Blink.
bool canTrap(const things::ThingDb& things, std::uint32_t spell, bool quest);

struct SpellBars {
    std::array<BarRow, 5> rows{};
    std::uint8_t row = 0; ///< the set in use (0-4)
    std::array<std::array<BarSlot, 3>, 3> traps{};
    std::uint8_t trapRow = 0;
    std::uint8_t summonOrder = 4; ///< kept for the .plr (the summon panel's default order)

    BarRow& current() { return rows[row]; }
    const BarRow& current() const { return rows[row]; }
    std::array<BarSlot, 3>& currentTrap() { return traps[trapRow]; }
    const std::array<BarSlot, 3>& currentTrap() const { return traps[trapRow]; }
    /// What MSG_TRY_SPELL builds the trap or bomber of the trap set in use with
    /// (client_buildTrap): its spells in order, then SPELL_GLYPH; empty if it has
    /// none.
    std::vector<std::uint32_t> trapCast() const;

    /// Whether any set holds `id` (buttonHaveSpellInBarMB).
    bool contains(std::uint32_t id) const;
    /// The first empty slot, from the set in use on round all five (buttonFindFirstEmptySlot), as (set, slot).
    std::optional<std::pair<int, int>> firstEmpty() const;
    /// The first empty slot in the set in use.
    std::optional<int> firstEmptyInRow() const;
    /// Empties every slot holding `id` (a revoked spell, ability or creature).
    void remove(std::uint32_t id);
    /// Puts `id` in a slot (book for a spell from the book: lit if it goes on its
    /// caster, flags 0x600).
    void place(int set, int slot, std::uint32_t id, const things::ThingDb& things);

    /// From the .plr's Gui section. Unknown names leave the slot empty.
    static SpellBars fromGui(const player::Gui& gui);
    /// For the .plr's Gui section (abilities by name for a warrior; spells otherwise).
    player::Gui toGui(player::CharacterClass characterClass) const;

    /// How many starting sets the class has.
    static int presetCount(player::CharacterClass characterClass);
    /// A new character's bars (from the class screen): a warrior's five abilities in
    /// the first set; a caster's `preset` (the game picks one at random) in all five sets.
    static SpellBars defaults(player::CharacterClass characterClass, int preset, const things::ThingDb& things);
};

} // namespace nox::client
