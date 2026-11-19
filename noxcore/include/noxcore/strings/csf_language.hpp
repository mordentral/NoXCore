// Which language a nox.csf is in, once at load: NoX Core's own text then follows the game's.
//
// Signals, strongest first:
//  - a table read as Windows-1251 (guessLegacyCodepage: the Russian fan translations) is Russian;
//  - the header's language 6 is Japanese and 8 Korean (the game's own table of languages: 0 US,
//    1 UK, 2 German, 3 French, 4 Spanish, 5 Italian, 6 Japanese, 7 Jabberwocky, 8 Korean; those two
//    get its multibyte fonts). The official Korean release's text is real UTF-16 Hangul;
//  - most of its words Cyrillic (a Unicode Russian file) or Hangul (a Korean file with another
//    header) make it Russian or Korean;
//  - the common short words of its text: the share of the words that are each language's articles,
//    conjunctions and prepositions ("the", "und", "les", "della", "los", "nie", "jsou"), all plain
//    ASCII, so a fan file storing Windows-1250 bytes (Polish, Czech) is read as well as a Latin-1
//    one. The best language must have at least kMinShare of the words and twice the runner-up's;
//  - else the header's language field (2 German, 3 French, 4 Spanish, 5 Italian). The fan
//    translations keep the English or German CD's value, so it is only a fallback.
// A language is only used when there is a language file for it (nox::l10n::chooseLanguage): today
// Russian and Korean; the others fall back to the system's language, then English. NoX Core draws
// Hangul (fonts/NotoSansKR-SemiBold-Hangul.ttf) but has no Japanese font yet.
#pragma once

#include "noxcore/strings/string_table.hpp"

#include <array>
#include <string>

namespace nox::strings {

struct CsfLanguageGuess {
    std::string code = "en"; ///< "en", "de", "fr", "it", "es", "pl", "cs", "ru", "ja", "ko"; "" when unknown
    const char* reason = ""; ///< for the log
    std::size_t words = 0;   ///< words read
    double share = 0;        ///< the chosen language's share of them
    double runnerUp = 0;     ///< the next language's
};

/// The language of `table`, from its text (its strings as decoded: after decodeLegacy under
/// Cp1251) and `codepage` (guessLegacyCodepage's, or the convention in use). Reads at most
/// kMaxWords words.
CsfLanguageGuess guessCsfLanguage(const StringTable& table, LegacyCodepage codepage);
inline constexpr std::size_t kMaxWords = 60000;
inline constexpr double kMinShare = 0.03;

} // namespace nox::strings
