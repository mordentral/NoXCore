// A text cut with "..." to fit a width (the menus' long names and translations), whole UTF-8
// letters at a time; the width is measured by the caller, in the units it draws in.
#pragma once

#include <string>

namespace noxsdl {

/// The last UTF-8 letter of `s` taken off: its continuation bytes, then its lead.
inline void popLetter(std::string& s)
{
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
        s.pop_back();
    }
    if (!s.empty()) {
        s.pop_back();
    }
}

/// `s` as it is if `width(s)` is within `room`; else cut letter by letter from its end until it
/// fits with "..." after it (`trimSpaces`: no spaces left before the "..."), "..." alone if
/// nothing does.
template <class Width>
std::string fitText(std::string s, int room, const Width& width, bool trimSpaces = false)
{
    if (width(s) <= room) {
        return s;
    }
    while (!s.empty() && width(s + "...") > room) {
        popLetter(s);
        while (trimSpaces && !s.empty() && s.back() == ' ') {
            s.pop_back();
        }
    }
    return s + "...";
}

} // namespace noxsdl
