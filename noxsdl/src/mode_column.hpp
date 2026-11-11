// The game list's Mode column: a NoX Core host's mode by its own name (the extended reply's mode
// name, lobby/extended.hpp version 6: "Delve", "Undead Tide", a mod's mode.json name), any other
// game's (a stock host, an older NoX Core one, a plain game) by its game flags (lobby::
// gameModeName: "arena", "quest"...). A name too wide for the column loses whole words from its end
// first ("Deep Delve - WIP" to "Deep Delve", "Co-op Campaign" to "Co-op"), then letters. Also the
// list's columns, the gap kept after each column's text, which rows only say why a listed address
// isn't there, and the From column. No SDL here (the tests use it).
#pragma once

#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/listing.hpp"

#include <string>
#include <string_view>

namespace noxsdl::mode_column {

/// The game list's columns (Name, Map, Mode, Players, Ping, From): where each starts, as a share of
/// the rows' width. (Mode has room for "Deep Delve" or "Undead Tide" whole, Players for its header,
/// Ping for "999 ms", From for its longest (fromText: "NoX Core Saved") and the padlock; Map and
/// the name share the rest.)
inline constexpr float kColumnStarts[6] = {0.0f, 0.263f, 0.39f, 0.548f, 0.646f, 0.748f};

/// Whether a game's row only says why it isn't listed (its name, "No response" or "Address not
/// found" across the columns): an address from servers.txt, --server or the saved servers that
/// didn't answer our ping, or one that didn't resolve. A game XWIS or the LAN listed is drawn from
/// its listing (map, mode, players) whether or not it answered, its ping "-" if it didn't.
inline bool statusRow(const nox::lobby::ServerEntry& e)
{
    return e.unresolved || (!e.answered && (e.sources & (nox::lobby::kFromXwis | nox::lobby::kFromLan)) == 0);
}

/// The From column: "NoX Core" for a NoX Core host, then where it was found: "XWIS", else "LAN",
/// else "Saved" (the saved servers) or "list" (servers.txt or --server). One place each, the first
/// of those: an XWIS game that's also on the LAN or in a list is XWIS's.
inline std::string fromText(const nox::lobby::ServerEntry& e, bool saved)
{
    std::string from = e.extended ? "NoX Core " : "";
    if (e.sources & nox::lobby::kFromXwis) {
        from += "XWIS";
    } else if (e.sources & nox::lobby::kFromLan) {
        from += "LAN";
    } else if (e.sources & nox::lobby::kFromList) {
        from += saved ? "Saved" : "list";
    }
    while (!from.empty() && from.back() == ' ') {
        from.pop_back();
    }
    return from;
}

/// The room kept clear after a column's text before the next column: 0.4 of a line, in the units
/// `lineHeight` is in (4 game pixels at the game's small line), so it grows with the text.
inline float columnGap(float lineHeight)
{
    return lineHeight * 0.4f;
}

/// What the Mode column says of a game, before it's fitted.
inline std::string modeText(const nox::lobby::ServerEntry& e)
{
    if (e.extended && !e.extended->modeName.empty()) {
        return e.extended->modeName;
    }
    return std::string(nox::lobby::gameModeName(e.game.gameFlags));
}

/// `text` fitted into `room` as `measure(s)` (a float width) measures it: whole as it is when it
/// fits; else its words from the start, as many as fit (a separator such as "-" left at the end
/// dropped); else (a first word too wide alone) its letters, cut between UTF-8 characters.
template <typename Measure>
std::string fitModeText(std::string_view text, float room, Measure&& measure)
{
    std::string whole(text);
    if (measure(whole) <= room) {
        return whole;
    }
    const auto isSeparator = [](std::string_view w) { return w == "-" || w == "/" || w == "|" || w == ":"; };
    std::string best;
    std::string words;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t space = text.find(' ', at);
        const std::string_view word = text.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
        at = space == std::string_view::npos ? text.size() : space + 1;
        if (word.empty()) {
            continue;
        }
        const std::string next = words.empty() ? std::string(word) : words + " " + std::string(word);
        if (measure(next) > room) {
            break;
        }
        words = next;
        if (!isSeparator(word)) {
            best = words; // (never ending on a lone separator)
        }
    }
    if (!best.empty()) {
        return best;
    }
    std::string cut = whole;
    while (!cut.empty() && measure(cut) > room) {
        // (the last character whole: its continuation bytes and its lead)
        std::size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80) {
            --n;
        }
        cut.resize(n);
    }
    return cut;
}

} // namespace noxsdl::mode_column
