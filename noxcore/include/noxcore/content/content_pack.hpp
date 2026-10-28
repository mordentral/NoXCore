// Content packs: a mod folder's content\ (mods\<id>\content\, installed by hand like the
// game-mode mods, never downloaded) holding patch files that change the game's data files as
// they're loaded: gamedata.patch, monster.patch, soundset.patch, modifier.patch, each a table in
// its file's own format holding only the records and fields that change (text_table.hpp), and
// thing.patch, the object types' changed attribute lines (thing_patch.hpp).
//
// A program that runs games installs the packs once at start-up (installContentPacks, from its
// mods folder); each data file's loader then applies them to the file's text before reading it,
// once, at load (applyContentPacks). With no pack installed the loaders read the files exactly as
// before: nothing is applied and nothing is copied.
//
// Every installed pack is applied whole wherever the file is loaded: a field only one side reads
// does nothing on the other, so the host gets the gameplay and a player the drawing, sounds and
// names, of the packs each has installed (the editor tags every field with that reach).
#pragma once

#include "noxcore/content/text_table.hpp"
#include "noxcore/things/thing_db.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nox::content {

struct ContentPack {
    std::string id;             ///< its mod folder's name
    std::filesystem::path dir;  ///< mods\<id>\content
    std::string title;          ///< pack.txt's title, else the id
    std::vector<TableFormat> tables; ///< the patch files it has
    bool things = false;             ///< and a thing.patch (thing_patch.hpp)
    bool strings = false;            ///< and a strings.patch (csf_document.hpp)
    bool audio = false;              ///< and an audio\ folder of <name>.wav samples
    bool dialog = false;             ///< and a dialog\ folder (spoken lines)
    bool music = false;              ///< and a music\ folder (tracks)
};

/// The patch file's name for a table: "gamedata.patch", "monster.patch"...
std::string patchFileName(TableFormat format);

/// The packs in `modsDir` (each mods\<id>\ with a content\ folder holding at least one patch
/// file), in id order (later ids win where two change the same field).
std::vector<ContentPack> findContentPacks(const std::filesystem::path& modsDir);

/// The packs every load applies from now on (a program's start-up; none until then). Returns
/// lines for the program's log: the packs and their patch files.
std::vector<std::string> installContentPacks(std::vector<ContentPack> packs);
/// installContentPacks(findContentPacks(modsDir)).
std::vector<std::string> installContentPacks(const std::filesystem::path& modsDir);
std::vector<ContentPack> installedContentPacks();

/// Applies the installed packs' patches for `format` to `plain`, a data file's decrypted bytes
/// (as the game's reader will read them). False, and `plain` untouched, when no installed pack
/// has one. Each change and problem goes to the content-pack log.
bool applyContentPacks(TableFormat format, std::vector<std::uint8_t>& plain);
/// The same with an explicit list (the editor's preview of a pack, the tests).
bool applyContentPacks(TableFormat format, std::vector<std::uint8_t>& plain, const std::vector<ContentPack>& packs,
                       std::vector<std::string>* log);

/// Where applyContentPacks' lines go: `sink` (a program's log), or, with none set, kept for
/// takeContentPackLog.
void setContentPackLog(std::function<void(const std::string&)> sink);
/// The lines kept since the last call.
std::vector<std::string> takeContentPackLog();
/// A line about the packs to that log (each time: a join refused for the types they add).
void writeContentPackLog(const std::string& line);

/// The object types content packs add to thing.bin (their NEW records), after every type it
/// has. A type travels between games as its number, so a game with them and one without (a stock
/// game, an older NoX Core, one with other packs) read each other's objects wrongly: a host with
/// them lets in only the games whose added types are the same (the same hash).
struct NewThingTypes {
    std::uint32_t count = 0;
    std::uint64_t hash = 0;         ///< newThingTypesHash's (0: none added)
    std::vector<std::string> packs; ///< the ids of the packs that add them
    /// Each added type and the stock type it's made like (its LIKE, or that one's when it names
    /// a type added before it), in the order they were added: what the installed packs' load
    /// gives things::setAddedTypeBases (the class tables and modifier.bin answer by it).
    std::vector<std::pair<std::string, std::string>> bases;
    bool operator==(const NewThingTypes&) const = default;
};

/// What identifies `db`'s types from `first` on (each one's name, lines, DRAW and pictures):
/// two games with the same hash read every one of them alike. 0 when there are none; never 0
/// otherwise.
std::uint64_t newThingTypesHash(const things::ThingDb& db, std::size_t first);

/// The installed packs' thing.patch files applied to a loaded thing.bin (loadThingDb, once);
/// false and `db` untouched when no installed pack has one. The types they add are then
/// newThingTypes(), and their stock types things::addedTypeBases().
bool applyContentPacksToThings(things::ThingDb& db);
/// The same with an explicit list (the editor, the tests); the types added go to `added`.
bool applyContentPacksToThings(things::ThingDb& db, const std::vector<ContentPack>& packs, std::vector<std::string>* log,
                               NewThingTypes* added = nullptr);
/// The types the installed packs added to thing.bin as it last loaded (none before it has).
NewThingTypes newThingTypes();

/// The installed packs' strings.patch files applied to nox.csf's bytes as it loads
/// (StringTable::load, once); false and `csf` untouched when no installed pack has one.
bool applyContentPacksToStrings(std::vector<std::uint8_t>& csf);
bool applyContentPacksToStrings(std::vector<std::uint8_t>& csf, const std::vector<ContentPack>& packs, std::vector<std::string>* log);

/// The installed packs' audio\ folders, the first installed first (a sample's <name>.wav in one
/// of them is played instead of Audio.bag's).
std::vector<std::filesystem::path> contentPackAudioDirs();
/// The installed packs' content folders, the first installed first (the files in their dialog and
/// music folders are played instead of the game folder's, the last installed pack's first).
std::vector<std::filesystem::path> contentPackDirs();

} // namespace nox::content
