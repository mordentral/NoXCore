// Spell ids: the numbers the protocol uses for spells (MSG_TRY_SPELL, MSG_REPORT_SPELL_AWARD,
// ...), which index the exe's name table ("SPELL_INVALID", "SPELL_ANCHOR", ...).
// thing.bin's SPEL records are matched to them by name.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace nox::things {

inline constexpr std::uint32_t kSpellCount = 137;

/// The summonings (the creature guide's spells): Summon Bat (0x4B) to 0x72.
inline constexpr std::uint32_t kFirstSummonSpell = 0x4B;
inline constexpr std::uint32_t kLastSummonSpell = 0x72;

constexpr bool isSummonSpell(std::uint32_t id) noexcept
{
    return kFirstSummonSpell <= id && id <= kLastSummonSpell;
}

/// What a player pays for each summoning, from 0x4B (spellManaCost's own per-creature table;
/// thing.bin's cost is 0 for all of them). The server charges these; the client's spell bar
/// darkens a summon's slot by them too (VD-34).
inline constexpr std::array<std::uint8_t, kLastSummonSpell - kFirstSummonSpell + 1> kSummonManaCosts = {
    15, 60, 60, 60, 30, 30, 30, 15, 60, 60, 15, 30, 30, 30, 85, 85, 30, 60, 85, 60,
    30, 30, 60, 30, 15, 30, 85, 30, 30, 15, 60, 30, 30, 30, 30, 60, 60, 60, 60, 60};

/// A summoning's price (kSummonManaCosts), or 0 for any other spell.
constexpr int summonManaCost(std::uint32_t id) noexcept
{
    return isSummonSpell(id) ? kSummonManaCosts[id - kFirstSummonSpell] : 0;
}

/// The spell's name ("SPELL_FIREBALL"), or "" past the table.
std::string_view spellName(std::uint32_t id) noexcept;

/// The id for a spell name (case-sensitive, as the game compares), or nullopt.
std::optional<std::uint32_t> spellId(std::string_view name) noexcept;

} // namespace nox::things
