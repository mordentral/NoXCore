// noxcore.exe's mods: the `mods` folder beside it (mods\<id>\: script modes, never downloaded),
// loaded at start-up and read again by Host Custom's Refresh; what loaded and what didn't, for
// the list's rows.
#pragma once

#include "noxcore/modes/mods.hpp"

#include <filesystem>

namespace noxsdl {

/// `mods` beside noxcore.exe (or the folder setModsFolder named).
std::filesystem::path modsFolderPath();
/// The mods read from `dir` from now on instead (--mods-dir: a map editor session's private test
/// of the session's mod, from the editor's own copy of it); refreshMods reads it.
void setModsFolder(std::filesystem::path dir);
/// The folder read into the process's registry (start-up), its report logged.
void loadModsAtStart();
/// The folder read again (Host Custom's Refresh: between games), its report logged.
void refreshMods();
/// The last read's report.
const nox::modes::ModsReport& modsReport();
/// The folder opened in the system's file browser (made first if it isn't there); whether it
/// could be.
bool openModsFolder();

} // namespace noxsdl
