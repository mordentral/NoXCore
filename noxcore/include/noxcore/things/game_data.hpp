// gamedata.bin: the game's tuning numbers (spell counts and durations, trap costs, speeds...).
//
// Text once decrypted (key FileKey::GameDataBin), read by the exe into a table that
// Game_GetGamedataFloat and gamedataGetFloatTable look names up in:
//   # comment
//   [SOLO | ARENA] Name = v1, v2 ...;    up to 255 numbers, split on spaces, commas and tabs
// A line prefixed SOLO is only for the solo game (engine flag 0x800 or 0x200000), one prefixed
// ARENA only for the arena (game flag 0x2000); a line with neither is for both.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::things {

/// A gamedata name for the look-ups made every frame: a constant at its call site, its place in
/// a GameData's index found at its first look-up and kept (found again only from another GameData),
/// so no name is hashed or compared after that. The same answers as the name's.
class GameDataKey {
public:
    constexpr explicit GameDataKey(std::string_view name) noexcept : name_(name) {}
    GameDataKey(const GameDataKey&) = delete;
    GameDataKey& operator=(const GameDataKey&) = delete;
    std::string_view name() const noexcept { return name_; }

private:
    friend struct GameData;
    std::string_view name_;
    /// (The GameData's serial << 32 | its slot + 1, 0 for a name it lacks; relaxed: any thread
    /// reads a whole one.)
    mutable std::atomic<std::uint64_t> found_{0};
};

struct GameData {
    enum class Section { Common, Solo, Arena };
    struct Entry {
        std::string name;
        std::vector<float> values;
        Section section = Section::Common;
    };
    std::vector<Entry> entries; ///< file order

    /// The look-ups' index (noxcpp's, for speed): each name's entries in file order, by the name
    /// without case; built by parseGameData (reindex). Entries added since: the look-ups scan.
    struct NoCaseHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept;
    };
    struct NoCaseEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept;
    };
    struct Named {
        std::vector<std::uint32_t> entries; ///< file order
        std::uint32_t slot = 0;             ///< its row of `picks`
    };
    std::unordered_map<std::string, Named, NoCaseHash, NoCaseEqual> byName;
    /// Each name's answer (an entry, -1 none) for each kind of game: index 1 if solo (game flags
    /// 0x800 or 0x200000), + 2 if online (0x2000), as find() picks it.
    std::vector<std::array<std::int32_t, 4>> picks;
    std::uint32_t serial = 0; ///< this index's number (GameDataKey's), new each reindex
    std::size_t indexed = 0; ///< the entries the index holds
    /// The one rebuild of every index and cache of this table (byName, picks, and a new serial,
    /// so every GameDataKey finds its name again). parseGameData calls it; ANYTHING that changes
    /// `entries` after load must call it again: the `indexed` count only notices entries added
    /// or removed, an in-place change of the same size (a renamed entry, a changed section)
    /// leaves the index stale until this is called. (Values changed in place are read through
    /// the entries: no rebuild needed for those.)
    void reindex();

    /// The values for `name` in a multiplayer (arena) game, from its ARENA or unprefixed line
    /// (the last if there are several); nullptr if none.
    const std::vector<float>* arena(std::string_view name) const;
    /// One value of an arena table (gamedataGetFloatTable: 0 past its end).
    float arena(std::string_view name, std::size_t index) const;
    /// The values for `name` as a game with `gameFlags` reads the file: a solo
    /// game (0x800 or 0x200000) skips the ARENA lines, a network game (0x2000) the SOLO ones;
    /// the last line standing wins. nullptr if none.
    const std::vector<float>* find(std::string_view name, std::uint32_t gameFlags) const;
    /// The same by a key.
    const std::vector<float>* find(const GameDataKey& key, std::uint32_t gameFlags) const;
};

/// A number as a game with `gameFlags` reads it (Game_GetGamedataFloat,
/// gamedataGetFloatTable), from `data`: 0 for a name the file lacks or an index past a table's
/// end, as the exe reads them. Every program that runs a game loads the install's gamedata.bin at
/// start-up (a missing one stops it, as a missing thing.bin does); null `data` reads 0 as well.
float gameNumber(const GameData* data, std::string_view name, std::uint32_t gameFlags, std::size_t index = 0);
/// The same by a key.
float gameNumber(const GameData* data, const GameDataKey& key, std::uint32_t gameFlags, std::size_t index = 0);

GameData parseGameData(std::string_view text);
/// Reads and decrypts gamedata.bin. Throws on a missing file.
GameData loadGameData(const std::filesystem::path& path);

} // namespace nox::things
