// A script mode's parts beyond its first toolbox, one set a
// ScriptedMode, reached from the toolbox with toolbox::partsOf(L). Each part is its own file.
// (Internal to the script modes' sources.)
#pragma once

#include "mod_events.hpp"
#include "mod_generate.hpp"
#include "mod_scheduler.hpp"
#include "mod_store.hpp"
#include "mod_weather.hpp"

#include "noxcore/server/game_host.hpp"

#include <cstdint>
#include <memory>
#include <string>

struct lua_State;

namespace nox::modes::script {

struct Parts {
    Scheduler scheduler;
    Events events;
    Generation generation;
    /// nox.weather's: nothing kept or sent till the mode first calls it.
    Weather weather;
    /// nox.store's (opened at the first call; none while the host keeps no store).
    std::shared_ptr<ModStore> store;
    /// This game's locks' token (nox.store.lock).
    std::uint64_t storeOwner = ModStore::newOwner();
    /// The asked hook running (onDamage, onExit; null: none): the world's changes refused in it.
    const char* asked = nullptr;
};

/// The parts' toolbox functions set in the `nox` table on top of L's stack: the scheduler's, the
/// events', the store's (nox.store when the host keeps one), the items', the players', the
/// generator's, the weather's.
void addPartsToolbox(lua_State* L, bool store);

/// configure's returned table (at `t`): its quest keys onto the host's options (13.7: quest,
/// questScreens, questMaps, forceClass, multiclass, monstersAlert, keepThingsOnDeath, serverName; absent: as they
/// were). `firstMap` the map it plays first (the quest's first). Quest saves are never turned on.
void applyHostOptions(lua_State* L, int t, server::GameHost::Options& o, const Generation& gen, const std::string& firstMap);

} // namespace nox::modes::script
