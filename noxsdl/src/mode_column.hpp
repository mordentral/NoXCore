// The game list's Mode column: a NoX Core host's mode by its own name (the extended reply's mode
// name, lobby/extended.hpp version 6: "Delve", "Undead Tide", a mod's mode.json name), any other
// game's (a stock host, an older NoX Core one, a plain game) by its game flags (lobby::
// gameModeName: "arena", "quest"...). A name too wide for the column loses whole words from its end
// first ("Deep Delve - WIP" to "Deep Delve", "Co-op Campaign" to "Co-op"), then letters. No SDL here
// (the tests use it).
#pragma once

#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/listing.hpp"

#include <string>
#include <string_view>

namespace noxsdl::mode_column {

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
