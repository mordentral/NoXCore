// .plr player files: multiplayer characters and the player part of solo/quest saves.
//
// Decrypted layout (Blowfish key 27):
//   repeat { u32 id; zero pad to 8 from the file start; u64 size; u8 data[size] }
//   zero pad to 8, plus whole zero blocks (solo saves end with one extra: their client sections
//   are appended by a second write)
//
// Sections: the server ones (2, 3, 4, 8, 5, 6, 9, 10, 11) appear only in solo/quest saves
// (Plr_SaveServerPlayerData, table at g_plrServerSections); the client ones (7, 1, 12) in every
// file. Each section starts with a u16 version. Layouts follow the section readers
// (plr_rwAttrib..plr_rwMusic).
#pragma once

#include "noxcore/map/objects.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::player {

enum class SectionId : std::uint32_t {
    FileInfo = 1,
    Attrib = 2,
    Status = 3,
    Inventory = 4,
    Spellbook = 5,
    Enchantment = 6,
    Gui = 7,
    FieldGuide = 8,
    Journal = 9,
    GameData = 10,
    Pad = 11,
    Music = 12,
};

enum class CharacterClass : std::uint8_t { Warrior = 0, Wizard = 1, Conjurer = 2 };

struct RawSection {
    std::uint32_t id = 0;
    std::vector<std::uint8_t> data;
};

struct PlayerFile {
    std::vector<RawSection> sections;
    std::uint8_t extraZeroBlocks = 0; // whole zero blocks after the final padding

    /// First section with this id, or nullptr.
    const RawSection* find(SectionId id) const;
};

/// Parses decrypted .plr bytes. Throws FormatError.
PlayerFile parsePlayerFile(std::span<const std::uint8_t> plain);
std::vector<std::uint8_t> serializePlayerFile(const PlayerFile& file);

PlayerFile loadPlayerFile(const std::filesystem::path& path);
void savePlayerFile(const std::filesystem::path& path, const PlayerFile& file);

// ---------------------------------------------------------------------------------------------
// Typed sections. parse() consumes the whole section; serialize() gives back identical bytes.

using Rgb = std::array<std::uint8_t, 3>;

/// Hair, skin, mustache, goatee and beard colours, in stored order.
using BodyColors = std::array<Rgb, 5>;
/// Pants, shirt 1, shirt 2, shoes 1, shoes 2 (palette indices).
using Clothing = std::array<std::uint8_t, 5>;

/// 7: spell/ability bars (plr_rwGUI).
struct Gui {
    struct Slot {
        std::string name; // SPELL_* or, for warriors, ABILITY_*
        std::uint8_t flag = 0;
    };
    std::uint16_t version = 3;
    CharacterClass characterClass = CharacterClass::Warrior;
    std::array<std::array<Slot, 5>, 5> bars;
    std::array<std::array<Slot, 3>, 3> traps; // non-warriors only
    std::uint8_t activeRow = 0;               // v >= 2
    std::uint8_t activeTrap = 0;              // v >= 2
    std::uint8_t summonOrder = 4;             // v >= 3

    static Gui parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 1: the save slot header shown in the load menu (parseFileInfoData).
struct FileInfo {
    std::uint16_t version = 12;
    std::uint32_t flags = 2; // 1 solo, 2 multiplayer, 4 quest, 8 unknown
    std::string savePath;
    std::string unused;                  // always empty
    std::array<std::uint16_t, 8> time{}; // SYSTEMTIME
    BodyColors colors{};
    Clothing clothing{};
    std::u16string name;
    CharacterClass characterClass = CharacterClass::Warrior;
    std::uint8_t isFemale = 0;
    std::uint8_t level = 1;
    std::string mapName;        // v > 10
    std::uint8_t questStage = 0; // v > 11

    static FileInfo parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 12: music state (plr_rwMusic).
struct Music {
    std::uint16_t version = 11;
    std::uint8_t solo = 0; // v >= 11; nothing follows when 0
    std::array<std::int32_t, 4> current{};
    std::vector<std::array<std::int32_t, 4>> stack;

    static Music parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 2: character attributes (plr_rwAttrib).
struct Attrib {
    std::uint16_t version = 5;
    std::uint32_t gameMode = 1; // v >= 5: 1 solo, 2 multiplayer, 4 quest; must match on load
    std::u16string name;        // under 25 characters
    std::uint32_t speed = 0;
    std::uint32_t strength = 0;
    std::uint32_t maxMana = 0;
    std::uint32_t maxHealth = 0;
    CharacterClass characterClass = CharacterClass::Warrior;
    std::uint8_t isFemale = 0;
    BodyColors colors{};
    Clothing clothing{}; // v >= 2
    std::uint8_t unknown88 = 0;
    std::uint32_t extraLives = 0;                 // v >= 3
    std::array<std::uint32_t, 9> unusedV3{};      // v == 3 only
    std::uint32_t highestQuestStage = 0;          // v >= 4

    static Attrib parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 3: health, mana and experience (plr_rwStatus).
struct Status {
    struct Values {
        std::uint16_t maxHealth = 0;
        std::uint16_t maxMana = 0;
        std::uint16_t health = 0;
        std::uint16_t mana = 0;
        std::uint8_t poison = 0;
        std::uint8_t unknown541 = 0;
        std::uint16_t unknown542 = 0;
        float experience = 0;
        std::uint16_t direction = 0; // v >= 2
    };
    std::uint16_t version = 2;
    std::optional<Values> values; // stored after a "has" byte

    static Status parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 4: carried items (plr_rwInventory). Items are stored as the type name followed by the object's own
/// xfer data (as in map ObjectData, without a size), so parsing needs thing.bin.
struct Inventory {
    struct Item {
        std::string type;
        /// ObjectXfer bytes: xfer version, header, type-specific fields. Decode with
        /// map::ObjectXfer::parse and map::xfer::decode.
        std::vector<std::uint8_t> xfer;
    };
    struct Contents {
        std::uint32_t gold = 0;
        std::vector<Item> items;
        std::vector<std::uint32_t> equipped; // script ids; u8 count
        std::uint32_t secondaryWeapon = 0;   // script id
        std::uint32_t lastQuiver = 0;        // v >= 2
        std::uint8_t trapCount = 0;          // v >= 3
    };
    std::uint16_t version = 3;
    std::optional<Contents> contents;

    static Inventory parse(std::span<const std::uint8_t> data, const things::ThingDb& things);
    std::vector<std::uint8_t> serialize() const;
};

/// 8: creatures seen (guiFieldbook).
struct FieldGuide {
    std::uint16_t version = 1;
    std::optional<std::vector<std::string>> creatures; // u8 count, at most 41

    static FieldGuide parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 5: known spells, or abilities for warriors (guiSpellbook).
struct Spellbook {
    struct Entry {
        std::string name;
        std::uint32_t level = 1; // v >= 2
    };
    std::uint16_t version = 3;
    std::optional<std::vector<Entry>> entries; // u8 count

    static Spellbook parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 6: active enchantments and, for warriors, ability state (guiEnchantment).
struct Enchantment {
    struct Buff {
        std::string name; // ENCHANT_*
        std::uint16_t framesLeft = 0; // 0 means one second's worth
        std::uint8_t power = 0;       // v >= 2
        std::uint32_t shieldHealth = 0; // ENCHANT_SHIELD, v >= 3
    };
    /// Warriors only; inside "has" from v5, after it in v4.
    struct Abilities {
        std::uint8_t berserkActive = 0;
        std::uint8_t treadLightlyActive = 0;
        std::uint32_t treadLightlyTimer = 0;
        std::vector<std::uint32_t> cooldowns; // abilities (berserkActive ? 2 : 1) .. 5
    };
    std::uint16_t version = 5;
    std::optional<std::vector<Buff>> buffs; // u8 count
    std::optional<Abilities> abilities;

    /// The class comes from the Attrib section.
    static Enchantment parse(std::span<const std::uint8_t> data, CharacterClass characterClass);
    std::vector<std::uint8_t> serialize() const;
};

/// 9: journal entries (plr_rwJournal).
struct Journal {
    struct Entry {
        std::string name; // under 64 bytes
        std::uint16_t type = 0;
    };
    std::uint16_t version = 1;
    std::optional<std::vector<Entry>> entries; // u16 count

    static Journal parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 10: solo game state (plr_rwGameData).
struct GameData {
    struct QuestVariable {
        std::string name;
        std::uint32_t type = 0; // 0 int, 1 bool
        std::uint32_t value = 0;
    };
    struct MagicWall {
        std::uint32_t unknown = 0;
        std::array<std::uint8_t, 9> fields{}; // x, y, prevTile, prevDir, prevFlags, tile, flags, dir, hp
    };
    std::uint16_t version = 5;
    std::uint32_t lastObjectScriptId = 0; // v >= 5
    /// The game stores a u16 length and then writes twice that many bytes (a bug): the map name
    /// and whatever followed it in memory. Kept raw; use mapName() for the name.
    std::vector<std::uint8_t> mapNameBytes;
    std::uint16_t questVariablesVersion = 1; // v >= 2
    std::vector<QuestVariable> questVariables;
    std::uint16_t magicWallsVersion = 1; // v >= 3
    std::vector<MagicWall> magicWalls;   // u8 count
    std::uint8_t questStage = 0;         // v >= 4

    std::string mapName() const;

    static GameData parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

/// 11: aligns the server sections to 8 bytes.
struct Pad {
    std::uint16_t version = 1;
    std::size_t zeros = 0;

    static Pad parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> serialize() const;
};

} // namespace nox::player
