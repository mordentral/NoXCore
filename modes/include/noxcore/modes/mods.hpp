// The mods a host has: each folder under `mods` beside the program (noxcore.exe, a dedicated server) with a
// mode.json, registered as a script mode (script/scripted_mode.hpp). Mods are never downloaded:
// a mod is there because someone put it there. A mod's maps\<Name>\<Name>.map are its games' own
// (server/mode_maps.hpp): a guest gets them through the map download, as any map it lacks.
#pragma once

#include "noxcore/modes/registry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace nox::modes {

/// What loading the mods folder did: the modes registered (their ids), and the folders that
/// aren't (each with why: a bad mode.json, no server script, an id that's taken...), for the lists
/// to show greyed.
struct ModsReport {
    std::vector<std::string> loaded;
    struct Broken {
        std::filesystem::path folder;
        std::string why;
    };
    std::vector<Broken> broken;
    /// What a loaded mod's folder holds that it doesn't take (a map left out of its maps\, and why).
    std::vector<Broken> notes;
};

/// `mods` beside the program in `programDir`.
std::filesystem::path modsFolder(const std::filesystem::path& programDir);

/// Each mod folder in `modsDir` (at most 64) registered in `registry` (none there: nothing).
ModsReport loadMods(const std::filesystem::path& modsDir, Registry& registry);

/// The mods folder read again (a list's Refresh, between games): the modes `previous` loaded
/// taken out of `registry`, then loadMods.
ModsReport reloadMods(const std::filesystem::path& modsDir, Registry& registry, const ModsReport& previous);

/// The report's lines for a log: "mods: <id> loaded", "mods: <folder>: <why>" (the broken, then
/// the notes).
std::vector<std::string> modsReportLines(const ModsReport& report);

} // namespace nox::modes
