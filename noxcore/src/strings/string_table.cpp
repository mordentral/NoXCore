// nox.csf parser, the guess at its 8-bit-era codepage and the UTF-8/UTF-16 helpers (format in
// string_table.hpp).
#include "noxcore/strings/string_table.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>

namespace nox::strings {

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

namespace {

using namespace csf;

/// A variant's text as stored, decoded once the whole file has been seen (tagsSwapped).
struct StoredText {
    std::u16string raw;
    bool inverted = false;
};

std::u16string readRaw(ByteReader& r, std::uint32_t length)
{
    std::u16string raw;
    raw.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i) {
        raw.push_back(static_cast<char16_t>(r.u16()));
    }
    return raw;
}

/// The text up to its end: inverted tags store each UTF-16 unit with all its bits flipped, and the
/// game treats the text as NUL-terminated (a raw 0 stops it, as does a unit that decodes to 0).
std::u16string decodeText(std::u16string_view raw, bool inverted)
{
    std::u16string s;
    for (const char16_t u : raw) {
        const auto c = static_cast<char16_t>(inverted ? ~u : u);
        if (u == 0 || c == 0) {
            break;
        }
        s.push_back(c);
    }
    return s;
}

/// Whether the file's text was stored the other way round from its tags: most of its non-empty
/// strings decode to nothing but U+F800 and up (what text below U+0800, every alphabet Nox's
/// languages use, becomes when its inversion is missing or doubled). A file stored as its tags say
/// has none or next to none such (a fullwidth-only line of a Japanese one).
bool storedSwapped(const std::vector<StoredText>& texts)
{
    std::size_t nonEmpty = 0;
    std::size_t allHigh = 0;
    for (const StoredText& t : texts) {
        const std::u16string s = decodeText(t.raw, t.inverted);
        if (s.empty()) {
            continue;
        }
        ++nonEmpty;
        allHigh += std::ranges::all_of(s, [](char16_t c) { return c >= 0xF800; }) ? 1 : 0;
    }
    return nonEmpty > 0 && allHigh * 2 > nonEmpty;
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
    std::vector<StoredText> stored; // (each variant's, in order)
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
            stored.push_back({readRaw(r, length), tag == kTagStrInv || tag == kTagStrWInv});
            if (tag == kTagStrW || tag == kTagStrWInv) {
                v.extra = r.rawString(r.u32());
            }
            e.variants.push_back(std::move(v));
        }
        variantsRead += n;
        t.entries_.push_back(std::move(e));
    }
    // The texts, now that it's known which way round the file stores them.
    t.tagsSwapped_ = storedSwapped(stored);
    std::size_t k = 0;
    for (StringEntry& e : t.entries_) {
        for (StringVariant& v : e.variants) {
            const StoredText& s = stored[k++];
            v.text = collapseWhitespace(decodeText(s.raw, s.inverted != t.tagsSwapped_));
        }
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
    if (const StringEntry* over = overlay_.find(label)) {
        return over;
    }
    const auto it = byLabel_.find(label);
    return it == byLabel_.end() ? nullptr : &entries_[it->second];
}

const StringEntry* StringTable::Overlay::find(std::string_view label) const
{
    // (Another thread's lookup never touches the entries: `on` is read first, the thread after it.)
    if (!on.load(std::memory_order_acquire) || thread.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
        return nullptr;
    }
    const auto it = entries.find(label);
    return it == entries.end() ? nullptr : &it->second;
}

bool StringTable::setOverlay(std::string_view label, std::u16string text, std::size_t most)
{
    if (!overlay_.on.load(std::memory_order_relaxed)) {
        overlay_.entries.clear();
        overlay_.thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    } else if (overlay_.thread.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
        return false; // (another thread's overlay: never two writers)
    }
    if (overlay_.entries.size() >= most && overlay_.entries.find(label) == overlay_.entries.end()) {
        return false;
    }
    StringEntry e;
    e.label = std::string(label);
    e.variants.push_back(StringVariant{std::move(text), {}});
    std::string key = e.label;
    overlay_.entries.insert_or_assign(std::move(key), std::move(e));
    overlay_.on.store(true, std::memory_order_release);
    return true;
}

void StringTable::clearOverlay(std::string_view prefix)
{
    if (!overlay_.on.load(std::memory_order_relaxed) || overlay_.thread.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
        return;
    }
    if (prefix.empty()) {
        overlay_.entries.clear();
    } else {
        std::erase_if(overlay_.entries, [&](const auto& kv) {
            return kv.first.size() >= prefix.size() && util::equalsNoCase(std::string_view(kv.first).substr(0, prefix.size()), prefix);
        });
    }
    if (overlay_.entries.empty()) {
        overlay_.on.store(false, std::memory_order_release);
    }
}

std::size_t StringTable::overlaySize() const
{
    return overlay_.on.load(std::memory_order_acquire) && overlay_.thread.load(std::memory_order_relaxed) == std::this_thread::get_id()
               ? overlay_.entries.size()
               : 0;
}

std::optional<std::u16string_view> StringTable::text(std::string_view label) const
{
    const StringEntry* e = find(label);
    if (!e || e->variants.empty()) {
        return std::nullopt;
    }
    return e->variants.front().text;
}

CodepageGuess guessLegacyCodepage(const StringTable& table)
{
    CodepageGuess g;
    // The header's language: the game's multibyte font sets (6 and 8) mean neither.
    if (table.language() == 6 || table.language() == 8) {
        g.reason = "the header's language has the game's multibyte fonts";
        return g;
    }
    std::array<std::size_t, 64> high{}; // (U+00C0-U+00FF, each: the shift's)
    for (const StringEntry& e : table.entries()) {
        for (const StringVariant& v : e.variants) {
            for (const char16_t c : v.text) {
                if ((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) {
                    ++g.asciiLetters;
                } else if (c >= 0xC0 && c <= 0xFF) {
                    ++high[c - 0xC0];
                    g.highLetters += c != 0xD7 && c != 0xF7 ? 1 : 0;
                } else if (c >= 0x400 && c <= 0x4FF) {
                    ++g.cyrillic;
                }
            }
        }
    }
    const std::size_t letters = g.asciiLetters + g.highLetters;
    if (g.highLetters >= kCp1251MinLetters) {
        g.shift = cp1251Shift(high);
    }
    if (g.cyrillic > 0) {
        g.reason = "it has real Cyrillic (Unicode)";
    } else if (g.highLetters >= kCp1251MinLetters && g.highLetters * 100 >= letters * kCp1251Percent) {
        g.codepage = LegacyCodepage::Cp1251;
        g.reason = "most of its letters are in U+00C0-U+00FF (Windows-1251's Cyrillic)";
    } else {
        g.reason = "its letters are mostly A-Z (accents as Latin-1)";
    }
    return g;
}

std::uint8_t cp1251Shift(const std::array<std::size_t, 64>& counts)
{
    // Russian letters' frequencies (per thousand of running text, А-Я order; Ё apart: it isn't in
    // the block), and the share of small letters (Russian text is mostly small letters).
    static constexpr double kPerMille[32] = {80.1, 15.9, 45.4, 17.0, 29.8, 84.5, 9.4, 16.5, 73.5, 12.1, 34.9,
                                             44.0, 32.1, 67.0, 109.7, 28.1, 47.3, 54.7, 62.6, 26.2, 2.6, 9.7,
                                             4.8, 14.4, 7.3, 3.6, 0.4, 19.0, 17.4, 3.2, 6.4, 20.1};
    static const auto kScore = [] {
        std::array<double, 64> s{};
        for (std::size_t i = 0; i < 64; ++i) {
            s[i] = std::log(kPerMille[i % 32] / 1000.0) + std::log(i < 32 ? 0.1 : 0.9);
        }
        return s;
    }();
    std::uint8_t best = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (std::uint8_t k = 0; k < 64; ++k) {
        double score = 0;
        for (std::size_t v = 0; v < 64; ++v) {
            score += static_cast<double>(counts[v]) * kScore[(v + 64 - k) % 64];
        }
        if (score > bestScore) {
            bestScore = score;
            best = k;
        }
    }
    return best;
}

namespace {

// Windows-1251's 0x80-0xBF; 0xC0-0xFF are А-я in order.
constexpr char16_t kCp1251High[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

// U+00C0-U+00FF's plain letters (toWire's for the Latin-1 letters Windows-1251 lacks): the
// multiplication and division signs as 'x' and '/', thorn as '?'.
constexpr char kLatin1Plain[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUY?saaaaaaaceeeeiiiidnooooo/ouuuuy?y";
static_assert(sizeof(kLatin1Plain) == 0x40 + 1);

/// The program's convention: the codepage in the low byte, the shift above it.
std::atomic<std::uint16_t> g_wireText{0};

/// A byte moved `by` places on within its block of 64 (0x80-0xBF or 0xC0-0xFF), wrapping round.
constexpr std::uint8_t rotated(std::uint8_t byte, int by)
{
    const int base = byte & 0xC0;
    return static_cast<std::uint8_t>(base + (((byte - base + by) % 64) + 64) % 64);
}

} // namespace

char16_t cp1251Char(std::uint8_t byte)
{
    if (byte < 0x80) {
        return byte;
    }
    return byte < 0xC0 ? kCp1251High[byte - 0x80] : static_cast<char16_t>(0x0410 + (byte - 0xC0));
}

int cp1251Byte(char32_t c)
{
    if (c < 0x80) {
        return static_cast<int>(c);
    }
    if (c >= 0x0410 && c <= 0x044F) {
        return static_cast<int>(0xC0 + (c - 0x0410));
    }
    for (int i = 0; i < 64; ++i) {
        if (kCp1251High[i] == c) {
            return 0x80 + i;
        }
    }
    return -1;
}

void setWireText(LegacyText text)
{
    g_wireText.store(static_cast<std::uint16_t>(static_cast<unsigned>(text.codepage) | (text.shift & 63u) << 8), std::memory_order_relaxed);
}

LegacyText wireText()
{
    const std::uint16_t v = g_wireText.load(std::memory_order_relaxed);
    return {static_cast<LegacyCodepage>(v & 0xFF), static_cast<std::uint8_t>(v >> 8)};
}

void setWireCodepage(LegacyCodepage codepage)
{
    setWireText({codepage, 0});
}

LegacyCodepage wireCodepage()
{
    return wireText().codepage;
}

char16_t fromWire(char16_t c, LegacyText text)
{
    if (text.codepage != LegacyCodepage::Cp1251 || c < 0x80 || c > 0xFF) {
        return c;
    }
    return cp1251Char(rotated(static_cast<std::uint8_t>(c), -text.shift));
}

std::u16string fromWire(std::u16string_view s)
{
    return fromWire(s, wireText());
}

std::u16string fromWire(std::u16string_view s, LegacyText text)
{
    std::u16string out(s);
    fromWireInPlace(out, text);
    return out;
}

void fromWireInPlace(std::u16string& s)
{
    fromWireInPlace(s, wireText());
}

void fromWireInPlace(std::u16string& s, LegacyText text)
{
    if (text.plain()) {
        return;
    }
    for (char16_t& c : s) {
        c = fromWire(c, text);
    }
}

char16_t toWire(char16_t c, LegacyText text)
{
    if (text.codepage == LegacyCodepage::Latin1 || c < 0x80) {
        return c;
    }
    if (const int b = cp1251Byte(c); b >= 0x80) {
        return rotated(static_cast<std::uint8_t>(b), text.shift);
    }
    if (c >= 0xC0 && c <= 0xFF) {
        return static_cast<char16_t>(kLatin1Plain[c - 0xC0]);
    }
    return c <= 0xFF ? u'?' : c;
}

std::u16string toWire(std::u16string_view s)
{
    return toWire(s, wireText());
}

std::u16string toWire(std::u16string_view s, LegacyText text)
{
    std::u16string out(s);
    if (!text.plain()) {
        for (char16_t& c : out) {
            c = toWire(c, text);
        }
    }
    return out;
}

std::string wire8ToUtf8(std::string_view bytes)
{
    if (util::validUtf8(bytes)) {
        return std::string(bytes); // (ASCII, or a NoX Core peer's UTF-8)
    }
    std::u16string wide;
    wide.reserve(bytes.size());
    for (const char b : bytes) {
        wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(b)));
    }
    fromWireInPlace(wide);
    return toUtf8(wide);
}

std::string utf8ToWire8(std::string_view utf8)
{
    const LegacyText text = wireText();
    if (text.plain()) {
        return std::string(utf8);
    }
    std::string out;
    for (const char16_t c : fromUtf8(utf8)) {
        const char16_t w = toWire(c, text);
        out.push_back(w <= 0xFF ? static_cast<char>(w) : '?');
    }
    return out;
}

std::string fitWire8(std::string_view utf8, std::size_t bytes)
{
    if (wireCodepage() == LegacyCodepage::Latin1) {
        return std::string(utf8.substr(0, bytes));
    }
    const std::u16string wide = fromUtf8(utf8);
    std::size_t n = std::min(wide.size(), bytes);
    if (n > 0 && n < wide.size() && wide[n - 1] >= 0xD800 && wide[n - 1] <= 0xDBFF) {
        --n; // (never half a surrogate pair)
    }
    return toUtf8(std::u16string_view(wide).substr(0, n));
}

void StringTable::decodeLegacy(LegacyText text)
{
    if (text.plain()) {
        return;
    }
    for (StringEntry& e : entries_) {
        for (StringVariant& v : e.variants) {
            for (char16_t& c : v.text) {
                c = fromWire(c, text);
            }
        }
    }
}

LegacyText adoptLegacyCodepage(StringTable* table, const CodepageGuess& guess, std::optional<LegacyCodepage> forced,
                               std::optional<std::uint8_t> forcedShift)
{
    LegacyText text{forced.value_or(guess.codepage), 0};
    if (!text.plain()) {
        text.shift = static_cast<std::uint8_t>(forcedShift.value_or(guess.shift) & 63u);
    }
    if (table) {
        table->decodeLegacy(text);
    }
    setWireText(text);
    return text;
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
    return fromUtf8Cut(s, std::u16string::npos);
}

std::u16string fromUtf8Cut(std::string_view s, std::size_t units)
{
    std::u16string out;
    std::size_t i = 0;
    while (i < s.size() && out.size() < units) {
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
        // Code points above the BMP become a surrogate pair (never half of one: with one unit
        // left it's left out, and the rest with it).
        if (cp >= 0x10000) {
            if (units - out.size() < 2) {
                break;
            }
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
