// modifier.bin parser (format in modifier.hpp) and the weapon/armour class bit tables that map
// items to their class bits (the bits the ALLOWED_* lists and the equip flags use).
#include "noxcore/things/modifier.hpp"
#include "noxcore/util/x87.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace nox::things {

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\0')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\0')) {
        s.remove_suffix(1);
    }
    return s;
}

/// "150 150 150 (helm top)" -> {150, 150, 150}.
Rgb parseRgb(std::string_view value, int line)
{
    Rgb out{};
    const char* p = value.data();
    const char* end = value.data() + value.size();
    for (auto& c : out) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            ++p;
        }
        int v = 0;
        const auto [next, ec] = std::from_chars(p, end, v);
        if (ec != std::errc{} || v < 0 || v > 255) {
            throw FormatError("modifier.bin line " + std::to_string(line) + ": bad colour");
        }
        c = static_cast<std::uint8_t>(v);
        p = next;
    }
    return out;
}

/// "COLOR3" -> 3.
std::uint8_t parseSlot(std::string_view value, int line)
{
    if (value.size() == 6 && value.starts_with("COLOR") && value[5] >= '1' && value[5] <= '6') {
        return static_cast<std::uint8_t>(value[5] - '0');
    }
    throw FormatError("modifier.bin line " + std::to_string(line) + ": bad colour slot");
}

// The definition keys naming which colour slot each modifier position recolours, in network order.
constexpr std::string_view kSlotKeys[] = {"EFFECTIVENESS", "MATERIAL", "PRIMARYENCHANTMENT", "SECONDARYENCHANTMENT"};

struct NamedBit {
    std::uint32_t bit;
    std::string_view name;
};

// Armour class bit -> the item drawn for it.
constexpr NamedBit kArmorItems[] = {
    {0x8000, "LeatherArmor"},       {0x10000, "ChainTunic"},       {0x20000, "Breastplate"},
    {0x200000, "LeatherHelm"},      {0x400000, "SteelHelm"},       {0x2000000, "SteelShield"},
    {0x1000000, "WoodenShield"},    {0x400, "StreetShirt"},        {0x4, "StreetPants"},
    {0x1, "StreetSneakers"},        {0x40, "LeatherBoots"},        {0x80, "LeatherArmoredBoots"},
    {0x100, "PlateBoots"},          {0x8, "MedievalPants"},        {0x10, "LeatherLeggings"},
    {0x20, "ChainLeggings"},        {0x200, "PlateLeggings"},      {0x800, "MedievalShirt"},
    {0x4000, "WizardRobe"},         {0x1000, "LeatherArmbands"},   {0x2000, "PlateArms"},
    {0x2, "MedievalCloak"},         {0x40000, "ChainCoif"},        {0x80000, "WizardHelm"},
    {0x100000, "ConjurerHelm"},     {0x800000, "OrnateHelm"},
};

// Weapon class bit -> item. Two wands share 0x80000; nameOf returns the first.
constexpr NamedBit kWeaponItems[] = {
    {0x4, "Bow"},
    {0x8, "CrossBow"},
    {0x200, "Longsword"},
    {0x400, "GreatSword"},
    {0x800, "MorningStar"},
    {0x1000, "BattleAxe"},
    {0x2000, "OgreAxe"},
    {0x100, "Sword"},
    {0x4000, "WarHammer"},
    {0x40, "RoundChakram"},
    {0x80, "FanChakram"},
    {0x2, "Quiver"},
    {0x8000, "StaffWooden"},
    {0x10000, "SulphorousFlareWand"},
    {0x20000, "SulphorousShowerWand"},
    {0x80000, "LesserFireballWand"},
    {0x80000, "DemonsBreathWand"},
    {0x400000, "DeathRayWand"},
    {0x200000, "InfinitePainWand"},
    {0x100000, "FireStormWand"},
    {0x40000, "ForceWand"},
    {0x800000, "OblivionHalberd"},
    {0x1000000, "OblivionHeart"},
    {0x2000000, "OblivionWierdling"},
    {0x4000000, "OblivionOrb"},
    {0x1, "Flag"},
};

/// How an effect's number is read, by its entry in the effect tables: Float (a float),
/// Int (an int) or None.
enum class EffectRead : std::uint8_t { Float, Int, None };

struct EffectEntry {
    std::string_view name;
    EffectRead read;
    EffectFn fn;
};

// The effect functions, grouped by the effect table (and so the keys) each belongs to.
constexpr EffectEntry kEffectEntries[] = {
    // (The attack table: ATTACKEFFECT, ATTACKPREHITEFFECT, ATTACKPREDAMAGEEFFECT)
    {"DamageMultiplierEffect", EffectRead::Float, EffectFn::DamageMultiplier},
    {"StunEffect", EffectRead::Int, EffectFn::Stun},
    {"FireEffect", EffectRead::Float, EffectFn::Fire},
    {"FireRingEffect", EffectRead::Int, EffectFn::FireRing},
    {"BlueFireRingEffect", EffectRead::Int, EffectFn::BlueFireRing},
    {"FrostEffect", EffectRead::Int, EffectFn::Frost},
    {"RecoilEffect", EffectRead::Float, EffectFn::Recoil},
    {"ConfuseEffect", EffectRead::Int, EffectFn::Confuse},
    {"LightningEffect", EffectRead::Float, EffectFn::Lightning},
    {"DrainManaEffect", EffectRead::Float, EffectFn::DrainMana},
    {"VampirismEffect", EffectRead::Float, EffectFn::Vampirism},
    {"PoisonEffect", EffectRead::Int, EffectFn::Poison},
    {"PanicEffect", EffectRead::Int, EffectFn::Panic},
    {"SympathyEffect", EffectRead::Float, EffectFn::Sympathy},
    {"ReadinessEffect", EffectRead::Int, EffectFn::Readiness},
    {"ProjectileSpeedEffect", EffectRead::Float, EffectFn::ProjectileSpeed},
    {"ReplenishmentEffect", EffectRead::Int, EffectFn::Replenishment},
    // (The defend table: DEFENDEFFECT, DEFENDCOLLIDEEFFECT)
    {"ArmorMultiplierEffect", EffectRead::Float, EffectFn::ArmorMultiplier},
    {"DurabilityMultiplierEffect", EffectRead::Float, EffectFn::DurabilityMultiplier},
    {"ResilienceEffect", EffectRead::Float, EffectFn::Resilience},
    {"InversionEffect", EffectRead::Int, EffectFn::Inversion},
    {"GripEffect", EffectRead::Int, EffectFn::Grip},
    {"BreakingEffect", EffectRead::Float, EffectFn::Breaking},
    {"PunctureProneEffect", EffectRead::Float, EffectFn::PunctureProne},
    // (The update table: UPDATEEFFECT)
    {"RegenerationUpdate", EffectRead::Int, EffectFn::RegenerationUpdate},
    {"ParasiteUpdate", EffectRead::Int, EffectFn::ParasiteUpdate},
    {"AttractionUpdate", EffectRead::Int, EffectFn::AttractionUpdate},
    {"ContinualReplenishmentUpdate", EffectRead::Int, EffectFn::ContinualReplenishmentUpdate},
    // (The engage table: ENGAGEEFFECT, DISENGAGEEFFECT)
    {"BrillianceEngage", EffectRead::Int, EffectFn::BrillianceEngage},
    {"BrillianceDisengage", EffectRead::Int, EffectFn::BrillianceDisengage},
    {"SpeedEngage", EffectRead::Float, EffectFn::SpeedEngage},
    {"SpeedDisengage", EffectRead::Float, EffectFn::SpeedDisengage},
    {"FireProtectEngage", EffectRead::Float, EffectFn::FireProtectEngage},
    {"FireProtectDisengage", EffectRead::Float, EffectFn::FireProtectDisengage},
    {"LightningProtectEngage", EffectRead::Float, EffectFn::LightningProtectEngage},
    {"LightningProtectDisengage", EffectRead::Float, EffectFn::LightningProtectDisengage},
    {"PoisonProtectEngage", EffectRead::Float, EffectFn::PoisonProtectEngage},
    {"PoisonProtectDisengage", EffectRead::Float, EffectFn::PoisonProtectDisengage},
    {"RegenerationEngage", EffectRead::None, EffectFn::RegenerationEngage},
    {"RegenerationDisengage", EffectRead::None, EffectFn::RegenerationDisengage},
};

/// The next token of `s` (with the delimiters " =\n\r\t"); "" at the end.
std::string_view nextToken(std::string_view& s)
{
    constexpr std::string_view kDelimiters = " =\n\r\t";
    const auto start = s.find_first_not_of(kDelimiters);
    if (start == std::string_view::npos) {
        s = {};
        return {};
    }
    s.remove_prefix(start);
    const auto end = s.find_first_of(kDelimiters);
    const std::string_view token = s.substr(0, end);
    s.remove_prefix(end == std::string_view::npos ? s.size() : end);
    return token;
}

/// An effect's function and number (the number is read with atof, -1.0 when there's none,
/// or with atoi, -1 when there's none).
ModifierEffect parseEffect(std::string_view value)
{
    ModifierEffect e;
    e.name = std::string(nextToken(value));
    e.fn = effectFn(e.name);
    // Functions not in the tables, and those that read no number, leave value and count at 0.
    for (const EffectEntry& entry : kEffectEntries) {
        if (entry.name != e.name || entry.read == EffectRead::None) {
            continue;
        }
        const std::string number(nextToken(value));
        if (entry.read == EffectRead::Float) {
            e.value = number.empty() ? -1.0f : static_cast<float>(std::strtod(number.c_str(), nullptr));
        } else {
            e.count = number.empty() ? -1 : std::atoi(number.c_str());
        }
        break;
    }
    return e;
}

/// The names ALLOWED_WEAPONS and ALLOWED_ARMOR take (one table for both).
constexpr NamedBit kAllowedNames[] = {
    {0, "NO_ARMOR"},
    {0x3FFFFFF, "ALL_ARMOR"},
    {0x4C0F, "ALL_CLOTH_ARMOR"},
    {0x2090D0, "ALL_LEATHER_ARMOR"},
    {0x3CC02, "ALL_TORSO_ARMOR"},
    {0xFC0000, "ALL_HELM_ARMOR"},
    {0x3000, "ALL_ARM_ARMOR"},
    {0x1C1, "ALL_FEET_ARMOR"},
    {0x23C, "ALL_LEG_ARMOR"},
    {0x3000000, "ALL_HAND_ARMOR"},
    {0x4, "STREET_PANTS"},
    {0x8, "MEDIEVAL_PANTS"},
    {0x1, "STREET_SNEAKERS"},
    {0x40, "LEATHER_BOOTS"},
    {0x80, "LEATHER_ARMORED_BOOTS"},
    {0x100, "PLATE_BOOTS"},
    {0x10, "LEATHER_LEGGINGS"},
    {0x20, "CHAIN_LEGGINGS"},
    {0x200, "PLATE_LEGGINGS"},
    {0x400, "STREET_SHIRT"},
    {0x800, "MEDIEVAL_SHIRT"},
    {0x4000, "WIZARD_ROBE"},
    {0x8000, "LEATHER_TUNIC"},
    {0x10000, "CHAIN_TUNIC"},
    {0x20000, "PLATE_BREAST"},
    {0x1000, "LEATHER_ARMBANDS"},
    {0x2000, "PLATE_ARMS"},
    {0x2, "MEDIEVAL_CLOAK"},
    {0x1000000, "ROUND_SHIELD"},
    {0x2000000, "KITE_SHIELD"},
    {0x40000, "CHAIN_COIF"},
    {0x80000, "WIZARD_HELM"},
    {0x100000, "CONJURER_HELM"},
    {0x200000, "LEATHER_HELM"},
    {0x400000, "PLATE_HELM"},
    {0x800000, "ORNATE_HELM"},
    {0, "NO_WEAPONS"},
    {0x7FFFFFE, "ALL_WEAPONS"},
    {0x47F0000, "ALL_MAGICAL_STAVES"},
    {0x7FF8000, "ALL_STAVES"},
    {0x47F00FE, "ALL_RANGED_WEAPONS"},
    {0x1, "FLAG"},
    {0x2, "QUIVER"},
    {0x4, "BOW"},
    {0x8, "CROSSBOW"},
    {0x10, "ARROW"},
    {0x20, "BOLT"},
    {0x40, "CHAKRAM"},
    {0x80, "SHURIKEN"},
    {0x100, "SWORD"},
    {0x200, "LONG_SWORD"},
    {0x400, "GREAT_SWORD"},
    {0x800, "MACE"},
    {0x1000, "AXE"},
    {0x2000, "OGRE_AXE"},
    {0x4000, "HAMMER"},
    {0x8000, "STAFF"},
    {0x10000, "STAFF_SULPHOROUS_FLARE"},
    {0x20000, "STAFF_SULPHOROUS_SHOWER"},
    {0x40000, "STAFF_LIGHTNING"},
    {0x80000, "STAFF_FIREBALL"},
    {0x100000, "STAFF_TRIPLE_FIREBALL"},
    {0x200000, "STAFF_FORCE_OF_NATURE"},
    {0x400000, "STAFF_DEATH_RAY"},
    {0x800000, "STAFF_OBLIVION_HALBERD"},
    {0x1000000, "STAFF_OBLIVION_HEART"},
    {0x2000000, "STAFF_OBLIVION_WIERDLING"},
    {0x4000000, "STAFF_OBLIVION_ORB"},
};

/// An ALLOWED_WEAPONS / ALLOWED_ARMOR list: a name, then "+ name" or "- name"
/// (each name's bits on or off), tokens split as nextToken does. An unknown name or operator
/// fails the exe's whole file; here the list stops there.
std::uint32_t parseAllowed(std::string_view value)
{
    std::uint32_t bits = 0;
    bool add = true;
    // Alternating name, operator, name...; the first name always adds.
    for (std::string_view name = nextToken(value); !name.empty();) {
        const auto it = std::find_if(std::begin(kAllowedNames), std::end(kAllowedNames), [&](const NamedBit& e) { return e.name == name; });
        if (it == std::end(kAllowedNames)) {
            break;
        }
        bits = add ? (bits | it->bit) : (bits & ~it->bit);
        const std::string_view op = nextToken(value);
        if (op != "+" && op != "-") {
            break;
        }
        add = op == "+";
        name = nextToken(value);
    }
    return bits;
}

/// ALLOWED_POSITION: names joined by + (set_bitmask_flags_from_plus_separated_names
/// over PRIMARY, SECONDARY; case aside, "NULL" none).
std::uint8_t parseAllowedPosition(std::string_view value)
{
    std::uint8_t bits = 0;
    if (value.starts_with("NULL")) {
        return 0;
    }
    while (!value.empty()) {
        const auto plus = value.find('+');
        const std::string_view word = value.substr(0, plus);
        if (util::equalsNoCase(word, "PRIMARY")) {
            bits |= 1;
        } else if (util::equalsNoCase(word, "SECONDARY")) {
            bits |= 2;
        }
        value = plus == std::string_view::npos ? std::string_view{} : value.substr(plus + 1);
    }
    return bits;
}

template <std::size_t N>
std::string_view nameOf(const NamedBit (&table)[N], std::uint32_t bit)
{
    for (const auto& e : table) {
        if (e.bit == bit) {
            return e.name;
        }
    }
    return {};
}

template <std::size_t N>
std::uint32_t bitOf(const NamedBit (&table)[N], std::string_view name)
{
    for (const auto& e : table) {
        if (e.name == name) {
            return e.bit;
        }
    }
    return 0;
}

/// The object types content packs added, as setAddedTypeBases publishes them: each one's stock
/// type and that type's class bits, worked out once as they're set.
struct AddedTypes {
    struct Entry {
        std::string base;
        std::uint32_t weaponBit = 0;
        std::uint32_t armorBit = 0;
    };
    std::vector<std::pair<std::string, std::string>> list;
    std::unordered_map<std::string, Entry, ModifierDb::NameHash, std::equal_to<>> byName;
};

std::mutex g_addedMutex;                                     ///< (setAddedTypeBases' writers)
std::vector<std::unique_ptr<const AddedTypes>> g_addedKept; ///< every set published, never freed (a reader may hold one)
std::atomic<const AddedTypes*> g_added{nullptr};             ///< the current set; nullptr: none added

/// An added type's entry; nullptr for any other name (and always with none added).
const AddedTypes::Entry* addedType(std::string_view name)
{
    const AddedTypes* added = g_added.load(std::memory_order_acquire);
    if (!added) {
        return nullptr;
    }
    const auto it = added->byName.find(name);
    return it == added->byName.end() ? nullptr : &it->second;
}

} // namespace

void setAddedTypeBases(std::vector<std::pair<std::string, std::string>> bases)
{
    const std::lock_guard lock(g_addedMutex);
    if (bases.empty()) {
        g_added.store(nullptr, std::memory_order_release);
        return;
    }
    // (thing.bin loaded again with the same packs, or packs installed again: the set there was)
    for (const auto& kept : g_addedKept) {
        if (kept->list == bases) {
            g_added.store(kept.get(), std::memory_order_release);
            return;
        }
    }
    auto added = std::make_unique<AddedTypes>();
    added->byName.reserve(bases.size());
    for (const auto& [name, base] : bases) {
        added->byName.try_emplace(name, AddedTypes::Entry{base, bitOf(kWeaponItems, base), bitOf(kArmorItems, base)});
    }
    added->list = std::move(bases);
    g_added.store(added.get(), std::memory_order_release);
    g_addedKept.push_back(std::move(added));
}

std::string_view addedTypeBase(std::string_view name)
{
    const AddedTypes::Entry* e = addedType(name);
    return e ? std::string_view(e->base) : std::string_view();
}

std::vector<std::pair<std::string, std::string>> addedTypeBases()
{
    const AddedTypes* added = g_added.load(std::memory_order_acquire);
    return added ? added->list : std::vector<std::pair<std::string, std::string>>{};
}

EffectFn effectFn(std::string_view name)
{
    if (name.empty()) {
        return EffectFn::None;
    }
    for (const EffectEntry& entry : kEffectEntries) {
        if (entry.name == name) {
            return entry.fn;
        }
    }
    return EffectFn::Other;
}

namespace {

/// The first entry of `list` named `name`: by `index` when it was made from the list as it stands
/// (`indexedOf` entries), else by walking it.
template <class T>
const T* findNamed(const std::vector<T>& list, const ModifierDb::NameIndex& index, std::size_t indexedOf, std::string_view name)
{
    if (indexedOf == list.size()) {
        const auto it = index.find(name);
        return it == index.end() ? nullptr : &list[it->second];
    }
    for (const auto& d : list) {
        if (d.name == name) {
            return &d;
        }
    }
    return nullptr;
}

template <class T>
void indexNames(const std::vector<T>& list, ModifierDb::NameIndex& index)
{
    index.clear();
    index.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        index.try_emplace(list[i].name, static_cast<std::uint32_t>(i)); // (the first of a name kept)
    }
}

} // namespace

void ModifierDb::reindex()
{
    indexNames(weapons, weaponIndex);
    indexNames(armor, armorIndex);
    indexNames(modifiers, modifierIndex);
    modifierById.fill(-1);
    for (std::size_t i = 0; i < modifiers.size(); ++i) {
        if (modifierById[modifiers[i].id] < 0) { // (the first of an id kept)
            modifierById[modifiers[i].id] = static_cast<std::int32_t>(i);
        }
    }
    indexedOf = {weapons.size(), armor.size(), modifiers.size()};
}

const EquipDef* ModifierDb::findWeapon(std::string_view name) const
{
    if (const EquipDef* def = findNamed(weapons, weaponIndex, indexedOf[0], name)) {
        return def;
    }
    const AddedTypes::Entry* added = addedType(name); // (a pack's type without its own entry: its stock type's)
    return added ? findNamed(weapons, weaponIndex, indexedOf[0], added->base) : nullptr;
}

const EquipDef* ModifierDb::findArmor(std::string_view name) const
{
    if (const EquipDef* def = findNamed(armor, armorIndex, indexedOf[1], name)) {
        return def;
    }
    const AddedTypes::Entry* added = addedType(name);
    return added ? findNamed(armor, armorIndex, indexedOf[1], added->base) : nullptr;
}

const ModifierDef* ModifierDb::modifier(std::uint8_t id) const
{
    if (id == 0xFF) {
        return nullptr;
    }
    if (indexedOf[2] == modifiers.size()) {
        const std::int32_t i = modifierById[id];
        return i < 0 ? nullptr : &modifiers[static_cast<std::size_t>(i)];
    }
    for (const auto& m : modifiers) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

const ModifierDef* ModifierDb::findModifier(std::string_view name) const
{
    return findNamed(modifiers, modifierIndex, indexedOf[2], name);
}

ModifierDb parseModifierDb(std::string_view text)
{
    const util::RoundingScope nearest(false); // (the numbers read to nearest whatever thread reads them: util/x87.hpp)
    ModifierDb db;
    // Line-based state machine: Top expects a section name, Section an entry name or END, Entry
    // KEY = value; statements or END.
    enum class Where { Top, Section, Entry } where = Where::Top;
    std::string section;
    EquipDef* equip = nullptr;
    ModifierDef* modifier = nullptr;
    std::uint8_t nextId = 0;
    std::string pending; // an attribute not yet ended by ';'
    int lineNo = 0;
    // Split off one line at a time; blank lines are ignored in every state.
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++lineNo;
        const std::string_view line = trim(raw);
        if (line.empty()) {
            continue;
        }
        switch (where) {
        case Where::Top:
            if (line != "WEAPON_DEFINITIONS" && line != "ARMOR_DEFINITIONS" && line != "EFFECTIVENESS" &&
                line != "MATERIAL" && line != "ENCHANTMENT") {
                throw FormatError("modifier.bin line " + std::to_string(lineNo) + ": unknown section");
            }
            section = line;
            where = Where::Section;
            break;
        case Where::Section:
            if (line == "END") {
                where = Where::Top;
                break;
            }
            equip = nullptr;
            modifier = nullptr;
            if (section == "WEAPON_DEFINITIONS") {
                equip = &db.weapons.emplace_back();
                equip->name = line;
            } else if (section == "ARMOR_DEFINITIONS") {
                equip = &db.armor.emplace_back();
                equip->name = line;
            } else {
                modifier = &db.modifiers.emplace_back();
                modifier->name = line;
                modifier->section = section;
                // Modifier ids count every EFFECTIVENESS, MATERIAL and ENCHANTMENT entry in file order.
                modifier->id = nextId++;
            }
            where = Where::Entry;
            break;
        case Where::Entry: {
            if (pending.empty() && line == "END") {
                where = Where::Section;
                break;
            }
            // An attribute runs to its ';', possibly over several lines (long ALLOWED_* lists).
            pending += pending.empty() ? "" : " ";
            pending += line;
            if (pending.back() != ';') {
                break;
            }
            // A whole statement: split it at '=' into KEY and value, dropping the ';'.
            const std::string statement = std::move(pending);
            pending.clear();
            const auto eq = statement.find('=');
            if (eq == std::string::npos) {
                throw FormatError("modifier.bin line " + std::to_string(lineNo) + ": expected KEY = value, got '" +
                                  statement + "'");
            }
            const std::string_view key = trim(std::string_view(statement).substr(0, eq));
            std::string_view value = trim(std::string_view(statement).substr(eq + 1));
            value = trim(value.substr(0, value.size() - 1)); // the ';'
            // Weapon and armor entries take COLOR1-6, the modifier colour slots, CLASSUSE, DESC and their stats;
            // modifier entries (EFFECTIVENESS, MATERIAL, ENCHANTMENT) the keys after this block. Unknown keys are ignored.
            if (equip) {
                if (key.size() == 6 && key.starts_with("COLOR") && key[5] >= '1' && key[5] <= '6') {
                    equip->colors[static_cast<std::size_t>(key[5] - '1')] = parseRgb(value, lineNo);
                }
                for (std::size_t i = 0; i < std::size(kSlotKeys); ++i) {
                    if (key == kSlotKeys[i]) {
                        equip->modifierSlots[i] = parseSlot(value, lineNo);
                    }
                }
                if (key == "CLASSUSE") {
                    // WARRIOR+WIZARD+CONJURER: bit n for player class n (getProjectileClassById's class byte).
                    std::uint8_t mask = 0;
                    for (const auto& [name, bit] : {std::pair{"WARRIOR", 1}, std::pair{"WIZARD", 2}, std::pair{"CONJURER", 4}}) {
                        if (value.find(name) != std::string_view::npos) {
                            mask |= static_cast<std::uint8_t>(bit);
                        }
                    }
                    equip->classUse = mask;
                }
                if (key == "DESC") {
                    equip->desc = std::string(value);
                }
                // Numbers parse as strtof does: the leading number, anything after it ignored.
                const auto number = [&]() { return std::strtof(std::string(value).c_str(), nullptr); };
                if (key == "ARMOR_VALUE") {
                    equip->armorValue = number();
                } else if (key == "DURABILITY") {
                    equip->durability = static_cast<int>(number());
                } else if (key == "DAMAGE_MIN") {
                    equip->damageMin = static_cast<int>(number());
                } else if (key == "DAMAGE_COEFFICIENT") {
                    equip->damageCoefficient = number();
                } else if (key == "REQUIRED_STRENGTH") {
                    equip->requiredStrength = static_cast<int>(number());
                } else if (key == "RANGE") {
                    equip->range = number(); // (the WarHammer's has a note after the number)
                }
            } else if (modifier) {
                if (key == "IDENTIFYDESC") {
                    modifier->identifyDesc = std::string(value);
                // Every *EFFECT key goes into `effects`; the eight known keys also fill their typed slot.
                } else if (key.ends_with("EFFECT")) {
                    // "Name value" (the value may be missing).
                    const auto sp = value.find(' ');
                    const std::string name(trim(value.substr(0, sp)));
                    const float v = sp == std::string_view::npos ? 0.0f
                                                                 : std::strtof(std::string(trim(value.substr(sp + 1))).c_str(), nullptr);
                    modifier->effects[name] = v;
                    static constexpr std::pair<std::string_view, ModifierEffect ModifierDef::*> kSlots[] = {
                        {"ATTACKEFFECT", &ModifierDef::attack},       {"ATTACKPREHITEFFECT", &ModifierDef::preHit},
                        {"ATTACKPREDAMAGEEFFECT", &ModifierDef::preDamage}, {"DEFENDEFFECT", &ModifierDef::defend},
                        {"DEFENDCOLLIDEEFFECT", &ModifierDef::defendCollide}, {"UPDATEEFFECT", &ModifierDef::update},
                        {"ENGAGEEFFECT", &ModifierDef::engage},       {"DISENGAGEEFFECT", &ModifierDef::disengage},
                    };
                    for (const auto& [slotKey, slot] : kSlots) {
                        if (key == slotKey) {
                            modifier->*slot = parseEffect(value);
                        }
                    }
                } else if (key == "COLOR") {
                    modifier->color = parseRgb(value, lineNo);
                } else if (key == "DESC" || key == "PRIMARYDESC") {
                    modifier->desc = std::string(value);
                } else if (key == "SECONDARYDESC") {
                    modifier->secondaryDesc = std::string(value);
                } else if (key == "ALLOWED_WEAPONS") {
                    modifier->allowedWeapons = parseAllowed(value);
                } else if (key == "ALLOWED_ARMOR") {
                    modifier->allowedArmor = parseAllowed(value);
                } else if (key == "ALLOWED_POSITION") {
                    modifier->allowedPosition = parseAllowedPosition(value);
                } else if (key == "WORTH") {
                    modifier->worth = std::atoi(std::string(value).c_str());
                }
            }
            break;
        }
        }
    }
    if (where != Where::Top || !pending.empty()) {
        throw FormatError("modifier.bin ends inside a section");
    }
    db.reindex();
    return db;
}

ModifierDb loadModifierDb(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    crypto::cipherForKey(crypto::FileKey::ModifierBin).decrypt(data);
    content::applyContentPacks(content::TableFormat::Modifier, data); // (the installed content packs' changes, once, as it loads; none installed: nothing)
    return parseModifierDb(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::string_view armorItemName(std::uint32_t armorBit)
{
    return nameOf(kArmorItems, armorBit);
}

std::string_view weaponItemName(std::uint32_t weaponBit)
{
    return nameOf(kWeaponItems, weaponBit);
}

std::uint32_t weaponBitOf(std::string_view item)
{
    if (const std::uint32_t bit = bitOf(kWeaponItems, item)) { // (every entry's bit is non-zero)
        return bit;
    }
    const AddedTypes::Entry* added = addedType(item);
    return added ? added->weaponBit : 0;
}

std::uint32_t armorBitOf(std::string_view item)
{
    if (const std::uint32_t bit = bitOf(kArmorItems, item)) {
        return bit;
    }
    const AddedTypes::Entry* added = addedType(item);
    return added ? added->armorBit : 0;
}

} // namespace nox::things
