// A solo game's save and its folders, as GAME.EXE keeps them:
//   <game dir>\Save\WORKING\Player.plr            the host player (Plr_SaveServerPlayerData, then
//                                                 the client's own sections)
//   <game dir>\Save\WORKING\<map>\<map>.map       each map visited, as the save left it
//   <game dir>\Save\AUTOSAVE\..., Save\SAVE%04d\  copies of WORKING
//
// The save: the host (slot 0x1F) alive, the folders made, a SaveGameLocation put at the player
// (or the exit's way out) with the player's script id, holding the player's things in the world
// while the map's written (saveMakePlayerLocation), the map written
// (mapSaveMap), the things back to the player and the location gone
// (monstersAllBelongToHost), Player.plr; a save named other than WORKING:
// "AutoSaveComplete" to everyone and WORKING copied to it. (The host's own drag and quick bar
// closed first, and its client's .plr sections appended: HostClient::saveStarting /
// saveClientSections, noxsdl's.)
#pragma once

#include "noxcore/map/map_file.hpp"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nox::server {

class Server;
class World;
struct ServerObject;

struct SoloSaveOptions {
    /// The game's folder (Game_GetWorkDir): Save\ is made under it.
    std::filesystem::path gameDir;
    /// the host client's own sections appended to Player.plr (GUI, file info,
    /// music...) and the file's closing 0 id after them; false when that fails (the save fails).
    /// None: the World's HostClient::saveClientSections; with neither, only the closing 0 id (one
    /// zero block) appended, as the sections' writer does with none.
    std::function<bool(const std::filesystem::path& plr)> clientSections;
};

/// the game saved as `name` (an exit's asking: `exit`, the player put at its way
/// out in the save); false when it fails.
bool saveSoloGame(Server& server, const SoloSaveOptions& options, std::string_view name, ServerObject* exit);

/// The map as the save writes it (mapSaveMap's sections, in its table's order).
map::MapFile saveMap(Server& server);

/// saveMakePlayerLocation: a SaveGameLocation at the host player (or `exit`'s way
/// out), the player's script id, owning the player's things in the world; null when there's no
/// host player.
ServerObject* makeSaveLocation(Server& server, ServerObject* exit);
/// monstersAllBelongToHost: the location's things back to the host player (a
/// creature it commanded acquired again: subclass 0x80, MSG_REPORT_ACQUIRE_CREATURE, its minimap),
/// the location's script id 0 and the location deleted.
void restoreFromSaveLocation(Server& server);

/// the saved game `name` (Save\<name>; WORKING itself) loaded, under
/// game flag 0x8000000: the friendly creatures and players' pixies gone, the slot
/// copied to WORKING, its Player.plr ("Savegame.c:AutoSaveNotFound" without one) read into the
/// host player with the script ids going on from its GameData's,
/// switch-to-solo and not-multiplayer, then the map it was saved on asked for from
/// WORKING (World::exitTo, the host loads the copy: no AUTOSAVE at that load); the host's client
/// (HostClient) plays the map intro and reads its own .plr sections (a failure fails the load; the
/// GUI's summon order set: orderUnitLocal), its console cleared; chat bubbles
/// cleared, "Savegame.c:GameLoaded". False where the exe's fails.
bool loadSoloGame(Server& server, const SoloSaveOptions& options, std::string_view name);

/// Plr_LoadClientPlayerData by the host's own client (World::hostClient: none, true):
/// `plr`'s client sections read (HostClient::loadClientSections), the GUI's summon order set on
/// the host and told as soon as it's read in a solo game (orderUnitLocal); false
/// when the read fails.
bool loadHostClientSections(World& world, ServerObject& host, const std::filesystem::path& plr);

/// The script ids' counter a saved game goes on from: in a solo game
/// (`solo`) its .plr's (`plr`, as on disk) GameData (id 10, version 5) last id, else (or 0)
/// 1000000000.
std::int32_t savedScriptCounter(std::span<const std::uint8_t> plr, bool solo);

/// the saved copy of the map an exit names ("<map>.map[:waypoint]", cut at its
/// last ':'; the folder its name less the last 4), Save\WORKING\<map>\<map>.map under `gameDir`,
/// when it's there.
std::optional<std::filesystem::path> savedMapCopy(const std::filesystem::path& gameDir, std::string_view exitMap);

/// The saved game's restore (every map's load, under game flag 0x8000000), with the host player:
/// `fromSave` (a visited map's saved copy: switch-to-solo), the player put on the
/// SaveGameLocation with its script id (the location gone), the timers' objects found (script_activatorResolveObjs),
/// and after a save by an exit the copy's own friendly creatures and the players' pixies gone
/// (those not `carried`: the live ones came along); then, every load, the host's creatures told
/// it again (acquired, or watched; its minimap) and switch-to-solo / "saved by an exit" off.
void soloMapLoaded(Server& server, bool fromSave, const std::vector<std::uint32_t>& carried);

namespace saves {
/// a folder made (false only when its parent's missing).
bool makeFolder(const std::filesystem::path& folder);
/// savegame_rm: Save\<name>'s Player.plr and each of its folders (their files) gone;
/// `removeFolder`, Save\<name> too.
void remove(const std::filesystem::path& gameDir, std::string_view name, bool removeFolder);
/// Save\<to> made and emptied, Save\<from>'s Player.plr and each folder's files
/// copied to it; false when a step fails (a file not copied still lets the rest go on).
bool copy(const std::filesystem::path& gameDir, std::string_view from, std::string_view to);
} // namespace saves

} // namespace nox::server
