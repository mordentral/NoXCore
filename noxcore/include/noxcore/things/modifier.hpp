// modifier.bin: weapon and armour definitions, and item modifiers (materials, effectiveness,
// enchantments). Decoded: the colours drawing needs and the names tooltips use.
//
// The file is text once decrypted (key FileKey::ModifierBin), read by the exe's
// parseModifierBin:
//   SECTION                      WEAPON_DEFINITIONS, ARMOR_DEFINITIONS, EFFECTIVENESS, MATERIAL
//     Name                       or ENCHANTMENT
//       KEY = value;             up to the ';', which may be lines later
//     END
//   END
// Definitions set COLOR1..COLOR6 = r g b (material slots 1-6, black when
// absent) and name the slots an item's modifiers recolour: EFFECTIVENESS, MATERIAL,
// PRIMARYENCHANTMENT, SECONDARYENCHANTMENT = COLORn. Modifiers have
// COLOR = r g b and an id: a counter over all modifier entries in file order, as the network's
// modifier bytes use (Obj_GetModifierById; 0xFF means none).
#pragma once

#include <array>
#include <map>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nox::things {

using Rgb = std::array<std::uint8_t, 3>;

/// A weapon or armour definition's drawing colours.
struct EquipDef {
    std::string name; ///< the item's thing name
    std::array<Rgb, 6> colors{}; ///< material slots 1-6
    /// Slot (1-6) each modifier position recolours, in the network order: effectiveness, material,
    /// primary enchantment, secondary enchantment; 0 = none.
    std::array<std::uint8_t, 4> modifierSlots{};
    /// CLASSUSE: bit n for the player class that may use it (1 warrior, 2 wizard, 4 conjurer);
    /// nullopt when the definition doesn't say (anyone may).
    std::optional<std::uint8_t> classUse;
    /// DESC: its name's string id (modifier.db:LongswordDesc), the base of an item's tooltip.
    std::string desc;
    /// What the identify page works with: armour's ARMOR_VALUE (the fraction of damage
    /// it takes), DURABILITY; a weapon's DAMAGE_MIN, DAMAGE_COEFFICIENT per point of strength above
    /// REQUIRED_STRENGTH (calcBoltDamage).
    float armorValue = 0;
    int durability = 0;
    int damageMin = 0;
    float damageCoefficient = 0;
    int requiredStrength = 0;
    /// RANGE: how far a melee weapon reaches past its wielder's edge (playerAttack
    /// traces a circle of the wielder's radius plus this; a WarHammer's is round its head, 35
    /// ahead), or a missile weapon's range. 0 when not given.
    float range = 0;
};

/// One of a modifier's effects (its record's function and number): the function
/// by its modifier.bin name, and the number after it as that function's entry in the effect
/// tables (attack, defend, update and engage) reads it: a float or an int,
/// -1 when there's no number; the other stays 0. RegenerationEngage and
/// RegenerationDisengage read none.
/// The effect functions by name (the exe's effect tables, in modifier.cpp's order); None for no
/// function, Other for a name none of the tables has.
enum class EffectFn : std::uint8_t {
    None,
    Other,
    // (The attack table)
    DamageMultiplier, Stun, Fire, FireRing, BlueFireRing, Frost, Recoil, Confuse, Lightning, DrainMana, Vampirism, Poison,
    Panic, Sympathy, Readiness, ProjectileSpeed, Replenishment,
    // (The defend table)
    ArmorMultiplier, DurabilityMultiplier, Resilience, Inversion, Grip, Breaking, PunctureProne,
    // (The update table)
    RegenerationUpdate, ParasiteUpdate, AttractionUpdate, ContinualReplenishmentUpdate,
    // (The engage table)
    BrillianceEngage, BrillianceDisengage, SpeedEngage, SpeedDisengage, FireProtectEngage, FireProtectDisengage,
    LightningProtectEngage, LightningProtectDisengage, PoisonProtectEngage, PoisonProtectDisengage, RegenerationEngage,
    RegenerationDisengage,
};

/// The function a modifier.bin effect name names (exact case), as parsing sets ModifierEffect::fn.
EffectFn effectFn(std::string_view name);

struct ModifierEffect {
    std::string name; ///< "" none
    float value = 0;
    int count = 0;
    EffectFn fn = EffectFn::None; ///< `name`, named once as it's read
};

struct ModifierDef {
    std::string name;
    std::string section; ///< EFFECTIVENESS, MATERIAL or ENCHANTMENT
    std::uint8_t id = 0;
    Rgb color{};
    /// DESC (effectiveness, material) or PRIMARYDESC (enchantments): the word it adds to an item's
    /// name (string id); SECONDARYDESC: an enchantment's when it's the second one.
    std::string desc;
    std::string secondaryDesc;
    /// IDENTIFYDESC: what the identify page says of it.
    std::string identifyDesc;
    /// Its effects by name with their value (ATTACKEFFECT, DEFENDEFFECT, ATTACKPREHITEFFECT...):
    /// DamageMultiplierEffect 0.94, ArmorMultiplierEffect, DurabilityMultiplierEffect, FireEffect
    /// 3.0, LightningEffect...
    std::map<std::string, float, std::less<>> effects;
    /// The same by key, as its record keeps them (ATTACKEFFECT,
    /// ATTACKPREHITEFFECT, ATTACKPREDAMAGEEFFECT: DEFENDEFFECT,
    /// DEFENDCOLLIDEEFFECT: UPDATEEFFECT: ENGAGEEFFECT,
    /// DISENGAGEEFFECT, each with its number).
    ModifierEffect attack, preHit, preDamage, defend, defendCollide, update, engage, disengage;
    /// ALLOWED_WEAPONS and ALLOWED_ARMOR: the weapon and armour class bits
    /// (weaponBitOf / armorBitOf) it may go on, from names joined by + and - (the
    /// names in modifier.cpp's table, e.g. ALL_WEAPONS - ALL_STAVES - QUIVER); ALLOWED_POSITION:
    /// 1 PRIMARY, 2 SECONDARY. 0 when not given.
    std::uint32_t allowedWeapons = 0;
    std::uint32_t allowedArmor = 0;
    std::uint8_t allowedPosition = 0;
    /// WORTH: what it adds to an item's price (shopGetItemCost).
    int worth = 0;
};

struct ModifierDb {
    std::vector<EquipDef> weapons;
    std::vector<EquipDef> armor;
    std::vector<ModifierDef> modifiers;

    /// Each by exact name, the first of a name; modifier by id, the first of an id. (By
    /// reindex's indexes when they're of the lists as they stand, else by walking the lists.)
    /// A weapon or armour of an object type a content pack added, without an entry of its own
    /// name: its stock type's (addedTypeBase).
    const EquipDef* findWeapon(std::string_view name) const;
    const EquipDef* findArmor(std::string_view name) const;
    const ModifierDef* modifier(std::uint8_t id) const; ///< nullptr for 0xFF or unknown ids
    const ModifierDef* findModifier(std::string_view name) const; ///< as maps name them

    /// The one rebuild of every index of this table (names and ids) from the lists as they
    /// stand. parseModifierDb calls it as the file is loaded; ANYTHING that changes the lists
    /// after load must call it again: the size checks only notice entries added or removed, an
    /// in-place change of the same size (a renamed entry, a changed id) leaves them stale until
    /// this is called. (ModifierEffect::fn is set as an effect is read: one who changes an
    /// effect's name sets it too, with effectFn.) A cache of what was loaded.
    void reindex();

    /// (The finders' cache, reindex's: each name's first entry, each id's first modifier (-1 none),
    /// and how many entries of each list they were made from.)
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    using NameIndex = std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>>;
    NameIndex weaponIndex;
    NameIndex armorIndex;
    NameIndex modifierIndex;
    std::array<std::int32_t, 256> modifierById{};
    std::array<std::size_t, 3> indexedOf{static_cast<std::size_t>(-1), static_cast<std::size_t>(-1), static_cast<std::size_t>(-1)};
};

/// Parses decrypted modifier.bin text. Throws FormatError on a malformed file.
ModifierDb parseModifierDb(std::string_view text);

/// Reads and decrypts modifier.bin.
ModifierDb loadModifierDb(const std::filesystem::path& path);

/// The item drawn for an armour class bit (the exe's table), e.g. 0x20000 -> "Breastplate".
std::string_view armorItemName(std::uint32_t armorBit);
/// The item drawn for a weapon class bit (the exe's table, first match), e.g. 0x200 -> "Longsword".
std::string_view weaponItemName(std::uint32_t weaponBit);
/// An item's weapon class bit (weaponInventoryEquipFlags' table), 0
/// for none; an object type a content pack added, its stock type's (addedTypeBase).
std::uint32_t weaponBitOf(std::string_view item);
/// An item's armour class bit (unitArmorInventoryEquipFlags' table), 0 for
/// none; an object type a content pack added, its stock type's (addedTypeBase).
std::uint32_t armorBitOf(std::string_view item);

/// The object types content packs add to thing.bin (NEW <name> LIKE <type>,
/// content/thing_patch.hpp), each with the stock type it's made like (through any chain of added
/// types: NEW B LIKE A, A made LIKE Longsword, is Longsword's). The class tables (weaponBitOf,
/// armorBitOf) and modifier.bin's weapon and armour entries (findWeapon, findArmor, when it has
/// none of its own name) answer for an added type as for its stock type, so it's worn, reported,
/// drawn and used as that one is. Set as thing.bin loads with the installed packs
/// (content::applyContentPacksToThings), emptied as packs are installed; with no type added
/// nothing answers differently. Set before the game runs; read from any thread.
void setAddedTypeBases(std::vector<std::pair<std::string, std::string>> bases);
/// The stock type `name` was added like; "" for a type no pack added.
std::string_view addedTypeBase(std::string_view name);
/// Every added type with its stock type, in the order they were added.
std::vector<std::pair<std::string, std::string>> addedTypeBases();

} // namespace nox::things
