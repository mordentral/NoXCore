// (noxcpp) Where a saved character was last played, on the character screens and as it's played
// (noxcore/client/character_notes.hpp: a section of NoX Core's own in the character's .plr).
//
// The words (no SDL here, so they're testable alone):
//   a mode's place        its name, with the mode's own line after it: "Deep Delve · level 12, act 3"
//   the co-op campaign    "Co-op · <the campaign's title>", the game's own "Co-op · the game's own campaign"
// The network character screen shows each character's place for the game being started in its
// kind column (in place of "Multiplayer"), the selected one's every place above the list, and,
// when the game is a mode's, a switch under the buttons: every character, or that mode's alone.
// A character with no note reads exactly as before.
//
// Kept as a NoX Core client plays (character_play.hpp): a mode's game (hosted here, or a remote
// host's mode, known by the name its extended reply gives), the co-op campaign (its Hello's
// campaign), and a mode's line (nox.characterNote, client level 15: untrusted, cleaned, never
// choosing the file: always the character the game was joined with); written into the .plr with
// its next write. Only a character the network character screen lists (a .plr in its folder):
// never a solo game's slot. Stock and arena games label nothing.
#pragma once

#include "noxcore/client/character_notes.hpp"
#include "noxcore/strings/l10n.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

namespace charnotes = nox::client::character_notes;

/// A place's name: a mode's title (its id without one), or "Co-op · <campaign>".
inline std::string placeTitle(const charnotes::Place& p)
{
    if (p.kind == charnotes::Place::Kind::Coop) {
        const std::string campaign = p.title.empty() ? NOX_TR("menus.characters.coop_own", "the game's own campaign") : p.title;
        return nox::l10n::format(NOX_TR("menus.characters.coop_place", "Co-op · {campaign}"), {{"campaign", campaign}});
    }
    return p.title.empty() ? p.id : p.title;
}

/// A place with the mode's own line after it ("Deep Delve · level 12, act 3"), as the title alone
/// without one.
inline std::string placeLine(const charnotes::Place& p)
{
    if (p.note.empty()) {
        return placeTitle(p);
    }
    return nox::l10n::format(NOX_TR("menus.characters.place_note", "{place} · {note}"), {{"place", placeTitle(p)}, {"note", p.note}});
}

/// Every place of `notes` for the line above the list: the one shown for `target` first, then the
/// rest newest first ("Last played: Deep Delve · level 12, act 3; Co-op · ..."); empty: none.
inline std::string placesLine(const charnotes::Notes& notes, const charnotes::Target& target)
{
    const charnotes::Place* first = charnotes::shownPlace(notes, target);
    if (!first) {
        return {};
    }
    std::string places = placeLine(*first);
    for (const charnotes::Place& p : notes.places) {
        if (&p != first) {
            places += "; " + placeLine(p);
        }
    }
    return nox::l10n::format(NOX_TR("menus.characters.last_played", "Last played: {places}"), {{"places", places}});
}

/// The switch's two choices: every character, or the target's alone.
inline std::string showAllText()
{
    return NOX_TR("menus.characters.show_all", "Show all");
}
inline std::string onlyTargetText(const charnotes::Target& target)
{
    charnotes::Place p;
    p.kind = target.kind;
    p.id = target.id;
    p.title = target.title;
    return nox::l10n::format(NOX_TR("menus.characters.only", "{place} only"), {{"place", placeTitle(p)}});
}

} // namespace noxsdl
