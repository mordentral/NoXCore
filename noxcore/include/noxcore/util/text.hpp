// Text helpers shared across noxcore and its programs: lower-case copies, compares without case
// (std::tolower's: the programs never change the C locale, so ASCII letters only), image names'
// lookup keys, and UTF-16LE fields read to their NUL.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::util {

/// A lower-case copy.
inline std::string lowered(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
inline std::string lowered(std::string_view s) { return lowered(std::string(s)); }
inline std::string lowered(const char* s) { return lowered(std::string(s)); }

/// Leading and trailing white space (std::isspace's) off.
inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// Equal without case.
inline bool equalsNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

/// std::tolower's answer in the C locale (the programs' only one), without its call: A-Z lowered,
/// every other byte as it is.
constexpr unsigned char lowerAscii(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 'A' && u <= 'Z' ? static_cast<unsigned char>(u + ('a' - 'A')) : u;
}

/// A hash and equality without case (equalsNoCase's), for an index looked up by any string_view
/// without a lower-case copy.
struct NoCaseHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept
    {
        std::size_t h = 14695981039346656037ull; // (FNV-1a over the lower-cased bytes)
        for (const char c : s) {
            h = (h ^ lowerAscii(c)) * 1099511628211ull;
        }
        return h;
    }
};
struct NoCaseEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (lowerAscii(a[i]) != lowerAscii(b[i])) {
                return false;
            }
        }
        return true;
    }
};

/// strcmp-style compare without case: -1, 0 or 1.
inline int compareNoCase(std::string_view a, std::string_view b) noexcept
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

/// An image name as VideoBag::findImage and ImagePack look it up: without its extension, in
/// lower case.
inline std::string normalizeName(std::string_view name)
{
    std::string s(name);
    if (const auto dot = s.rfind('.'); dot != std::string::npos) {
        s.resize(dot);
    }
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// An image name to look up in an index of normalizeName's keys, normalised as it's compared
/// (no copy made).
struct RawImageName {
    std::string_view name;
    std::string_view stem() const { return name.substr(0, name.rfind('.')); } ///< (without its extension)
};
/// The hash and equality of such an index: keys as normalizeName made them, looked up by a
/// RawImageName (the same answer as comparing normalizeName of it).
struct ImageNameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view key) const noexcept { return NoCaseHash{}(key); }
    std::size_t operator()(RawImageName raw) const noexcept { return NoCaseHash{}(raw.stem()); }
};
struct ImageNameEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    bool operator()(RawImageName raw, std::string_view key) const noexcept { return NoCaseEqual{}(raw.stem(), key); }
    bool operator()(std::string_view key, RawImageName raw) const noexcept { return NoCaseEqual{}(raw.stem(), key); }
};

/// A text message's text appended as MSG_TEXT_MESSAGE carries it: each character a byte (narrow:
/// its low byte) or UTF-16LE, then the NUL in the same width.
inline void appendText(std::vector<std::uint8_t>& out, std::u16string_view text, bool narrow)
{
    for (const char16_t c : text) {
        out.push_back(static_cast<std::uint8_t>(c));
        if (!narrow) {
            out.push_back(static_cast<std::uint8_t>(c >> 8));
        }
    }
    out.push_back(0);
    if (!narrow) {
        out.push_back(0);
    }
}

/// UTF-16LE bytes up to the first NUL (or the end).
inline std::u16string utf16Until(std::span<const std::uint8_t> b)
{
    std::u16string s;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        const auto c = static_cast<char16_t>(b[i] | b[i + 1] << 8);
        if (c == 0) {
            break;
        }
        s += c;
    }
    return s;
}

} // namespace nox::util
