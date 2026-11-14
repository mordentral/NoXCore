// The code editor's colouring (code_editor.hpp): a line cut into runs by kind, in one of the
// editor's languages. NoxScript is the map scripts' (nsc's C-like syntax: // and /* */ comments,
// "//@" flags); Luau is the mods' (-- and --[[ ]] comments, '...', "..." and `...` strings, [[ ]]
// long strings, Luau's keywords). No ImGui: the tests read the runs.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace noxsdl::editor {

enum class Tok : std::uint8_t { Plain, Keyword, Type, Builtin, Function, Number, String, Comment, Flag, Punct };

enum class CodeLanguage : std::uint8_t { NoxScript, Luau };

struct TokSpan {
    int begin = 0;
    int end = 0;
    Tok tok = Tok::Plain;
};

/// One line's runs (spaces between them left out). `state` is what the line starts inside and,
/// after, what the next one does (0: nothing; NoxScript 1: a /* */ comment; Luau: a long comment
/// or string and its level, kLuauLong...). `classify` colours a word that isn't a keyword (unset:
/// plain).
std::vector<TokSpan> lexLine(CodeLanguage language, std::string_view line, std::uint8_t& state,
                             const std::function<Tok(std::string_view word)>& classify);

/// Luau's long brackets as a line's state: a comment's or a string's, and its level (the '='s
/// between its brackets, 0-62).
inline constexpr std::uint8_t kLuauLongComment = 1;
inline constexpr std::uint8_t kLuauLongString = 64;

/// A UTF-8 continuation byte (not a character's first): columns are byte offsets, and the lexer
/// and the code editor step over a letter whole.
inline bool continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

/// A name's first character (an ASCII letter or '_'), and any of its others (digits too).
inline bool identStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool identChar(char c)
{
    return identStart(c) || (c >= '0' && c <= '9');
}

/// The column after the character at `col` (a UTF-8 letter whole).
inline int nextCol(std::string_view s, int col)
{
    const int n = static_cast<int>(s.size());
    if (col >= n) {
        return n;
    }
    ++col;
    while (col < n && continuation(s[static_cast<std::size_t>(col)])) {
        ++col;
    }
    return col;
}

/// The language's line comment ("//", "--").
std::string_view lineComment(CodeLanguage language);

/// Luau's auto-indent: whether a line (up to the caret; `state` what it starts inside) leaves a
/// block or a bracket open (function, then, do, repeat, { ( [ against end, until, } ) ]; or it
/// ends with else), so the next line goes in a step. Comments and strings don't count.
bool luauOpensBlock(std::string_view line, std::uint8_t state);
/// Luau's auto-indent: whether a line is only a word that closes a block (end, else, elseif,
/// until) after its spaces, so typing it takes the line back a step.
bool luauClosesBlock(std::string_view line);

} // namespace noxsdl::editor
