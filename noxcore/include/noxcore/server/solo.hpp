// The solo game's own (game flag 0x800 without the network's 0x2000: the campaign, its one player
// the host's, slot 0x1F).
//   - The pause (game flag 0x40000, Game_SetPaused: a solo game's only). While it's on the
//     frame count stands still and the server's tick runs only its first part: the time limit and
//     the like (updateServer), the map's start (MapInitialize, MapEntry), the timed pause's end
//     and the level-up moment's end; not the units, the respawns,
//     the spells, the abilities, the script timers, the votes nor the deletions. Off only with no
//     timed pause pending.
//   - A timed pause (g_pauseUntilInputSeq, the host's mouse input count + n; ours the
//     server's ticks, World::ticks), its end looked for each tick: a solo map's load sets one
//     of 30.
//   - The level-up moment (not while one is on nor while paused): a LevelUp (or
//     OblivionUp) at the player, MSG_FX 0x9A there, for a level the sound 0x386 and
//     "expLevel.c:LevelUP", the player in state 0x1E (its animation at 4), the game paused; 5 s on
//     (real time, ours the ticks) its end: the FX again, the object
//     gone, the player in state 0xD, unpaused (no book's flight on the host).
//   - The host's client's book flight (an award flying to the bars, bookFillAll)
//     is a moment too: the level-up moment (0, kind) at its start (the game paused, untimed)
//     and Game_SetPaused(0) as it lands. While it flies the level-up's end doesn't
//     unpause, and the host can't be saved nor take an exit.
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/mode_hooks.hpp"

#include <cstdint>
#include <random>

namespace nox::server {

/// Game_SetPaused. (noxcpp: a pause asked for `why` a NoX Core mode may refuse: World::modeHooks'
/// mayPause; unpausing is never asked.)
void setPaused(World& world, bool paused, PauseReason why = PauseReason::Other);
/// Paused for `ticks` (0: the timed pause forgotten).
void pauseFor(World& world, std::uint32_t ticks);
/// Each tick: a timed pause over, unpaused (not while the host's client shows a
/// briefing).
void pauseTick(World& world);
/// The host's client asks it before its in-game menu (consoleEsc) and the AutoSave / AutoLoad
/// keys: out of a solo game yes; else the host player there, no save asked for or waiting, no
/// timed pause, 30 frames past a load, no widescreen (the client's), the host not airborne, and
/// mayAct.
bool hostMayOpenMenu(const World& world, std::uint32_t frame);
/// The level-up moment for `unit` (`kind` 0 LevelUp, 1 OblivionUp).
void levelUpMoment(Fight& fight, ServerObject* unit, int kind);
/// The tick's check: the moment over after 5 s.
void levelUpTick(Fight& fight);
/// The moment over (if on): the FX again for a level, the object gone, the player in
/// state 0xD, unpaused unless the host's book flies.
void levelUpEnd(Fight& fight);
/// GetHost: the host player's (slot 0x1F) unit, if any.
ServerObject* hostPlayer(World& world);
const ServerObject* hostPlayer(const World& world);
/// A save asked for (or, `pending` false, none), by `exit` (an Exit: after the save
/// its map is loaded), not before `delay` frames from now.
void requestSave(World& world, bool pending, std::uint32_t exit, std::uint32_t delay, std::uint32_t frame);
/// A save asked for or its wait under way.
bool saveBusy(const World& world);
/// `unit` (the host's) may be saved or take an exit now: no save's wait under way,
/// not talking to an NPC (DialogWith, dialog.hpp), neither NoUpdate nor dead, and no book flying
/// on the host's client.
bool mayAct(const World& world, const ServerObject& unit, std::uint32_t frame);
/// Out of a network game, a creature friendly to the players (subclass
/// 0x100): it goes with them from map to map.
bool isUnit(const World& world, const ServerObject& o);
/// In a solo game, a pixie whose owners lead to a player (kept at a map change).
bool playersPixie(World& world, const ServerObject& o);
/// The solo team (mapReadSetFlags's for a solo map, type bit 1, before the mode's 0x800): game
/// flag 0x200 (so a map change doesn't outfit the player anew), team 1 ("COOP") with the host's
/// player in it, gameplay flag 1 off.
void soloTeam(World& world, std::minstd_rand& rng, std::uint32_t frame);

} // namespace nox::server
