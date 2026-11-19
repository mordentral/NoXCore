// Which language a nox.csf is in (csf_language.hpp).
#include "noxcore/strings/csf_language.hpp"

#include <algorithm>
#include <string_view>

namespace nox::strings {

namespace {

struct Words {
    const char* code;
    std::initializer_list<std::string_view> words;
};

// Each language's most common short words, only those no other language here shares (Italian's
// "le" and "del", Spanish's "que", Czech's "a" and "to" left out), all ASCII.
const Words kLanguages[] = {
    {"en", {"the", "and", "you", "your", "of", "to", "is", "with", "are", "this", "for"}},
    {"de", {"der", "die", "und", "nicht", "ist", "mit", "den", "sie", "ein", "eine", "auf", "das", "von", "sich", "zu",
            "dem", "des", "werden", "wird"}},
    {"fr", {"les", "des", "est", "vous", "pas", "pour", "dans", "sur", "avec", "votre", "et", "une", "du", "au", "ce"}},
    {"it", {"il", "di", "che", "non", "per", "della", "gli", "sono", "questo", "nel", "alla", "delle", "e", "dei"}},
    {"es", {"el", "los", "las", "para", "por", "este", "esta", "pero", "y", "su", "sus", "muy"}},
    {"pl", {"nie", "jest", "przez", "lub", "jak", "tylko", "jego", "oraz", "przed", "tego", "w", "aby", "mnie"}},
    {"cs", {"je", "jsou", "pro", "nebo", "jako", "jeho", "ale", "pouze", "kde", "jste", "v", "ve", "byl"}},
};
constexpr std::size_t kLanguageCount = std::size(kLanguages);

/// Part of a word: an ASCII letter, or any character past ASCII but a few signs and spaces (a file
/// storing Windows-1250 bytes has letters all over U+0080-U+00FF: ż at U+00BF, ś at U+009C).
bool letter(char16_t c)
{
    if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) return true;
    if (c < 0x80) return false;
    switch (c) {
    case 0xA0: case 0xA1: case 0xAB: case 0xB7: case 0xBB: case 0xD7: case 0xF7: return false;
    default: return c < 0x2000 || c > 0x206F;
    }
}

} // namespace

CsfLanguageGuess guessCsfLanguage(const StringTable& table, LegacyCodepage codepage)
{
    CsfLanguageGuess g;
    if (codepage == LegacyCodepage::Cp1251) {
        g.code = "ru";
        g.reason = "its letters are Windows-1251's (a Russian translation)";
        return g;
    }
    if (table.language() == 6 || table.language() == 8) {
        g.code = table.language() == 6 ? "ja" : "ko";
        g.reason = "the header's language (the game's multibyte fonts)";
        return g;
    }
    std::array<std::size_t, kLanguageCount> hits{};
    std::size_t cyrillicWords = 0, hangulWords = 0;
    std::string word;
    bool cyrillic = false, hangul = false;
    const auto endWord = [&] {
        if (!word.empty() || cyrillic) {
            ++g.words;
            if (cyrillic) {
                ++cyrillicWords;
            } else if (hangul) {
                ++hangulWords;
            } else {
                for (std::size_t i = 0; i < kLanguageCount; ++i) {
                    if (std::find(kLanguages[i].words.begin(), kLanguages[i].words.end(), word) != kLanguages[i].words.end()) {
                        ++hits[i];
                        break;
                    }
                }
            }
        }
        word.clear();
        cyrillic = hangul = false;
    };
    for (const StringEntry& e : table.entries()) {
        if (e.variants.empty()) continue;
        for (const char16_t c : e.variants.front().text) {
            if (c >= 0x400 && c <= 0x4FF) {
                cyrillic = true;
            } else if ((c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) {
                hangul = true; // (syllables and jamo)
                word += '\x01';
            } else if (letter(c)) {
                // (A non-ASCII letter keeps the word whole and makes it match nothing.)
                word += c < 0x80 ? static_cast<char>(c | 0x20) : '\x01';
            } else {
                endWord();
            }
        }
        endWord();
        if (g.words >= kMaxWords) break;
    }
    if (g.words == 0) {
        g.code.clear();
        g.reason = "no text";
        return g;
    }
    if (cyrillicWords * 2 > g.words) {
        g.code = "ru";
        g.reason = "most of its words are Cyrillic";
        g.share = static_cast<double>(cyrillicWords) / static_cast<double>(g.words);
        return g;
    }
    if (hangulWords * 2 > g.words) {
        g.code = "ko";
        g.reason = "most of its words are Hangul";
        g.share = static_cast<double>(hangulWords) / static_cast<double>(g.words);
        return g;
    }
    std::size_t best = 0, second = kLanguageCount;
    for (std::size_t i = 1; i < kLanguageCount; ++i) {
        if (hits[i] > hits[best]) best = i;
    }
    for (std::size_t i = 0; i < kLanguageCount; ++i) {
        if (i != best && (second == kLanguageCount || hits[i] > hits[second])) second = i;
    }
    g.share = static_cast<double>(hits[best]) / static_cast<double>(g.words);
    g.runnerUp = second < kLanguageCount ? static_cast<double>(hits[second]) / static_cast<double>(g.words) : 0.0;
    if (g.share >= kMinShare && g.share >= 2 * g.runnerUp) {
        g.code = kLanguages[best].code;
        g.reason = "its common words";
        return g;
    }
    switch (table.language()) {
    case 0:
    case 1: g.code = "en"; break;
    case 2: g.code = "de"; break;
    case 3: g.code = "fr"; break;
    case 4: g.code = "es"; break;
    case 5: g.code = "it"; break;
    default: g.code.clear(); break;
    }
    g.reason = g.code.empty() ? "its words say nothing clear and the header names no language" :
                                "its words say nothing clear: the header's language";
    return g;
}

} // namespace nox::strings
