// A mod's own game data on noxcore.exe's side (things/data_files.hpp, server/mode_data.hpp): a
// game whose mod ships thing.bin, modifier.bin or gamedata.bin is read with those in place of the
// install's, from its first map till the game is left, then the install's again.
//
// The app's three tables (App::things, modifiers, gameData) stay the objects everything holds; their
// contents are swapped (the install's kept in App::installTables meanwhile), on the main thread
// before the game's first map, with nothing of any game drawn yet. What was made from them is made
// afresh: every type's art (ThingArt::reset), the types the scene names, the spell chant's
// phonemes and the looping sounds; with a whole thing.bin, the sound effects' table too (its AUD
// and AVNT sections: game_sounds.hpp; a thing.patch can't change them). What keeps the install's
// (made once at start-up, from the install's own tables): the minimap's invisible wall sets, the
// HUD's and cursor's pictures by name.
//
// No host thread reads the app's tables meanwhile: a hosted mod game's server reads its own
// (LocalHost::modeData), and the hosts that left their games are let go first.
#pragma once

#include "noxcore/things/data_files.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace noxsdl {
struct App;

/// The live session's handler (HeadlessClient::setGameDataHandler): the set read (the client's
/// three files) and made the app's; "" when it is, else why not (the tables stay as they were).
std::string useGameData(App& app, std::uint32_t hash, const std::vector<nox::things::DataFileBytes>& files);
/// The install's tables again, if a mod's are in their place (a game left, another joined).
void useInstallData(App& app);

} // namespace noxsdl
