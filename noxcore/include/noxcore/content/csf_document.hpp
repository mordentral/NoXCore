// nox.csf as a document: every label's variants as the file stores them (their tags, their
// UTF-16 units as written, inverted or not, the speech file a W tag carries), so the file
// written back is the same bytes, and a changed text is written in its variant's own form. The
// game's reader (strings/string_table) keeps only the text as it shows it.
//
// A content pack's strings.patch changes labels' texts:
//
//   # comments
//   QuitMenu.wnd:Quit = Leave       the label's text (its variants become this one); \n, \t, \\ escapes
//   Some:Label += Another text      one more variant (the game picks one at random)
//   Some:Label.speech = file        the first variant's speech file (a dialog .wav's name)
//   -Some:Label                     the label removed
//
// A label the file doesn't have is added at the end.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::content {

struct CsfVariant {
    std::uint32_t tag = 0;            ///< "STR ", "STRW", "Str " or "StrW" as the u32 the game compares
    std::vector<std::uint16_t> units; ///< as stored (inverted for the mixed-case tags; a NUL may end it early)
    std::string speech;               ///< W tags: the speech file

    bool inverted() const;
    bool wide() const; ///< a W tag (carries `speech`)
    /// The text as the game shows it (decoded, cut at a NUL, its whitespace collapsed).
    std::u16string shown() const;
    /// The text as stored, decoded (up to a NUL), not collapsed.
    std::u16string text() const;
    /// Stores `text` in this variant's own form.
    void setText(std::u16string_view text);
};

struct CsfEntry {
    std::string label;
    std::vector<CsfVariant> variants;
};

class CsfDocument {
public:
    /// Reads a whole .csf. Throws FormatError.
    static CsfDocument parse(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> write() const;

    std::vector<CsfEntry>& entries() { return entries_; }
    const std::vector<CsfEntry>& entries() const { return entries_; }
    /// The first entry with this label, case aside (as the game finds labels), or nullptr.
    CsfEntry* find(std::string_view label);
    const CsfEntry* find(std::string_view label) const;
    /// A new label at the end (one plain variant).
    CsfEntry& add(std::string label, std::u16string_view text);

private:
    std::uint32_t version_ = 0;
    std::uint32_t headerEntries_ = 0;
    std::uint32_t headerVariants_ = 0;
    std::uint32_t unknown_ = 0;
    std::uint32_t language_ = 0;
    std::size_t readEntries_ = 0;  ///< the labels and variants the file had (its header may say otherwise)
    std::size_t readVariants_ = 0;
    std::vector<CsfEntry> entries_;
};

/// The game's limits on a label (at most 48 bytes of printable ASCII: the network drops longer
/// ids and the table keeps 48), a label's variants (255), a text (10,239 UTF-16 units) and a
/// speech file's name (ASCII). "" or what's wrong.
std::string checkStringsLabel(std::string_view label);
std::string checkStringsText(std::u16string_view text, bool inverted = false);
std::string checkSpeechName(std::string_view name);
inline constexpr std::size_t kMaxStringVariants = 255;

/// A text's printf conversions in order ("%s", "%d", "%.2f"; "%%" isn't one). The game's clients
/// format some texts with arguments: a patched text keeps its label's stock conversions, a new
/// label has none, and %n is never allowed.
std::vector<std::string> printfConversions(std::u16string_view text);

/// strings.patch (above).
struct StringsPatchLine {
    std::string label;
    enum Op : std::uint8_t { Set, Add, Speech, Remove } op = Set;
    std::u16string text; ///< Set / Add: the text (escapes undone); Speech: the file name (as UTF-16)
};
std::vector<StringsPatchLine> parseStringsPatch(std::string_view text, std::vector<std::string>* problems = nullptr);
std::string writeStringsPatch(const std::vector<StringsPatchLine>& lines, std::string_view header = {});
/// A text as strings.patch writes it (\n, \t, \\ escaped), and back.
std::string escapeStringsText(std::u16string_view text);
std::u16string unescapeStringsText(std::string_view text);

struct StringsPatchResult {
    std::vector<std::string> changed;
    std::vector<std::string> added;
    std::vector<std::string> problems;
};
StringsPatchResult applyStringsPatch(CsfDocument& doc, const std::vector<StringsPatchLine>& patch);

} // namespace nox::content
