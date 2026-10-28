// The front end's character screens as GAME.EXE's shell runs them (mainmenu.c, selchar.c,
// SelClass.c, SelColor.c), the parts with no window in them:
//
//   Solo      windowMainMenuProc's 0x6F: maps\con01a\con01a.map there (checkHasSoloMaps) and
//             the game's CD, else "mainmenu.c:caution"; then game flag 0x800 and, when
//             client_countSaveFiles finds a saved game, the character screen (SelChar.wnd, 500),
//             else the class screen.
//   SelChar   game_showSelChar / findAutosaves: in a solo game the 14 slots (savegame_sub,
//             solo_saves.hpp); in a network one every Save\*.plr the .plr reader reads that suits
//             the game (quest characters for a quest game, the others for the rest).
//   Load      windowSelCharProc's 0x1F6, in a solo game: the slot copied to WORKING, its File
//             Info held, the class's first map named, then the saved map's copy in WORKING (else
//             the game's own), switch-to-solo, not-multiplayer, no AUTOSAVE at that load
//             (mapLoadOrSaveMB(1)) and the script ids going on from the save's.
//   Delete    0x1F7: AUTOSAVE refused ("AutoSaveDeleteNotAllowed"), an empty slot NoCanDo, else
//             asked, then the slot's folder gone (savegame_rm(name, 1)).
//   New       the class screen (game_showSelClass: the spell bars emptied, a network game's
//             random starting set only out of a solo game) and the looks (SelColor.wnd); Accept
//             (its solo branch): WORKING emptied, Save\WORKING\Player.plr written with the
//             client's sections (writeNewCharacter), then the class's first map.
#pragma once

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/player/player_file.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client::solo_menu {

/// checkHasSoloMaps: maps\con01a\con01a.map under the game's folder.
bool hasSoloMaps(const std::filesystem::path& gameDir);

/// client_countSaveFiles (Save\ made first): AUTOSAVE's Player.plr if it's there. Its
/// 13 "SAVE%04d" checks are given no number (the format reads the caller's EDI instead), so they
/// look for one path that isn't there and count nothing: an original bug.
int countSaveFiles(const std::filesystem::path& saveRoot);

/// The class's first map, as windowSelCharProc and the class screen's Accept name it: War01a, Wiz01a, Con01a
/// ("" for a class past 2: the map left as it was).
std::string_view firstMap(int characterClass);

/// The 8 characters before a path's last '\' (a slot's folder, "SAVE0001"); `out`
/// left as it was when the path has none.
void folderOf(std::string_view path, std::string& out);

/// What the character screen's Delete (0x1F7) does in a solo game for `slot`, `remembered` being
/// the remembered save name (the slot's path's: kept from the last time for an
/// empty slot, whose path is empty, an original bug).
enum class DeleteAnswer {
    NotAllowed, ///< AUTOSAVE: "GUISave.c:AutoSaveDeleteNotAllowed" (OK) with NoCanDo
    NoCanDo,    ///< an empty slot: the NoCanDo sound
    Ask,        ///< "GUISave.c:DeleteSaveTitle" / DeleteSaveMessage, Yes / No
};
DeleteAnswer soloDelete(std::string& remembered, const solo_saves::Slot& slot);

/// Whether a character's .plr (its File Info's flags) is listed for the game being
/// joined; `questGame`: isQuest or the Join's quest flag (a quest game hosted, or the
/// listed game's flags & 0x1000 at the Join, windowMultiplayerSub).
bool listedForGame(std::uint32_t fileFlags, bool questGame);

/// client_countPlayerFiles04: the Save\*.plr whose File Info is flagged 4 (a quest
/// character); a file that doesn't read, or has no File Info, counts as the one before it (the
/// record isn't cleared).
int countQuestCharacters(const std::filesystem::path& saveFolder);
/// client_countPlayerFiles02: the same for flag 2 (a network character). The Join and
/// the Host (its 0x2711) go to the character screen when this (in a quest
/// game countQuestCharacters) isn't 0, else straight to the class screen.
int countNetworkCharacters(const std::filesystem::path& saveFolder);

/// A line of the network character screen's list (findAutosaves): the record's path (empty when no
/// file was read into it) and File Info.
struct Character {
    std::filesystem::path path;
    player::FileInfo info;
};
/// findAutosaves out of a solo game: Save\ made, then each Save\*.plr (not a
/// folder) read by the .plr reader into the next record of a zeroed array, kept when
/// listedForGame lists it for the game (`questGame`); a file that doesn't read leaves the record as it was (zero,
/// or the last one turned down), one with no File Info only gets its path. So out of a quest an
/// unreadable .plr is listed as a zeroed record (no name, no path): an original bug. Sorted by
/// the newest first (year, month, day, hour, minute, second, ms).
std::vector<Character> listCharacters(const std::filesystem::path& saveFolder, bool questGame);

/// The File Info section's flags as parseFileInfoData writes them: out of the
/// network 1 (2 and 4 off); in it 2, or 4 for a quest game (game flag 0x1000, isQuest), 1 off.
std::uint32_t fileInfoFlags(std::uint32_t held, bool network, bool questGame);

/// The class screen Accept's solo branch: WORKING emptied (savegame_rm("WORKING", 0)), Save\ and
/// Save\WORKING\ made, and Save\WORKING\Player.plr written by the .plr writer (info, 1): the GUI
/// section (`gui`: the bars as the class screen left them), File Info (`held` as
/// parseFileInfoData writes it: the file's path, `now`, level 1 (with no player), `mapName` and the held
/// stage) and Music, then the 0 id. The path written, or empty if it couldn't be.
std::filesystem::path writeNewCharacter(const std::filesystem::path& saveRoot, const player::FileInfo& held,
                                        const player::Gui& gui, const player::Music& music, std::string_view mapName,
                                        const std::array<std::uint16_t, 8>& now);

/// The map windowSelCharProc's Load starts a solo game on, once the slot is in WORKING: "%s.map"
/// of the File Info's map as it is (an empty one names ".map": the class's first map, set just
/// before, replaced by it), Save\WORKING\<map>\<map>.map when it's there, else the game's
/// maps\<map>\<map>.map.
std::filesystem::path continueMap(const std::filesystem::path& saveRoot, const std::filesystem::path& gameDir,
                                  std::string_view mapName);

} // namespace nox::client::solo_menu
