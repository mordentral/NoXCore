// The quest's reward generator, as the game's works: what each RewardMarker on a quest map turns
// into, by the stage. The tables below hold the game's own values; rewards.hpp sums up the rules. The same
// tables also decide what a quest player's saved character may bring in (questItemValid & co).
#include "noxcore/server/rewards.hpp"

#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <span>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// randInt (lo..hi; hi below lo: no draw, hi).
int randInt(Fight& fight, int lo, int hi)
{
    if (hi - lo + 1 == 0) {
        return hi;
    }
    return nox::util::uniform_int_distribution<int>(lo, hi)(fight.rng);
}

/// The item table: weight, thing ('#': a staff with Replenishment1), kind (1 weapon,
/// 2 armour, 4 potion), the stage slots it's in.
struct ItemRow {
    std::uint8_t weight;
    std::string_view name;
    std::uint8_t kind;
    std::uint8_t slots;
};
// Slots are stageSlot's five bits (1 the first stages ... 0x10 stage 10 on): 0x1F every stage,
// 0x1E from slot 2, 0x1C from slot 4, 0x18 from slot 8, 0x10 the last only.
constexpr ItemRow kItems[] = {
    {4, "FanChakram", 1, 0x1F},       {4, "Sword", 1, 0x1F},
    {4, "LongSword", 1, 0x1F},        {4, "MorningStar", 1, 0x1F},
    {4, "StaffWooden", 1, 0x1F},      {4, "LesserFireballWand", 1, 0x1F},
    {4, "BattleAxe", 1, 0x1E},        {4, "OgreAxe", 1, 0x1E},
    {4, "ForceWand", 1, 0x1E},        {4, "Quiver", 1, 0x1E},
    {4, "Bow", 1, 0x1E},              {4, "RoundChakram", 1, 0x1C},
    {4, "FireStormWand", 1, 0x1C},    {2, "DeathRayWand", 1, 0x1C},
    {1, "CrossBow", 1, 0x1C},         {2, "GreatSword", 1, 0x18},
    {2, "WarHammer", 1, 0x18},        {2, "#ForceWand", 1, 0x18},
    {2, "#LesserFireballWand", 1, 0x18}, {2, "#FireStormWand", 1, 0x18},
    {1, "#DeathRayWand", 1, 0x18},    {1, "InfinitePainWand", 1, 0x10},
    {4, "MedievalShirt", 2, 0x1F},    {4, "MedievalPants", 2, 0x1F},
    {4, "MedievalCloak", 2, 0x1F},    {4, "WizardRobe", 2, 0x1F},
    {4, "WizardHelm", 2, 0x1F},       {4, "ConjurerHelm", 2, 0x1F},
    {4, "LeatherArmbands", 2, 0x1F},  {4, "LeatherArmor", 2, 0x1F},
    {4, "LeatherHelm", 2, 0x1F},      {4, "LeatherBoots", 2, 0x1F},
    {4, "LeatherLeggings", 2, 0x1F},  {4, "WoodenShield", 2, 0x1E},
    {4, "ChainTunic", 2, 0x1E},       {4, "ChainLeggings", 2, 0x1E},
    {4, "ChainCoif", 2, 0x1E},        {4, "LeatherArmoredBoots", 2, 0x1E},
    {2, "SteelHelm", 2, 0x1E},        {2, "SteelShield", 2, 0x1E},
    {2, "Breastplate", 2, 0x1C},      {2, "OrnateHelm", 2, 0x1C},
    {2, "PlateBoots", 2, 0x1C},       {2, "PlateLeggings", 2, 0x1C},
    {2, "PlateArms", 2, 0x1C},        {12, "RedPotion", 4, 0x1F},
    {8, "BluePotion", 4, 0x1F},       {4, "CurePoisonPotion", 4, 0x1F},
    {1, "HastePotion", 4, 0x1F},      {1, "InvisibilityPotion", 4, 0x1F},
    {1, "ShieldPotion", 4, 0x1F},     {1, "VampirismPotion", 4, 0x1F},
    {1, "FireProtectPotion", 4, 0x1F}, {1, "ShockProtectPotion", 4, 0x1F},
    {1, "PoisonProtectPotion", 4, 0x1F}, {1, "InvulnerabilityPotion", 4, 0x1F},
    {1, "InfravisionPotion", 4, 0x1F},
};

/// A modifier table's row (the effectiveness, material and enchantment tables): its group (the
/// enchantments': a second must differ from the first's), name, stage slots, and the armour and
/// weapon bits it may not go on.
struct ModRow {
    std::uint8_t group;
    std::string_view name;
    std::uint8_t slots;
    std::uint32_t armorExcl;
    std::uint32_t weaponExcl;
};
// The wand staffs' weapon bits (ObjectSubclass::StaffWands): the flare, shower, lightning,
// fireball, triple fireball, force of nature and death ray staffs, and the oblivion orb.
constexpr std::uint32_t kStaves = 0x47F0000u;
constexpr ModRow kEnchantments[] = {
    {1, "Stun1", 3, 0, kStaves},           {1, "Stun2", 6, 0, kStaves},
    {1, "Stun3", 0xC, 0, kStaves},         {1, "Stun4", 0x18, 0, kStaves},
    {2, "Fire1", 3, 0, kStaves},           {2, "Fire2", 6, 0, kStaves},
    {2, "Fire3", 0xC, 0, kStaves},         {2, "Fire4", 0x18, 0, kStaves},
    {3, "Impact1", 3, 0, kStaves},         {3, "Impact2", 6, 0, kStaves},
    {3, "Impact3", 0xC, 0, kStaves},       {3, "Impact4", 0x18, 0, kStaves},
    {4, "Confuse1", 3, 0, kStaves},        {4, "Confuse2", 6, 0, kStaves},
    {4, "Confuse3", 0xC, 0, kStaves},      {4, "Confuse4", 0x18, 0, kStaves},
    {5, "Lightning1", 3, 0, kStaves},      {5, "Lightning2", 6, 0, kStaves},
    {5, "Lightning3", 0xC, 0, kStaves},    {5, "Lightning4", 0x18, 0, kStaves},
    {7, "Vampirism1", 3, 0, kStaves},      {7, "Vampirism2", 6, 0, kStaves},
    {7, "Vampirism3", 0xC, 0, kStaves},    {7, "Vampirism4", 0x18, 0, kStaves},
    {8, "Venom1", 3, 0, kStaves},          {8, "Venom2", 6, 0, kStaves},
    {8, "Venom3", 0xC, 0, kStaves},        {8, "Venom4", 0x18, 0, kStaves},
    {9, "FireProtect1", 3, 0, 0},          {9, "FireProtect2", 6, 0, 0},
    {9, "FireProtect3", 0xC, 0, 0},        {9, "FireProtect4", 0x18, 0, 0},
    {10, "LightningProtect1", 3, 0, 0},    {10, "LightningProtect2", 6, 0, 0},
    {10, "LightningProtect3", 0xC, 0, 0},  {10, "LightningProtect4", 0x18, 0, 0},
    {11, "PoisonProtect1", 3, 0, 0},       {11, "PoisonProtect2", 6, 0, 0},
    {11, "PoisonProtect3", 0xC, 0, 0},     {11, "PoisonProtect4", 0x18, 0, 0},
    {12, "Regeneration1", 3, 0, 0},        {12, "Regeneration2", 6, 0, 0},
    {12, "Regeneration3", 0xC, 0, 0},      {12, "Regeneration4", 0x18, 0, 0},
    {13, "Speed1", 3, 0, 0},               {13, "Speed2", 6, 0, 0},
    {13, "Speed3", 0xC, 0, 0},             {13, "Speed4", 0x18, 0, 0},
    {14, "Readiness1", 3, 0, 0},           {14, "Readiness2", 6, 0, 0},
    {14, "Readiness3", 0xC, 0, 0},         {14, "Readiness4", 0x18, 0, 0},
    {15, "ProjectileSpeed1", 3, 0, 0},     {15, "ProjectileSpeed2", 6, 0, 0},
    {15, "ProjectileSpeed3", 0xC, 0, 0},   {15, "ProjectileSpeed4", 0x18, 0, 0},
};
constexpr std::uint32_t kPowerExcl = kStaves | 0xCu; // (no bows or crossbows)
constexpr ModRow kWeaponPower[] = {
    {0, "WeaponPower2", 3, 0, kPowerExcl},    {0, "WeaponPower3", 6, 0, kPowerExcl},
    {0, "WeaponPower4", 0xC, 0, kPowerExcl},  {0, "WeaponPower5", 0x18, 0, kPowerExcl},
    {0, "WeaponPower6", 0x10, 0, kPowerExcl},
};
constexpr ModRow kArmorQuality[] = {
    {0, "ArmorQuality2", 3, 0, 0},   {0, "ArmorQuality3", 6, 0, 0},  {0, "ArmorQuality4", 0xC, 0, 0},
    {0, "ArmorQuality5", 0x18, 0, 0}, {0, "ArmorQuality6", 0x10, 0, 0},
};
constexpr ModRow kMaterials[] = {
    {0, "Material3", 3, 0, kPowerExcl},    {0, "Material4", 6, 0, kPowerExcl},  {0, "Material5", 0xC, 0, kPowerExcl},
    {0, "Material6", 0x18, 0, kPowerExcl}, {0, "Material7", 0x10, 0, kPowerExcl},
};

/// The spell table: weight, spell, stage slots. (Ten rows with neither weight nor
/// slots: never drawn.)
struct SpellRow {
    std::uint8_t weight;
    std::uint8_t spell;
    std::uint8_t slots;
};
constexpr SpellRow kSpells[] = {
    {4, 0x47, 0x1F}, {4, 0x1B, 0x1F}, {4, 0x32, 0x1F}, {1, 0x22, 0x1F}, {1, 0x81, 0x1F}, {4, 0x86, 0x1F},
    {4, 0x0E, 0x1F}, {4, 0x80, 0x1F}, {4, 0x34, 0x1F}, {4, 0x3A, 0x1F}, {2, 0x3C, 0x1F}, {2, 0x09, 0x1F},
    {2, 0x29, 0x1F}, {1, 0x0D, 0x1F}, {1, 0x36, 0x1F}, {1, 0x26, 0x1F}, {1, 0x2A, 0x1F}, {1, 0x48, 0x1F},
    {1, 0x3D, 0x1F}, {1, 0x3E, 0x1F}, {1, 0x40, 0x1F}, {4, 0x24, 0x1E}, {4, 0x18, 0x1E}, {4, 0x15, 0x1E},
    {4, 0x0A, 0x1E}, {1, 0x01, 0x1E}, {1, 0x08, 0x1E}, {4, 0x23, 0x1E}, {4, 0x82, 0x1E}, {2, 0x04, 0x1E},
    {2, 0x05, 0x1E}, {4, 0x2B, 0x1C}, {2, 0x43, 0x1C}, {2, 0x16, 0x1C}, {2, 0x84, 0x1C}, {1, 0x0C, 0x1C},
    {1, 0x1A, 0x1C}, {4, 0x1D, 0x1C}, {2, 0x25, 0x1C}, {1, 0x4A, 0x1C}, {4, 0x17, 0x18}, {4, 0x33, 0x18},
    {2, 0x10, 0x18}, {2, 0x27, 0x18}, {2, 0x87, 0x18}, {2, 0x88, 0x18}, {0, 0x13, 0},    {0, 0x1F, 0},
    {0, 0x21, 0},    {0, 0x28, 0},    {0, 0x38, 0},    {0, 0x41, 0},    {0, 0x42, 0},    {0, 0x46, 0},
    {0, 0x7F, 0},    {0, 0x74, 0},
};
/// The field guide table: weight, creature (kGuideNames' n), stage slots.
constexpr SpellRow kGuides[] = {
    {4, 0x20, 0x1F}, {4, 0x22, 0x1F}, {4, 0x23, 0x1F}, {4, 0x24, 0x1F}, {4, 0x11, 0x1F}, {4, 0x16, 0x1F},
    {4, 0x1A, 0x1F}, {4, 0x18, 0x1F}, {4, 0x02, 0x1E}, {4, 0x03, 0x1E}, {4, 0x21, 0x1E}, {4, 0x14, 0x1E},
    {4, 0x17, 0x1E}, {4, 0x0B, 0x1E}, {4, 0x12, 0x1E}, {4, 0x13, 0x1E}, {4, 0x1D, 0x1C}, {4, 0x0E, 0x1C},
    {4, 0x0D, 0x1C}, {4, 0x15, 0x1C}, {4, 0x04, 0x1C}, {4, 0x09, 0x1C}, {4, 0x10, 0x18}, {4, 0x0A, 0x18},
    {4, 0x1B, 0x18}, {4, 0x27, 0x18}, {4, 0x28, 0x18}, {2, 0x0F, 0x10}, {2, 0x1F, 0x10}, {1, 0x25, 0x10},
    {1, 0x26, 0x10},
};

/// server_rewardGen_pickRandomSlots: the stage's slot.
std::uint32_t stageSlot(Fight& fight, std::uint32_t stage)
{
    if (10 < stage || stage == 10) {
        return 0x10;
    }
    if (stage == 0) {
        return 1;
    }
    // Each slot's odds in percent (slots 1, 2, 4, 8, 0x10): an even stage split between two slots,
    // an odd one 75 on its own slot and 12.5 either side.
    std::array<float, 5> odds{};
    if (stage == 1) {
        odds[0] = 87.5f;
        odds[1] = 12.5f;
    } else if (stage == 9) {
        odds[3] = 12.5f;
        odds[4] = 87.5f;
    } else if ((stage & 1u) == 0) {
        odds[(stage >> 1) - 1] = 50.0f;
        odds[stage >> 1] = 50.0f;
    } else {
        const std::uint32_t h = stage >> 1;
        odds[h - 1] = 12.5f;
        odds[h] = 75.0f;
        odds[h + 1] = 12.5f;
    }
    // A roll of 0-100 in half steps, walked against the running sum.
    const float roll = static_cast<float>(randInt(fight, 0, 200)) * 0.5f;
    float sum = 0.0f;
    for (int i = 0; i < 5; ++i) {
        sum += odds[static_cast<std::size_t>(i)];
        if (roll <= sum) {
            return 1u << i;
        }
    }
    return 1;
}

bool typeExists(const Fight& fight, std::string_view name)
{
    // (getUnitDefDd10: the type's record)
    if (!name.empty() && name.front() == '#') {
        name.remove_prefix(1);
    }
    return fight.things && fight.things->findThing(name) != nullptr;
}

/// The item table's draw for `kind` at slot `slot`: its row, or none.
const ItemRow* drawItem(Fight& fight, std::uint8_t kind, std::uint32_t slot)
{
    int total = 0;
    for (const ItemRow& r : kItems) {
        if ((r.kind & kind) && (r.slots & slot) && typeExists(fight, r.name)) {
            total += r.weight;
        }
    }
    if (total == 0) {
        return nullptr;
    }
    const int roll = randInt(fight, 0, total - 1);
    int sum = 0;
    for (const ItemRow& r : kItems) {
        if ((r.kind & kind) && (r.slots & slot) && typeExists(fight, r.name)) {
            sum += r.weight;
            if (roll < sum) {
                return &r;
            }
        }
    }
    return nullptr;
}

/// A weighted table's draw (the spells' and guides'): its entry, 0 for none.
template <std::size_t N>
int drawWeighted(Fight& fight, const SpellRow (&table)[N], std::uint32_t slot)
{
    int total = 0;
    for (const SpellRow& r : table) {
        if (r.slots & slot) {
            total += r.weight;
        }
    }
    if (total == 0) {
        return 0;
    }
    const int roll = randInt(fight, 0, total - 1);
    int sum = 0;
    for (const SpellRow& r : table) {
        if (r.slots & slot) {
            sum += r.weight;
            if (roll < sum) {
                return r.spell;
            }
        }
    }
    return 0;
}

/// One of the `n` entries of a marker's list that are set, at random (0 when none).
template <std::size_t N>
int drawListed(Fight& fight, const std::array<bool, N>& list)
{
    int count = 0;
    for (const bool b : list) {
        count += b ? 1 : 0;
    }
    if (count == 0) {
        return 0;
    }
    const int k = randInt(fight, 0, count - 1);
    int seen = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (list[i]) {
            if (seen == k) {
                return static_cast<int>(i);
            }
            ++seen;
        }
    }
    return 0;
}

/// Whether a modifier row may go on the item: in `slot`, allowed on its class bit (ALLOWED_ARMOR
/// or ALLOWED_WEAPONS), not excluded, and (`position` not 0) in that position.
struct ModFit {
    const things::ModifierDb* db;
    bool armor;
    std::uint32_t bit;

    const things::ModifierDef* def(const ModRow& r) const { return db ? db->findModifier(r.name) : nullptr; }
    bool fits(const ModRow& r, std::uint32_t slot, std::uint8_t position) const
    {
        const things::ModifierDef* d = def(r);
        if (!d || !(r.slots & slot)) {
            return false;
        }
        const std::uint32_t allowed = armor ? d->allowedArmor : d->allowedWeapons;
        const std::uint32_t excl = armor ? r.armorExcl : r.weaponExcl;
        return (allowed & bit) && !(excl & bit) && (position == 0 || (d->allowedPosition & position));
    }
    template <std::size_t N>
    int count(const ModRow (&table)[N], std::uint32_t slot, std::uint8_t position = 0) const
    {
        int n = 0;
        for (const ModRow& r : table) {
            n += fits(r, slot, position) ? 1 : 0;
        }
        return n;
    }
    /// The draw: its row's index, or -1.
    template <std::size_t N>
    int draw(Fight& fight, const ModRow (&table)[N], std::uint32_t slot, std::uint8_t position = 0) const
    {
        const int n = count(table, slot, position);
        if (n == 0) {
            return -1;
        }
        const int k = randInt(fight, 0, n - 1);
        int seen = 0;
        for (std::size_t i = 0; i < N; ++i) {
            if (fits(table[i], slot, position)) {
                if (seen == k) {
                    return static_cast<int>(i);
                }
                ++seen;
            }
        }
        return -1;
    }
};

ServerObject* make(Fight& fight, std::string_view type, float x, float y)
{
    if (!fight.things || !fight.things->findThing(type)) {
        return nullptr;
    }
    return &fight.world.spawn(*fight.things, type, x, y);
}

/// rewardMakeWeapon / rewardMakeArmor.
ServerObject* makeEquipment(Fight& fight, bool armor, std::uint32_t stage, float x, float y)
{
    // The stage's slot (a single bit, 1 to 0x10), then a row of the weapon (1) or armour (2) table drawn for it.
    const std::uint32_t slot = stageSlot(fight, stage);
    const ItemRow* row = drawItem(fight, armor ? 2 : 1, slot);
    if (!row) {
        return nullptr;
    }
    std::string_view name = row->name;
    // A leading '#' marks a staff that may get Replenishment1.
    const bool replenish = name.front() == '#';
    if (replenish) {
        name.remove_prefix(1);
    }
    const ModFit fit{fight.modifiers, armor, armor ? things::armorBitOf(name) : things::weaponBitOf(name)};
    ServerObject* item = make(fight, name, x, y);
    if (!item) {
        return nullptr;
    }
    if (!armor && (item->classes & oc::Wand) && (item->subclasses & kStaves)) {
        // A staff: only Replenishment1, and only the table's '#' ones.
        if (replenish) {
            if (const things::ModifierDef* m = fight.modifiers ? fight.modifiers->findModifier("Replenishment1") : nullptr) {
                item->modifiers = {0xFF, 0xFF, m->id, 0xFF};
            }
        }
        return item;
    }
    int lo = 0;
    int hi = 0;
    // How many modifiers, by the slot (slot 1: none).
    switch (slot) {
    case 2: lo = 0; hi = 1; break;
    case 4: lo = 0; hi = 2; break;
    case 8: lo = 1; hi = 3; break;
    case 0x10: lo = 2; hi = 4; break;
    default: return item;
    }
    const int n = randInt(fight, lo, hi);
    if (n == 0) {
        return item;
    }
    // Which: 1 power or quality, 2 material, 4 an enchantment, 8 a second.
    std::uint32_t which = 0;
    if (n == 1) {
        // One: an enchantment 20 in 100, a material 50, power or quality 30.
        const int r = randInt(fight, 1, 100);
        which = r < 0x15 ? 4u : (0x32 < r ? 2u : 1u);
    } else if (n == 2) {
        // Two: power and an enchantment 12 in 100, material and an enchantment 13, power and material 75.
        const int r = randInt(fight, 1, 100);
        which = r < 0xD ? 5u : (0x19 < r ? 3u : 6u);
    } else if (n == 3) {
        which = 7;
    } else {
        which = 0xF;
    }
    const auto& first = armor ? kArmorQuality : kWeaponPower;
    // None of a kind to be had: the next kind instead.
    if ((which & 1u) && fit.count(first, slot) == 0) {
        if (!(which & 2u)) {
            which |= 2u;
        } else if (!(which & 4u)) {
            which |= 4u;
        } else if (!(which & 8u)) {
            which |= 8u;
        }
        which &= ~1u;
    }
    if ((which & 2u) && fit.count(kMaterials, slot) == 0) {
        if (!(which & 4u)) {
            which |= 4u;
        } else if (!(which & 8u)) {
            which |= 8u;
        }
        which &= ~2u;
    }
    if ((which & 4u) && fit.count(kEnchantments, slot, 1) == 0) {
        which &= ~0xCu;
    }
    if (which == 0) {
        return item;
    }
    // Modifier slots: [0] power or quality, [1] material, [2] enchantment, [3] second enchantment; 0xFF empty.
    std::array<std::uint8_t, 4> mods{0xFF, 0xFF, 0xFF, 0xFF};
    const auto idOf = [&](const ModRow& r) { return fit.def(r)->id; };
    if (which & 1u) {
        if (const int i = fit.draw(fight, first, slot); 0 <= i) {
            mods[0] = idOf(first[static_cast<std::size_t>(i)]);
        }
    }
    if (which & 2u) {
        if (const int i = fit.draw(fight, kMaterials, slot); 0 <= i) {
            mods[1] = idOf(kMaterials[static_cast<std::size_t>(i)]);
        }
    }
    // The enchantments' slot: the stage's half (1-4) and one either side, drawn; the slot taken as
    // that number, not its bit (armour's first enchantment keeps the stage's slot).
    const auto enchantSlot = [&] {
        std::uint32_t e = stage >> 1;
        if (e == 0) {
            e = 1;
        } else if (4 < e) {
            e = 4;
        }
        return static_cast<std::uint32_t>(randInt(fight, std::max(static_cast<int>(e) - 1, 1), std::min(static_cast<int>(e) + 1, 4)));
    };
    int firstEnchant = 0; // (the exe's local, 0 when there's no first: Stun1's group)
    if (which & 4u) {
        const std::uint32_t s = armor ? slot : enchantSlot();
        if (const int i = fit.draw(fight, kEnchantments, s, 1); 0 <= i) {
            mods[2] = idOf(kEnchantments[static_cast<std::size_t>(i)]);
            firstEnchant = i;
        }
    }
    if (which & 8u) {
        const std::uint32_t s = enchantSlot();
        if (const int i = fit.draw(fight, kEnchantments, s, 2); 0 <= i) {
            // A second enchantment of the first one's group is dropped.
            if (kEnchantments[static_cast<std::size_t>(firstEnchant)].group != kEnchantments[static_cast<std::size_t>(i)].group) {
                mods[3] = idOf(kEnchantments[static_cast<std::size_t>(i)]);
            }
        }
    }
    // modifSetItemAttrs.
    // Only a weapon, armour, wand or flag takes modifiers.
    if (item->classes & 0x13001000u) {
        item->modifiers = mods;
        item->touch();
    }
    return item;
}

/// rewardSpellBook.
ServerObject* makeSpellBook(Fight& fight, const ServerObject::RewardMarker& m, std::uint32_t stage, float x, float y)
{
    int spell = 0;
    if (m.listed & 1u) {
        spell = drawListed(fight, m.spells);
    } else {
        spell = drawWeighted(fight, kSpells, stageSlot(fight, stage));
    }
    if (spell == 0 || !fight.things) {
        return nullptr;
    }
    // Whose (playerCheckSpellClass: everyone's 0x1000000, a wizard's 0x2000000, a
    // conjurer's 0x4000000).
    const things::SpellDef* def = fight.things->findSpell(static_cast<std::uint32_t>(spell));
    const std::uint32_t flags = def ? def->flags : 0;
    const bool wizard = (flags & 0x1000000u) || (flags & 0x2000000u);
    const bool conjurer = (flags & 0x1000000u) || (flags & 0x4000000u);
    const char* type = nullptr;
    if (wizard && conjurer) {
        type = "CommonSpellBook";
    } else if (wizard) {
        type = "WizardSpellBook";
    } else if (conjurer) {
        type = "ConjurerSpellBook";
    } else {
        return nullptr;
    }
    ServerObject* book = make(fight, type, x, y);
    if (book) {
        book->bookEntry = static_cast<std::uint8_t>(spell);
    }
    return book;
}

/// rewardAbilityBook.
ServerObject* makeAbilityBook(Fight& fight, const ServerObject::RewardMarker& m, float x, float y)
{
    const int ability = (m.listed & 2u) ? drawListed(fight, m.abilities) : randInt(fight, 1, 5);
    if (ability == 0) {
        return nullptr;
    }
    ServerObject* book = make(fight, "AbilityBook", x, y);
    if (book) {
        book->bookEntry = static_cast<std::uint8_t>(ability);
    }
    return book;
}

/// rewardFieldGuide.
ServerObject* makeFieldGuide(Fight& fight, const ServerObject::RewardMarker& m, std::uint32_t stage, float x, float y)
{
    const int creature = (m.listed & 4u) ? drawListed(fight, m.guides) : drawWeighted(fight, kGuides, stageSlot(fight, stage));
    if (creature == 0) {
        return nullptr;
    }
    ServerObject* guide = make(fight, "FieldGuide", x, y);
    if (guide && creature <= things::kGuideCreatures) {
        guide->guideCreature = std::string(things::kGuideNames[static_cast<std::size_t>(creature - 1)]); // (guideNameByN)
    }
    return guide;
}

/// rewardMakePotion.
ServerObject* makePotion(Fight& fight, std::uint32_t stage, float x, float y)
{
    const ItemRow* row = drawItem(fight, 4, stageSlot(fight, stage));
    return row ? make(fight, row->name, x, y) : nullptr;
}

/// createGem.
ServerObject* makeGold(Fight& fight, std::uint32_t stage, float x, float y)
{
    const std::uint32_t slot = stageSlot(fight, stage);
    // Gold below slot 4, else 90 in 100; a chest or a pile, even odds.
    if (slot < 4 || randInt(fight, 1, 100) < 0x5B) {
        ServerObject* gold = make(fight, randInt(fight, 1, 2) == 1 ? "QuestGoldChest" : "QuestGoldPile", x, y);
        if (!gold) {
            return nullptr;
        }
        int lo = 100;
        int hi = 250;
        switch (slot) {
        case 2: lo = 200; hi = 500; break;
        case 4: lo = 400; hi = 1000; break;
        case 8: lo = 800; hi = 2000; break;
        case 0x10: lo = 1600; hi = 4000; break;
        default: break;
        }
        gold->goldAmount = static_cast<std::uint32_t>(randInt(fight, lo, hi));
        return gold;
    }
    // A gem: a Ruby 49 in 100, an Emerald 40, a Diamond 11.
    const int r = randInt(fight, 1, 100);
    return make(fight, r < 0x32 ? "RubyGem" : (r < 0x5A ? "EmeraldGem" : "DiamondGem"), x, y);
}

bool isMarker(const ServerObject& o) { return o.typeName == "RewardMarker" || o.typeName == "RewardMarkerPlus"; }

/// A bow or crossbow (Weapon, subclass 0xC): a Quiver with it.
bool wantsQuiver(const ServerObject& item) { return (item.classes & oc::Weapon) && (item.subclasses & 0xCu); }

bool isChest(const World& world, const ServerObject& o)
{
    const things::ThingDef* t = world.thingDb ? world.thingDb->findThing(o.typeName) : nullptr;
    return t && t->attribute("INIT").value_or("") == "ChestInit"; // (Init == initChest)
}

/// The map's objects as the exe's list holds them at its load (World::loadOrder), those still
/// there.
std::vector<ServerObject*> listed(World& world)
{
    std::vector<ServerObject*> out;
    for (const std::uint32_t e : world.loadOrder) {
        if (ServerObject* o = world.byExtent(e); o && o->holder == 0) {
            out.push_back(o);
        }
    }
    return out;
}

void remove(World& world, ServerObject& o)
{
    deleteObject(world, o); // (delayedDeleteObject)
    forgetRespawn(world, o.extent);
}

/// One marker of kind 0x80 at random an Ankh.
void placeAnkh(Fight& fight)
{
    const auto objects = listed(fight.world);
    int count = 0;
    for (const ServerObject* o : objects) {
        count += isMarker(*o) && o->rewardMarker && (o->rewardMarker->kinds & 0x80u) ? 1 : 0;
    }
    // No such marker: k is -1 and nothing is made.
    const int k = randInt(fight, 0, count - 1);
    int seen = 0;
    for (ServerObject* o : objects) {
        if (!isMarker(*o) || !o->rewardMarker || !(o->rewardMarker->kinds & 0x80u)) {
            continue;
        }
        if (seen == k) {
            if (make(fight, "Ankh", o->x, o->y)) {
                remove(fight.world, *o);
                return;
            }
        } else {
            ++seen;
        }
    }
}

/// Which markers are placed (flag 0x80) and which red potions stay.
void chooseMarkers(Fight& fight)
{
    World& world = fight.world;
    // The share: half on stage 1, else by the players in (0.4 for 1-2, 0.7 for 3-4, 1 for 5-6;
    // other counts leave it unset in the exe, an original bug; none here).
    float share = 0.0f;
    if (world.questStage == 1) {
        share = 0.5f;
    } else {
        int players = 0;
        for (const auto& [e, o] : world.objects()) {
            players += o.player && !o.gone() && o.player->questActive == 1 ? 1 : 0;
        }
        if (players == 1 || players == 2) {
            share = std::bit_cast<float>(0x3ECCCCCDu);  // 0.4
        } else if (players == 3 || players == 4) {
            share = std::bit_cast<float>(0x3F333333u);  // 0.7
        } else if (players == 5 || players == 6) {
            share = 1.0f;
        }
    }
    std::vector<ServerObject*> markers;
    std::vector<ServerObject*> potions;
    for (ServerObject* o : listed(world)) {
        if (o->typeName == "RewardMarker" && o->rewardMarker) {
            // Marker flags: 1 always placed, 0x80 chosen to be placed.
            if (o->rewardMarker->flags & 1u) {
                o->rewardMarker->flags |= 0x80u; // (always placed)
            } else {
                markers.push_back(o);
            }
        } else if (o->typeName == "RewardMarkerPlus" && o->rewardMarker) {
            o->rewardMarker->flags |= 0x80u;
        } else if (o->typeName == "RedPotion") {
            potions.push_back(o);
        }
    }
    // trunc(n x share + 0.5) of them after a shuffle (from the last down, each with one at or
    // below it).
    const auto shuffledShare = [&](std::vector<ServerObject*>& list) {
        const auto keep = static_cast<std::uint32_t>(util::ftol(util::X87::fromDouble(static_cast<double>(list.size())) * share + util::X87::fromDouble(0.5)));
        for (std::size_t i = list.size() - 1; 0 < i; --i) {
            const int j = randInt(fight, 0, static_cast<int>(i));
            std::swap(list[static_cast<std::size_t>(j)], list[i]);
        }
        return keep;
    };
    if (!markers.empty()) {
        const std::uint32_t n = shuffledShare(markers);
        for (std::uint32_t i = 0; i < n && i < markers.size(); ++i) {
            markers[i]->rewardMarker->flags |= 0x80u;
        }
    }
    if (!potions.empty()) {
        const std::uint32_t n = shuffledShare(potions);
        for (std::size_t i = n; i < potions.size(); ++i) {
            remove(world, *potions[i]);
        }
    }
}

} // namespace

ServerObject* activateRewardMarker(Fight& fight, const ServerObject& marker, std::uint32_t stage, float x, float y)
{
    if (!marker.rewardMarker) {
        return nullptr;
    }
    const auto& m = *marker.rewardMarker;
    if (marker.typeName == "RewardMarkerPlus") {
        stage += 2;
    }
    // A marker's chance 1-4 lets it give on a roll of 0-100 at most 75, 50, 25 or 5 (0: always).
    static constexpr int kOdds[5] = {0, 0x4B, 0x32, 0x19, 5};
    if (1 <= m.chance && m.chance <= 4 && kOdds[m.chance] < randInt(fight, 0, 100)) {
        return nullptr;
    }
    // The kinds' weights, by bit: 1 spell book, 2 ability book, 4 field guide, 8 weapon, 0x10 armour,
    // 0x20 gold or a gem, 0x40 potion, 0x80 gold or a gem too.
    static constexpr std::array<int, 8> kWeights{16, 2, 2, 24, 16, 23, 16, 1}; // (the exe's table)
    int total = 0;
    for (std::size_t i = 0; i < kWeights.size(); ++i) {
        total += (m.kinds & (1u << i)) ? kWeights[i] : 0;
    }
    if (total == 0) {
        return nullptr;
    }
    const int roll = randInt(fight, 1, total);
    std::uint32_t kind = 0;
    int sum = 0;
    for (std::size_t i = 0; i < kWeights.size(); ++i) {
        if (m.kinds & (1u << i)) {
            sum += kWeights[i];
            if (roll <= sum) {
                kind = 1u << i;
                break;
            }
        }
    }
    switch (kind) {
    case 1: return makeSpellBook(fight, m, stage, x, y);
    case 2: return makeAbilityBook(fight, m, x, y);
    case 4: return makeFieldGuide(fight, m, stage, x, y);
    case 8: return makeEquipment(fight, false, stage, x, y);
    case 0x10: return makeEquipment(fight, true, stage, x, y);
    case 0x40: return makePotion(fight, stage, x, y);
    default: return makeGold(fight, stage, x, y); // (0x20, 0x80)
    }
}

void questRewardPass(Fight& fight)
{
    World& world = fight.world;
    const auto stage = static_cast<std::uint32_t>(world.questStage);
    placeAnkh(fight);
    chooseMarkers(fight);
    Items items{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    // (A marker made an Ankh is still on the list till the frame's end: chosen, it gives its item
    // too, an original bug.)
    for (ServerObject* o : listed(world)) {
        if (isMarker(*o)) {
            if (o->rewardMarker && (o->rewardMarker->flags & 0x80u)) {
                if (ServerObject* item = activateRewardMarker(fight, *o, stage, o->x, o->y); item && wantsQuiver(*item)) {
                    const auto [qx, qy] = clearPointNear(world, fight.rng, item->x, item->y, 30.0f);
                    make(fight, "Quiver", qx, qy);
                }
            }
            remove(world, *o);
        // A chest's markers: each one's item a stage better, put in the chest (and a Quiver with a bow).
        } else if (isChest(world, *o)) {
            for (const std::uint32_t e : keysOf(o->inventory)) {
                ServerObject* in = world.byExtent(e);
                if (!in || !isMarker(*in)) {
                    continue;
                }
                ServerObject* item = activateRewardMarker(fight, *in, stage + 1, o->x, o->y);
                items.removeFromInventory(*o, *in);
                remove(world, *in);
                if (item) {
                    items.put(*o, *item, false);
                    if (wantsQuiver(*item)) {
                        if (ServerObject* q = make(fight, "Quiver", o->x, o->y)) {
                            items.put(*o, *q, false);
                        }
                    }
                }
            }
        }
    }
}

void giveRewards(Fight& fight, ServerObject& unit, std::uint32_t stage, int count)
{
    if (!fight.things) {
        return;
    }
    // A fresh RewardMarker (createRewardMarker: every kind, always), freed after.
    ServerObject marker;
    marker.typeName = "RewardMarker";
    marker.rewardMarker.emplace();
    Items items{fight.world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    for (int i = 0; i < count; ++i) {
        if (ServerObject* item = activateRewardMarker(fight, marker, stage, unit.x, unit.y)) {
            items.put(unit, *item, false);
        }
    }
}

bool questSpellLosable(int spell)
{
    for (const SpellRow& r : kSpells) {
        if (r.spell == spell && r.slots != 0) {
            return spell != 0 && spell != 0x22 && spell != 0x1B && spell != 9 && spell != 0x29;
        }
    }
    return false;
}

bool questGuideLosable(int creature)
{
    for (const SpellRow& r : kGuides) {
        if (r.spell == creature && r.slots != 0) {
            return creature != 0;
        }
    }
    return false;
}

namespace {

/// In the spell table with slots (as the exe walks it).
bool inSpellTable(int spell)
{
    return std::ranges::any_of(kSpells, [&](const SpellRow& r) { return r.spell == spell && r.slots != 0; });
}

/// In the guide table with slots (as the exe walks it).
bool inGuideTable(int creature)
{
    return std::ranges::any_of(kGuides, [&](const SpellRow& r) { return r.spell == creature && r.slots != 0; });
}

/// The first `n` letters alike, case aside.
bool sameStart(std::string_view a, std::string_view b, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        const char x = i < a.size() ? a[i] : '\0';
        const char y = i < b.size() ? b[i] : '\0';
        if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
    return true;
}

const ModRow* findRow(std::span<const ModRow> rows, const things::ModifierDef* m)
{
    for (const ModRow& r : rows) {
        if (m && r.name == m->name) {
            return &r;
        }
    }
    return nullptr;
}

/// The item table has a '#' row (a staff with Replenishment1) of its type.
bool replenishedStaff(const ServerObject& item)
{
    return std::ranges::any_of(kItems, [&](const ItemRow& r) { return r.name.starts_with('#') && r.name.substr(1) == item.typeName; });
}

} // namespace

bool questModifiersValid(const ServerObject& item, const things::ModifierDb& db)
{
    const bool weapon = (item.classes & oc::Weapon) != 0;
    const std::uint32_t bits = weapon ? things::weaponBitOf(item.typeName) : things::armorBitOf(item.typeName);
    const auto allowed = [&](const things::ModifierDef& m) { return ((weapon ? m.allowedWeapons : m.allowedArmor) & bits) != 0; };
    const auto excluded = [&](const ModRow& r) { return ((weapon ? r.weaponExcl : r.armorExcl) & bits) != 0; };
    if (const things::ModifierDef* m = db.modifier(item.modifiers[0])) {
        const ModRow* r = findRow(weapon ? std::span<const ModRow>(kWeaponPower) : std::span<const ModRow>(kArmorQuality), m);
        if (!r || !allowed(*m) || excluded(*r)) {
            return false;
        }
    }
    if (const things::ModifierDef* m = db.modifier(item.modifiers[1])) {
        const ModRow* r = findRow(kMaterials, m);
        if (!r || !allowed(*m) || excluded(*r)) {
            return false;
        }
    }
    for (std::size_t slot = 2; slot < 4; ++slot) {
        const things::ModifierDef* m = db.modifier(item.modifiers[slot]);
        if (!m) {
            continue;
        }
        if (!(m->allowedPosition & (slot == 2 ? 1u : 2u))) {
            return false;
        }
        const bool replenishment = m->name == "Replenishment1" || m->name == "Replenishment2" || m->name == "Replenishment3" ||
                                   m->name == "Replenishment4";
        const ModRow* r = findRow(kEnchantments, m);
        if (!r && !replenishment) {
            return false;
        }
        if (!replenishment) {
            if (!allowed(*m) || excluded(*r)) {
                return false;
            }
            continue;
        }
        if (!replenishedStaff(item)) {
            return false;
        }
    }
    return true;
}

bool questSpellValid(int spell)
{
    // In the spell table with slots, or one of 0x2E-0x31 and 0x7A-0x7D; a
    // summoning (0x4B-0x72) always.
    const bool listed = inSpellTable(spell) || (0x2E <= spell && spell <= 0x31) || (0x7A <= spell && spell <= 0x7D);
    return listed || (0x4B <= spell && spell <= 0x72);
}

bool questGuideValid(int creature)
{
    // In the guide table with slots, or in the exe's list of the spiders.
    static constexpr int kSpiders[] = {0x18, 7, 8, 0x19, 0x1A};
    return inGuideTable(creature) || std::ranges::find(kSpiders, creature) != std::end(kSpiders);
}

bool questItemValid(const ServerObject& item, const things::ModifierDb* modifiers)
{
    // The exe's quest item check.
    if (item.classes & oc::Key) {
        return false;
    }
    if (item.classes & oc::Food) {
        return (item.subclasses & 0x1FF78u) != 0;  // the potions: food subclasses but SIMPLE, APPLE, JUG and MUSHROOM
    }
    if (item.classes & oc::InfoBook) {
        // A spell book's spell in the spell table, a field guide's creature in the
        // guide table, an ability book's ability 1-5.
        if (item.subclasses & 1u) {
            return inSpellTable(item.bookEntry);
        }
        if (item.subclasses & 2u) {
            int creature = 0; // (Game_GetCreatureGuideIndex: 0 unknown)
            for (std::size_t i = 0; i < things::kGuideNames.size(); ++i) {
                if (item.guideCreature == things::kGuideNames[i]) {
                    creature = static_cast<int>(i) + 1;
                    break;
                }
            }
            return inGuideTable(creature);
        }
        return (item.subclasses & 4u) && item.bookEntry != 0 && item.bookEntry < 6;
    }
    if (item.typeName == "Diamond" || item.typeName == "Emerald" || item.typeName == "Ruby") {
        return true;
    }
    const bool listed = std::ranges::any_of(kItems, [&](const ItemRow& r) {
        return (r.name.starts_with('#') ? r.name.substr(1) : r.name) == item.typeName;
    });
    if (item.typeName == "SulphorousFlareWand" || item.typeName == "StreetSneakers" || item.typeName == "StreetShirt" ||
        item.typeName == "StreetPants") {
        // The starting wand only with Replenishment1 (its weapon bit 0x10000), the
        // street clothes (armour bits 0x405) only with their colours (UserColo...).
        // (Without modifier.bin, none to check.)
        if (modifiers && (item.classes & oc::Weapon) && (things::weaponBitOf(item.typeName) & 0x10000u)) {
            const things::ModifierDef* third = modifiers->modifier(item.modifiers[2]);
            if (item.modifiers[0] != 0xFF || item.modifiers[1] != 0xFF || !third || third->name != "Replenishment1" ||
                item.modifiers[3] != 0xFF) {
                return false;
            }
        }
        if (modifiers && (item.classes & oc::Armor) && (things::armorBitOf(item.typeName) & 0x405u)) {
            for (const std::uint8_t id : item.modifiers) {
                const things::ModifierDef* m = modifiers->modifier(id);
                if (m && !sameStart(m->name, "UserColo", 8)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (!listed) {
        return false;
    }
    if (item.classes & (oc::Weapon | oc::Armor)) {
        return !modifiers || questModifiersValid(item, *modifiers);
    }
    return true;
}

} // namespace nox::server
