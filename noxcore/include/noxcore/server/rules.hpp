// A game's rules: the spells, weapons and armour allowed (the game settings' + 0x18 spell bitmap,
// + 0x2C weapons, + 0x30 armour), read from and written to a map's rule files
// (maps\<map>\user.rul, else maps\<map>\<map>.rul; the Server Settings' rule sets, maps\<map>\<name>.rul).
//
//   reading   everything allowed, then (flags 2) the file named or user.rul, and
//             (flags 1) when that read nothing the map's own <map>.rul a line at a
//             time (up to its '\n'), each split into words (a quoted one whole)
//             and taken by a section header ([ELIMINATION], [DEATHMATCH],
//             [CAPTURE_THE_FLAG], [KING_OF_THE_REALM], [FLAGBALL], [COMMON], [QUEST]) sets the
//             section; in a section of the mode, "set spell <name> off" (a spell of a class:
//             flags 0x7000000) clears its bit, "set weapon <name> off|on" and "set armor <name>
//             off|on" (the host's: game flag 1) clear or set theirs; a line that isn't the mode's
//             own section's (other sections, [COMMON], anything else) is kept (`other`). In a
//             flagball game Wall (0x84) is off whatever the file says.
//   writing (not for a game of settings flag 0x80): the file (its name lower case)
//             the kept lines first, then the mode's header, a blank line, and a
//             `set spell "<name>" off` for each spell of a class not allowed, `set Armor "<thing>"
//             off`, `set weapon "<thing>" off` for each not allowed.
// The words compare without case. Weapons and armour are thing.bin's by the exe's
// tables, a bit each.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::server {

// What a game allows: spell n is bit n % 32 of word n / 32; weapons and armour a bit each, by the
// tables below. Every bit set: everything allowed.
struct RuleSet {
    std::array<std::uint32_t, 5> spells{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    std::uint32_t weapons = 0xFFFFFFFFu;
    std::uint32_t armor = 0xFFFFFFFFu;
    std::vector<std::string> other; ///< the lines kept for writing back

    bool spellAllowed(int spell) const { return (spells[static_cast<std::size_t>(spell) >> 5 & 0xFF] >> (spell & 31)) & 1u; }
    void setSpell(int spell, bool on);
    bool operator==(const RuleSet& o) const { return spells == o.spells && weapons == o.weapons && armor == o.armor; }
};

/// A weapon's or armour's bit and its thing (and its name's label), as the exe's tables have them.
struct RuleItem {
    const char* thing;
    const char* label; ///< modifier.db:<...>Desc
    std::uint32_t bit;
};
/// The weapons (with their labels') and armour.
const std::vector<RuleItem>& ruleWeapons();
const std::vector<RuleItem>& ruleArmour();
/// The weapons page's order (guiObjlistLoad: by bit from 4) and the armour's
/// (from 1): the labels' tables walked by bit.
std::vector<const RuleItem*> ruleWeaponsByBit();
std::vector<const RuleItem*> ruleArmourByBit();
/// The labels' table entry for one bit (the first with that bit),
/// or null.
const RuleItem* ruleWeaponFor(std::uint32_t bit);
const RuleItem* ruleArmourFor(std::uint32_t bit);
/// `rules`' weapons and armour into the types turned off (World::disabledTypes): each item's
/// thing off when its bit is clear, on when it's set (the Flag always on: Disable All clears its
/// bit, but it isn't on the Weapons page); the object types content packs added like one with it
/// (things::addedTypeBases).
void applyRuleTypes(const RuleSet& rules, std::set<std::string>& disabledTypes);

/// a mode's section header ("[DEATHMATCH]"...), none for no mode of the table.
std::optional<std::string> ruleSection(std::uint32_t mode);

/// for `map`'s folder (`mapsDir`\<map, 8 letters>\): `flags` 2 the file (`file`, else
/// user.rul), 1 the map's own when that read nothing, 4 in a WOL game (`wol`) the game folder's
/// internet.rul after; for `mode` (game flags); `host` (game flag 1: weapons and armour count).
RuleSet readRules(const std::filesystem::path& mapsDir, const std::string& map, const std::optional<std::string>& file,
                  int flags, std::uint32_t mode, const things::ThingDb& things, bool host = true, bool wol = false);
/// `rules` for `mode` into `mapsDir`\<map>\<file> (lower case). In a WOL game
/// (`wol`) a spell off only by internet.rul isn't written. False where it couldn't be written.
bool writeRules(const std::filesystem::path& mapsDir, const std::string& map, const std::string& file, const RuleSet& rules,
                std::uint32_t mode, const things::ThingDb& things, bool wol = false);
/// The rule sets in a map's folder (its *.rul but user.rul and the map's own).
std::vector<std::string> ruleSets(const std::filesystem::path& mapsDir, const std::string& map);

} // namespace nox::server
