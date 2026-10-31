// Where the game's GUI breaks text into lines, apart from any font: the glyph widths come from
// the caller (the TrueType font at the size it's drawn, or fixed widths in the tests), so the
// lines are worked out in the same units the text is drawn in.
//
//   wrapLines     drawStringWrap (and drawGetStringSize's line count): the general wrap every
//                 wrapped label, list line and message uses. A line breaks after its last space
//                 once a glyph takes it past the width; a word with no space before it on the
//                 line breaks one character early (the character that still fit goes to the
//                 next line, uncounted: OB-416); '\r', '\n' and "\r\n" end a line. A word too
//                 long for the line right after a newline, with a space somewhere earlier in the
//                 text, takes that stale space as its break (OB-417).
//   dialogLines   the NPC dialog's text (GUIDlg's text setter): the text split at '\r' / '\n'
//                 with the empty pieces dropped (a blank line between paragraphs never shows:
//                 OB-418), each piece cut into the longest run of whole words that fits, a word
//                 too long for a line cut at the last character that fits, one space skipped
//                 after a cut, at most `maxLines` lines.
//   centredLines  a centred STATICTEXT's lines when its text needs more than one
//                 (wndStaticDrawNoImage): words onto a line while the line fits the width, a
//                 newline ending one; a word too long alone is a line of its own (drawn wrapped
//                 inside the width, so as more than one row; at the text's end, where the game
//                 reads on past it, the text ends: OB-419).
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// A glyph's advance, in the units the widths are given in.
using GlyphAdvance = std::function<float(char32_t)>;

/// Decodes one code point from UTF-8 at `pos`, advancing it. Invalid bytes decode as U+FFFD.
char32_t nextCodepoint(std::string_view utf8, std::size_t& pos);

/// drawStringWrap's lines of `utf8` at `maxWidth` (0: no wrapping, only the newlines). An empty
/// text is one empty line; a trailing empty line (after a final newline or break) is dropped.
std::vector<std::string> wrapLines(std::string_view utf8, float maxWidth, const GlyphAdvance& advance);

/// The NPC dialog's list lines for `utf8` at `maxWidth` (the list box's width less 10).
std::vector<std::string> dialogLines(std::string_view utf8, float maxWidth, std::size_t maxLines,
                                     const GlyphAdvance& advance);

/// A centred STATICTEXT's lines of `utf8` at `maxWidth`, for a text wrapLines makes more than one
/// line of; each is drawn centred on its own (and wrapped again at the width, which only splits a
/// lone word too long for it).
std::vector<std::string> centredLines(std::string_view utf8, float maxWidth, const GlyphAdvance& advance);

} // namespace noxsdl
