// Spell id <-> name table (see spell_ids.hpp).
#include "noxcore/things/spell_ids.hpp"

#include <algorithm>
#include <array>

namespace nox::things {

namespace {

// The exe's spell name table, in order: the index is the spell id.
constexpr std::array<std::string_view, kSpellCount> kNames{
    "SPELL_INVALID",
    "SPELL_ANCHOR",
    "SPELL_ARACHNAPHOBIA",
    "SPELL_BLIND",
    "SPELL_BLINK",
    "SPELL_BURN",
    "SPELL_CANCEL",
    "SPELL_CHAIN_LIGHTNING_BOLT",
    "SPELL_CHANNEL_LIFE",
    "SPELL_CHARM",
    "SPELL_CLEANSING_FLAME",
    "SPELL_CLEANSING_MANA_FLAME",
    "SPELL_CONFUSE",
    "SPELL_COUNTERSPELL",
    "SPELL_CURE_POISON",
    "SPELL_DEATH",
    "SPELL_DEATH_RAY",
    "SPELL_DETECT_MAGIC",
    "SPELL_DETONATE",
    "SPELL_DETONATE_GLYPHS",
    "SPELL_DISENCHANT_ALL",
    "SPELL_TURN_UNDEAD",
    "SPELL_DRAIN_MANA",
    "SPELL_EARTHQUAKE",
    "SPELL_LIGHTNING",
    "SPELL_EXPLOSION",
    "SPELL_FEAR",
    "SPELL_FIREBALL",
    "SPELL_FIREWALK",
    "SPELL_FIST",
    "SPELL_FORCE_FIELD",
    "SPELL_FORCE_OF_NATURE",
    "SPELL_FREEZE",
    "SPELL_FUMBLE",
    "SPELL_GLYPH",
    "SPELL_GREATER_HEAL",
    "SPELL_HASTE",
    "SPELL_INFRAVISION",
    "SPELL_INVERSION",
    "SPELL_INVISIBILITY",
    "SPELL_INVULNERABILITY",
    "SPELL_LESSER_HEAL",
    "SPELL_LIGHT",
    "SPELL_CHAIN_LIGHTNING",
    "SPELL_LOCK",
    "SPELL_MARK",
    "SPELL_MARK_1",
    "SPELL_MARK_2",
    "SPELL_MARK_3",
    "SPELL_MARK_4",
    "SPELL_MAGIC_MISSILE",
    "SPELL_SHIELD",
    "SPELL_METEOR",
    "SPELL_METEOR_SHOWER",
    "SPELL_MOONGLOW",
    "SPELL_NULLIFY",
    "SPELL_MANA_BOMB",
    "SPELL_PHANTOM",
    "SPELL_PIXIE_SWARM",
    "SPELL_PLASMA",
    "SPELL_POISON",
    "SPELL_PROTECTION_FROM_ELECTRICITY",
    "SPELL_PROTECTION_FROM_FIRE",
    "SPELL_PROTECTION_FROM_MAGIC",
    "SPELL_PROTECTION_FROM_POISON",
    "SPELL_PULL",
    "SPELL_PUSH",
    "SPELL_OVAL_SHIELD",
    "SPELL_RESTORE_HEALTH",
    "SPELL_RESTORE_MANA",
    "SPELL_RUN",
    "SPELL_SHOCK",
    "SPELL_SLOW",
    "SPELL_SMALL_ZAP",
    "SPELL_STUN",
    "SPELL_SUMMON_BAT",
    "SPELL_SUMMON_BLACK_BEAR",
    "SPELL_SUMMON_BEAR",
    "SPELL_SUMMON_BEHOLDER",
    "SPELL_SUMMON_BOMBER",
    "SPELL_SUMMON_CARNIVOROUS_PLANT",
    "SPELL_SUMMON_ALBINO_SPIDER",
    "SPELL_SUMMON_SMALL_ALBINO_SPIDER",
    "SPELL_SUMMON_EVIL_CHERUB",
    "SPELL_SUMMON_EMBER_DEMON",
    "SPELL_SUMMON_GHOST",
    "SPELL_SUMMON_GIANT_LEECH",
    "SPELL_SUMMON_IMP",
    "SPELL_SUMMON_MECHANICAL_FLYER",
    "SPELL_SUMMON_MECHANICAL_GOLEM",
    "SPELL_SUMMON_MIMIC",
    "SPELL_SUMMON_OGRE",
    "SPELL_SUMMON_OGRE_BRUTE",
    "SPELL_SUMMON_OGRE_WARLORD",
    "SPELL_SUMMON_SCORPION",
    "SPELL_SUMMON_SHADE",
    "SPELL_SUMMON_SKELETON",
    "SPELL_SUMMON_SKELETON_LORD",
    "SPELL_SUMMON_SPIDER",
    "SPELL_SUMMON_SMALL_SPIDER",
    "SPELL_SUMMON_SPITTING_SPIDER",
    "SPELL_SUMMON_STONE_GOLEM",
    "SPELL_SUMMON_TROLL",
    "SPELL_SUMMON_URCHIN",
    "SPELL_SUMMON_WASP",
    "SPELL_SUMMON_WILLOWISP",
    "SPELL_SUMMON_WOLF",
    "SPELL_SUMMON_BLACK_WOLF",
    "SPELL_SUMMON_WHITE_WOLF",
    "SPELL_SUMMON_ZOMBIE",
    "SPELL_SUMMON_VILE_ZOMBIE",
    "SPELL_SUMMON_DEMON",
    "SPELL_SUMMON_LICH",
    "SPELL_SUMMON_DRYAD",
    "SPELL_SUMMON_URCHIN_SHAMAN",
    "SPELL_SWAP",
    "SPELL_TAG",
    "SPELL_TELEPORT_OTHER_TO_MARK_1",
    "SPELL_TELEPORT_OTHER_TO_MARK_2",
    "SPELL_TELEPORT_OTHER_TO_MARK_3",
    "SPELL_TELEPORT_OTHER_TO_MARK_4",
    "SPELL_TELEPORT_POP",
    "SPELL_TELEPORT_TO_MARK_1",
    "SPELL_TELEPORT_TO_MARK_2",
    "SPELL_TELEPORT_TO_MARK_3",
    "SPELL_TELEPORT_TO_MARK_4",
    "SPELL_TELEPORT_TO_TARGET",
    "SPELL_TELEKINESIS",
    "SPELL_TOXIC_CLOUD",
    "SPELL_TRIGGER_GLYPH",
    "SPELL_VAMPIRISM",
    "SPELL_VILLAIN",
    "SPELL_WALL",
    "SPELL_WINK",
    "SPELL_SUMMON_CREATURE",
    "SPELL_MARK_LOCATION",
    "SPELL_TELEPORT_TO_MARKER",
};

} // namespace

std::string_view spellName(std::uint32_t id) noexcept
{
    return id < kNames.size() ? kNames[id] : std::string_view{};
}

namespace {

struct NameId {
    std::string_view name;
    std::uint32_t id;
};

// The table sorted by name (a name given twice: its lowest id first), for spellId's search.
constexpr auto kByName = [] {
    std::array<NameId, kSpellCount> out{};
    for (std::uint32_t i = 0; i < kSpellCount; ++i) {
        out[i] = {kNames[i], i};
    }
    std::sort(out.begin(), out.end(), [](const NameId& a, const NameId& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    return out;
}();

} // namespace

std::optional<std::uint32_t> spellId(std::string_view name) noexcept
{
    // (The first id of that name, as walking the table finds it.)
    const auto it = std::lower_bound(kByName.begin(), kByName.end(), name, [](const NameId& e, std::string_view n) { return e.name < n; });
    return it != kByName.end() && it->name == name ? std::optional<std::uint32_t>(it->id) : std::nullopt;
}

} // namespace nox::things
