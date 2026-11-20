// A mod's own game data (mods\<id>\data\): what its games read in place of the install's
// thing.bin, modifier.bin, gamedata.bin, monster.bin and soundset.bin, on the server and in each
// NoX Core client of the game (server/mode_data.hpp, MSG_NOXCPP_GAMEDATA: net/game_data_proto.hpp).
// Every other game reads the install's as always.
//
// Two forms of each file, at most one of them a file:
//   - **An override** (the primary form, small): <name>.patch, plain text holding only the records
//     and fields the mod adds or changes, in the content packs' patch formats (the Content Editor
//     writes them): thing.patch (content/thing_patch.hpp: a type's attribute lines by its name,
//     NEW <Name> LIKE <Type> for a new type), modifier.patch, gamedata.patch, monster.patch,
//     soundset.patch (content/text_table.hpp: the table's own format, each record by its name,
//     only its changed fields; "-KEY" removes a key's lines; '#' comments). The game's reader
//     reads the install's file, the override applied over it (mergeDataFile): field-level, so a
//     record names only what changes. Every line must apply: a record or line it can't (a type
//     that isn't there, a bad value, a broken structure) refuses it. New types go after every type
//     the install's thing.bin has, in the patch's order: their numbers are the same wherever the
//     same install's file is read, and the merged table's check (mergedCheck) proves it.
//   - **A whole file** (an option): <name>.bin, the game's file as it keeps it (encrypted), read in
//     place of the install's.
//
// What's checked of one, wherever it comes from (a mod's folder, a server): its size (not empty, at
// most its cap; a whole file in whole cipher blocks), then its reader (a whole thing.bin's strict
// reader; a text table must give a record; an override must parse without a problem). The install's
// content packs (content_pack.hpp) never apply to a mod's game: its tables are the install's raw
// files with the mod's overrides, or the mod's files, the same on the host and every client.
#pragma once

#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::things {

/// The files, by the number the wire carries: the five whole files, then their five overrides.
enum class DataFile : std::uint8_t {
    Things = 0,    ///< thing.bin: types, images, names, spells (server and client)
    Modifiers = 1, ///< modifier.bin: weapons, armour, materials, enchantments (server and client)
    GameData = 2,  ///< gamedata.bin: the game's numbers (server and client)
    Monsters = 3,  ///< monster.bin: the creatures' numbers (the server's alone)
    SoundSets = 4, ///< soundset.bin: the creatures' sounds (the server's alone)
    ThingsPatch = 5,    ///< thing.patch
    ModifiersPatch = 6, ///< modifier.patch
    GameDataPatch = 7,  ///< gamedata.patch
    MonstersPatch = 8,  ///< monster.patch
    SoundSetsPatch = 9, ///< soundset.patch
};
inline constexpr std::size_t kDataFileCount = 10;
/// The game's files themselves (Things ... SoundSets).
inline constexpr std::size_t kDataTableCount = 5;

/// Its file's name ("thing.bin", "thing.patch").
std::string_view dataFileName(DataFile file) noexcept;
/// The file a name is (any case: "Thing.BIN"); none: not one of them.
std::optional<DataFile> dataFileNamed(std::string_view name) noexcept;
/// Whether it's an override (a .patch).
bool isDataPatch(DataFile file) noexcept;
/// The game's file it stands for or changes (thing.patch: Things).
DataFile dataTableOf(DataFile file) noexcept;
/// The most bytes it may have (thing.bin 8 MB; the text tables and every override 1 MB).
std::size_t dataFileMaxBytes(DataFile file) noexcept;
/// The most a set of them may have together.
inline constexpr std::size_t kMaxDataFilesBytes = 12u << 20;
/// Whether a client reads it (thing, modifier and gamedata, whole or override); the server reads all.
bool clientReadsDataFile(DataFile file) noexcept;

/// One file of a set, as it is on disk (a whole file encrypted, an override plain text), with its
/// CRC-32 (io::crc32); an override's `merged`, when known, is the check of the table it gives
/// over the install's file (mergedCheck): a client compares its own merge with the host's.
struct DataFileBytes {
    DataFile kind = DataFile::Things;
    std::vector<std::uint8_t> bytes;
    std::uint32_t crc = 0;
    std::uint32_t merged = 0;
};

/// "" when `bytes` (as on disk) can stand for `file`, else why not ("not whole cipher blocks", the
/// reader's error, "no records", an override's first problem). An override is only read here; what
/// it changes is checked when it's applied (mergeDataFile). Never throws.
std::string checkDataFile(DataFile file, std::span<const std::uint8_t> bytes);

/// The tables a set of files gives; one left out where the set has neither form of it (the
/// install's is read for that one, as for any game).
struct DataTables {
    std::optional<ThingDb> things;
    std::optional<ModifierDb> modifiers;
    std::optional<GameData> gameData;
    std::optional<MonsterDb> monsters; ///< (read for a solo game's numbers or a network game's: `solo`)
    std::optional<SoundSets> soundSets;
    /// Each override's merged check (mergedCheck), by its table (DataFile Things ... SoundSets);
    /// 0 where it has none.
    std::array<std::uint32_t, kDataTableCount> merged{};
    /// The types a thing.patch added (NEW), each with the install's type it's made like (its LIKE,
    /// or that one's when it names a type added before it), in order: what the game's class tables
    /// and modifier.bin answer them by (setAddedTypeBases).
    std::vector<std::pair<std::string, std::string>> addedBases;
};
/// The set's tables read (`clientOnly`: the client's three alone). An override is applied over the
/// install's file in `installDir` (its raw file: no content pack). A file whose `merged` isn't 0
/// must give that check (a client's: else its install's file isn't the host's). Throws
/// std::runtime_error "<file>: <why>" for the first that won't read or apply.
DataTables readDataTables(std::span<const DataFileBytes> files, bool solo, const std::filesystem::path& installDir,
                          bool clientOnly = false);

} // namespace nox::things
