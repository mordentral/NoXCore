// The content model's schema (schema.hpp). Which side reads each field is from NoX Core's own
// readers: the server's (noxcore/src/server, the modes) or the client's (noxsdl, noxcore/src/client).
#include "content/schema.hpp"

#include "noxcore/audio/sound_names.hpp"
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
    {"HEALTH", FieldType::Int, S, "Its health (over thing.bin's HEALTH).", 1, 65535, {}, true, "server/monsters.cpp"},
    {"QUESTHEALTH", FieldType::Int, S, "The health a quest scales by its difficulty.", 0, 65535, {}, true, "server/quest.cpp"},
    {"SPEED", FieldType::Int, S, "Walking speed (32 a unit a frame).", 0, 1000, {}, true, "server/monsters.cpp"},
    {"RUN_MULTIPLIER", FieldType::Float, S, "Running speed as a multiple of walking.", 0, 10, {}, true, "server/monsters.cpp"},
    {"RETREAT_RATIO", FieldType::Float, S, "The share of its health at which it retreats (0 never).", 0, 1, {}, true, "server/monsters.cpp"},
    {"RESUME_RATIO", FieldType::Float, S, "The share of its health at which it fights again.", 0, 1, {}, true, "server/monsters.cpp"},
    {"FLEE_RANGE", FieldType::Float, S, "How far it runs when fleeing.", 0, 2000, {}, true, "server/monsters.cpp"},
    {"STATUS", FieldType::Flags, S, "Behaviour flags, joined by '+'.", 0, 0, kMonsterStatus, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_FRAME", FieldType::Int, S, "The animation frame its blow lands on.", 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_RANGE", FieldType::Float, S, "How far its blow reaches.", 0, 500, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_DAMAGE", FieldType::Int, S, "Damage a blow does.", 0, 10000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_IMPACT", FieldType::Float, S, "How hard a blow knocks back.", 0, 1000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_DAMAGE_TYPE", FieldType::Enum, S, "The kind of damage a blow does.", 0, 0, kDamageTypes, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_MIN_DELAY", FieldType::Int, S, "Fewest frames between blows.", 0, 3000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_MAX_DELAY", FieldType::Int, S, "Most frames between blows.", 0, 3000, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_CHANCE", FieldType::Int, S, "Chance (percent) a blow poisons.", 0, 100, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_STRENGTH", FieldType::Int, S, "Poison a blow adds.", 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_ATTACK_POISON_MAX", FieldType::Int, S, "Most poison its blows build up.", 0, 255, {}, true, "server/monsters.cpp"},
    {"MELEE_STRIKE_FUNCTION", FieldType::Function, S, "The engine function its blow runs (NULL none).", 0, 0, {}, true, "server/monsters.cpp"},
    {"MISSILE_NAME", FieldType::Thing, S, "The thing it shoots (NULL none).", 0, 0, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_RANGE", FieldType::Float, S, "How far away it shoots from.", 0, 2000, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_FRAME", FieldType::Int, S, "The animation frame its shot leaves on.", 0, 255, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_MIN_DELAY", FieldType::Int, S, "Fewest frames between shots.", 0, 3000, {}, true, "server/monsters.cpp"},
    {"MISSILE_ATTACK_MAX_DELAY", FieldType::Int, S, "Most frames between shots.", 0, 3000, {}, true, "server/monsters.cpp"},
    {"MOVE_SOUND_FRAME_A", FieldType::Int, S, "A walking frame that plays its step sound.", 0, 255, {}, true, "server/monsters.cpp"},
    {"MOVE_SOUND_FRAME_B", FieldType::Int, S, "The other walking frame with a step sound.", 0, 255, {}, true, "server/monsters.cpp"},
    {"DIE_FUNCTION", FieldType::Function, S, "The engine function run when it dies (NULL none).", 0, 0, {}, true, "server/monsters.cpp"},
    {"DEAD_FUNCTION", FieldType::Function, S, "The engine function run while it's dead (NULL none).", 0, 0, {}, true, "server/monsters.cpp"},
    {"EXPERIENCE", FieldType::Int, S, "Read only by mods (nox.monsterInfo): the experience a kill gives is thing.bin's EXPERIENCE.", 0, 10000000, {}, true,
     "modes/script/src/scripted_mode.cpp"},
};

// soundset.bin (parseSoundSetBin): only hosts read it; they send the chosen sound's id.
#define NOX_SLOT(k, d) {k, FieldType::Sound, S, d, 0, 0, {}, false, "server/sound.cpp, server/monsters.cpp"}
const FieldSpec kSoundSet[] = {
    NOX_SLOT("LISTEN_SOUND", "When it hears something."),
    NOX_SLOT("TAUNT_SOUND", "After its blow lands."),
    NOX_SLOT("THREATEN_SOUND", "When it threatens."),
    NOX_SLOT("IDLE_SOUND", "Now and then while idle."),
    NOX_SLOT("ENGAGE_SOUND", "When it starts a fight."),
    NOX_SLOT("MELEE_ATTACK_INIT_SOUND", "As it winds up a blow."),
    NOX_SLOT("MELEE_ATTACK_SOUND", "As it strikes."),
    NOX_SLOT("MELEE_ATTACK_HITTING_SOUND", "When its blow hits."),
    NOX_SLOT("MELEE_ATTACK_MISSING_SOUND", "When its blow misses."),
    NOX_SLOT("MISSILE_ATTACK_INIT_SOUND", "As it aims a shot."),
    NOX_SLOT("MISSILE_ATTACK_SOUND", "As it shoots."),
    NOX_SLOT("FLEE_SOUND", "When it flees."),
    NOX_SLOT("RETREAT_SOUND", "When it retreats."),
    NOX_SLOT("SPELL_INIT_SOUND", "As it starts a spell."),
    NOX_SLOT("DIE_SOUND", "When it dies."),
    NOX_SLOT("INJURED_SOUND", "When it's hurt."),
    NOX_SLOT("RECOGNIZE_SOUND", "When it sees an enemy."),
    NOX_SLOT("MOVE_SOUND", "Its steps (on MOVE_SOUND_FRAME_A and _B)."),
};
#undef NOX_SLOT

// modifier.bin weapon and armour definitions.
const FieldSpec kEquip[] = {
    {"COLOR1", FieldType::Rgb, C, "Material colour 1 (r g b) the item's grey parts are tinted with.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR2", FieldType::Rgb, C, "Material colour 2.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR3", FieldType::Rgb, C, "Material colour 3.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR4", FieldType::Rgb, C, "Material colour 4.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR5", FieldType::Rgb, C, "Material colour 5.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"COLOR6", FieldType::Rgb, C, "Material colour 6.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"EFFECTIVENESS", FieldType::Slot, C, "The colour slot its effectiveness modifier recolours.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"MATERIAL", FieldType::Slot, C, "The colour slot its material modifier recolours.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"PRIMARYENCHANTMENT", FieldType::Slot, C, "The colour slot its first enchantment recolours.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"SECONDARYENCHANTMENT", FieldType::Slot, C, "The colour slot its second enchantment recolours.", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"CLASSUSE", FieldType::Flags, B,
     "The classes that may use it, joined by '+' (absent: anyone). The host decides who may equip it; a player's own copy picks the Caution cursor and the automatic "
     "switch to another weapon, so a player without the pack sees those differ (no desync).",
     0, 0, kClasses, false, "server/items.cpp; noxsdl/src/main.cpp (cursor); client/headless_client.cpp", false, true},
    {"DESC", FieldType::StringId, B, "Its name's string id.", 0, 0, {}, false, "server/items.cpp (messages); noxsdl/src/item_names.cpp"},
    {"ARMOR_VALUE", FieldType::Float, B, "Armour: the share of a blow's damage it stops.", 0, 1, {}, false, "server/combat.cpp; noxsdl/src/inventory.cpp"},
    {"DURABILITY", FieldType::Int, S, "Its health in network games.", 0, 65535, {}, false, "server/world.cpp"},
    {"DAMAGE_MIN", FieldType::Int, B, "Weapon: its least damage.", 0, 10000, {}, false, "server/combat.hpp; noxsdl/src/inventory.cpp"},
    {"DAMAGE_COEFFICIENT", FieldType::Float, B, "Weapon: damage added per point of strength over REQUIRED_STRENGTH.", 0, 100, {}, false,
     "server/combat.hpp; noxsdl/src/inventory.cpp"},
    {"REQUIRED_STRENGTH", FieldType::Int, B, "Weapon: the strength needed to wield it.", 0, 1000, {}, false, "server/items.cpp; noxsdl/src/inventory.cpp"},
    {"RANGE", FieldType::Float, B, "Weapon: how far a melee blow reaches past its wielder's edge, or a missile weapon's range. (Bots plan with their own copy.)", 0,
     2000, {}, false, "server/player.cpp; bot/bot.cpp"},
    {"DAMAGE_TYPE", FieldType::Enum, ReadBy::Unknown,
     "Weapon: a damage type. The original game's reader takes it (a name it doesn't know fails the whole file); nothing in NoX Core uses it.", 0, 0,
     kWeaponDamageTypes, false, "", false, true},
};

// modifier.bin modifiers (EFFECTIVENESS, MATERIAL, ENCHANTMENT). Their order is their ids.
const FieldSpec kModifier[] = {
    {"COLOR", FieldType::Rgb, C, "The colour it tints an item's slot with (r g b).", 0, 0, {}, false, "noxsdl/src/player_art.cpp"},
    {"DESC", FieldType::StringId, B, "The word it adds to an item's name (string id).", 0, 0, {}, false, "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"PRIMARYDESC", FieldType::StringId, B, "Enchantments: the word it adds as the first one.", 0, 0, {}, false, "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"SECONDARYDESC", FieldType::StringId, B, "Enchantments: the word it adds as the second one.", 0, 0, {}, false,
     "server/items.cpp; noxsdl/src/item_names.cpp"},
    {"IDENTIFYDESC", FieldType::StringId, C, "What the identify page says of it.", 0, 0, {}, false, "noxsdl/src/inventory.cpp"},
    {"ATTACKEFFECT", FieldType::Effect, B, "On attack: an effect function and its number (the number also shows on the identify page).", 0, 0, {}, false,
     "server/item_effects.cpp; noxsdl/src/inventory.cpp"},
    {"ATTACKPREHITEFFECT", FieldType::Effect, B, "Before a hit lands: stun, fire, push...", 0, 0, {}, false, "server/item_effects.cpp"},
    {"ATTACKPREDAMAGEEFFECT", FieldType::Effect, B, "Before damage: vampirism, poison...", 0, 0, {}, false, "server/item_effects.cpp"},
    {"DEFENDEFFECT", FieldType::Effect, B, "When the wearer is hit: armour or durability multipliers.", 0, 0, {}, false, "server/item_effects.cpp"},
    {"DEFENDCOLLIDEEFFECT", FieldType::Effect, S, "When something touches the wearer.", 0, 0, {}, false, "server/item_effects.cpp"},
    {"UPDATEEFFECT", FieldType::Effect, S, "Every so often while worn (regeneration...).", 0, 0, {}, false, "server/item_effects.cpp"},
    {"ENGAGEEFFECT", FieldType::Effect, S, "When put on (speed...).", 0, 0, {}, false, "server/item_effects.cpp"},
    {"DISENGAGEEFFECT", FieldType::Effect, S, "When taken off.", 0, 0, {}, false, "server/item_effects.cpp"},
    {"ALLOWED_WEAPONS", FieldType::Allowed, S, "The weapons random rewards may put it on (names joined by + and -).", 0, 0, {}, false, "server/rewards.cpp"},
    {"ALLOWED_ARMOR", FieldType::Allowed, S, "The armour random rewards may put it on.", 0, 0, {}, false, "server/rewards.cpp"},
    {"ALLOWED_POSITION", FieldType::Flags, S, "PRIMARY, SECONDARY or both (joined by '+'), or NULL.", 0, 0, kPositions, false, "server/rewards.cpp"},
    {"WORTH", FieldType::Int, S, "What it adds to an item's price.", 0, 1000000, {}, false, "server/trade.cpp"},
};

// gamedata.bin's names NoX Core reads, in groups that share a side and an explanation.
struct NameGroup {
    std::vector<std::string_view> names;
    ReadBy readBy;
    std::string_view docs;
};

const NameGroup kGameData[] = {
    {{"WarriorMaxHealth", "WizardMaxHealth", "ConjurerMaxHealth"}, B, "The class's top health (the stats page shows it)."},
    {{"WarriorMaxMana", "WizardMaxMana", "ConjurerMaxMana"}, B, "The class's top mana."},
    {{"WarriorMaxStrength", "WizardMaxStrength", "ConjurerMaxStrength"}, B, "The class's top strength."},
    {{"WarriorMaxSpeed", "WizardMaxSpeed", "ConjurerMaxSpeed"}, B, "The class's top speed."},
    {{"BaseHealth", "BaseMana", "BaseStrength", "BaseSpeed"}, S, "Solo: the level-1 stats every class starts from."},
    {{"XPTable"}, B, "The experience each level needs (the stats page shows the next one)."},
    {{"BerserkerChargeDelay", "WarcryDelay", "HarpoonDelay", "TreadLightlyDelay", "EyeOfTheWolfDelay"}, B,
     "Frames before the warrior ability is ready again (the HUD's cooldown shows it)."},
    {{"BerserkerChargeDuration", "WarCryDuration", "HarpoonDuration", "TreadLightlyDuration", "EyeOfTheWolfDuration"}, B,
     "How long the ability lasts (frames). (Bots read their own copy.)"},
    {{"BerserkerDamage", "BerserkerStunDuration", "BerserkerPainRatio"}, S, "Berserker Charge: its hit's damage; the stun and the share of health lost hitting a wall."},
    {{"MaxHarpoonDistance", "MinHarpoonDistance", "MaxHarpoonExistence", "HarpoonDamage", "HarpoonForce"}, S, "The harpoon's reach, life, damage and pull."},
    {{"MaxHarpoonFlightDistance"}, B, "The farthest a harpoon flies. (Bots plan with their own copy.)"},
    {{"ItemDamagePercentage", "ItemDamageFromBlockPercentage"}, S, "The share of a blow's damage worn items (or a blocking shield) take."},
    {{"FireSpellProtection", "ElectricitySpellProtection", "PoisonSpellProtection"}, S, "Protection enchantments' reduction, by power."},
    {{"ShockDamage", "VampirismCoeff"}, S, "Shock's discharge damage; Vampirism's share of damage returned, by power."},
    {{"FieldGuideDamageBonus"}, S, "Solo: damage multiplier against creatures in your field guide."},
    {{"BoltSoloDamageMin"}, S, "Solo: an archer bolt's least damage."},
    {{"OblivionStaffRechargeRate", "ForceOfNatureStaffLimit"}, S, "Oblivion staves' recharge frames; the most Force of Nature staves one player may hold."},
    {{"DefaultAmmoAmount", "DefaultAmmoAmountQuest"}, S, "Arrows or chakrams in a new quiver."},
    {{"QuestDurabilityMultiplier", "QuestStaffChargeMultiplier"}, S, "Quest: weapon and armour durability, staff charges."},
    {{"ArrowTrapDamage", "ToxicCloudLifetime", "SmallToxicCloudLifetime"}, S, "Arrow traps' damage; toxic clouds' seconds."},
    {{"MaxTrapCount", "MaxBomberCount", "GlyphRange"}, S, "The most glyph traps and bombers a player may have; a glyph's reach."},
    {{"EnergyBoltTrapCost", "LightningTrapCost", "ManaBombTrapCost"}, B, "Mana the spell costs in a trap (the quick bar shades by it)."},
    {{"UrchinStoneDamage", "ImpShotDamage"}, S, "Thrown stones' and imp shots' damage."},
    {{"DeathBallCollideDamage", "DeathBallCancelRange", "DeathBallOutRadius", "DeathBallInRadius", "DeathBallNearbyDamage"}, S, "The death ball's damage and blast."},
    {{"MagicMissileDamage", "MagicMissileSplashDamage", "MagicMissileRange", "MagicMissilePushRange", "MagicMissileForce"}, S, "Magic missiles' damage, seek range and push."},
    {{"MagicMissileCount", "PixieCount"}, B, "Missiles or pixies per cast and the most out, by power (the spell greys at the cap)."},
    {{"PixieReturnTimeout", "TargetedSpellLifetime", "UnTargetedSpellLifetime"}, S, "Seconds before pixies and homing spells give up."},
    {{"FireballSpeedCoeff", "PullPowerCoeff", "PushPowerCoeff"}, S, "Fireball speed; Pull and Push force, by power."},
    {{"EarthquakeRange", "EarthquakeDamage", "EarthquakeJiggle"}, S, "Earthquake's radius, damage and screen shake."},
    {{"DeathRayOutRadius", "DeathRayInRadius", "DeathRayDamage"}, S, "Death Ray's radii and damage."},
    {{"BurnDuration", "FistOfVengeanceDamage", "FistSpeed", "MeteorDamage", "MeteorSpeed"}, S, "Burning flames' life; Fist of Vengeance and Meteor."},
    {{"CounterspellRange", "InversionRange"}, S, "Counterspell's and Inversion's radius."},
    {{"ShockTrapDamage", "PlasmaSearchTime", "PlasmaDamage", "PlasmaDamageHecubah", "LesserHealAmount", "TurnUndeadKillPoints"}, S,
     "Shock in a trap; Plasma; Lesser Heal; the undead health Turn Undead destroys."},
    {{"ManaBombInitPower", "ManaBombDeltaPower", "ManaBombGlyphDuration", "ManaBombInRadius", "ManaBombShakeMag"}, S, "Mana Bomb's charge, blast and shake."},
    {{"ManaBombOutRadius"}, B, "Mana Bomb's outer blast radius (the charging sparks' ring shows it)."},
    {{"LightningRange", "LightningGlyphDamage", "EnergyBoltGlyphDamage", "LightningDamage", "LightningSearchTime", "EnergyBoltDamage"}, S,
     "Lightning and Energy Bolt: reach, damage, how long a bolt keeps striking."},
    {{"ManaDrainCoeff", "ChannelLifeCoeff", "TeleportDelay"}, S, "Drain Mana, Channel Life, the delay before a teleport."},
    {{"ManaDrainRange"}, B, "Drain Mana's reach. (Bots plan with their own copy.)"},
    {{"FearEnchantDuration", "AnchorEnchantDuration", "ConfuseEnchantDuration", "HasteEnchantDuration", "InfravisionEnchantDuration",
      "InvisibilityEnchantDuration", "InvulnerabilityEnchantDuration", "LightEnchantDuration", "ProtectElectricityEnchantDuration",
      "ProtectFireEnchantDuration", "ProtectPoisonEnchantDuration", "RunEnchantDuration", "ShockEnchantDuration", "SlowEnchantDuration",
      "StunEnchantDuration", "VampirismEnchantDuration", "MoonglowEnchantmentDuration"},
     S, "The enchantment's length in frames."},
    {{"QuestVampDurMultiplier"}, S, "Quest: Vampirism's length factor."},
    {{"SummonDuration", "CharmSmallDuration", "CharmMediumDuration", "CharmLargeDuration"}, S, "Summons' length by creature size; Charm's by size and power."},
    {{"ShieldDuration", "ShieldHealth", "TagDurationPerLevel"}, S, "Force Field's length and hit points; Mark's frames per power."},
    {{"ZombieDeadDuration"}, S, "The fewest and most frames a zombie stays down."},
    {{"GeneratorMaxActiveCreaturesHigh", "GeneratorMaxActiveCreaturesNormal", "GeneratorMaxActiveCreaturesLow", "GeneratorMaxActiveCreaturesSingular",
      "MaxOnscreenMonsterCount"},
     S, "The most live creatures a generator keeps, by its setting; the cap on nearby monsters."},
    {{"SpawnRateHighValue", "SpawnRateNormalValue", "SpawnRateLowValue", "SpawnRateVeryLowValue", "SpawnRateVeryVeryLowValue"}, S,
     "A generator's spawn interval by its rate setting."},
    {{"QuestHardcoreStage", "QuestHardcoreSpawnRateIncrease", "QuestHardcoreSpawnCap", "GeneratorMaxHealth"}, S, "Quest: hardcore spawning; generators' health cap."},
    {{"SuddenDeathCountdown", "SuddenDeathPlayerThreshold", "SuddenDeathLifeTime"}, S, "Sudden death's timing and trigger."},
    {{"KotRKingKillsPawnPoints", "KotRPawnKillsKingPoints", "KotRKingKillsKingPoints"}, S, "King of the Realm scores."},
    {{"FlagballPossDuration", "FlagballResetVel"}, S, "Flagball: frames a player may hold it; its reset speed."},
    {{"CamperRadiusSq", "CamperStartTime", "CamperFadeTime"}, S, "Anti-camping: radius (squared) and timers."},
    {{"MaxExtraLives", "QuestGameStartingExtraLives"}, S, "Quest: the most extra lives; lives at the start."},
    {{"PlayerDifficultyDelta", "PlayerDamageDiffCoeff", "PlayerDamageDiffInit", "PlayerDamageCap", "SystemHealthDiffCoeff", "SystemHealthDiffInit",
      "SystemHealthCap"},
     S, "Quest difficulty: per extra player; damage players take; monsters' health."},
    {{"MinionsAlwaysStage", "WarpGateCutoffStage", "QuestExitTimerStart"}, S, "Quest stages: bosses always, warp gates close; the exit countdown."},
    {{"QuestSpellWorthMultiplier", "QuestGuideWorthMultiplier", "QuestModifierWorthMultiplier", "QuestSellMultiplier", "ShopAnkhCutoffStage",
      "RepairCoefficient"},
     S, "Quest shops' prices; the stage shops stop selling ankhs; repair prices."},
};

// thing.bin THNG attributes (things/thing_db's text lines and its binary ones), by who reads them.
#define NOX_T(k, ty, side, d, r) {k, FieldType::ty, side, d, 0, 0, {}, false, r, false}
#define NOX_TS(k, ty, side, d, r) {k, FieldType::ty, side, d, 0, 0, {}, false, r, true}
const FieldSpec kThing[] = {
    NOX_TS("CLASS", Text, B, "Its classes, joined by '+' (MONSTER, WEAPON, IMMOBILE...). They decide which of a map's objects each player loads itself and which the server sends: a player without the same value can see an object twice or not at all.",
           "server/world.cpp; noxsdl/src/map_view.cpp"),
    NOX_T("SUBCLASS", Text, B, "Its subclasses, joined by '+' (SWORD, SMALL_MONSTER...).", "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_TS("FLAGS", Text, B, "Object flags, joined by '+' (NO_COLLIDE, SHADOW...); with CLASS, which objects players load themselves.",
           "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_TS("COLLIDE", Text, S, "Its collision function; part of the test of which map objects players load themselves.", "server/world.cpp"),
    NOX_TS("XFER", Text, B, "How a map stores the object's record: every map's objects of this type are read by it, on both sides.", "map/xfer.cpp"),
    NOX_T("EXTENT", Text, B, "Its collision shape (CIRCLE r, BOX w h); players use their copy only for drawing.", "server/world.cpp; noxsdl/src/thing_art.cpp"),
    NOX_T("WEIGHT", Int, B, "Its weight.", "server/world.cpp; noxsdl/src/inventory.cpp"),
    NOX_T("MASS", Float, S, "How hard it is to push.", "server/world.cpp"),
    NOX_T("SPEED", Text, S, "Its speed.", "server/world.cpp"),
    NOX_T("MATERIAL", Text, S, "What it's made of: picks its impact and drop sounds.", "server/world.cpp"),
    NOX_T("CARRYCAPACITY", Int, S, "How much it can carry.", "server/world.cpp"),
    NOX_T("HEALTH", Int, S, "Its health (a monster's comes from monster.bin).", "server/world.cpp"),
    NOX_T("EXPERIENCE", Int, S, "The experience a kill gives.", "server/world.cpp"),
    NOX_T("WORTH", Int, S, "Its price in shops.", "server/trade.cpp"),
    NOX_T("PICKUP", Function, S, "Its pick-up function.", "server/world.cpp"),
    NOX_T("DROP", Function, S, "Its drop function.", "server/world.cpp"),
    NOX_T("USE", Function, S, "Its use function (wands: charges, missile, rate, sound).", "server/world.cpp"),
    NOX_T("INIT", Function, S, "Its init function.", "server/world.cpp"),
    NOX_T("CREATE", Function, S, "Its create function.", "server/world.cpp"),
    NOX_T("UPDATE", Function, S, "Its update function (and its numbers).", "server/world.cpp"),
    NOX_T("DIE", Function, S, "Its death function.", "server/world.cpp"),
    NOX_T("DAMAGE", Function, S, "Its damage function.", "server/world.cpp"),
    NOX_T("DAMAGESOUND", Function, S, "The sound function when it's hit.", "server/world.cpp"),
    NOX_T("SIZE", Text, C, "Its drawing box (w h).", "noxsdl/src/thing_art.cpp"),
    NOX_T("Z", Int, C, "How high it's drawn.", "noxsdl/src/thing_art.cpp"),
    NOX_T("ZSIZE", Text, C, "Its drawing height range.", "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTCOLOR", Text, C, "The colour of the light it gives (r g b).", "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTINTENSITY", Float, C, "How bright its light is.", "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTPENUMBRA", Text, C, "Its light's soft edge.", "noxsdl/src/thing_art.cpp"),
    NOX_T("LIGHTDIRECTION", Text, C, "Its light's direction.", "noxsdl/src/thing_art.cpp"),
    NOX_T("CLIENTUPDATE", Function, C, "The drawing update players run for it.", "noxsdl/src/thing_art.cpp"),
    NOX_T("LIFETIME", Int, C, "How long players draw it (an effect).", "noxsdl/src/effects.cpp"),
    NOX_T("AUDIOLOOP", Sound, C, "The sound it plays all the time.", "noxsdl/src/main.cpp"),
    NOX_T("PRETTYNAME", StringId, C, "Its name's string id (tooltips).", "noxsdl/src/item_names.cpp"),
    NOX_T("DESCRIPTION", StringId, C, "Its description's string id (identify page).", "noxsdl/src/inventory.cpp"),
    NOX_T("MENUICON", Int, B, "Its inventory picture (a video.bag image). The server writes only types with one into saves.", "server/object_save.cpp; noxsdl/src/map_preload.cpp"),
    NOX_T("PRETTYIMAGE", Int, C, "Its large picture (a video.bag image).", "noxsdl/src/map_preload.cpp"),
    NOX_T("DRAW", Text, B, "How it's drawn: its animations. The server times monsters' and players' blows by their frames.", "server/monsters.cpp; noxsdl/src/thing_art.cpp"),
    NOX_T("PREPROCESS", Text, ReadBy::Unknown, "The original game's type reader takes it; nothing in NoX Core reads it.", ""),
    NOX_T("DESTROY", Text, ReadBy::Unknown, "The original game's type reader takes it; nothing in NoX Core reads it.", ""),
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
    case ReadBy::Server: return "server";
    case ReadBy::Client: return "client";
    case ReadBy::Both: return "both";
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
    case Reach::ServerOnly: return "server-only";
    case Reach::ClientVisible: return "client-visible";
    case Reach::ServerShown: return "server, shown by clients";
    case Reach::Structural: return "structural";
    }
    return "?";
}

const char* reachDocs(Reach r)
{
    switch (r) {
    case Reach::ServerOnly: return "Takes effect for everyone in a game hosted with this pack, players with the stock game included: nothing to install on theirs.";
    case Reach::ClientVisible: return "Only players who installed this pack see it; it changes nothing for anyone else and doesn't upset a game.";
    case Reach::ServerShown:
        return "Gameplay follows the host's pack; players without it see the stock figure where their game shows it (a cooldown bar, an identify page).";
    case Reach::Structural:
        return "The host and the players must agree on it (it decides what each side loads, or numbers the network carries). A built-in type's CLASS, XFER, FLAGS and "
               "COLLIDE are locked: a pack can't change them (only a new type, in a later version, will choose its own).";
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
            return std::format("{} is outside {} .. {}", v, spec.min, spec.max);
        }
        return {};
    };
    switch (spec.type) {
    case FieldType::Int:
        if (!number(v, n) || n != static_cast<double>(static_cast<long long>(n))) {
            return "not a whole number (the game reads " + std::to_string(std::atoi(v.c_str())) + ")";
        }
        return range();
    case FieldType::Float:
        if (!number(v, n)) {
            return "not a number (the game reads 0)";
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
                    return "'" + v.substr(s, i - s) + "' isn't a number (the game reads 0)";
                }
                ++count;
            }
        }
        if (count == 0) {
            return "no numbers";
        }
        return count > 255 ? "more than 255 numbers (the game reads the first 255)" : std::string();
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
                return "'" + std::string(name) + "' is a name the game doesn't know (it counts for nothing)";
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
                return "a colour is three numbers 0-255 (r g b); the game refuses the whole file otherwise";
            }
            p = r.ptr;
        }
        return {};
    }
    case FieldType::Slot:
        if (v.size() == 6 && v.starts_with("COLOR") && v[5] >= '1' && v[5] <= '6') {
            return {};
        }
        return "a slot is COLOR1 .. COLOR6; the game refuses the whole file otherwise";
    case FieldType::Sound:
        if (!nox::audio::soundId(v)) {
            return "'" + v + "' isn't one of the game's sound names: it would be silent";
        }
        return {};
    default:
        return {};
    }
}

} // namespace videotools::content
