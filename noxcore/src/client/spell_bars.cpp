// The client's spell bars: slots filled, searched and emptied, converted to and from the .plr's
// Gui section, and a new character's starting sets. See spell_bars.hpp.
#include "noxcore/client/spell_bars.hpp"

#include "noxcore/things/spell_ids.hpp"

#include <string_view>

namespace nox::client {

namespace {

/// The starting sets: a warrior's abilities, then three wizard and three conjurer
/// sets of five spell ids.
constexpr std::array<std::uint8_t, 5> kWarriorPreset{1, 2, 3, 4, 5};
constexpr std::array<std::array<std::uint8_t, 5>, 3> kWizardPresets{{
    {0x04, 0x33, 0x32, 0x1B, 0x29}, // blink, shield, magic missile, fireball, lesser heal
    {0x04, 0x33, 0x2B, 0x1B, 0x29}, // ... chain lightning ...
    {0x27, 0x33, 0x32, 0x10, 0x29}, // invisibility, shield, magic missile, death ray, lesser heal
}};
constexpr std::array<std::array<std::uint8_t, 5>, 3> kConjurerPresets{{
    {0x53, 0x5C, 0x3A, 0x1D, 0x29}, // evil cherub, ogre brute, pixie swarm, fist, lesser heal
    {0x57, 0x4A, 0x80, 0x3A, 0x29}, // imp, stun, toxic cloud, pixie swarm, lesser heal
    {0x04, 0x4D, 0x34, 0x3A, 0x29}, // blink, bear, meteor, pixie swarm, lesser heal
}};

// An ability's id from its ABILITY_* name; 0 (ABILITY_INVALID) for an unknown one.
std::uint32_t abilityId(std::string_view name)
{
    for (std::size_t i = 1; i < kAbilityNames.size(); ++i) {
        if (name == kAbilityNames[i]) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return 0;
}

} // namespace

std::uint32_t spellFlags(const things::ThingDb& things, std::uint32_t spell)
{
    const things::SpellDef* s = things.findSpell(spell);
    return s ? s->flags : 0;
}

bool canTrap(const things::ThingDb& things, std::uint32_t spell, bool quest)
{
    // 0x89 is kSpellCount (137): past the last spell id. Spell 4 is Blink.
    if (spell == 0 || spell >= 0x89 || (quest && spell == 4)) {
        return false;
    }
    return (spellFlags(things, spell) & 0x400000) == 0;
}

std::vector<std::uint32_t> SpellBars::trapCast() const
{
    std::vector<std::uint32_t> out;
    for (const auto& s : currentTrap()) {
        if (s.id != 0) {
            out.push_back(s.id);
        }
    }
    if (!out.empty()) {
        out.push_back(kGlyph);
    }
    return out;
}

bool SpellBars::contains(std::uint32_t id) const
{
    for (const auto& r : rows) {
        for (const auto& s : r) {
            if (s.id == id) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::pair<int, int>> SpellBars::firstEmpty() const
{
    for (int n = 0; n < 5; ++n) {
        const int r = (row + n) % 5;
        for (int i = 0; i < 5; ++i) {
            if (rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(i)].id == 0) {
                return std::pair{r, i};
            }
        }
    }
    return std::nullopt;
}

std::optional<int> SpellBars::firstEmptyInRow() const
{
    for (int i = 0; i < 5; ++i) {
        if (current()[static_cast<std::size_t>(i)].id == 0) {
            return i;
        }
    }
    return std::nullopt;
}

void SpellBars::remove(std::uint32_t id)
{
    // empties the id but leaves the flag byte.
    for (auto& r : rows) {
        for (auto& s : r) {
            if (s.id == id) {
                s.id = 0;
            }
        }
    }
}

void SpellBars::place(int set, int slot, std::uint32_t id, const things::ThingDb& things)
{
    BarSlot& s = rows[static_cast<std::size_t>(set)][static_cast<std::size_t>(slot)];
    s.id = id;
    // Flags 0x600: a spell that goes on its caster, so its slot starts lit.
    s.target = id != 0 && (spellFlags(things, id) & 0x600) != 0;
}

SpellBars SpellBars::fromGui(const player::Gui& gui)
{
    SpellBars b;
    const bool warrior = gui.characterClass == player::CharacterClass::Warrior;
    const auto slot = [&](const player::Gui::Slot& s) {
        BarSlot out;
        if (warrior) {
            out.id = abilityId(s.name);
        } else if (const auto id = things::spellId(s.name)) {
            out.id = *id;
        }
        out.target = (s.flag & 1) != 0;
        return out;
    };
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t i = 0; i < 5; ++i) {
            b.rows[r][i] = slot(gui.bars[r][i]);
        }
    }
    // Only casters have trap sets; a warrior's stay empty.
    if (!warrior) {
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t i = 0; i < 3; ++i) {
                b.traps[r][i] = slot(gui.traps[r][i]);
            }
        }
    }
    b.row = gui.activeRow < 5 ? gui.activeRow : 0;
    b.trapRow = gui.activeTrap < 3 ? gui.activeTrap : 0;
    b.summonOrder = gui.summonOrder;
    return b;
}

player::Gui SpellBars::toGui(player::CharacterClass characterClass) const
{
    player::Gui g;
    g.characterClass = characterClass;
    const bool warrior = characterClass == player::CharacterClass::Warrior;
    const auto slot = [&](const BarSlot& s) {
        player::Gui::Slot out;
        if (warrior) {
            out.name = kAbilityNames[s.id < kAbilityNames.size() ? s.id : 0];
        } else {
            const std::string_view name = things::spellName(s.id);
            out.name = name.empty() ? std::string(things::spellName(0)) : std::string(name);
        }
        out.flag = s.target ? 1 : 0;
        return out;
    };
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t i = 0; i < 5; ++i) {
            g.bars[r][i] = slot(rows[r][i]);
        }
    }
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t i = 0; i < 3; ++i) {
            g.traps[r][i] = warrior ? player::Gui::Slot{} : slot(traps[r][i]);
        }
    }
    g.activeRow = row;
    g.activeTrap = trapRow;
    g.summonOrder = summonOrder;
    return g;
}

int SpellBars::presetCount(player::CharacterClass characterClass)
{
    switch (characterClass) {
    case player::CharacterClass::Warrior:
        return 1;
    case player::CharacterClass::Wizard:
        return static_cast<int>(kWizardPresets.size());
    case player::CharacterClass::Conjurer:
        return static_cast<int>(kConjurerPresets.size());
    }
    return 0;
}

SpellBars SpellBars::defaults(player::CharacterClass characterClass, int preset, const things::ThingDb& things)
{
    SpellBars b;
    if (characterClass == player::CharacterClass::Warrior) {
        // Mode 3 of book: the abilities as they are, unlit, in the first set only.
        for (std::size_t i = 0; i < 5; ++i) {
            b.rows[0][i].id = kWarriorPreset[i];
        }
        return b;
    }
    const auto& presets = characterClass == player::CharacterClass::Wizard ? kWizardPresets : kConjurerPresets;
    const auto& p = presets[static_cast<std::size_t>(preset) % presets.size()];
    for (int r = 0; r < 5; ++r) {
        for (int i = 0; i < 5; ++i) {
            b.place(r, i, p[static_cast<std::size_t>(i)], things);
        }
    }
    return b;
}

} // namespace nox::client
