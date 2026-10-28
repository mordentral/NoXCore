// Finding the installed game's data folder, the way OpenNox does (opennox-lib datapath, MIT; see
// THIRD_PARTY_NOTICES.txt), with the program's own folder first. The first of these that holds the game's
// data wins:
//   1. the NOX_DATA environment variable;
//   2. the program's own folder, then the current folder (if it's another);
//   3. (not Windows) a "Nox" folder under the XDG data, Wine, Lutris, Heroic and home prefixes;
//   4. the registry (HKLM, its 32-bit view too): Westwood\Nox InstallPath and FolderPath
//      (the original install, GOG, Nox Reloaded), Westwood Studios\NOX "Install Dir" (Origin);
//      a value naming an .exe means its folder;
//   5. GOG: the Galaxy game keys (Games\1207658964 PATH / WORKINGDIR), the offline installer's
//      (GOGNOX PATH), Galaxy's client folder + Games\Nox, then the usual GOG folders;
//   6. Nox Reloaded's folders (Program Files\Nox), then Origin's (Origin Games\Nox);
//   7. the folder the program was built with (`configured`, a build's NOX_GAME_DIR), if any.
// The fixed folders are tried on C:\ and D:\ (elsewhere, under the prefixes of 3). A folder
// holds the game's data if it has gamedata.bin, modifier.bin, monster.bin and thing.bin.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nox::io {

/// Whether `dir` has the game's data files (OpenNox's CheckData).
bool isGameDir(const std::filesystem::path& dir);

/// A place the game is looked for, and why (for the log: "next to the program", ...).
struct GameDirCandidate {
    std::filesystem::path dir;
    std::string reason;
};

/// Every place to look, in the order above (some may not hold the game). `exeDir` is the
/// program's folder; `configured` the last resort, left out if empty.
std::vector<GameDirCandidate> gameDirSearch(const std::filesystem::path& exeDir,
                                            const std::filesystem::path& configured = {});

/// The first of gameDirSearch's places holding the game's data (its folder made absolute), or
/// nullopt. `log`, if given, hears each place tried.
std::optional<GameDirCandidate> locateGameDir(const std::filesystem::path& exeDir, const std::filesystem::path& configured,
                                              const std::function<void(const std::string&)>& log = {});

/// gameDirSearch's folders, with no configured last resort.
std::vector<std::filesystem::path> gameDirCandidates(const std::filesystem::path& exeDir);

/// locateGameDir's folder, with no configured last resort.
std::optional<std::filesystem::path> findGameDir(const std::filesystem::path& exeDir,
                                                 const std::function<void(const std::string&)>& log = {});

} // namespace nox::io
