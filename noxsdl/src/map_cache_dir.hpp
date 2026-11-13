// Where the maps a server sends us are saved by default (--map-cache names another folder): the
// noxsdl_maps folder beside noxcore.exe, as its log is, else (a folder it can't write in: one in
// Program Files, a protected folder) the one in the user's own settings folder, where the log
// goes then too. Never the current folder: a shortcut or launcher can start the program in one it
// can't write in (C:\Windows\System32), and a download then failed as it finished, the game left.
#pragma once

#include <filesystem>
#include <functional>

namespace noxsdl {

/// Whether files can be made in `dir` (made if missing): a probe file written and removed.
bool canWriteIn(const std::filesystem::path& dir);

/// The download folder: `exeDir`/noxsdl_maps, else `prefDir()`/noxsdl_maps (asked for only then),
/// the first one that can be written in (an empty folder skipped); if neither, the first of them
/// there is (and with neither, noxsdl_maps in the current folder).
std::filesystem::path defaultMapCache(const std::filesystem::path& exeDir, const std::function<std::filesystem::path()>& prefDir);

} // namespace noxsdl
