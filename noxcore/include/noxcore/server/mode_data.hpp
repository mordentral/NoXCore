// A NoX Core mode's own game data (a mod's data\<file>, beside its mode.json: things/data_files.hpp),
// read once when the mode is: a game of that mode reads those files where it would read the
// install's (its host hands it the tables: GameHost's things, Options' gameData, modifiers,
// monsterDb and soundSets), and sends them to its NoX Core clients before their first map
// (GameHost::Options::modeData, MSG_NOXCPP_GAMEDATA). No other game sees them, solo games
// included: only a host that hosts the mode hands its options the set.
//
// What the set keeps is checked as it's read, never again: a real folder of the mod's own, real
// files (no symbolic link or junction), each resolving inside it, named as the game's files are
// (any case: thing.patch ... or thing.bin ...; one form of a file at most), each under its cap and
// read without fault (an override parsed; what it changes is applied over the install's file when a
// game is made: tables). A file of another name is left out (a note); a bad one refuses the mod. The bytes are kept as read, so what the
// mod's games read and what their clients are sent is the same file.
#pragma once

#include "noxcore/things/data_files.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nox::server {

class ModeData {
public:
    /// The files in `dir`; null when it has none (or there's no `dir`). `why` gets the reason a
    /// file refuses the set (null then too); `notes` (when given) a line for each file left out.
    static std::shared_ptr<const ModeData> read(const std::filesystem::path& dir, std::string& why,
                                                std::vector<std::string>* notes = nullptr);
    /// A set of these files (each checked as read does; why's empty when they all are). For the
    /// tests and a program that has them in memory.
    static std::shared_ptr<const ModeData> fromFiles(std::vector<things::DataFileBytes> files, std::string& why);

    /// Its files, in DataFile order, each once.
    const std::vector<things::DataFileBytes>& files() const noexcept { return files_; }
    /// The file of a kind it has; null: none (the install's is read for it).
    const things::DataFileBytes* find(things::DataFile kind) const;
    /// What tells this set from another (over each file's kind, size and CRC): never 0.
    std::uint32_t hash() const noexcept { return hash_; }
    /// Its bytes together.
    std::size_t bytes() const noexcept;
    /// Its tables read afresh for a game (`solo`: monster.bin's solo numbers), its overrides applied
    /// over the install's files in `installDir`. Throws std::runtime_error as things::readDataTables
    /// (an override that doesn't apply there: the game isn't made).
    things::DataTables tables(bool solo, const std::filesystem::path& installDir) const;

private:
    std::vector<things::DataFileBytes> files_;
    std::uint32_t hash_ = 0;
};

} // namespace nox::server
