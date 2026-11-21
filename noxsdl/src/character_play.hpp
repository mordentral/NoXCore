// (noxcpp) The labels of the character we play (character_labels.hpp): where this game is played,
// and a mode's line about it, put in the character's labels in memory as they become known. They
// are written into its .plr only when NoX Core writes that file anyway (the spell bars as a network
// game is left, a quest's or the co-op campaign's save kept: player/character_store.hpp), or once as
// the game ends when no such write took them, and that only in a game whose end writes the character
// in the original game too (a network game that isn't a quest); in any other game they're dropped.
// Nothing here runs per frame; no write happens while the game is played but those.
#pragma once

#include "noxcore/client/character_notes.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace noxsdl {

struct App;

/// Whether `plr` is a character the network character screen lists (a .plr in its folder): only
/// those get labels (never a solo game's slot, a campaign mode's save, or a co-op file kept in a
/// mod's folder).
bool listedCharacter(const App& app, const std::filesystem::path& plr);

/// The game we're in is now in its world (once a game): the place it's played at put in the labels
/// of the character we joined with, when it's a mode's (hosted here: its id and name; a remote
/// host's: the name its extended reply gives, its id when we have a mode of that name). The co-op
/// campaign waits for its Hello (characterPlayedCoop); stock and arena games label nothing.
void characterPlayedHere(App& app);
/// The co-op campaign's Hello came (a guest's, once a game): `campaignId` and `title` the mod
/// campaign's (both empty: the game's own). Put in the labels of the character we joined with and,
/// when it's listed too, of `coopFile` (the "(Co-op).plr" the campaign keeps; empty: none yet). Call
/// it with the co-op file before that file's write (keepCoopCharacter), so the write takes them.
void characterPlayedCoop(App& app, std::string_view campaignId, std::string_view title, const std::filesystem::path& coopFile = {});
/// A mode's line about our character came (nox.characterNote; untrusted: cleaned here, and only
/// ever put in the labels of the character we joined with, at this game's place). In memory only.
void characterSummary(App& app, std::string_view line);
/// The game's end: labels no write took written now (each file once, its other sections as they
/// were) when the game's end writes the character (character_store::gameEndWrites: a network game
/// that isn't a quest), dropped in any other; this game's place forgotten.
void characterNotesEnd(App& app);

} // namespace noxsdl
