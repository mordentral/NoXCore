// A Delving Mode host's set-up (delve.hpp), shared by the dedicated server and noxsdl's Host
// Quest/Delve: the director's old maps swept, its lobby written, and the host's options made a
// run's.
#pragma once

#include "noxcore/server/delve.hpp"
#include "noxcore/server/game_host.hpp"

#include <filesystem>
#include <functional>

namespace nox::server {

/// The host's options for a run, with `director` (which must outlive the host): no map cycle or
/// vote (the delve picks its maps); a quest without its rules, stage or results screens, nor its
/// saves (the players' own characters kept); noxsdl players multiclass, or with `conjurers` every
/// player a conjurer whatever its character; the quest's maps the delve's alone; listed as the
/// lobby (the director renames it each map); each level written as the quest goes to it
/// (DelveDirector::prepare), `onLevel` told after. Its own old maps swept first, its lobby written:
/// returns the lobby's .map, the map to host.
std::filesystem::path setUpDelveHost(DelveDirector& director, GameHost::Options& o, bool conjurers,
                                     std::function<void(const DelveDirector&)> onLevel = {});
/// The host's hooks into `director` (setUpDelveHost's, and the pool's): its players' answers at
/// a campfire, by chat (a stock client) and MSG_NOXCPP_MODE_F4 (a noxsdl); and the delve's
/// monsters alert (GameHost::Options::monstersAlert).
void hookDelveHost(DelveDirector& director, GameHost::Options& o);

} // namespace nox::server
