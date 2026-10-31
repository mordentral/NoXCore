// Text wrapping as the game's GUI does it (text_wrap.hpp), over code points with the widths the
// caller gives.
#include "text_wrap.hpp"

#include <cstdint>

namespace noxsdl {

namespace {

/// A UTF-8 text as code points, with where each starts, so lines come back as the text's own
/// bytes.
class Decoded {
public:
    explicit Decoded(std::string_view utf8) : text_(utf8)
    {
        for (std::size_t pos = 0; pos < utf8.size();) {
            starts_.push_back(pos);
            points_.push_back(nextCodepoint(utf8, pos));
        }
        starts_.push_back(utf8.size());
    }

    std::ptrdiff_t size() const { return static_cast<std::ptrdiff_t>(points_.size()); }
    /// The code point at `i`, 0 past the end (the game's strings end in a 0).
    char32_t at(std::ptrdiff_t i) const { return i >= 0 && i < size() ? points_[static_cast<std::size_t>(i)] : 0; }
    /// Code points [from, to) as UTF-8.
    std::string sub(std::ptrdiff_t from, std::ptrdiff_t to) const
    {
        if (from >= to) {
            return {};
        }
        const std::size_t a = starts_[static_cast<std::size_t>(from)];
        return std::string(text_.substr(a, starts_[static_cast<std::size_t>(to)] - a));
    }
    /// The width of code points [from, to) (line breaks take none, as in the game's measuring).
    float width(std::ptrdiff_t from, std::ptrdiff_t to, const GlyphAdvance& advance) const
    {
        float w = 0;
        for (std::ptrdiff_t i = from; i < to; ++i) {
            const char32_t c = at(i);
            if (c != '\n' && c != '\r') {
                w += advance(c);
            }
        }
        return w;
    }

private:
    std::string_view text_;
    std::vector<char32_t> points_;
    std::vector<std::size_t> starts_;
};

} // namespace

char32_t nextCodepoint(std::string_view s, std::size_t& pos)
{
    constexpr char32_t kReplacement = 0xFFFD;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(s[i]); };
    const std::uint8_t b0 = byte(pos++);
    if (b0 < 0x80) {
        return b0;
    }
    // The lead byte gives the sequence's length (2-4 bytes) and its top bits; each continuation
    // byte (10xxxxxx) adds 6 more.
    int more = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        more = 1;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        more = 2;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        more = 3;
        cp = b0 & 0x07;
    } else {
        return kReplacement;
    }
    for (int i = 0; i < more; ++i) {
        if (pos >= s.size() || (byte(pos) & 0xC0) != 0x80) {
            return kReplacement;
        }
        cp = (cp << 6) | (byte(pos++) & 0x3F);
    }
    return cp;
}

std::vector<std::string> wrapLines(std::string_view utf8, float maxWidth, const GlyphAdvance& advance)
{
    // drawStringWrap: one pass over the text, `from` the first character of the line not yet
    // drawn, `width` what the line has counted so far.
    const Decoded t(utf8);
    std::vector<std::string> lines;
    std::ptrdiff_t from = 0;
    std::ptrdiff_t lastSpace = -1; // the last space seen; a newline doesn't forget it (OB-417)
    char32_t prev = 0;
    float width = 0;
    // The line drawn up to `to` (nothing when `from` is already past it).
    const auto endLine = [&](std::ptrdiff_t to) {
        lines.push_back(t.sub(from, to));
        if (from < to) {
            from = to;
        }
    };
    for (std::ptrdiff_t i = 0; i < t.size(); ++i) {
        const char32_t c = t.at(i);
        if (c == '\n' || c == '\r') {
            // A line break ("\r\n" is one), the break character itself skipped.
            if (prev != '\r') {
                endLine(i);
                width = 0;
            }
            ++from;
        } else if (c == '\t') {
            width += advance(c); // (to the tab stop; a tab never breaks the line)
        } else {
            if (c == ' ') {
                lastSpace = i;
            }
            const float before = width;
            width += advance(c);
            // Past the width: the line ends at its last space (skipped), else one character
            // before this one; counting starts again after the end, so a word carried over is
            // counted again, but not the character a word with no space gave up (OB-416).
            // (A glyph wider than the whole width on a line of its own stays: the game would
            // break before it for ever.)
            if (maxWidth > 0 && width > maxWidth && (lastSpace >= 0 || before > 0)) {
                const std::ptrdiff_t end = lastSpace >= 0 ? lastSpace : i - 1;
                endLine(end);
                if (lastSpace >= 0) {
                    lastSpace = -1;
                    ++from;
                }
                width = 0;
                i = end; // on from the character after the end
            }
        }
        prev = c;
    }
    endLine(t.size());
    if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

std::vector<std::string> dialogLines(std::string_view utf8, float maxWidth, std::size_t maxLines,
                                     const GlyphAdvance& advance)
{
    std::vector<std::string> lines;
    // The pieces between line breaks, the empty ones skipped (wcstok's).
    std::size_t start = 0;
    while (start < utf8.size()) {
        const std::size_t stop = utf8.find_first_of("\r\n", start);
        const std::string_view piece = utf8.substr(start, stop == std::string_view::npos ? stop : stop - start);
        start = stop == std::string_view::npos ? utf8.size() : stop + 1;
        if (piece.empty()) {
            continue;
        }
        const Decoded p(piece);
        const std::ptrdiff_t n = p.size();
        std::ptrdiff_t a = 0; // where the next line starts
        while (true) {
            // The rest whole if it fits; else back from its end, space by space, to the longest
            // run of words that fits; with no space to go back to, as many characters as fit.
            std::ptrdiff_t cut = n - a;
            std::ptrdiff_t back = n - a;
            float width = p.width(a, n, advance);
            while (width > maxWidth) {
                std::ptrdiff_t at = back;
                bool noSpace = at == 0;
                while (!noSpace && p.at(a + at) != ' ') {
                    if (--at == 0) {
                        noSpace = true;
                    }
                }
                if (noSpace) {
                    float w = 0;
                    cut = 0;
                    for (std::ptrdiff_t i = a; i < n; ++i) {
                        w += advance(p.at(i));
                        if (w > maxWidth) {
                            break;
                        }
                        ++cut;
                    }
                    break;
                }
                width = p.width(a, a + at, advance);
                back = at - 1;
                cut = at;
            }
            const std::string line = p.sub(a, a + cut);
            a += cut;
            if (p.at(a) == ' ') {
                ++a; // one space after the cut
            }
            if (lines.size() >= maxLines) {
                return lines;
            }
            lines.push_back(line);
            if (a >= n) {
                break;
            }
        }
    }
    return lines;
}

std::vector<std::string> centredLines(std::string_view utf8, float maxWidth, const GlyphAdvance& advance)
{
    const Decoded t(utf8);
    const std::ptrdiff_t n = t.size();
    std::vector<std::string> lines;
    std::ptrdiff_t pos = 0;
    while (true) {
        // Words onto the line while it fits: each try runs to the next space (or the end), cut
        // at a newline before it.
        const std::ptrdiff_t lineStart = pos;
        std::ptrdiff_t accepted = pos;
        std::ptrdiff_t scan = pos;
        bool last = false;
        while (true) {
            std::ptrdiff_t end = scan;
            while (end < n && t.at(end) != ' ') {
                ++end;
            }
            last = end >= n;
            bool newline = false;
            for (std::ptrdiff_t q = accepted; q <= end; ++q) {
                if (t.at(q) == '\n') {
                    newline = true;
                    end = q;
                    last = false;
                    break;
                }
            }
            if (t.width(lineStart, end, advance) <= maxWidth) {
                accepted = end;
                scan = end + 1;
                if (last || newline) {
                    break;
                }
                continue;
            }
            // Too wide: the words before; a word too long alone is the line itself.
            if (accepted == lineStart) {
                accepted = end;
            }
            last = false;
            break;
        }
        lines.push_back(t.sub(lineStart, accepted));
        pos = accepted + 1; // past the space or newline
        // (A word too long alone at the very end leaves the game reading on past the text's
        // end; here the text ends there: OB-419.)
        if (last || pos > n) {
            break;
        }
    }
    return lines;
}

} // namespace noxsdl
