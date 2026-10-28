// nox.csf: the localised string table.
//
// Layout: "CSF " header of 20 bytes (magic, version, entry count, variant
// count, unknown), plus a u32 language from version 2. Then entries:
//   "LBL " u32 variantCount; u32 labelLength; label
//   variantCount x { tag; u32 length; u16 text[length]; for W tags: u32 n; char extra[n] }
// Tags are "STR ", "STRW", "Str " and "StrW" (stored byte-reversed); in the mixed-case ones the
// text's bits are inverted. The game then collapses whitespace.
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
    std::uint32_t headerEntryCount_ = 0;
    std::uint32_t headerVariantCount_ = 0;
    std::size_t variantCount_ = 0;
    std::vector<StringEntry> entries_;
    std::unordered_map<std::string, std::size_t, util::NoCaseHash, util::NoCaseEqual> byLabel_; // label (any case) -> index
};

/// UTF-16 to UTF-8 (unpaired surrogates become U+FFFD).
std::string toUtf8(std::u16string_view s);
/// UTF-8 to UTF-16 (malformed bytes become U+FFFD).
std::u16string fromUtf8(std::string_view s);

} // namespace nox::strings
