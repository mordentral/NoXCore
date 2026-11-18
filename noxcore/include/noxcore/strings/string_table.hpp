// nox.csf: the localised string table.
//
// Layout: "CSF " header of 20 bytes (magic, version, entry count, variant
// count, unknown), plus a u32 language from version 2. Then entries:
//   "LBL " u32 variantCount; u32 labelLength; label
//   variantCount x { tag; u32 length; u16 text[length]; for W tags: u32 n; char extra[n] }
// Tags are "STR ", "STRW", "Str " and "StrW" (stored byte-reversed); in the mixed-case ones the
// text's bits are inverted. The game then collapses whitespace. It reads the header's language only
// to pick its multibyte fonts (6 and 8); the text itself is never converted, so a code point is
// whatever the font draws at it.
//
// Two departures, both for files the game itself would show as garbage or rely on its fonts for:
//  - A file whose text is stored without the inversion its tags call for (plain UTF-16 under "Str ",
//    or inverted under "STR ", as a re-encoding tool can leave it) reads as nothing but U+F800 and
//    up; when most of its strings do, every string is read the other way round (tagsSwapped).
//  - Fan translations (the Russian ones) store each letter's Windows-1251 byte as the code point
//    and ship .fnt fonts that draw Cyrillic there. guessLegacyCodepage tells such a file from a
//    Latin-1 one; the text is kept as stored (it goes on the wire to clients with those fonts) and
//    the client's fonts read it as Windows-1251 (legacyToUnicode).
//
// Lookup (Str_GetString) is a case-insensitive binary search over the labels, picking a random
// variant when there are several.
#pragma once

#include "noxcore/util/text.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::strings {

/// The file's tags as the u32 the game compares (the file stores them byte-reversed); shared by
/// the table's reader and content/csf_document.cpp's.
namespace csf {
inline constexpr std::uint32_t kMagic = 0x43534620;      // "CSF "
inline constexpr std::uint32_t kTagLabel = 0x4C424C20;   // "LBL "
inline constexpr std::uint32_t kTagStr = 0x53545220;     // "STR "
inline constexpr std::uint32_t kTagStrW = 0x53545257;    // "STRW"
inline constexpr std::uint32_t kTagStrInv = 0x53747220;  // "Str " (inverted text)
inline constexpr std::uint32_t kTagStrWInv = 0x53747257; // "StrW" (inverted text)
} // namespace csf

/// The game's whitespace pass on a string's text: drops leading spaces and spaces before a
/// newline or tab or at the end, and collapses runs of spaces.
std::u16string collapseWhitespace(const std::u16string& in);

struct StringVariant {
    std::u16string text; // as the game shows it: decoded, cut at a NUL, whitespace collapsed
    std::string extra;   // W tags only: an 8-bit string (a speech file name)
};

struct StringEntry {
    std::string label; // "file:name", e.g. "QuitMenu.wnd:Quit"
    std::vector<StringVariant> variants;
};

class StringTable {
public:
    /// Parses a whole .csf file. Throws FormatError.
    static StringTable parse(std::span<const std::uint8_t> data);
    static StringTable load(const std::filesystem::path& path);

    std::uint32_t version() const { return version_; }
    std::uint32_t language() const { return language_; } // 0 before version 2
    /// The file's text was stored without its tags' inversion (or with it under the plain tags) and
    /// was read the other way round (see the top of this file).
    bool tagsSwapped() const { return tagsSwapped_; }
    const std::vector<StringEntry>& entries() const { return entries_; }
    std::size_t variantCount() const { return variantCount_; }

    /// Counts the header claims; the game ignores them.
    std::uint32_t headerEntryCount() const { return headerEntryCount_; }
    std::uint32_t headerVariantCount() const { return headerVariantCount_; }

    /// Entry with this label, compared case-insensitively like the game; nullptr if missing.
    /// With duplicate labels the first wins.
    const StringEntry* find(std::string_view label) const;

    /// The first variant's text, or nullopt if the label is missing or has no variants.
    std::optional<std::u16string_view> text(std::string_view label) const;

    /// The one rebuild of this table's index (each label's first entry, without case) from the
    /// entries as they stand. parse calls it; ANYTHING that changes the entries after load (an
    /// overlay, through a mutator added for it) must call it again: nothing here notices a change,
    /// an in-place one of the same size included, until this is called.
    void reindex();

private:
    std::uint32_t version_ = 0;
    std::uint32_t language_ = 0;
    bool tagsSwapped_ = false;
    std::uint32_t headerEntryCount_ = 0;
    std::uint32_t headerVariantCount_ = 0;
    std::size_t variantCount_ = 0;
    std::vector<StringEntry> entries_;
    std::unordered_map<std::string, std::size_t, util::NoCaseHash, util::NoCaseEqual> byLabel_; // label (any case) -> index
};

/// How the text's U+0080-U+00FF is meant: Latin-1 (as the official translations use it) or each a
/// Windows-1251 byte (the Russian fan translations, whose fonts draw Cyrillic there).
enum class LegacyCodepage : std::uint8_t { Latin1, Cp1251 };

/// What a table's letters say about its U+0080-U+00FF (guessLegacyCodepage).
struct CodepageGuess {
    LegacyCodepage codepage = LegacyCodepage::Latin1;
    std::size_t asciiLetters = 0; ///< A-Z, a-z
    std::size_t highLetters = 0;  ///< U+00C0-U+00FF but the signs U+00D7 and U+00F7 (Windows-1251's А-я)
    std::size_t cyrillic = 0;     ///< real Cyrillic, U+0400-U+04FF
    const char* reason = "";      ///< why, for the log
};

/// Whether a table's U+0080-U+00FF are Windows-1251 bytes, from its text, once at load:
///  - the header's language 6 or 8 (the game's multibyte fonts): never;
///  - any real Cyrillic (U+0400-U+04FF): never (the file is Unicode already);
///  - else when at least kCp1251MinLetters letters are in U+00C0-U+00FF and they are at least
///    kCp1251Percent of all letters. A Russian file's letters are nearly all there (cp1251's А-я);
///    the official French, German, Italian, Spanish and Polish files have a few percent (accents,
///    umlauts; Polish's own letters are mostly outside Latin-1), an English one none.
/// The header's language can't say Russian (the fan files keep the English or German CD's), and an
/// installed .fnt that differs from the stock one says nothing about which letters it draws.
CodepageGuess guessLegacyCodepage(const StringTable& table);
inline constexpr std::size_t kCp1251MinLetters = 50;
inline constexpr std::size_t kCp1251Percent = 25; ///< (a part-translated file passes; Latin-1 ones stay under 10)

/// The Windows-1251 character of a byte (0x00-0x7F themselves; 0x98, which has none, itself).
char16_t cp1251Char(std::uint8_t byte);
/// A character as `codepage` means it: under Cp1251, U+0080-U+00FF are Windows-1251 bytes; all
/// else (and everything under Latin1) is itself.
inline char32_t legacyToUnicode(char32_t c, LegacyCodepage codepage)
{
    return codepage == LegacyCodepage::Cp1251 && c >= 0x80 && c <= 0xFF ? cp1251Char(static_cast<std::uint8_t>(c)) : c;
}

/// UTF-16 to UTF-8 (unpaired surrogates become U+FFFD).
std::string toUtf8(std::u16string_view s);
/// UTF-8 to UTF-16 (malformed bytes become U+FFFD).
std::u16string fromUtf8(std::string_view s);
/// As fromUtf8, at most `units` UTF-16 units: the first character that wouldn't fit whole (a
/// surrogate pair with one unit left) and all after it left out; never a sequence cut in half.
std::u16string fromUtf8Cut(std::string_view s, std::size_t units);

} // namespace nox::strings
