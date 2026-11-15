// Filling the game's text formats (nox.csf's strings and the client's own patterns) with values,
// as the game's swprintf calls fill them. The screens read the % fields in a few different ways,
// and each way is kept as it is: one scanner whose Rules say which fields it takes and what it
// does at the edges (%%, a width, values running out), plus two plain replacers.
//
//   fill               scans for %[width][l]<kind>; Rules picks the kinds, the width characters,
//                      whether %ls counts, zero padding, %% and what a field past the values does
//   replaceThroughLetter  each % through the next letter (%d, %s, %.2f, %5d) is the next value
//   replacePercentPairs   each % and the character after it is the next value
//
// The presets are the screens' own ways (named for what they do, not for the screen):
//   kStringsKept     %s / %S only, %% left alone, a field past the values left as written
//   kStringsAndInts  %s / %S / %d, %% a percent, a field past the values empty
//   kLongStrings     %s / %S / %ls, %% a percent, a field past the values empty
//   kWidths          %[0-9-]*(d|s|S), the width dropped, %% a percent, past the values empty
//   kZeroPadded      %[0-9]*(d|s|S), a width starting with 0 pads with zeros, %% a percent
// Every way works on bytes (UTF-8 in, UTF-8 out), values put in as they are (never scanned again).
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace nox::client::fields {

/// How fill() reads a % field. A field is % then any of `widthChars`, then (with `longPrefix`)
/// one 'l' that isn't the format's last character, then one of `kinds`. A % that starts no field
/// is copied as it is and the scan goes on from the character after it.
struct Rules {
    std::string_view kinds = "sS";   ///< the letters that take the next value
    std::string_view widthChars;     ///< characters allowed between % and the letter (none: none)
    bool longPrefix = false;         ///< %ls (one 'l' before the letter) counts as %s
    bool zeroPad = false;            ///< a width starting with '0' pads the value to it with zeros
    bool percentEscape = true;       ///< %% (after any width or 'l') is one %
    bool keepPastValues = false;     ///< a field with no value left stays as written (else it's empty)
};

inline constexpr Rules kStringsKept{.kinds = "sS", .percentEscape = false, .keepPastValues = true};
inline constexpr Rules kStringsAndInts{.kinds = "sSd"};
inline constexpr Rules kLongStrings{.kinds = "sS", .longPrefix = true};
inline constexpr Rules kWidths{.kinds = "dsS", .widthChars = "0123456789-"};
inline constexpr Rules kZeroPadded{.kinds = "dsS", .widthChars = "0123456789", .zeroPad = true};

/// `fmt` with its fields filled in order from `values` by `rules`. (A zero-padded width too big
/// for an int throws std::out_of_range, as std::stoi does.)
std::string fill(std::string_view fmt, std::initializer_list<std::string_view> values, const Rules& rules);

/// Each % through the next letter after it (anything between skipped: %d, %s, %.2f, %%d) replaced
/// by the next value, until the values run out or a % has no letter after it.
std::string replaceThroughLetter(std::string fmt, std::initializer_list<std::string_view> values);

/// Each % and the one character after it (whatever it is: %S, %d, %%) replaced by the next value,
/// until the values run out or a % is the format's last character.
std::string replacePercentPairs(std::string fmt, std::initializer_list<std::string_view> values);

} // namespace nox::client::fields
