// The content model's schema (schema.hpp). Which side reads each field is from NoX Core's own
// readers: the server's (noxcore/src/server, the modes) or the client's (noxsdl, noxcore/src/client).
#include "content/schema.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/util/text.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <mutex>

namespace videotools::content {

using nox::content::TableFormat;
using nox::util::equalsNoCase;

namespace {

constexpr std::string_view kMonsterStatus[] = {"DESTROY_WHEN_DEAD", "CHECK",           "CAN_BLOCK",     "CAN_DODGE",     "CAN_CAST_SPELLS",
                                               "HOLD_YOUR_GROUND",  "SUMMONED",        "ALERT",         "INJURED",       "CAN_SEE_FRIENDS",
                                               "CAN_HEAL_SELF",     "CAN_HEAL_OTHERS", "CAN_RUN",       "RUNNING",       "ALWAYS_RUN",
                                               "NEVER_RUN",         "BOT",             "MORPHED",       "ON_FIRE",       "STAY_DEAD",
                                               "FRUSTRATED"};
constexpr std::string_view kDamageTypes[] = {"BLADE",    "FLAME",  "CRUSH",   "IMPALE",      "DRAIN",  "POISON",    "DISPEL_UNDEAD",
                                             "EXPLOSION", "BITE",  "ELECTRIC", "CLAW",       "IMPACT", "LAVA",      "DEATH_MAGIC",
                                             "PLASMA",   "MANA_BOMB", "ZAP_RAY", "AIRBORNE_ELECTRIC"};
constexpr std::string_view kWeaponDamageTypes[] = {"DAMAGE_BLADE",     "DAMAGE_FLAME",   "DAMAGE_CRUSH",   "DAMAGE_IMPALE",       "DAMAGE_DRAIN",
                                                   "DAMAGE_POISON",    "DAMAGE_DISPEL_UNDEAD", "DAMAGE_EXPLOSION", "DAMAGE_BITE", "DAMAGE_ELECTRIC",
                                                   "DAMAGE_CLAW",      "DAMAGE_IMPACT",  "DAMAGE_LAVA",    "DAMAGE_DEATH_MAGIC",  "DAMAGE_PLASMA",
                                                   "DAMAGE_MANA_BOMB", "DAMAGE_ZAP_RAY", "DAMAGE_AIRBORNE_ELECTRIC"};
constexpr std::string_view kClasses[] = {"WARRIOR", "WIZARD", "CONJURER"};
constexpr std::string_view kPositions[] = {"PRIMARY", "SECONDARY"};

constexpr ReadBy S = ReadBy::Server;
constexpr ReadBy C = ReadBy::Client;
constexpr ReadBy B = ReadBy::Both;

// monster.bin (servParseMonsterDef): loaded only by hosts; the game client never reads it.
const FieldSpec kMonster[] = {
    {"HEALTH", FieldType::Int, S, NOX_TRK("content.field.monster.health", "Its health (over thing.bin's HEALTH)."), 1, 65535, {}, true, "server/monsters.cpp"},
    {"QUESTHEALTH", FieldType::Int, S, NOX_TRK("content.field.monster.questhealth", "The health a quest scales by its difficulty."), 0, 65535, {}, true, "server/quest.cpp"},
    {"SPEED", FieldType::Int, S, NOX_TRK("content.field.monster.speed", "Walking speed (32 a unit a frame)."), 0, 1000, {}, true, "server/monsters.cpp"},
    {"RUN_MULTIPLIER", FieldType::Float, S, NOX_TRK("content.field.monster.run_multiplier", "Running speed as a multiple of walking."), 0, 10, {}, true, "server/monsters.cpp"},
    {"RETREAT_RATIO", FieldType::Float, S, NOX_TRK("content.field.monster.retreat_ratio", "The share of its health at which it retreats (0 never)."), 0, 1, {}, true, "server/monsters.cpp"},
    {"RESUME_RATIO", FieldType::Float, S, NOX_TRK("content.field.monster.resume_ratio", "The share of its health at which it fights again."), 0, 1, {}, true, "server/monsters.cpp"},
    {"FLEE_RANGE", FieldType::Float, S, NOX_TRK("content.field.monster.flee_range", "How far it runs when fleeing."), 0, 2000, {}, true, "server/monsters.cpp"},
    {"STATUS", FieldType::Flags, S, NOX_TRK("content.field.monster.status", "Behaviour flags, joined by '+'."), 0, 0, kMonsterStatus, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_FRAME", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_frame", "The animation frame its blow lands on."), 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_RANGE", FieldType::Float, S, NOX_TRK("content.field.monster.melee_attack_range", "How far its blow reaches."), 0, 500, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_DAMAGE", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_damage", "Damage a blow does."), 0, 10000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_IMPACT", FieldType::Float, S, NOX_TRK("content.field.monster.melee_attack_impact", "How hard a blow knocks back."), 0, 1000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_DAMAGE_TYPE", FieldType::Enum, S, NOX_TRK("content.field.monster.melee_attack_damage_type", "The kind of damage a blow does."), 0, 0, kDamageTypes, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_MIN_DELAY", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_min_delay", "Fewest frames between blows."), 0, 3000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_MAX_DELAY", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_max_delay", "Most frames between blows."), 0, 3000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_CHANCE", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_poison_chance", "Chance (percent) a blow poisons."), 0, 100, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_STRENGTH", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_poison_strength", "Poison a blow adds."), 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_MAX", FieldType::Int, S, NOX_TRK("content.field.monster.melee_attack_poison_max", "Most poison its blows build up."), 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_STRIKE_FUNCTION", FieldType::Function, S, NOX_TRK("content.field.monster.melee_strike_function", "The engine function its blow runs (NULL none)."), 0, 0, {}, true, "server/monsters.cpp"},
    {"MISSILE_NAME", FieldType::Thing, S, NOX_TRK("content.field.monster.missile_name", "The thing it shoots (NULL none)."), 0, 0, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_RANGE", FieldType::Float, S, NOX_TRK("content.field.monster.missile_attack_range", "How far away it shoots from."), 0, 2000, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_FRAME", FieldType::Int, S, NOX_TRK("content.field.monster.missile_attack_frame", "The animation frame its shot leaves on."), 0, 255, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_MIN_DELAY", FieldType::Int, S, NOX_TRK("content.field.monster.missile_attack_min_delay", "Fewest frames between shots."), 0, 3000, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_MAX_DELAY", FieldType::Int, S, NOX_TRK("content.field.monster.missile_attack_max_delay", "Most frames between shots."), 0, 3000, {}, true, "server/monsters.cpp"},
    {"MOVE_SOUND_FRAME_A", FieldType::Int, S, NOX_TRK("content.field.monster.move_sound_frame_a", "A walking frame that plays its step sound."), 0, 255, {}, true, "server/monsters.cpp"},
    {"MOVE_SOUND_FRAME_B", FieldType::Int, S, NOX_TRK("content.field.monster.move_sound_frame_b", "The other walking frame with a step sound."), 0, 255, {}, true, "server/monsters.cpp"},
    {"DIE_FUNCTION", FieldType::Function, S, NOX_TRK("content.field.monster.die_function", "The engine function run when it dies (NULL none)."), 0, 0, {}, true, "server/monsters.cpp"},
    {"DEAD_FUNCTION", FieldType::Function, S, NOX_TRK("content.field.monster.dead_function", "The engine function run while it's dead (NULL none)."), 0, 0, {}, true, "server/monsters.cpp"},
    {"EXPERIENCE", FieldType::Int, S, NOX_TRK("content.field.monster.experience", "Read only by mods (nox.monsterInfo): the experience a kill gives is thing.bin's EXPERIENCE."), 0, 10000000, {}, true,
     "modes/script/src/scripted_mode.cpp"},
};

// soundset.bin (parseSoundSetBin): only hosts read it; they send the chosen sound's id.
#define NOX_SLOT(k, d) {k, FieldType::Sound, S, d, 0, 0, {}, false, "server/sound.cpp, server/monsters.cpp"}
const FieldSpec kSoundSet[] = {
    NOX_SLOT("LISTEN_SOUND", NOX_TRK("content.field.soundset.listen_sound", "When it hears something.")),
    NOX_SLOT("TAUNT_SOUND", NOX_TRK("content.field.soundset.taunt_sound", "After its blow lands.")),
    NOX_SLOT("THREATEN_SOUND", NOX_TRK("content.field.soundset.threaten_sound", "When it threatens.")),
    NOX_SLOT("IDLE_SOUND", NOX_TRK("content.field.soundset.idle_sound", "Now and then while idle.")),
    NOX_SLOT("ENGAGE_SOUND", NOX_TRK("content.field.soundset.engage_sound", "When it starts a fight.")),
    NOX_SLOT("MELEE_ATTACK_INIT_SOUND", NOX_TRK("content.field.soundset.melee_attack_init_sound", "As it winds up a blow.")),
    NOX_SLOT("MELEE_ATTACK_SOUND", NOX_TRK("content.field.soundset.melee_attack_sound", "As it strikes.")),
    NOX_SLOT("MELEE_ATTACK_HITTING_SOUND", NOX_TRK("content.field.soundset.melee_attack_hitting_sound", "When its blow hits.")),
    NOX_SLOT("MELEE_ATTACK_MISSING_SOUND", NOX_TRK("content.field.soundset.melee_attack_missing_sound", "When its blow misses.")),
    NOX_SLOT("MISSILE_ATTACK_INIT_SOUND", NOX_TRK("content.field.soundset.missile_attack_init_sound", "As it aims a shot.")),
    NOX_SLOT("MISSILE_ATTACK_SOUND", NOX_TRK("content.field.soundset.missile_attack_sound", "As it shoots.")),
    NOX_SLOT("FLEE_SOUND", NOX_TRK("content.field.soundset.flee_sound", "When it flees.")),
    NOX_SLOT("RETREAT_SOUND", NOX_TRK("content.field.soundset.retreat_sound", "When it retreats.")),
    NOX_SLOT("SPELL_INIT_SOUND", NOX_TRK("content.field.soundset.spell_init_sound", "As it starts a spell.")),
    NOX_SLOT("DIE_SOUND", NOX_TRK("content.field.soundset.die_sound", "When it dies.")),
    NOX_SLOT("INJURED_SOUND", NOX_TRK("content.field.soundset.injured_sound", "When it's hurt.")),
    NOX_SLOT("RECOGNIZE_SOUND", NOX_TRK("content.field.soundset.recognize_sound", "When it sees an enemy.")),
    NOX_SLOT("MOVE_SOUND", NOX_TRK("content.field.soundset.move_sound", "Its steps (on MOVE_SOUND_FRAME_A and _B).")),
};
#undef NOX_SLOT

// modifier.bin weapon and armour definitions.
const FieldSpec kEquip[] = {
    {"COLOR1", FieldType::Rgb, C, NOX_TRK("content.field.equip.color1", "Material colour 1 (r g b) the item's grey parts are tinted with."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR2", FieldType::Rgb, C, NOX_TRK("content.field.equip.color2", "Material colour 2."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR3", FieldType::Rgb, C, NOX_TRK("content.field.equip.color3", "Material colour 3."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR4", FieldType::Rgb, C, NOX_TRK("content.field.equip.color4", "Material colour 4."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR5", FieldType::Rgb, C, NOX_TRK("content.field.equip.color5", "Material colour 5."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR6", FieldType::Rgb, C, NOX_TRK("content.field.equip.color6", "Material colour 6."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"EFFECTIVENESS", FieldType::Slot, C, NOX_TRK("content.field.equip.effectiveness", "The colour slot its effectiveness modifier recolours."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"MATERIAL", FieldType::Slot, C, NOX_TRK("content.field.equip.material", "The colour slot its material modifier recolours."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"PRIMARYENCHANTMENT", FieldType::Slot, C, NOX_TRK("content.field.equip.primaryenchantment", "The colour slot its first enchantment recolours."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"SECONDARYENCHANTMENT", FieldType::Slot, C, NOX_TRK("content.field.equip.secondaryenchantment", "The colour slot its second enchantment recolours."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"CLASSUSE", FieldType::Flags, B,
     NOX_TRK("content.field.equip.classuse",
             "The classes that may use it, joined by '+' (absent: anyone). The host decides who may equip it; a player's own copy picks the Caution cursor and the automatic "
             "switch to another weapon, so a player without the pack sees those differ (no desync)."),
     0, 0, kClasses, false, "server/items.cpp; noxsdl/src/main.cpp (cursor); client/headless_client.cpp", false, true},
    {"DESC", FieldType::StringId, B, NOX_TRK("content.field.equip.desc", "Its name's string id."), 0, 0, {}, false, "server/items.cpp (messages); noxsdl/src/item_names.cpp"},
    {"ARMOR_VALUE", FieldType::Float, B, NOX_TRK("content.field.equip.armor_value", "Armour: the share of a blow's damage it stops."), 0, 1, {}, false, "server/combat.cpp; noxsdl/src/inventory.cpp"},
    {"DURABILITY", FieldType::Int, S, NOX_TRK("content.field.equip.durability", "Its health in network games."), 0, 65535, {}, false, "server/world.cpp"},
    {"DAMAGE_MIN", FieldType::Int, B, NOX_TRK("content.field.equip.damage_min", "Weapon: its least damage."), 0, 10000, {}, false, "server/combat.hpp; noxsdl/src/inventory.cpp"},
    {"DAMAGE_COEFFICIENT", FieldType::Float, B, NOX_TRK("content.field.equip.damage_coefficient", "Weapon: damage added per point of strength over REQUIRED_STRENGTH."), 0, 100, {}, false,
     "server/combat.hpp; noxsdl/src/inventory.cpp"},
    {"REQUIRED_STRENGTH", FieldType::Int, B, NOX_TRK("content.field.equip.required_strength", "Weapon: the strength needed to wield it."), 0, 1000, {}, false, "server/items.cpp; noxsdl/src/inventory.cpp"},
    {"RANGE", FieldType::Float, B, NOX_TRK("content.field.equip.range", "Weapon: how far a melee blow reaches past its wielder's edge, or a missile weapon's range. (Bots plan with their own copy.)"), 0,
     2000, {}, false, "server/player.cpp; bot/bot.cpp"},
    {"DAMAGE_TYPE", FieldType::Enum, ReadBy::Unknown,
     NOX_TRK("content.field.equip.damage_type",
             "Weapon: a damage type. The original game's reader takes it (a name it doesn't know fails the whole file); nothing in NoX Core uses it."),
     0, 0,
     kWeaponDamageTypes, false, "", false, true},
};

// modifier.bin modifiers (EFFECTIVENESS, MATERIAL, ENCHANTMENT). Their order is their ids.
const FieldSpec kModifier[] = {
    {"COLOR", FieldType::Rgb, C, NOX_TRK("content.field.modifier.color", "The colour it tints an item's slot with (r g b)."), 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"DESC", FieldType::StringId, B, NOX_TRK("content.field.modifier.desc", "The word it adds to an item's name (string id)."), 0, 0, {}, false, "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"PRIMARYDESC", FieldType::StringId, B, NOX_TRK("content.field.modifier.primarydesc", "Enchantments: the word it adds as the first one."), 0, 0, {}, false, "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"SECONDARYDESC", FieldType::StringId, B, NOX_TRK("content.field.modifier.secondarydesc", "Enchantments: the word it adds as the second one."), 0, 0, {}, false,
     "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"IDENTIFYDESC", FieldType::StringId, C, NOX_TRK("content.field.modifier.identifydesc", "What the identify page says of it."), 0, 0, {}, false, "noxsdl/src/inventory.cpp"},
    {"ATTACKEFFECT", FieldType::Effect, B, NOX_TRK("content.field.modifier.attackeffect", "On attack: an effect function and its number (the number also shows on the identify page)."), 0, 0, {}, false,
     "server/item_effects.cpp; noxsdl/src/inventory.cpp"},
    {"ATTACKPREHITEFFECT", FieldType::Effect, B, NOX_TRK("content.field.modifier.attackprehiteffect", "Before a hit lands: stun, fire, push..."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"ATTACKPREDAMAGEEFFECT", FieldType::Effect, B, NOX_TRK("content.field.modifier.attackpredamageeffect", "Before damage: vampirism, poison..."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"DEFENDEFFECT", FieldType::Effect, B, NOX_TRK("content.field.modifier.defendeffect", "When the wearer is hit: armour or durability multipliers."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"DEFENDCOLLIDEEFFECT", FieldType::Effect, S, NOX_TRK("content.field.modifier.defendcollideeffect", "When something touches the wearer."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"UPDATEEFFECT", FieldType::Effect, S, NOX_TRK("content.field.modifier.updateeffect", "Every so often while worn (regeneration...)."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"ENGAGEEFFECT", FieldType::Effect, S, NOX_TRK("content.field.modifier.engageeffect", "When put on (speed...)."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"DISENGAGEEFFECT", FieldType::Effect, S, NOX_TRK("content.field.modifier.disengageeffect", "When taken off."), 0, 0, {}, false, "server/item_effects.cpp"},
    {"ALLOWED_WEAPONS", FieldType::Allowed, S, NOX_TRK("content.field.modifier.allowed_weapons", "The weapons random rewards may put it on (names joined by + and -)."), 0, 0, {}, false, "server/rewards.cpp"},
    {"ALLOWED_ARMOR", FieldType::Allowed, S, NOX_TRK("content.field.modifier.allowed_armor", "The armour random rewards may put it on."), 0, 0, {}, false, "server/rewards.cpp"},
    {"ALLOWED_POSITION", FieldType::Flags, S, NOX_TRK("content.field.modifier.allowed_position", "PRIMARY, SECONDARY or both (joined by '+'), or NULL."), 0, 0, kPositions, false, "server/rewards.cpp"},
    {"WORTH", FieldType::Int, S, NOX_TRK("content.field.modifier.worth", "What it adds to an item's price."), 0, 1000000, {}, false, "server/trade.cpp"},
};

// gamedata.bin's names NoX Core reads, in groups that share a side and an explanation.
struct NameGroup {
    std::vector<std::string_view> names;
    ReadBy readBy;
    nox::l10n::Key docs;
};

const NameGroup kGameData[] = {
    {{"WarriorMaxHealth", "WizardMaxHealth", "ConjurerMaxHealth"}, B, NOX_TRK("content.field.gamedata.warrior_max_health", "The class's top health (the stats page shows it).")},
    {{"WarriorMaxMana", "WizardMaxMana", "ConjurerMaxMana"}, B, NOX_TRK("content.field.gamedata.warrior_max_mana", "The class's top mana.")},
    {{"WarriorMaxStrength", "WizardMaxStrength", "ConjurerMaxStrength"}, B, NOX_TRK("content.field.gamedata.warrior_max_strength", "The class's top strength.")},
    {{"WarriorMaxSpeed", "WizardMaxSpeed", "ConjurerMaxSpeed"}, B, NOX_TRK("content.field.gamedata.warrior_max_speed", "The class's top speed.")},
    {{"BaseHealth", "BaseMana", "BaseStrength", "BaseSpeed"}, S, NOX_TRK("content.field.gamedata.base_health", "Solo: the level-1 stats every class starts from.")},
    {{"XPTable"}, B, NOX_TRK("content.field.gamedata.xp_table", "The experience each level needs (the stats page shows the next one).")},
    {{"BerserkerChargeDelay", "WarcryDelay", "HarpoonDelay", "TreadLightlyDelay", "EyeOfTheWolfDelay"}, B,
     NOX_TRK("content.field.gamedata.berserker_charge_delay", "Frames before the warrior ability is ready again (the HUD's cooldown shows it).")},
    {{"BerserkerChargeDuration", "WarCryDuration", "HarpoonDuration", "TreadLightlyDuration", "EyeOfTheWolfDuration"}, B,
     NOX_TRK("content.field.gamedata.berserker_charge_duration", "How long the ability lasts (frames). (Bots read their own copy.)")},
    {{"BerserkerDamage", "BerserkerStunDuration", "BerserkerPainRatio"}, S, NOX_TRK("content.field.gamedata.berserker_damage", "Berserker Charge: its hit's damage; the stun and the share of health lost hitting a wall.")},
    {{"MaxHarpoonDistance", "MinHarpoonDistance", "MaxHarpoonExistence", "HarpoonDamage", "HarpoonForce"}, S, NOX_TRK("content.field.gamedata.max_harpoon_distance", "The harpoon's reach, life, damage and pull.")},
    {{"MaxHarpoonFlightDistance"}, B, NOX_TRK("content.field.gamedata.max_harpoon_flight_distance", "The farthest a harpoon flies. (Bots plan with their own copy.)")},
    {{"ItemDamagePercentage", "ItemDamageFromBlockPercentage"}, S, NOX_TRK("content.field.gamedata.item_damage_percentage", "The share of a blow's damage worn items (or a blocking shield) take.")},
    {{"FireSpellProtection", "ElectricitySpellProtection", "PoisonSpellProtection"}, S, NOX_TRK("content.field.gamedata.fire_spell_protection", "Protection enchantments' reduction, by power.")},
    {{"ShockDamage", "VampirismCoeff"}, S, NOX_TRK("content.field.gamedata.shock_damage", "Shock's discharge damage; Vampirism's share of damage returned, by power.")},
    {{"FieldGuideDamageBonus"}, S, NOX_TRK("content.field.gamedata.field_guide_damage_bonus", "Solo: damage multiplier against creatures in your field guide.")},
    {{"BoltSoloDamageMin"}, S, NOX_TRK("content.field.gamedata.bolt_solo_damage_min", "Solo: an archer bolt's least damage.")},
    {{"OblivionStaffRechargeRate", "ForceOfNatureStaffLimit"}, S, NOX_TRK("content.field.gamedata.oblivion_staff_recharge_rate", "Oblivion staves' recharge frames; the most Force of Nature staves one player may hold.")},
    {{"DefaultAmmoAmount", "DefaultAmmoAmountQuest"}, S, NOX_TRK("content.field.gamedata.default_ammo_amount", "Arrows or chakrams in a new quiver.")},
    {{"QuestDurabilityMultiplier", "QuestStaffChargeMultiplier"}, S, NOX_TRK("content.field.gamedata.quest_durability_multiplier", "Quest: weapon and armour durability, staff charges.")},
    {{"ArrowTrapDamage", "ToxicCloudLifetime", "SmallToxicCloudLifetime"}, S, NOX_TRK("content.field.gamedata.arrow_trap_damage", "Arrow traps' damage; toxic clouds' seconds.")},
    {{"MaxTrapCount", "MaxBomberCount", "GlyphRange"}, S, NOX_TRK("content.field.gamedata.max_trap_count", "The most glyph traps and bombers a player may have; a glyph's reach.")},
    {{"EnergyBoltTrapCost", "LightningTrapCost", "ManaBombTrapCost"}, B, NOX_TRK("content.field.gamedata.energy_bolt_trap_cost", "Mana the spell costs in a trap (the quick bar shades by it).")},
    {{"UrchinStoneDamage", "ImpShotDamage"}, S, NOX_TRK("content.field.gamedata.urchin_stone_damage", "Thrown stones' and imp shots' damage.")},
    {{"DeathBallCollideDamage", "DeathBallCancelRange", "DeathBallOutRadius", "DeathBallInRadius", "DeathBallNearbyDamage"}, S, NOX_TRK("content.field.gamedata.death_ball_collide_damage", "The death ball's damage and blast.")},
    {{"MagicMissileDamage", "MagicMissileSplashDamage", "MagicMissileRange", "MagicMissilePushRange", "MagicMissileForce"}, S, NOX_TRK("content.field.gamedata.magic_missile_damage", "Magic missiles' damage, seek range and push.")},
    {{"MagicMissileCount", "PixieCount"}, B, NOX_TRK("content.field.gamedata.magic_missile_count", "Missiles or pixies per cast and the most out, by power (the spell greys at the cap).")},
    {{"PixieReturnTimeout", "TargetedSpellLifetime", "UnTargetedSpellLifetime"}, S, NOX_TRK("content.field.gamedata.pixie_return_timeout", "Seconds before pixies and homing spells give up.")},
    {{"FireballSpeedCoeff", "PullPowerCoeff", "PushPowerCoeff"}, S, NOX_TRK("content.field.gamedata.fireball_speed_coeff", "Fireball speed; Pull and Push force, by power.")},
    {{"EarthquakeRange", "EarthquakeDamage", "EarthquakeJiggle"}, S, NOX_TRK("content.field.gamedata.earthquake_range", "Earthquake's radius, damage and screen shake.")},
    {{"DeathRayOutRadius", "DeathRayInRadius", "DeathRayDamage"}, S, NOX_TRK("content.field.gamedata.death_ray_out_radius", "Death Ray's radii and damage.")},
    {{"BurnDuration", "FistOfVengeanceDamage", "FistSpeed", "MeteorDamage", "MeteorSpeed"}, S, NOX_TRK("content.field.gamedata.burn_duration", "Burning flames' life; Fist of Vengeance and Meteor.")},
    {{"CounterspellRange", "InversionRange"}, S, NOX_TRK("content.field.gamedata.counterspell_range", "Counterspell's and Inversion's radius.")},
    {{"ShockTrapDamage", "PlasmaSearchTime", "PlasmaDamage", "PlasmaDamageHecubah", "LesserHealAmount", "TurnUndeadKillPoints"}, S,
     NOX_TRK("content.field.gamedata.shock_trap_damage", "Shock in a trap; Plasma; Lesser Heal; the undead health Turn Undead destroys.")},
    {{"ManaBombInitPower", "ManaBombDeltaPower", "ManaBombGlyphDuration", "ManaBombInRadius", "ManaBombShakeMag"}, S, NOX_TRK("content.field.gamedata.mana_bomb_init_power", "Mana Bomb's charge, blast and shake.")},
    {{"ManaBombOutRadius"}, B, NOX_TRK("content.field.gamedata.mana_bomb_out_radius", "Mana Bomb's outer blast radius (the charging sparks' ring shows it).")},
    {{"LightningRange", "LightningGlyphDamage", "EnergyBoltGlyphDamage", "LightningDamage", "LightningSearchTime", "EnergyBoltDamage"}, S,
     NOX_TRK("content.field.gamedata.lightning_range", "Lightning and Energy Bolt: reach, damage, how long a bolt keeps striking.")},
    {{"ManaDrainCoeff", "ChannelLifeCoeff", "TeleportDelay"}, S, NOX_TRK("content.field.gamedata.mana_drain_coeff", "Drain Mana, Channel Life, the delay before a teleport.")},
    {{"ManaDrainRange"}, B, NOX_TRK("content.field.gamedata.mana_drain_range", "Drain Mana's reach. (Bots plan with their own copy.)")},
    {{"FearEnchantDuration", "AnchorEnchantDuration", "ConfuseEnchantDuration", "HasteEnchantDuration", "InfravisionEnchantDuration",
      "InvisibilityEnchantDuration", "InvulnerabilityEnchantDuration", "LightEnchantDuration", "ProtectElectricityEnchantDuration",
      "ProtectFireEnchantDuration", "ProtectPoisonEnchantDuration", "RunEnchantDuration", "ShockEnchantDuration", "SlowEnchantDuration",
      "StunEnchantDuration", "VampirismEnchantDuration", "MoonglowEnchantmentDuration"},
     S, NOX_TRK("content.field.gamedata.fear_enchant_duration", "The enchantment's length in frames.")},
    {{"QuestVampDurMultiplier"}, S, NOX_TRK("content.field.gamedata.quest_vamp_dur_multiplier", "Quest: Vampirism's length factor.")},
    {{"SummonDuration", "CharmSmallDuration", "CharmMediumDuration", "CharmLargeDuration"}, S, NOX_TRK("content.field.gamedata.summon_duration", "Summons' length by creature size; Charm's by size and power.")},
    {{"ShieldDuration", "ShieldHealth", "TagDurationPerLevel"}, S, NOX_TRK("content.field.gamedata.shield_duration", "Force Field's length and hit points; Mark's frames per power.")},
    {{"ZombieDeadDuration"}, S, NOX_TRK("content.field.gamedata.zombie_dead_duration", "The fewest and most frames a zombie stays down.")},
    {{"GeneratorMaxActiveCreaturesHigh", "GeneratorMaxActiveCreaturesNormal", "GeneratorMaxActiveCreaturesLow", "GeneratorMaxActiveCreaturesSingular",
      "MaxOnscreenMonsterCount"},
     S, NOX_TRK("content.field.gamedata.generator_max_active_creatures_high", "The most live creatures a generator keeps, by its setting; the cap on nearby monsters.")},
    {{"SpawnRateHighValue", "SpawnRateNormalValue", "SpawnRateLowValue", "SpawnRateVeryLowValue", "SpawnRateVeryVeryLowValue"}, S,
     NOX_TRK("content.field.gamedata.spawn_rate_high_value", "A generator's spawn interval by its rate setting.")},
    {{"QuestHardcoreStage", "QuestHardcoreSpawnRateIncrease", "QuestHardcoreSpawnCap", "GeneratorMaxHealth"}, S, NOX_TRK("content.field.gamedata.quest_hardcore_stage", "Quest: hardcore spawning; generators' health cap.")},
    {{"SuddenDeathCountdown", "SuddenDeathPlayerThreshold", "SuddenDeathLifeTime"}, S, NOX_TRK("content.field.gamedata.sudden_death_countdown", "Sudden death's timing and trigger.")},
    {{"KotRKingKillsPawnPoints", "KotRPawnKillsKingPoints", "KotRKingKillsKingPoints"}, S, NOX_TRK("content.field.gamedata.kotr_king_kills_pawn_points", "King of the Realm scores.")},
    {{"FlagballPossDuration", "FlagballResetVel"}, S, NOX_TRK("content.field.gamedata.flagball_poss_duration", "Flagball: frames a player may hold it; its reset speed.")},
    {{"CamperRadiusSq", "CamperStartTime", "CamperFadeTime"}, S, NOX_TRK("content.field.gamedata.camper_radius_sq", "Anti-camping: radius (squared) and timers.")},
    {{"MaxExtraLives", "QuestGameStartingExtraLives"}, S, NOX_TRK("content.field.gamedata.max_extra_lives", "Quest: the most extra lives; lives at the start.")},
    {{"PlayerDifficultyDelta", "PlayerDamageDiffCoeff", "PlayerDamageDiffInit", "PlayerDamageCap", "SystemHealthDiffCoeff", "SystemHealthDiffInit",
      "SystemHealthCap"},
     S, NOX_TRK("content.field.gamedata.player_difficulty_delta", "Quest difficulty: per extra player; damage players take; monsters' health.")},
    {{"MinionsAlwaysStage", "WarpGateCutoffStage", "QuestExitTimerStart"}, S, NOX_TRK("content.field.gamedata.minions_always_stage", "Quest stages: bosses always, warp gates close; the exit countdown.")},
    {{"QuestSpellWorthMultiplier", "QuestGuideWorthMultiplier", "QuestModifierWorthMultiplier", "QuestSellMultiplier", "ShopAnkhCutoffStage",
      "RepairCoefficient"},
     S, NOX_TRK("content.field.gamedata.quest_spell_worth_multiplier", "Quest shops' prices; the stage shops stop selling ankhs; repair prices.")},
};

// thing.bin THNG attributes (things/thing_db's text lines and its binary ones), by who reads them.
#define NOX_T(k, ty, side, d, r) {k, FieldType::ty, side, d, 0, 0, {}, false, r, false}
#define NOX_TS(k, ty, side, d, r) {k, FieldType::ty, side, d, 0, 0, {}, false, r, true}
const FieldSpec kThing[] = {
    NOX_TS("CLASS", Text, B, NOX_TRK("content.field.thing.class", "Its classes, joined by '+' (MONSTER, WEAPON, IMMOBILE...). They decide which of a map's objects each player loads itself and which the server sends: a player without the same value can see an object twice or not at all."),
           "server/world.cpp; noxsdl/src/map_view.cpp"),
    NOX_T("SUBCLASS", Text, B, NOX_TRK("content.field.thing.subclass", "Its subclasses, joined by '+' (SWORD, SMALL_MONSTER...)."), "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_TS("FLAGS", Text, B, NOX_TRK("content.field.thing.flags", "Object flags, joined by '+' (NO_COLLIDE, SHADOW...); with CLASS, which objects players load themselves."),
           "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_TS("COLLIDE", Text, S, NOX_TRK("content.field.thing.collide", "Its collision function; part of the test of which map objects players load themselves."), "server/world.cpp"),
    NOX_TS("XFER", Text, B, NOX_TRK("content.field.thing.xfer", "How a map stores the object's record: every map's objects of this type are read by it, on both sides."), "map/xfer.cpp"),
    NOX_T("EXTENT", Text, B, NOX_TRK("content.field.thing.extent", "Its collision shape (CIRCLE r, BOX w h); players use their copy only for drawing."), "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_T("WEIGHT", Int, B, NOX_TRK("content.field.thing.weight", "Its weight."), "server/world.cpp; noxsdl/src/inventory.cpp"),
    NOX_T("MASS", Float, S, NOX_TRK("content.field.thing.mass", "How hard it is to push."), "server/world.cpp"),
    NOX_T("SPEED", Text, S, NOX_TRK("content.field.thing.speed", "Its speed."), "server/world.cpp"),
    NOX_T("MATERIAL", Text, S, NOX_TRK("content.field.thing.material", "What it's made of: picks its impact and drop sounds."), "server/world.cpp"),
    NOX_T("CARRYCAPACITY", Int, S, NOX_TRK("content.field.thing.carrycapacity", "How much it can carry."), "server/world.cpp"),
    NOX_T("HEALTH", Int, S, NOX_TRK("content.field.thing.health", "Its health (a monster's comes from monster.bin)."), "server/world.cpp"),
    NOX_T("EXPERIENCE", Int, S, NOX_TRK("content.field.thing.experience", "The experience a kill gives."), "server/world.cpp"),
    NOX_T("WORTH", Int, S, NOX_TRK("content.field.thing.worth", "Its price in shops."), "server/trade.cpp"),
    NOX_T("PICKUP", Function, S, NOX_TRK("content.field.thing.pickup", "Its pick-up function."), "server/world.cpp"),
    NOX_T("DROP", Function, S, NOX_TRK("content.field.thing.drop", "Its drop function."), "server/world.cpp"),
    NOX_T("USE", Function, S, NOX_TRK("content.field.thing.use", "Its use function (wands: charges, missile, rate, sound)."), "server/world.cpp"),
    NOX_T("INIT", Function, S, NOX_TRK("content.field.thing.init", "Its init function."), "server/world.cpp"),
    NOX_T("CREATE", Function, S, NOX_TRK("content.field.thing.create", "Its create function."), "server/world.cpp"),
    NOX_T("UPDATE", Function, S, NOX_TRK("content.field.thing.update", "Its update function (and its numbers)."), "server/world.cpp"),
    NOX_T("DIE", Function, S, NOX_TRK("content.field.thing.die", "Its death function."), "server/world.cpp"),
    NOX_T("DAMAGE", Function, S, NOX_TRK("content.field.thing.damage", "Its damage function."), "server/world.cpp"),
    NOX_T("DAMAGESOUND", Function, S, NOX_TRK("content.field.thing.damagesound", "The sound function when it's hit."), "server/world.cpp"),
    NOX_T("SIZE", Text, C, NOX_TRK("content.field.thing.size", "Its drawing box (w h)."), "noxsdl/src/thing_art.cpp"),
    NOX_T("Z", Int, C, NOX_TRK("content.field.thing.z", "How high it's drawn."), "noxsdl/src/thing_art.cpp"),
    NOX_T("ZSIZE", Text, C, NOX_TRK("content.field.thing.zsize", "Its drawing height range."), "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTCOLOR", Text, C, NOX_TRK("content.field.thing.lightcolor", "The colour of the light it gives (r g b)."), "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTINTENSITY", Float, C, NOX_TRK("content.field.thing.lightintensity", "How bright its light is."), "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTPENUMBRA", Text, C, NOX_TRK("content.field.thing.lightpenumbra", "Its light's soft edge."), "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTDIRECTION", Text, C, NOX_TRK("content.field.thing.lightdirection", "Its light's direction."), "noxsdl/src/thing_art.cpp"),
    NOX_T("CLIENTUPDATE", Function, C, NOX_TRK("content.field.thing.clientupdate", "The drawing update players run for it."), "noxsdl/src/thing_art.cpp"),
    NOX_T("LIFETIME", Int, C, NOX_TRK("content.field.thing.lifetime", "How long players draw it (an effect)."), "noxsdl/src/effects.cpp"),
    NOX_T("AUDIOLOOP", Sound, C, NOX_TRK("content.field.thing.audioloop", "The sound it plays all the time."), "noxsdl/src/main.cpp"),
    NOX_T("PRETTYNAME", StringId, C, NOX_TRK("content.field.thing.prettyname", "Its name's string id (tooltips)."), "noxsdl/src/item_names.cpp"),
    NOX_T("DESCRIPTION", StringId, C, NOX_TRK("content.field.thing.description", "Its description's string id (identify page)."), "noxsdl/src/inventory.cpp"),
    NOX_T("MENUICON", Int, B, NOX_TRK("content.field.thing.menuicon", "Its inventory picture (a video.bag image). The server writes only types with one into saves."), "server/object_save.cpp; noxsdl/src/map_preload.cpp"),
    NOX_T("PRETTYIMAGE", Int, C, NOX_TRK("content.field.thing.prettyimage", "Its large picture (a video.bag image)."), "noxsdl/src/map_preload.cpp"),
    NOX_T("DRAW", Text, B, NOX_TRK("content.field.thing.draw", "How it's drawn: its animations. The server times monsters' and players' blows by their frames."), "server/monsters.cpp; noxsdl/src/thing_art.cpp"),
    NOX_T("PREPROCESS", Text, ReadBy::Unknown, NOX_TRK("content.field.thing.unused", "The original game's type reader takes it; nothing in NoX Core reads it."), ""),
    NOX_T("DESTROY", Text, ReadBy::Unknown, NOX_TRK("content.field.thing.unused", "The original game's type reader takes it; nothing in NoX Core reads it."), ""),
};
#undef NOX_T
#undef NOX_TS

std::vector<FieldSpec> buildGameData()
{
    std::vector<FieldSpec> out;
    for (const NameGroup& g : kGameData) {
        for (const std::string_view n : g.names) {
            FieldSpec f;
            f.key = n;
            f.type = FieldType::Numbers;
            f.readBy = g.readBy;
            f.docs = g.docs;
            f.gameKinds = true;
            out.push_back(f);
        }
    }
    return out;
}

bool number(std::string_view s, double& out)
{
    const std::string t(nox::util::trim(s));
    if (t.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && (*end == '\0' || *end == 'f');
}

} // namespace

const char* readByLabel(ReadBy r)
{
    switch (r) {
    case ReadBy::Server: return NOX_TRC("content.readby.server", "server");
    case ReadBy::Client: return NOX_TRC("content.readby.client", "client");
    case ReadBy::Both: return NOX_TRC("content.readby.both", "both");
    default: return "?";
    }
}

Reach reachOf(ReadBy r)
{
    switch (r) {
    case ReadBy::Client: return Reach::ClientVisible;
    case ReadBy::Both: return Reach::ServerShown;
    default: return Reach::ServerOnly;
    }
}

const char* reachLabel(Reach r)
{
    switch (r) {
    case Reach::ServerOnly: return NOX_TRC("content.reach.server_only", "server-only");
    case Reach::ClientVisible: return NOX_TRC("content.reach.client_visible", "client-visible");
    case Reach::ServerShown: return NOX_TRC("content.reach.server_shown", "server, shown by clients");
    case Reach::Structural: return NOX_TRC("content.reach.structural", "structural");
    }
    return "?";
}

const char* reachDocs(Reach r)
{
    switch (r) {
    case Reach::ServerOnly:
        return NOX_TRC("content.reach.server_only_docs",
                       "Takes effect for everyone in a game hosted with this pack, players with the stock game included: nothing to install on theirs.");
    case Reach::ClientVisible:
        return NOX_TRC("content.reach.client_visible_docs", "Only players who installed this pack see it; it changes nothing for anyone else and doesn't upset a game.");
    case Reach::ServerShown:
        return NOX_TRC("content.reach.server_shown_docs",
                       "Gameplay follows the host's pack; players without it see the stock figure where their game shows it (a cooldown bar, an identify page).");
    case Reach::Structural:
        return NOX_TRC("content.reach.structural_docs",
                       "The host and the players must agree on it (it decides what each side loads, or numbers the network carries). A built-in type's CLASS, XFER, FLAGS and "
                       "COLLIDE are locked: a pack can't change them (only a new type, in a later version, will choose its own).");
    }
    return "";
}

Reach stronger(Reach a, Reach b)
{
    const auto rank = [](Reach r) {
        switch (r) {
        case Reach::ClientVisible: return 0;
        case Reach::ServerOnly: return 1;
        case Reach::ServerShown: return 2;
        case Reach::Structural: return 3;
        }
        return 0;
    };
    return rank(a) >= rank(b) ? a : b;
}

std::span<const FieldSpec> tableFields(TableFormat format, std::string_view section)
{
    switch (format) {
    case TableFormat::Monster: return kMonster;
    case TableFormat::SoundSet: return kSoundSet;
    case TableFormat::Modifier:
        if (section == "WEAPON_DEFINITIONS" || section == "ARMOR_DEFINITIONS") {
            return kEquip;
        }
        return kModifier;
    case TableFormat::GameData: return gameDataNames();
    }
    return {};
}

const FieldSpec* tableField(TableFormat format, std::string_view key, std::string_view section)
{
    if (format == TableFormat::GameData) {
        return gameDataName(key);
    }
    for (const FieldSpec& f : tableFields(format, section)) {
        if (equalsNoCase(f.key, key)) {
            return &f;
        }
    }
    return nullptr;
}

std::span<const FieldSpec> thingFields()
{
    return kThing;
}

const FieldSpec* thingField(std::string_view key)
{
    for (const FieldSpec& f : kThing) {
        if (equalsNoCase(f.key, key)) {
            return &f;
        }
    }
    return nullptr;
}

Reach reachOf(const FieldSpec& spec)
{
    return spec.structural ? Reach::Structural : reachOf(spec.readBy);
}

std::span<const FieldSpec> gameDataNames()
{
    static const std::vector<FieldSpec> names = buildGameData();
    return names;
}

const FieldSpec* gameDataName(std::string_view name)
{
    for (const FieldSpec& f : gameDataNames()) {
        if (equalsNoCase(f.key, name)) {
            return &f;
        }
    }
    return nullptr;
}

std::string checkValue(const FieldSpec& spec, std::string_view value)
{
    const std::string v(nox::util::trim(value));
    double n = 0;
    const auto range = [&]() -> std::string {
        if ((spec.min != 0 || spec.max != 0) && (n < spec.min || n > spec.max)) {
            return nox::l10n::format(NOX_TR("content.check.outside_range", "{value} is outside {min} .. {max}"),
                                     {{"value", v}, {"min", std::format("{}", spec.min)}, {"max", std::format("{}", spec.max)}});
        }
        return {};
    };
    switch (spec.type) {
    case FieldType::Int:
        if (!number(v, n) || n != static_cast<double>(static_cast<long long>(n))) {
            return nox::l10n::format(NOX_TR("content.check.not_whole", "not a whole number (the game reads {n})"), {{"n", std::atoi(v.c_str())}});
        }
        return range();
    case FieldType::Float:
        if (!number(v, n)) {
            return NOX_TR("content.check.not_number", "not a number (the game reads 0)");
        }
        return range();
    case FieldType::Numbers: {
        std::size_t count = 0;
        for (std::size_t i = 0; i < v.size();) {
            while (i < v.size() && (v[i] == ' ' || v[i] == ',' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n')) {
                ++i;
            }
            const std::size_t s = i;
            while (i < v.size() && !(v[i] == ' ' || v[i] == ',' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n')) {
                ++i;
            }
            if (i > s) {
                if (!number(std::string_view(v).substr(s, i - s), n)) {
                    return nox::l10n::format(NOX_TR("content.check.word_not_number", "'{word}' isn't a number (the game reads 0)"), {{"word", v.substr(s, i - s)}});
                }
                ++count;
            }
        }
        if (count == 0) {
            return NOX_TR("content.check.no_numbers", "no numbers");
        }
        return count > 255 ? NOX_TR("content.check.too_many_numbers", "more than 255 numbers (the game reads the first 255)") : std::string();
    }
    case FieldType::Flags:
    case FieldType::Enum: {
        if (spec.type == FieldType::Flags && (equalsNoCase(v, "NULL") || v.empty())) {
            return {};
        }
        std::string_view rest = v;
        while (!rest.empty()) {
            const auto plus = spec.type == FieldType::Flags ? rest.find('+') : std::string_view::npos;
            const std::string_view name = nox::util::trim(rest.substr(0, plus));
            bool known = false;
            for (const auto& n2 : spec.names) {
                known = known || (spec.exactCase ? n2 == name : equalsNoCase(n2, name));
            }
            if (!known) {
                return nox::l10n::format(NOX_TR("content.check.unknown_name", "'{name}' is a name the game doesn't know (it counts for nothing)"), {{"name", name}});
            }
            rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
        }
        return {};
    }
    case FieldType::Rgb: {
        int c[3] = {};
        const char* p = v.data();
        const char* end = v.data() + v.size();
        for (int& x : c) {
            while (p < end && (*p == ' ' || *p == '\t')) {
                ++p;
            }
            const auto r = std::from_chars(p, end, x);
            if (r.ec != std::errc{} || x < 0 || x > 255) {
                return NOX_TR("content.check.bad_colour", "a colour is three numbers 0-255 (r g b); the game refuses the whole file otherwise");
            }
            p = r.ptr;
        }
        return {};
    }
    case FieldType::Slot:
        if (v.size() == 6 && v.starts_with("COLOR") && v[5] >= '1' && v[5] <= '6') {
            return {};
        }
        return NOX_TR("content.check.bad_slot", "a slot is COLOR1 .. COLOR6; the game refuses the whole file otherwise");
    case FieldType::Sound:
        if (!nox::audio::soundId(v)) {
            return nox::l10n::format(NOX_TR("content.check.unknown_sound", "'{name}' isn't one of the game's sound names: it would be silent"), {{"name", v}});
        }
        return {};
    default:
        return {};
    }
}

} // namespace videotools::content
