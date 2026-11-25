// (noxcpp) Hangul syllables composed from the two-set (Dubeolsik) keyboard's jamo, for the game
// controller's on-screen keyboard (pad/osk.hpp). No SDL here.
//
// The keys give compatibility jamo (U+3131-U+3163: consonants and vowels as printed on the keys);
// what's typed is put together into precomposed syllables (U+AC00-U+D7A3) as a Korean input
// method does:
//
//   initial      a consonant starts a syllable (ㄱ); a second consonant with no vowel yet finishes
//                the first as it is (ㄱ ㄴ: "ㄱ", then ㄴ).
//   medial       a vowel joins the initial (가); ㅗ ㅜ ㅡ take a second vowel (ㅘ ㅙ ㅚ ㅝ ㅞ ㅟ ㅢ);
//                a vowel with no initial stands alone (ㅏ, ㅘ).
//   final        a consonant after the vowel is the final (간), and a second one makes a double
//                final where Hangul has one (ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ); ㄸ ㅃ ㅉ are never finals.
//   splitting    a vowel after a final takes the final's last consonant as its own initial: 간 ㅏ
//                is 가나, 닭 ㅏ is 달가, 갔 ㅓ is 가써.
//   backspace    takes the last jamo typed off the syllable being made (닭, 달, 다, ㄷ).
//
// What's finished comes back from input() as UTF-8 for the field; the syllable still being made is
// preedit() (shown beside the field, typed into it by flush()).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::pad {

class HangulComposer {
public:
    /// Whether `c` is a compatibility jamo the composer takes (U+3131-U+3163).
    static bool isJamo(char32_t c);
    static bool isVowel(char32_t c);

    /// A jamo typed: what that finished (UTF-8; usually nothing), the syllable it starts or adds
    /// to kept. Anything that isn't a jamo finishes the syllable and comes back after it.
    std::string input(char32_t c);
    /// The last jamo typed taken off the syllable being made; false when nothing is being made.
    bool backspace();
    /// The syllable being made (UTF-8; empty when none).
    std::string preedit() const;
    /// The syllable being made, finished (UTF-8), and nothing being made after.
    std::string flush();
    bool composing() const { return !keys_.empty(); }
    void clear();

private:
    struct State {
        int initial = -1; ///< 0-18, -1 none
        int medial = -1;  ///< 0-20, -1 none
        int final = 0;    ///< 1-27, 0 none
    };
    static bool add(State& s, char32_t c);
    static State replay(const std::vector<char32_t>& keys);
    static std::string render(const State& s);

    std::vector<char32_t> keys_; ///< the jamo of the syllable being made, as typed
    State state_;
};

/// UTF-8 for one code point.
std::string utf8Of(char32_t c);
/// The first code point of UTF-8 text (0 for none or a broken one).
char32_t firstCodePoint(std::string_view utf8);

} // namespace noxsdl::pad
