// Type-specific object fields (ObjectXfer::extra), one struct per xfer function.
//
// Each layout follows the named GAME.EXE function for the xfer versions the game still reads;
// field names come from the verified NoXLib ObjDataXfer classes where known. Fields present only
// from some version are optional or noted, and the version that controls them is the record's
// ObjectXfer::xferVersion.
#pragma once

#include "noxcore/map/sections.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nox::things {
struct ThingDef;
}

namespace nox::map::xfer {

/// Four modifier (enchantment) names; empty when unset.
using Modifiers = std::array<std::string, 4>;

struct Default { bool operator==(const Default&) const = default; }; // XFerDefault: nothing after the header

struct SpellPagePedestal { // XFerSpellPagePedistal
    std::uint32_t spell = 0;
    bool operator==(const SpellPagePedestal&) const = default;
};

struct Readable { // XFerReadable
    std::string text; // string table id
    bool operator==(const Readable&) const = default;
};

struct Exit { // XFerExit
    std::string mapName;
    float x = 0; // v > 0x1E; a warp gate's way out
    float y = 0;
    bool operator==(const Exit&) const = default;
};

struct Door { // XFerDoor
    std::uint32_t currentDirection = 0;
    std::uint32_t lock = 0;
    std::uint32_t direction = 0; // v >= 0x29
    bool operator==(const Door&) const = default;
};

struct Trigger { // unitTriggerXfer (v >= 0x29)
    std::int32_t sizeX = 0;
    std::int32_t sizeY = 0;
    std::array<std::uint8_t, 3> edgeColor{};
    std::array<std::uint8_t, 3> backColor{};
    std::uint32_t unknown1 = 0;
    ScriptHandler onPressed;
    ScriptHandler onReleased;
    ScriptHandler onCollided; // v > 0x1E
    std::uint32_t allowedClasses = 0;
    std::uint32_t ignoredClasses = 0;
    std::uint8_t allowedTeam = 0;
    std::uint8_t ignoredTeam = 0;
    std::uint8_t state = 0;    // v > 0x3C
    std::uint8_t unknown2 = 0; // v > 0x3C
    std::uint32_t animFrame = 0; // v > 0x3C
    bool operator==(const Trigger&) const = default;
};

struct Hole { // XFerHole (v >= 0x2A)
    std::uint32_t scriptActivated = 0;
    ScriptHandler script;
    std::int32_t fallX = 0;
    std::int32_t fallY = 0;
    std::uint32_t scriptTime = 0;
    std::uint16_t scriptTimeout = 0;
    bool operator==(const Hole&) const = default;
};

struct Transporter { // XFerTransporter
    std::uint32_t targetExtent = 0; // 0 = unlinked
    bool operator==(const Transporter&) const = default;
};

struct Elevator { // XFerElevator
    std::uint32_t linkedExtent = 0;
    std::uint32_t height = 0; // v > 0x28
    std::uint8_t status = 0;  // v > 0x3C: 0 down, 1 moving down, 2 up, 3 moving up
    bool operator==(const Elevator&) const = default;
};

struct ElevatorShaft { // XFerElevatorShaft
    std::uint32_t linkedExtent = 0;
    bool operator==(const ElevatorShaft&) const = default;
};

struct Mover { // XFerMover (v > 0x29)
    std::uint32_t movingSpeed = 0;
    std::uint32_t waypoint = 0;
    std::uint32_t movedExtent = 0;
    std::uint8_t moveType = 0;       // its state: 0 ready, 1 moving, 2 paused, 3 done
    std::uint32_t waypointStart = 0; // the waypoint it heads for now
    std::uint32_t waypointEnd = 0;   // the waypoint it came from
    float speedBase = 0;
    float speedCurrent = 0;
    bool operator==(const Mover&) const = default;
};

struct Glyph { // XFerGlyph (v >= 0x29)
    std::uint8_t direction = 0;
    float targetX = 0;
    float targetY = 0;
    std::vector<std::string> spells; // at most 255
    bool operator==(const Glyph&) const = default;
};

struct InvisibleLight { // XFerInvLight (v >= 0x2A): the drawable's light block
    std::uint32_t type = 0;
    float intensity = 0;
    std::int32_t radius = 0;
    std::int32_t intensityFx = 0;
    std::array<std::int32_t, 3> color{};
    std::uint16_t direction = 0;
    std::uint16_t cone = 0;
    std::uint32_t omni = 0;
    std::uint16_t animFlags = 0;
    std::array<std::array<std::uint8_t, 3>, 16> colorKeys{};
    std::array<std::uint8_t, 16> intensityKeys{};
    std::array<std::uint8_t, 16> coneKeys{};
    std::uint16_t colorStep = 0;
    std::uint16_t intensityStep = 0;
    std::uint16_t coneStep = 0;
    std::uint32_t target = 0;
    std::uint16_t rotationSpeed = 0;
    std::uint16_t rotationMax = 0;
    std::uint8_t unused = 0x80;
    std::uint32_t isAnti = 0;
    bool operator==(const InvisibleLight&) const = default;
};

struct Sentry { // XFerSentry
    float basePosition = 0; // radians
    float rotateSpeed = 0;
    float currentPosition = 0; // v > 0x3C
    bool operator==(const Sentry&) const = default;
};

struct SpellReward { // (v >= 0x29)
    std::string spell;
    bool operator==(const SpellReward&) const = default;
};

struct AbilityReward {
    std::string ability;
    bool operator==(const AbilityReward&) const = default;
};

struct FieldGuide { // XFerFieldGuide
    std::string creature; // under 64 bytes
    bool operator==(const FieldGuide&) const = default;
};

struct Weapon { // XFerWeapon (v >= 0x0B)
    Modifiers modifiers;
    /// Charges, stored for wands and staves only (see decode).
    struct Charges {
        std::uint8_t current = 0;
        std::uint8_t max = 0;
        std::uint32_t unknown = 0; // v > 0x3C
        bool operator==(const Charges&) const = default;
    };
    std::optional<Charges> charges;
    std::uint16_t health = 0; // v > 0x29
    std::uint8_t unknown1 = 0;  // v == 0x3F only
    std::uint32_t unknown2 = 0; // v > 0x3F
    bool operator==(const Weapon&) const = default;
};

struct Armor { // XFerArmor (v >= 0x0B)
    Modifiers modifiers;
    std::uint16_t health = 0;   // v > 0x28
    std::uint8_t unknown1 = 0;  // v == 0x3D only
    std::uint32_t unknown2 = 0; // v > 0x3D
    bool operator==(const Armor&) const = default;
};

struct Ammo { // XFerAmmo
    Modifiers modifiers;
    std::uint8_t current = 0;
    std::uint8_t limit = 0;
    bool operator==(const Ammo&) const = default;
};

struct Team { // XFerTeam
    Modifiers modifiers;
    bool operator==(const Team&) const = default;
};

struct Gold { // XFerGold
    std::uint32_t amount = 0;
    bool operator==(const Gold&) const = default;
};

struct Obelisk { // XFerObelisk (v > 0x3C)
    std::uint32_t manaPool = 0;
    std::uint8_t minimapKnown = 0;
    bool operator==(const Obelisk&) const = default;
};

struct ToxicCloud { // XFerToxicCloud
    std::uint32_t lifetime = 0;
    bool operator==(const ToxicCloud&) const = default;
};

struct RewardMarker { // XFerRewardMarker
    std::uint32_t rewardType = 0;
    std::uint32_t unknown1 = 0;
    std::vector<std::string> spells;
    std::vector<std::string> abilities;
    std::vector<std::string> creatures; // field guide entries
    std::array<std::uint32_t, 5> unknown{};
    std::uint32_t activateChance = 0; // v > 0x3D: 1 75%, 2 50%, 3 25%, 4 5%, else always
    std::uint8_t rareOrSpecial = 0;   // v > 0x3E
    bool operator==(const RewardMarker&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Monsters. Fields named unknownNNN are the exe's MonsterUpdateData.FieldNNN.

/// One entry on a monster's AI action stack. Argument types come from the action
/// table: 0 location (8 bytes), 7 one byte, others 4 bytes; `value` holds them little-endian.
struct AiActionEntry {
    struct Arg {
        std::uint8_t type = 0;
        std::uint64_t value = 0;
        bool operator==(const Arg&) const = default;
    };
    std::string action; // ACTION_* or DEPENDENCY_* name
    std::vector<Arg> args;
    std::uint32_t unknown = 0;
    bool operator==(const AiActionEntry&) const = default;
};

/// Monster AI state (XFer_ActionData, versions 1..4). Savegames fill it in; maps usually store only
/// `present` = 0. Frame fields are absolute frames relative to `saveFrame`.
struct ActionData {
    struct Body {
        std::uint32_t saveFrame = 0;
        std::uint32_t unknown1 = 0; // not stored in the object
        std::vector<std::array<std::uint8_t, 8>> unknownList1;
        std::uint32_t unknown2 = 0;
        std::array<std::uint8_t, 8> unknown3{};
        std::uint32_t frame1 = 0;
        std::uint8_t unknown4 = 0;
        std::vector<std::uint32_t> waypoints;
        std::uint32_t unknown5 = 0;
        std::array<std::uint8_t, 8> unknown6{};
        std::uint32_t unknown7 = 0;
        std::array<std::uint8_t, 8> unknown8{};
        std::string sound;
        std::array<std::uint8_t, 8> unknown9{};
        std::uint32_t frame2 = 0;
        std::array<std::uint8_t, 3> unknown10{};
        std::uint32_t unknown11 = 0; // version < 3 only
        std::uint32_t frame3 = 0;
        std::array<std::uint8_t, 8> unknown12{};
        std::uint32_t unknown13 = 0; // not stored in the object
        std::uint32_t frame4 = 0;
        std::uint32_t frame5 = 0;
        std::vector<AiActionEntry> stack; // stored as the top index (i8, -1 when empty)
        std::vector<std::uint32_t> unknownList2;
        std::uint32_t unknown14 = 0; // object script id
        std::uint32_t frame6 = 0;
        std::uint32_t unknown15 = 0;
        std::uint32_t unknown16 = 0;
        std::uint8_t unknown17 = 0;
        std::uint8_t unknown18 = 0;
        std::string unknownName;
        /// Version > 3.
        struct V4 {
            std::array<std::uint32_t, 3> unknown1{};
            std::uint32_t scriptId1 = 0;
            std::uint32_t unknown2 = 0;
            std::array<std::uint32_t, 4> frames1{};
            std::array<std::uint32_t, 2> frames2{};
            std::uint32_t unknown3 = 0;
            std::uint32_t frame3 = 0;
            std::uint8_t unknown4 = 0;
            std::uint32_t scriptId2 = 0;
            std::array<std::uint32_t, 2> frames4{};
            std::uint32_t scriptId3 = 0;
            std::vector<std::uint32_t> scriptIds; // u8 count
            bool operator==(const V4&) const = default;
        };
        std::optional<V4> v4;
        bool operator==(const Body&) const = default;
    };

    std::uint16_t version = 4;
    std::uint8_t present = 0; // version > 1; the body follows when non-zero
    std::optional<Body> body;
    bool operator==(const ActionData&) const = default;
};

struct MonsterSpell {
    std::string name;
    std::uint32_t flags = 0;
    bool operator==(const MonsterSpell&) const = default;
};

/// Shopkeeper inventory (monster subclass SHOPKEEPER; item reader XFer_ReadShopItem).
struct Shop {
    struct Item {
        std::uint8_t count = 0;
        std::string type;      // object type name
        std::string parameter; // spell, ability or creature name for books
        Modifiers modifiers;
        bool operator==(const Item&) const = default;
    };
    std::uint32_t unknown1 = 0;
    std::uint32_t unknown2 = 0; // xfer version > 0x3C
    std::string name;
    std::vector<Item> items; // at most 255
    bool operator==(const Shop&) const = default;
};

/// Active enchantments (XFer_ReadMonsterBuffs, versions 1..2).
struct Buffs {
    struct Buff {
        std::string enchantment; // ENCHANT_* name
        std::uint8_t power = 0;
        std::uint32_t timer = 0;
        std::uint32_t shieldHealth = 0; // ENCHANT_SHIELD in version 2 only
        bool operator==(const Buff&) const = default;
    };
    std::uint16_t version = 2;
    std::vector<Buff> buffs; // at most 255
    bool operator==(const Buffs&) const = default;
};

using NpcColors = std::array<std::array<std::uint8_t, 3>, 6>;

/// Shared monster/NPC script events, in stored order.
struct MonsterScripts {
    ScriptHandler enemySighted;
    ScriptHandler death;
    // The server keeps this as the monster's look delay (Monster::lookDelay).
    std::uint16_t unknown305 = 0; // stored between death and lookingForEnemy
    ScriptHandler lookingForEnemy;
    ScriptHandler changeFocus;
    ScriptHandler isHit;
    ScriptHandler retreat;
    ScriptHandler collision;
    ScriptHandler hearEnemy;
    ScriptHandler endOfWaypoint;
    ScriptHandler lostEnemy;
    bool operator==(const MonsterScripts&) const = default;
};

struct Monster { // XFerMonster (v >= 0x3C)
    std::array<std::int32_t, 2> direction{}; // direction vector
    MonsterScripts scripts;
    std::uint32_t unknown1 = 0;
    std::uint8_t unknown333 = 0;  // roam flags (Monster::field333)
    std::uint32_t statusFlags = 0;
    std::uint32_t unknown338 = 0;  // starting health as a fraction of the full (a float's bits)
    std::uint32_t retreatLevel = 0;
    std::uint32_t resumeLevel = 0;
    std::uint32_t sightRange = 0;
    std::uint32_t aggression = 0;
    std::string unknown341;  // the name of whom it escorts (Monster::escortName)
    std::vector<MonsterSpell> spells;
    std::array<std::uint16_t, 10> spellDelays{}; // per spell use, the least and most frames between casts
    std::uint32_t unknown329 = 0;  // escort distance (a float's bits)
    std::uint32_t unknown510 = 0;  // spell power (Monster::spellPower)
    std::uint32_t unknown330 = 0;  // missile aim accuracy, 0..1 (a float's bits)
    std::array<std::string, 3> defaultSpells; // three spells by name (a bomber's)
    std::string action;
    ActionData actionData;
    std::uint8_t unknown361 = 0;  // the map's health kept (Monster::healthFromMap)
    std::optional<Shop> shop; // subclass SHOPKEEPER
    std::uint32_t unknown0 = 0;
    std::uint32_t subclassBits = 0; // SUBCLASS & 0x180
    std::uint16_t health = 0;
    std::array<std::uint8_t, 4> unknown337{}; // 1: resume level, retreat level, status, spells from its type
    std::optional<NpcColors> colors;       // subclass FEMALE_NPC
    std::optional<std::string> voiceSet;   // subclass FEMALE_NPC
    Buffs buffs;                           // v > 0x3D
    std::optional<std::string> voiceSet2;  // v > 0x3E, subclass WOUNDED_NPC
    std::uint8_t poison = 0;               // v >= 0x40
    bool operator==(const Monster&) const = default;
};

struct Npc { // XFerNPC (v >= 0x3C)
    std::array<std::int32_t, 2> direction{};
    MonsterScripts scripts;
    std::uint32_t unknown1 = 0;
    NpcColors colors{};
    std::uint8_t unknown333 = 0;  // roam flags (Monster::field333)
    std::uint32_t statusFlags = 0;
    std::uint32_t unknown338 = 0;  // starting health as a fraction of the full (a float's bits)
    std::uint32_t retreatLevel = 0;
    std::uint32_t resumeLevel = 0;
    std::uint32_t sightRange = 0;
    std::uint16_t health = 0;
    std::uint32_t aggression = 0;
    std::string unknown341;  // the name of whom it escorts (Monster::escortName)
    std::vector<MonsterSpell> spells;
    std::array<std::uint16_t, 10> spellDelays{};
    std::uint32_t unknown329 = 0;  // escort distance (a float's bits)
    std::uint32_t unknown510 = 0;  // spell power (Monster::spellPower)
    std::uint8_t unknown331 = 0;  // strength (Monster::field331)
    std::uint32_t unknown332 = 0;  // speed stat: base speed = this / 2 + 1.7 (a float's bits)
    std::uint32_t unknown330 = 0;  // missile aim accuracy, 0..1 (a float's bits)
    std::array<std::string, 3> defaultSpells;
    std::string action;
    ActionData actionData;
    std::uint8_t unknown361 = 0;  // the map's health kept (Monster::healthFromMap)
    std::uint32_t unknown0 = 0;
    std::uint32_t healthMax = 0;
    std::uint32_t subclassBits = 0;
    std::uint16_t healthCurrent = 0;
    std::uint32_t experience = 0;
    std::string voiceSet;
    Buffs buffs;             // v > 0x3C
    std::uint8_t poison = 0; // v >= 0x3E
    bool operator==(const Npc&) const = default;
};

struct MonsterGenerator { // XFerMonsterGen (v >= 0x3C)
    /// A monster template: a whole embedded object record (decode record.data with
    /// ObjectXfer::parse and the type's xfer).
    struct Template {
        std::string type;
        ObjectRecord record;
        /// The template's inventory, written after it by the game's object saver (xfer_saveObj):
        /// records in stream order, nested by each header's inventory count.
        std::vector<ObjectRecord> inventory;
        bool operator==(const Template&) const = default;
    };
    std::vector<std::uint8_t> unknown1; // u8 count
    std::uint8_t unknown2 = 0;
    std::uint8_t unknown3 = 0;
    std::uint32_t unknown4 = 0;
    std::array<ScriptHandler, 4> scripts;
    std::vector<std::vector<Template>> groups; // u8 count; u8 templates per group
    std::vector<std::uint8_t> unknown5;        // v > 0x3D; u8 count
    std::uint32_t unknown6 = 0;                // v > 0x3E
    bool operator==(const MonsterGenerator&) const = default;
};

/// Records older than the typed layouts (xfer version < 0x3C), kept as bytes.
struct Raw {
    std::vector<std::uint8_t> bytes;
    bool operator==(const Raw&) const = default;
};

using Data = std::variant<Default, SpellPagePedestal, Readable, Exit, Door, Trigger, Hole, Transporter, Elevator,
                          ElevatorShaft, Mover, Glyph, InvisibleLight, Sentry, SpellReward, AbilityReward,
                          FieldGuide, Weapon, Armor, Ammo, Team, Gold, Obelisk, ToxicCloud, RewardMarker, Monster,
                          Npc, MonsterGenerator, Raw>;

/// What the xfer functions check besides the version: the object type's CLASS and SUBCLASS.
struct TypeInfo {
    std::uint32_t classes = 0;
    std::uint32_t subclasses = 0;
};

/// An object type's xfer function name (its XFER, DefaultXfer when it has none) and its CLASS /
/// SUBCLASS bits: what decoding and encoding its records takes.
struct TypeXfer {
    std::string xfer;
    TypeInfo info;
};
TypeXfer typeXferOf(const things::ThingDef& type);

/// Decodes ObjectXfer::extra for the named xfer function ("DoorXfer", ...). Throws FormatError
/// for unknown xfer names or bytes that don't match the layout exactly.
///
/// `extraOffset` is where `extra` starts within the record data (ObjectXfer::extraOffset()); it
/// places the 8-byte alignment of MonsterGenerator templates, which counts from the file start
/// (record data always starts 8-aligned).
Data decode(std::string_view xferName, std::uint16_t xferVersion, std::span<const std::uint8_t> extra,
            const TypeInfo& type, std::size_t extraOffset);

/// Reads one xfer's fields from a stream, stopping where they end (for places that store
/// objects without a length, like .plr inventories). Versions below 0x3C aren't supported here.
Data read(ByteReader& r, std::string_view xferName, std::uint16_t xferVersion, const TypeInfo& type,
          std::size_t extraOffset);

/// Encodes back to ObjectXfer::extra for the same version, type and offset.
std::vector<std::uint8_t> encode(const Data& data, std::uint16_t xferVersion, const TypeInfo& type,
                                 std::size_t extraOffset);

/// Writes an embedded object record's data (MonsterGenerator templates). When empty, the bytes
/// are written as one call; the map CRC walk supplies one that re-encodes them field by field.
using RecordDataWriter = std::function<void(ByteWriter&, const ObjectRecord&)>;

/// Like encode, into an existing writer whose call boundaries follow the game's (see ByteWriter).
void write(ByteWriter& w, const Data& data, std::uint16_t xferVersion, const TypeInfo& type,
           std::size_t extraOffset, const RecordDataWriter& nested = {});

} // namespace nox::map::xfer
