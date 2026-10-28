// An Undead Tide host's set-up (tide.hpp), shared by the dedicated server and (later) noxsdl's
// Host dialog: the director's old maps swept, its lobby written, and the host's options made a
// run's.
#pragma once

#include "noxcore/server/game_host.hpp"
#include "noxcore/server/tide.hpp"

#include <filesystem>
#include <functional>

namespace nox::server {

/// The host's options for a run, with `director` (which must outlive the host): no map cycle or
/// vote; a quest without its rules, stage or results screens, nor its saves (the players' own
/// characters kept); the quest's maps the arena alone, written as the quest goes to it
/// (TideDirector::prepare), `onArena` told after; its creatures look all about them; the players'
/// picks by chat. Its own old maps swept first, its lobby written: returns the lobby's .map, the
/// map to host.
std::filesystem::path setUpTideHost(TideDirector& director, GameHost::Options& o, std::function<void(const TideDirector&)> onArena = {});

} // namespace nox::server
