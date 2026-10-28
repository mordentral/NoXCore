// nox.csf parser and the UTF-8/UTF-16 helpers (format in string_table.hpp).
#include "noxcore/strings/string_table.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <cctype>

namespace nox::strings {

namespace {

// Tags as the u32 the game compares (the file stores them byte-reversed).
constexpr std::uint32_t kTagLabel = 0x4C424C20;   // "LBL "
constexpr std::uint32_t kTagStr = 0x53545220;     // "STR "
constexpr std::uint32_t kTagStrW = 0x53545257;    // "STRW"
constexpr std::uint32_t kTagStrInv = 0x53747220;  // "Str " (inverted text)
constexpr std::uint32_t kTagStrWInv = 0x53747257; // "StrW" (inverted text)
constexpr std::uint32_t kMagic = 0x43534620;      // "CSF "

/// The game's whitespace pass: drops leading spaces and spaces before a newline or
/// tab or at the end, and collapses runs of spaces.
std::u16string collapseWhitespace(const std::u16string& in)
{
    std::u16string out;
    // `prev` starts as a non-space so the first character is never treated as a run of spaces.
    char16_t prev = 0xFFFF;
    bool lineStart = true;
    for (char16_t c : in) {
        if (c == u' ') {
            if (prev != u' ' && !lineStart) {
                out.push_back(c);
                lineStart = false;
                prev = c;
            }
        } else if (c == u'\n' || c == u'\t') {
            if (prev == u' ') {
                out.pop_back();
            }
            out.push_back(c);
            lineStart = true;
            prev = c;
        } else {
            out.push_back(c);
            lineStart = false;
            prev = c;
        }
    }
    if (prev == u' ') {
        out.pop_back();
    }
    return out;
}

std::u16string readText(ByteReader& r, std::uint32_t length, bool inverted)
{
    std::u16string s;
    bool ended = false;
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint16_t raw = r.u16();
        // Inverted tags store each UTF-16 unit with all its bits flipped.
        const char16_t c = static_cast<char16_t>(inverted ? ~raw : raw);
        // The game treats the text as NUL-terminated: a raw 0 stops it, as does a character
        // that decodes to 0.
        if (raw == 0 || c == 0) {
            ended = true;
        }
        if (!ended) {
            s.push_back(c);
        }
    }
    return collapseWhitespace(s);
}

} // namespace

StringTable StringTable::parse(std::span<const std::uint8_t> data)
{
    ByteReader r(data, "csf");
    StringTable t;
    if (r.u32() != kMagic) {
        r.fail("not a CSF file");
    }
    t.version_ = r.u32();
    const std::uint32_t entryCount = r.u32();
    const std::uint32_t variantCount = r.u32();
    r.u32(); // unknown
    if (t.version_ >= 2) {
        t.language_ = r.u32();
    }

    std::size_t variantsRead = 0;
    // Entries run to the end of the file; the header counts are only recorded.
    while (!r.atEnd()) {
        if (r.u32() != kTagLabel) {
            r.fail("expected LBL");
        }
        StringEntry e;
        const std::uint32_t n = r.u32();
        e.label = r.rawString(r.u32());
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t tag = r.u32();
            if (tag != kTagStr && tag != kTagStrW && tag != kTagStrInv && tag != kTagStrWInv) {
                r.fail("unknown string tag");
            }
            StringVariant v;
            const std::uint32_t length = r.u32();
            v.text = readText(r, length, tag == kTagStrInv || tag == kTagStrWInv);
            if (tag == kTagStrW || tag == kTagStrWInv) {
                v.extra = r.rawString(r.u32());
            }
            e.variants.push_back(std::move(v));
        }
        variantsRead += n;
        t.entries_.push_back(std::move(e));
    }
    // The game reads until the end of the file and doesn't check these.
    t.headerEntryCount_ = entryCount;
    t.headerVariantCount_ = variantCount;
    t.variantCount_ = variantsRead;
    t.reindex();
    return t;
}

void StringTable::reindex()
{
    byLabel_.clear();
    byLabel_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        byLabel_.emplace(entries_[i].label, i); // (emplace keeps the first entry when a label repeats)
    }
}

StringTable StringTable::load(const std::filesystem::path& path)
{
    auto bytes = readFileBytes(path);
    content::applyContentPacksToStrings(bytes); // (the installed content packs' texts, once, as it loads; none installed: nothing)
    return parse(bytes);
}

const StringEntry* StringTable::find(std::string_view label) const
{
    const auto it = byLabel_.find(label);
    return it == byLabel_.end() ? nullptr : &entries_[it->second];
}

std::optional<std::u16string_view> StringTable::text(std::string_view label) const
{
    const StringEntry* e = find(label);
    if (!e || e->variants.empty()) {
        return std::nullopt;
    }
    return e->variants.front().text;
}

std::string toUtf8(std::u16string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t cp = s[i];
        // A high surrogate followed by a low one combines into one code point.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        // Encode the code point as 1 to 4 UTF-8 bytes.
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string fromUtf8(std::string_view s)
{
    std::u16string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<std::uint8_t>(s[i]);
        // Sequence length from the lead byte: 0xxxxxxx 1, 110xxxxx 2, 1110xxxx 3, 11110xxx 4, else invalid.
        const std::size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        std::uint32_t cp = len == 1 ? b : len == 2 ? b & 0x1Fu : len == 3 ? b & 0x0Fu : b & 0x07u;
        bool ok = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            const auto c = static_cast<std::uint8_t>(s[i + k]);
            ok = (c & 0xC0) == 0x80;
            cp = (cp << 6) | (c & 0x3Fu);
        }
        if (!ok) {
            out.push_back(u'\xFFFD');
            ++i;
            continue;
        }
        i += len;
        // Code points above the BMP become a surrogate pair.
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

} // namespace nox::strings
