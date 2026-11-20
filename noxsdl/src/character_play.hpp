// (noxcpp) The note beside the character we play (character_labels.hpp): where this game is played,
// and a mode's line about it, written as they become known. Nothing here runs per frame but a
// flag's test; a file is written at most once every few seconds, and once more as the game ends.
#pragma once

#include "noxcore/client/character_notes.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace noxsdl {

struct App;

/// Whether `plr` is a character the network character screen lists (a .plr in its folder): only
/// those get a note (never a solo game's slot, a campaign mode's save, or a co-op file kept in a
/// mod's folder).
bool listedCharacter(const App& app, const std::filesystem::path& plr);

/// The game we're in is now in its world (once a game): the place it's played at noted beside the
/// character we joined with, when it's a mode's (hosted here: its id and name; a remote host's: the
/// name its extended reply gives, its id when we have a mode of that name). The co-op campaign
/// waits for its Hello (characterPlayedCoop); stock and arena games note nothing.
void characterPlayedHere(App& app);
/// The co-op campaign's Hello came (a guest's, once a game): `campaignId` and `title` the mod
/// campaign's (both empty: the game's own). Noted beside the character we joined with and, when
/// it's listed too, beside `coopFile` (the "(Co-op).plr" the campaign keeps; empty: none yet).
void characterPlayedCoop(App& app, std::string_view campaignId, std::string_view title, const std::filesystem::path& coopFile = {});
/// A mode's line about our character came (nox.characterNote; untrusted: cleaned here, and only
/// ever written beside the character we joined with, at this game's place).
void characterSummary(App& app, std::string_view line);
/// Each frame from a mode's UI: a line held back (written a moment ago) written once it's due.
void characterNotesDue(App& app);
/// The game's end: a line held back written now; this game's place forgotten.
void characterNotesEnd(App& app);

} // namespace noxsdl
