// Reading and writing a map's rule files (.rul) and the weapon and armour bit tables; rules.hpp
// describes the files.
#include "noxcore/server/rules.hpp"

#include "noxcore/io/file.hpp"

#include "noxcore/things/modifier.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace nox::server {

namespace fs = std::filesystem;

namespace {

using util::equalsNoCase; // (equal without case)
using util::lowered;

/// The sections' table: each header and its mode.
struct Section {
    const char* header;
    std::uint32_t mode;
};
constexpr std::array<Section, 7> kSections{{
    {"[ELIMINATION]", 0x400},
    {"[DEATHMATCH]", 0x100},
    {"[CAPTURE_THE_FLAG]", 0x20},
    {"[KING_OF_THE_REALM]", 0x10},
    {"[FLAGBALL]", 0x40},
    {"[COMMON]", 0x17F0},
    {"[QUEST]", 0x1000},
}};

/// A line's words: split at blanks, a quoted word whole.
std::vector<std::string> wordsOf(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        if (line[i] == '"') {
            const auto end = line.find('"', i + 1);
            out.push_back(line.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
        } else {
            const auto end = line.find_first_of(" \t\r\n", i);
            out.push_back(line.substr(i, end == std::string::npos ? std::string::npos : end - i));
            i = end == std::string::npos ? line.size() : end;
        }
    }
    return out;
}

const RuleItem* itemByThing(const std::vector<RuleItem>& items, std::string_view thing)
{
    // The item by its thing's name.
    for (const auto& i : items) {
        if (equalsNoCase(i.thing, thing)) {
            return &i;
        }
    }
    return nullptr;
}

constexpr std::uint32_t kClassSpell = 0x7000000; ///< a spell of a class (or common)

/// one line's words for `mode` (the section kept in `section`); whether it's the
/// mode's own section's (not kept).
bool takeLine(const std::vector<std::string>& w, RuleSet& rules, std::uint32_t mode, std::uint32_t& section,
              const things::ThingDb& things, bool host)
{
    // A section header: the section from here on (the line not kept when it's the mode's own).
    for (const auto& s : kSections) {
        if (w[0] == s.header) {
            section = s.mode;
            return mode == s.mode;
        }
    }
    // Outside the mode's sections, or not a "set" line: kept as it is.
    if (!(mode & section) || !equalsNoCase(w[0], "set") || w.size() < 2) {
        return false;
    }
    if (equalsNoCase(w[1], "spell")) {
        if (w.size() != 4) {
            return false;
        }
        const auto id = things::spellId(w[2]);
        const things::SpellDef* def = id ? things.findSpell(*id) : nullptr;
        if (!def) {
            return false; // (spellNameToN; its title: not without the strings)
        }
        if ((def->flags & kClassSpell) && equalsNoCase(w[3], "off")) {
            rules.setSpell(static_cast<int>(*id), false);
        }
    } else if (equalsNoCase(w[1], "weapon") || equalsNoCase(w[1], "armor")) {
        if (w.size() != 4 || !host) {
            return false;
        }
        const bool weapon = equalsNoCase(w[1], "weapon");
        const RuleItem* item = itemByThing(weapon ? ruleWeapons() : ruleArmour(), w[2]);
        if (!item || item->bit == 0) {
            return false;
        }
        std::uint32_t& mask = weapon ? rules.weapons : rules.armor;
        mask = equalsNoCase(w[3], "off") ? (mask & ~item->bit) : (mask | item->bit);
    } else {
        return false;
    }
    return mode == section;
}

/// the file's lines (nothing read: false).
bool readFile(const fs::path& path, RuleSet& rules, std::uint32_t mode, const things::ThingDb& things, bool host)
{
    std::ifstream in(resolveNoCase(path), std::ios::binary); // (any case, as on Windows)
    if (!in) {
        return false;
    }
    std::uint32_t section = 0x17F0; // (COMMON's till a header)
    for (std::string line; std::getline(in, line);) {
        if (line.size() > 255) {
            line.resize(255);
        }
        if (line.empty()) {
            continue;
        }
        const auto w = wordsOf(line);
        if (w.empty()) {
            continue;
        }
        if (!takeLine(w, rules, mode, section, things, host)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            rules.other.push_back(line);
        }
    }
    return true;
}

} // namespace

void RuleSet::setSpell(int spell, bool on)
{
    auto& word = spells[static_cast<std::size_t>(spell) >> 5 & 0xFF];  // (spell n: word n / 32, bit n % 32)
    const std::uint32_t bit = 1u << (spell & 31);
    word = on ? (word | bit) : (word & ~bit);
}

const std::vector<RuleItem>& ruleWeapons()
{
    static const std::vector<RuleItem> kItems = {
        {"Bow", "Modifier.db:BowDesc", 0x4},
        {"CrossBow", "Modifier.db:CrossbowDesc", 0x8},
        {"Longsword", "modifier.db:LongswordDesc", 0x200},
        {"GreatSword", "modifier.db:GreatSwordDesc", 0x400},
        {"MorningStar", "modifier.db:MorningStarDesc", 0x800},
        {"BattleAxe", "modifier.db:BattleAxeDesc", 0x1000},
        {"OgreAxe", "modifier.db:OgreAxeDesc", 0x2000},
        {"Sword", "modifier.db:SwordDesc", 0x100},
        {"WarHammer", "modifier.db:WarHammerDesc", 0x4000},
        {"RoundChakram", "Modifier.db:RoundChakramDesc", 0x40},
        {"FanChakram", "modifier.db:FanChakramDesc", 0x80},
        {"Quiver", "Modifier.db:QuiverDesc", 0x2},
        {"StaffWooden", "modifier.db:StaffWoodenDesc", 0x8000},
        {"SulphorousFlareWand", "modifier.db:SulphorousFlareWandDesc", 0x10000},
        {"SulphorousShowerWand", "modifier.db:SulphorousShowerWandDesc", 0x20000},
        {"LesserFireballWand", "modifier.db:LesserFireballWandDesc", 0x80000},
        {"DemonsBreathWand", "modifier.db:LesserFireballWandDesc", 0x80000}, // (sharing its bit)
        {"DeathRayWand", "modifier.db:DeathRayWandDesc", 0x400000},
        {"InfinitePainWand", "modifier.db:InfinitePainWandDesc", 0x200000},
        {"FireStormWand", "modifier.db:FireStormWandDesc", 0x100000},
        {"ForceWand", "modifier.db:ForceWandDesc", 0x40000},
        {"OblivionHalberd", "modifier.db:OblivionHalberdDesc", 0x800000},
        {"OblivionHeart", "modifier.db:OblivionHeartDesc", 0x1000000},
        {"OblivionWierdling", "modifier.db:OblivionWierdlingDesc", 0x2000000},
        {"OblivionOrb", "modifier.db:OblivionOrbDesc", 0x4000000},
        {"Flag", "Modifier.db:FlagDesc", 0x1},
    };
    return kItems;
}

const std::vector<RuleItem>& ruleArmour()
{
    static const std::vector<RuleItem> kItems = {
        {"LeatherArmor", "modifier.db:LeatherArmorDesc", 0x8000},
        {"ChainTunic", "modifier.db:ChainTunicDesc", 0x10000},
        {"Breastplate", "modifier.db:BreastplateDesc", 0x20000},
        {"LeatherHelm", "modifier.db:LeatherHelmDesc", 0x200000},
        {"SteelHelm", "modifier.db:SteelHelmDesc", 0x400000},
        {"SteelShield", "modifier.db:SteelShieldDesc", 0x2000000},
        {"WoodenShield", "modifier.db:WoodenShieldDesc", 0x1000000},
        {"StreetShirt", "modifier.db:StreetShirtDesc", 0x400},
        {"StreetPants", "modifier.db:StreetPantsDesc", 0x4},
        {"StreetSneakers", "modifier.db:StreetSneakersDesc", 0x1},
        {"LeatherBoots", "modifier.db:LeatherBootsDesc", 0x40},
        {"LeatherArmoredBoots", "modifier.db:LeatherArmoredBootsDesc", 0x80},
        {"PlateBoots", "modifier.db:PlateBootsDesc", 0x100},
        {"MedievalPants", "modifier.db:MedievalPantsDesc", 0x8},
        {"LeatherLeggings", "modifier.db:LeatherLeggingsDesc", 0x10},
        {"ChainLeggings", "modifier.db:ChainLeggingsDesc", 0x20},
        {"PlateLeggings", "modifier.db:PlateLeggingsDesc", 0x200},
        {"MedievalShirt", "modifier.db:MedievalShirtDesc", 0x800},
        {"WizardRobe", "modifier.db:WizardRobeDesc", 0x4000},
        {"LeatherArmbands", "modifier.db:LeatherArmbandsDesc", 0x1000},
        {"PlateArms", "modifier.db:PlateArmsDesc", 0x2000},
        {"MedievalCloak", "modifier.db:MedievalCloakDesc", 0x2},
        {"ChainCoif", "modifier.db:ChainCoifDesc", 0x40000},
        {"WizardHelm", "modifier.db:WizardHelmDesc", 0x80000},
        {"ConjurerHelm", "modifier.db:ConjurerHelmDesc", 0x100000},
        {"OrnateHelm", "modifier.db:OrnateHelmDesc", 0x800000},
    };
    return kItems;
}

namespace {

std::vector<const RuleItem*> byBit(const std::vector<RuleItem>& items, std::uint32_t from, int count)
{
    // guiObjlistLoad: the labels' table for each bit
    // from `from`, `count` of them; a bit without an entry none.
    std::vector<const RuleItem*> out;
    std::uint32_t bit = from;
    for (int i = 0; i < count; ++i, bit <<= 1) {
        for (const auto& item : items) {
            if (item.bit == bit) {
                out.push_back(&item);
                break;
            }
        }
    }
    return out;
}

} // namespace

std::vector<const RuleItem*> ruleWeaponsByBit()
{
    return byBit(ruleWeapons(), 4, 0x19);
}

std::vector<const RuleItem*> ruleArmourByBit()
{
    return byBit(ruleArmour(), 1, 0x1A);
}

const RuleItem* ruleWeaponFor(std::uint32_t bit)
{
    const auto& items = ruleWeapons();
    const auto it = std::ranges::find(items, bit, &RuleItem::bit);
    return it != items.end() ? &*it : nullptr;
}

const RuleItem* ruleArmourFor(std::uint32_t bit)
{
    const auto& items = ruleArmour();
    const auto it = std::ranges::find(items, bit, &RuleItem::bit);
    return it != items.end() ? &*it : nullptr;
}

void applyRuleTypes(const RuleSet& rules, std::set<std::string>& disabledTypes)
{
    const auto added = things::addedTypeBases();
    const auto allow = [&](std::string_view thing, bool on) {
        const auto one = [&](std::string_view type) {
            if (on) {
                disabledTypes.erase(std::string(type));
            } else {
                disabledTypes.insert(std::string(type));
            }
        };
        one(thing);
        for (const auto& [type, stock] : added) {
            if (stock == thing) {
                one(type); // (a type a content pack made like it: the rules go by the class)
            }
        }
    };
    for (const auto& i : ruleWeapons()) {
        // (The Flag's bit, 1, isn't on the Weapons page but Disable All clears it: never kept
        // from a flag game's flags.)
        allow(i.thing, (rules.weapons & i.bit) || i.bit == 0x1);
    }
    for (const auto& i : ruleArmour()) {
        allow(i.thing, (rules.armor & i.bit) != 0);
    }
}

std::optional<std::string> ruleSection(std::uint32_t mode)
{
    for (const auto& s : kSections) {
        if ((mode & 0x17F0u) == s.mode) {
            return std::string(s.header);
        }
    }
    return std::nullopt;
}

RuleSet readRules(const fs::path& mapsDir, const std::string& map, const std::optional<std::string>& file, int flags,
                  std::uint32_t mode, const things::ThingDb& things, bool host, bool wol)
{
    RuleSet rules;
    const std::string folder = map.substr(0, 8);
    bool read = false;
    if (flags & 2) {
        read = readFile(mapsDir / folder / (file ? *file : std::string("user.rul")), rules, mode & 0x17F0u, things, host);
    }
    if ((flags & 1) && !read) {
        readFile(mapsDir / folder / (folder + ".rul"), rules, mode & 0x17F0u, things, host);
    }
    if (wol && (flags & 4)) {
        readFile(mapsDir.parent_path() / "internet.rul", rules, mode & 0x17F0u, things, host); // (the game's folder)
    }
    if (mode & 0x40u) {
        rules.setSpell(0x84, false); // (a flagball game: no Wall)
    }
    return rules;
}

bool writeRules(const fs::path& mapsDir, const std::string& map, const std::string& file, const RuleSet& rules, std::uint32_t mode,
                const things::ThingDb& things, bool wol)
{
    if (mode & 0x80u) {
        return false; // (a chat game's: not written)
    }
    // In a WOL game a spell's line is written only where internet.rul alone (flags 4) allows it,
    // or the map's file (flags 3: user.rul, else its own) has it off: internet.rul's own aren't
    // copied in. That read comes after the file being written is opened (and emptied), so for
    // user.rul it reads nothing (all allowed but flag ball's Wall).
    RuleSet internet;
    RuleSet fileRules;
    if (wol) {
        internet = readRules(mapsDir, map, std::nullopt, 4, mode, things, true, true);
        fileRules = equalsNoCase(file, "user.rul") ? readRules(mapsDir, std::string(), std::nullopt, 0, mode, things)
                                           : readRules(mapsDir, map, std::nullopt, 3, mode, things);
    }
    const fs::path path = mapsDir / map.substr(0, 8) / lowered(file);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    for (const auto& l : rules.other) {
        out << l << '\n';
    }
    if (const auto header = ruleSection(mode)) {
        out << *header; // (printed, then the line end)
    }
    out << '\n';
    for (std::uint32_t id = 1; id < 0x89; ++id) {
        const things::SpellDef* def = things.findSpell(id);
        const bool written = !wol || internet.spellAllowed(static_cast<int>(id)) || !fileRules.spellAllowed(static_cast<int>(id));
        if (def && !rules.spellAllowed(static_cast<int>(id)) && (def->flags & kClassSpell) && written) {
            out << "set spell \"" << things::spellName(id) << "\" off\n";
        }
    }
    for (std::uint32_t bit = 1; bit != 0 && bit < (1u << 26); bit <<= 1) {
        if (!(rules.armor & bit)) {
            for (const auto& i : ruleArmour()) {
                if (i.bit == bit) {
                    out << "set Armor \"" << i.thing << "\" off\n";
                    break;
                }
            }
        }
    }
    for (std::uint32_t bit = 1; bit != 0 && bit < (1u << 27); bit <<= 1) {
        if (!(rules.weapons & bit)) {
            for (const auto& i : ruleWeapons()) {
                if (i.bit == bit) {
                    out << "set weapon \"" << i.thing << "\" off\n";
                    break;
                }
            }
        }
    }
    return static_cast<bool>(out);
}

std::vector<std::string> ruleSets(const fs::path& mapsDir, const std::string& map)
{
    std::vector<std::string> out;
    const std::string folder = map.substr(0, 8);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(mapsDir / folder, ec)) {
        const fs::path p = e.path();
        if (!equalsNoCase(p.extension().string(), ".rul")) {
            continue;
        }
        const std::string stem = p.stem().string();
        if (equalsNoCase(stem, folder) || equalsNoCase(stem, "user")) {
            continue;
        }
        out.push_back(stem);
    }
    std::ranges::sort(out);
    return out;
}

} // namespace nox::server
