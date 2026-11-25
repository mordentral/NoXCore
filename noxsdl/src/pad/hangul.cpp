// (noxcpp) The on-screen keyboard's Hangul composer (pad/hangul.hpp).
#include "pad/hangul.hpp"

#include <array>
#include <string_view>

namespace noxsdl::pad {

namespace {

constexpr char32_t kFirstJamo = 0x3131; // ㄱ
constexpr char32_t kFirstVowel = 0x314F; // ㅏ
constexpr char32_t kLastJamo = 0x3163;  // ㅣ
constexpr char32_t kSyllableBase = 0xAC00;

// The 30 compatibility consonants (U+3131-U+314E: ㄱ ㄲ ㄳ ㄴ ㄵ ㄶ ㄷ ㄸ ㄹ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅁ ㅂ ㅃ ㅄ ㅅ ㅆ ㅇ
// ㅈ ㅉ ㅊ ㅋ ㅌ ㅍ ㅎ) as an initial (0-18, -1: none) and as a final (1-27, 0: none).
constexpr std::array<int, 30> kInitial{0, 1, -1, 2, -1, -1, 3, 4, 5, -1, -1, -1, -1, -1, -1,
                                       -1, 6, 7, 8, -1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
constexpr std::array<int, 30> kFinal{1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13, 14,
                                     15, 16, 17, 0, 18, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27};

/// The compatibility consonant (its index from U+3131) for an initial.
int consonantOfInitial(int initial)
{
    for (std::size_t i = 0; i < kInitial.size(); ++i) {
        if (kInitial[i] == initial) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Two vowels as one (ㅗ ㅏ: ㅘ), by their medial numbers; -1 when they don't go together.
int compoundVowel(int first, int second)
{
    struct Pair {
        int first, second, both;
    };
    static constexpr Pair kPairs[] = {{8, 0, 9},   {8, 1, 10},  {8, 20, 11}, // ㅗ + ㅏ ㅐ ㅣ
                                      {13, 4, 14}, {13, 5, 15}, {13, 20, 16}, // ㅜ + ㅓ ㅔ ㅣ
                                      {18, 20, 19}};                          // ㅡ + ㅣ
    for (const Pair& p : kPairs) {
        if (p.first == first && p.second == second) {
            return p.both;
        }
    }
    return -1;
}

/// A final and the consonant after it (its index from U+3131) as a double final; 0 for none.
int doubleFinal(int final, int consonant)
{
    struct Pair {
        int final, consonant, both;
    };
    static constexpr Pair kPairs[] = {
        {1, 20, 3},                                                               // ㄱ ㅅ: ㄳ
        {4, 23, 5},   {4, 29, 6},                                                 // ㄴ ㅈ ㄵ, ㄴ ㅎ ㄶ
        {8, 0, 9},    {8, 16, 10}, {8, 17, 11}, {8, 20, 12}, {8, 27, 13}, {8, 28, 14}, {8, 29, 15}, // ㄹ ...
        {17, 20, 18},                                                             // ㅂ ㅅ: ㅄ
    };
    for (const Pair& p : kPairs) {
        if (p.final == final && p.consonant == consonant) {
            return p.both;
        }
    }
    return 0;
}

} // namespace

std::string utf8Of(char32_t c)
{
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

char32_t firstCodePoint(std::string_view s)
{
    if (s.empty()) {
        return 0;
    }
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char c = b(0);
    if (c < 0x80) {
        return c;
    }
    const std::size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (n == 0 || s.size() < n) {
        return 0;
    }
    char32_t cp = c & (0x7F >> n);
    for (std::size_t i = 1; i < n; ++i) {
        if ((b(i) & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (b(i) & 0x3F);
    }
    return cp;
}

bool HangulComposer::isJamo(char32_t c)
{
    return c >= kFirstJamo && c <= kLastJamo;
}

bool HangulComposer::isVowel(char32_t c)
{
    return c >= kFirstVowel && c <= kLastJamo;
}

bool HangulComposer::add(State& s, char32_t c)
{
    if (isVowel(c)) {
        const int v = static_cast<int>(c - kFirstVowel);
        if (s.final != 0) {
            return false; // (the caller splits the final off)
        }
        if (s.medial < 0) {
            s.medial = v; // (after an initial, or alone)
            return true;
        }
        const int both = compoundVowel(s.medial, v);
        if (both < 0) {
            return false;
        }
        s.medial = both;
        return true;
    }
    const int k = static_cast<int>(c - kFirstJamo);
    if (s.initial < 0 && s.medial < 0) {
        s.initial = kInitial[static_cast<std::size_t>(k)];
        return s.initial >= 0;
    }
    if (s.initial < 0 || s.medial < 0) {
        return false; // (an initial with no vowel yet, or a vowel alone: this one starts anew)
    }
    if (s.final == 0) {
        s.final = kFinal[static_cast<std::size_t>(k)];
        return s.final != 0;
    }
    const int both = doubleFinal(s.final, k);
    if (both == 0) {
        return false;
    }
    s.final = both;
    return true;
}

HangulComposer::State HangulComposer::replay(const std::vector<char32_t>& keys)
{
    State s;
    for (const char32_t c : keys) {
        add(s, c); // (a syllable's own keys always go together)
    }
    return s;
}

std::string HangulComposer::render(const State& s)
{
    if (s.initial >= 0 && s.medial >= 0) {
        return utf8Of(kSyllableBase + static_cast<char32_t>((s.initial * 21 + s.medial) * 28 + s.final));
    }
    if (s.initial >= 0) {
        return utf8Of(kFirstJamo + static_cast<char32_t>(consonantOfInitial(s.initial)));
    }
    if (s.medial >= 0) {
        return utf8Of(kFirstVowel + static_cast<char32_t>(s.medial));
    }
    return {};
}

std::string HangulComposer::input(char32_t c)
{
    if (!isJamo(c)) {
        std::string out = flush();
        out += utf8Of(c);
        return out;
    }
    State next = state_;
    if (add(next, c)) {
        keys_.push_back(c);
        state_ = next;
        return {};
    }
    if (isVowel(c) && state_.final != 0) {
        // The final's last consonant goes on to the new syllable as its initial.
        const char32_t moved = keys_.back();
        keys_.pop_back();
        std::string done = render(replay(keys_));
        keys_ = {moved, c};
        state_ = replay(keys_);
        return done;
    }
    std::string done = render(state_);
    keys_.clear();
    state_ = State{};
    if (add(state_, c)) {
        keys_.push_back(c);
    } else {
        done += utf8Of(c); // (a consonant that can't start a syllable: as it is)
        state_ = State{};
    }
    return done;
}

bool HangulComposer::backspace()
{
    if (keys_.empty()) {
        return false;
    }
    keys_.pop_back();
    state_ = replay(keys_);
    return true;
}

std::string HangulComposer::preedit() const
{
    return render(state_);
}

std::string HangulComposer::flush()
{
    std::string out = render(state_);
    clear();
    return out;
}

void HangulComposer::clear()
{
    keys_.clear();
    state_ = State{};
}

} // namespace noxsdl::pad
