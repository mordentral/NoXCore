// The game's files as the Content Editor opens them: every format noxcore reads, loaded from one
// game folder through noxcore's own readers (so the editor sees what the game sees), plus the
// lists of loose files (maps, windows, characters, mods...). Nothing here writes.
#pragma once

#include "noxcore/audio/audio_bag.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/thing_db.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videotools::content {

/// The kinds of game file the editor knows, in the content browser's order.
enum class FileKind {
    Things,     ///< thing.bin
    Modifiers,  ///< modifier.bin
    GameData,   ///< gamedata.bin
    Monsters,   ///< monster.bin
    SoundSets,  ///< soundset.bin
    Strings,    ///< nox.csf
    Images,     ///< video.bag / video.idx and the HD image pack
    Audio,      ///< Audio.bag / Audio.idx
    Dialog,     ///< Dialog\*.wav
    Windows,    ///< window\*.wnd
    Maps,       ///< maps\<name>\<name>.map (and .nxz)
    Rules,      ///< *.rul
    Characters, ///< Save\*.plr
    Mods,       ///< mods\<id>\ (mode.json, content\)
    Text,       ///< mapcycle.txt, motd.txt
    Fonts,      ///< *.fnt
    Palette,    ///< default.pal
};

inline constexpr int kFileKindCount = static_cast<int>(FileKind::Palette) + 1;

/// As the browser shows it ("Things", "Weapons & armour", ...).
const char* fileKindLabel(FileKind kind);
/// The file it comes from, for the record editor's path line ("thing.bin", "nox.csf", ...).
const char* fileKindFile(FileKind kind);

/// One file's load: where it was looked for, and why it failed (for the Problems panel).
struct FileStatus {
    FileKind kind = FileKind::Things;
    std::filesystem::path path;
    bool loaded = false;
    bool optional = false; ///< a missing one isn't a problem (an install without Dialog\, mods\...)
    std::string error;
};

/// Where the loose files the editor lists live.
struct GameFileDirs {
    std::filesystem::path gameDir;
    /// Characters: the game's Save folder (empty: <gameDir>\Save).
    std::filesystem::path saveDir;
    /// Mods and content packs: the mods folder beside noxcore.exe (empty: none listed; never the
    /// game folder's, which noxcore.exe doesn't read).
    std::filesystem::path modsDir;
};

class GameFiles {
public:
    /// Loads every file it finds (one that fails is noted in status(), not thrown).
    static GameFiles load(const GameFileDirs& dirs);

    const GameFileDirs& dirs() const { return dirs_; }
    const std::vector<FileStatus>& status() const { return status_; }
    const FileStatus* statusOf(FileKind kind) const;

    const nox::things::ThingDb* things() const { return things_ ? &*things_ : nullptr; }
    const nox::things::ModifierDb* modifiers() const { return modifiers_ ? &*modifiers_ : nullptr; }
    const nox::things::GameData* gameData() const { return gameData_ ? &*gameData_ : nullptr; }
    /// monster.bin as a solo game reads it, and as a network (arena) game does.
    const nox::things::MonsterDb* monstersSolo() const { return monstersSolo_ ? &*monstersSolo_ : nullptr; }
    const nox::things::MonsterDb* monstersArena() const { return monstersArena_ ? &*monstersArena_ : nullptr; }
    const nox::things::SoundSets* soundSets() const { return soundSets_ ? &*soundSets_ : nullptr; }
    const nox::strings::StringTable* strings() const { return strings_ ? &*strings_ : nullptr; }
    /// Audio.bag (nullptr without one). Not thread-safe: decode on the UI thread.
    nox::audio::AudioBag* audio() const { return audio_.get(); }

    /// Loose files, sorted by name (paths relative to their folder where shown).
    const std::vector<std::filesystem::path>& dialog() const { return dialog_; }
    const std::vector<std::filesystem::path>& windows() const { return windows_; }
    const std::vector<std::filesystem::path>& maps() const { return maps_; }
    const std::vector<std::filesystem::path>& rules() const { return rules_; }
    const std::vector<std::filesystem::path>& characters() const { return characters_; }
    const std::vector<std::filesystem::path>& mods() const { return mods_; }
    const std::vector<std::filesystem::path>& texts() const { return texts_; }
    const std::vector<std::filesystem::path>& fonts() const { return fonts_; }
    const std::vector<std::filesystem::path>& palettes() const { return palettes_; }

private:
    GameFileDirs dirs_;
    std::vector<FileStatus> status_;
    std::optional<nox::things::ThingDb> things_;
    std::optional<nox::things::ModifierDb> modifiers_;
    std::optional<nox::things::GameData> gameData_;
    std::optional<nox::things::MonsterDb> monstersSolo_;
    std::optional<nox::things::MonsterDb> monstersArena_;
    std::optional<nox::things::SoundSets> soundSets_;
    std::optional<nox::strings::StringTable> strings_;
    std::shared_ptr<nox::audio::AudioBag> audio_;
    std::vector<std::filesystem::path> dialog_, windows_, maps_, rules_, characters_, mods_, texts_, fonts_, palettes_;
};

} // namespace videotools::content
