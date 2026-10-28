// The code editor's colouring (code_lexer.hpp).
#include "editor/code_lexer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace noxsdl::editor {

namespace {

bool continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

bool identStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool identChar(char c)
{
    return identStart(c) || (c >= '0' && c <= '9');
}

/// The column after the character at `col` (a UTF-8 letter whole).
int nextCol(std::string_view s, int col)
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

// ---- NoxScript (the map scripts')

bool isNoxKeyword(std::string_view w)
{
    static constexpr std::array<std::string_view, 22> k{"if", "else", "while", "for", "switch", "case", "default", "break", "continue",
                                                        "return", "goto", "true", "false", "self", "other", "TRUE", "FALSE", "SELF",
                                                        "OTHER", "assert", "Wait", "__goto_if"};
    return std::ranges::find(k, w) != k.end() || w == "__goto_ifnot";
}

bool isNoxType(std::string_view w)
{
    return w == "int" || w == "float" || w == "string" || w == "object" || w == "void";
}

std::vector<TokSpan> lexNoxScript(std::string_view s, std::uint8_t& state, const std::function<Tok(std::string_view)>& classify)
{
    std::vector<TokSpan> out;
    bool inComment = state != 0;
    const int n = static_cast<int>(s.size());
    int i = 0;
    while (i < n) {
        const int start = i;
        if (inComment) {
            const auto close = s.find("*/", static_cast<std::size_t>(i));
            i = close == std::string_view::npos ? n : static_cast<int>(close) + 2;
            if (close != std::string_view::npos) {
                inComment = false;
            }
            out.push_back({start, i, Tok::Comment});
            continue;
        }
        const char c = s[static_cast<std::size_t>(i)];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[static_cast<std::size_t>(i + 1)] == '/') {
            const bool flag = i + 2 < n && s[static_cast<std::size_t>(i + 2)] == '@';
            out.push_back({i, n, flag ? Tok::Flag : Tok::Comment});
            break;
        }
        if (c == '/' && i + 1 < n && s[static_cast<std::size_t>(i + 1)] == '*') {
            inComment = true;
            i += 2;
            const auto close = s.find("*/", static_cast<std::size_t>(i));
            if (close != std::string_view::npos) {
                inComment = false;
                i = static_cast<int>(close) + 2;
            } else {
                i = n;
            }
            out.push_back({start, i, Tok::Comment});
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < n && s[static_cast<std::size_t>(i)] != '"') {
                i += s[static_cast<std::size_t>(i)] == '\\' ? 2 : 1;
            }
            i = std::min(i + 1, n);
            out.push_back({start, i, Tok::String});
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[static_cast<std::size_t>(i + 1)])))) {
            while (i < n && (std::isalnum(static_cast<unsigned char>(s[static_cast<std::size_t>(i)])) || s[static_cast<std::size_t>(i)] == '.')) {
                ++i;
            }
            out.push_back({start, i, Tok::Number});
            continue;
        }
        if (identStart(c)) {
            while (i < n && identChar(s[static_cast<std::size_t>(i)])) {
                ++i;
            }
            const std::string_view w(s.data() + start, static_cast<std::size_t>(i - start));
            Tok t = isNoxType(w) ? Tok::Type : isNoxKeyword(w) ? Tok::Keyword : classify ? classify(w) : Tok::Plain;
            out.push_back({start, i, t});
            continue;
        }
        i = nextCol(s, i);
        out.push_back({start, i, Tok::Punct});
    }
    state = inComment ? 1 : 0;
    return out;
}

// ---- Luau (the mods')

bool isLuauKeyword(std::string_view w)
{
    static constexpr std::array<std::string_view, 22> k{"and",  "break", "do",    "else", "elseif", "end",    "false", "for",
                                                        "function", "if", "in", "local", "nil", "not", "or", "repeat",
                                                        "return", "then", "true", "until", "while", "continue"};
    return std::ranges::find(k, w) != k.end();
}

/// A long bracket's level at `i` ("[[" 0, "[==[" 2); -1: not one.
int longOpen(std::string_view s, int i)
{
    const int n = static_cast<int>(s.size());
    if (i >= n || s[static_cast<std::size_t>(i)] != '[') {
        return -1;
    }
    int j = i + 1;
    while (j < n && s[static_cast<std::size_t>(j)] == '=') {
        ++j;
    }
    if (j < n && s[static_cast<std::size_t>(j)] == '[') {
        const int level = j - i - 1;
        return level <= 62 ? level : -1;
    }
    return -1;
}

/// Where the long bracket of `level` that's open closes in `s` from `i` (just past its "]=]");
/// npos: not on this line.
std::size_t longClose(std::string_view s, int i, int level)
{
    const std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
    const std::size_t at = s.find(close, static_cast<std::size_t>(i));
    return at == std::string_view::npos ? at : at + close.size();
}

std::vector<TokSpan> lexLuau(std::string_view s, std::uint8_t& state, const std::function<Tok(std::string_view)>& classify)
{
    std::vector<TokSpan> out;
    const int n = static_cast<int>(s.size());
    int i = 0;
    // Inside a long comment or string from the line before.
    if (state != 0) {
        const bool comment = state < kLuauLongString;
        const int level = comment ? state - kLuauLongComment : state - kLuauLongString;
        const std::size_t end = longClose(s, 0, level);
        i = end == std::string_view::npos ? n : static_cast<int>(end);
        if (end != std::string_view::npos) {
            state = 0;
        }
        if (i > 0) {
            out.push_back({0, i, comment ? Tok::Comment : Tok::String});
        }
    }
    while (i < n) {
        const int start = i;
        const char c = s[static_cast<std::size_t>(i)];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '-' && i + 1 < n && s[static_cast<std::size_t>(i + 1)] == '-') {
            const int level = longOpen(s, i + 2);
            if (level < 0) {
                out.push_back({i, n, Tok::Comment});
                break;
            }
            const std::size_t end = longClose(s, i + 4 + level, level);
            if (end == std::string_view::npos) {
                state = static_cast<std::uint8_t>(kLuauLongComment + level);
                i = n;
            } else {
                i = static_cast<int>(end);
            }
            out.push_back({start, i, Tok::Comment});
            continue;
        }
        if (const int level = longOpen(s, i); level >= 0) {
            const std::size_t end = longClose(s, i + 2 + level, level);
            if (end == std::string_view::npos) {
                state = static_cast<std::uint8_t>(kLuauLongString + level);
                i = n;
            } else {
                i = static_cast<int>(end);
            }
            out.push_back({start, i, Tok::String});
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            ++i;
            while (i < n && s[static_cast<std::size_t>(i)] != c) {
                i += s[static_cast<std::size_t>(i)] == '\\' ? 2 : 1;
            }
            i = std::min(i + 1, n);
            out.push_back({start, i, Tok::String});
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[static_cast<std::size_t>(i + 1)])))) {
            const bool hex = c == '0' && i + 1 < n && (s[static_cast<std::size_t>(i + 1)] == 'x' || s[static_cast<std::size_t>(i + 1)] == 'X');
            while (i < n) {
                const char d = s[static_cast<std::size_t>(i)];
                const bool sign = (d == '+' || d == '-') && !hex && (s[static_cast<std::size_t>(i - 1)] == 'e' || s[static_cast<std::size_t>(i - 1)] == 'E');
                if (!(std::isalnum(static_cast<unsigned char>(d)) || d == '.' || d == '_' || sign)) {
                    break;
                }
                ++i;
            }
            out.push_back({start, i, Tok::Number});
            continue;
        }
        if (identStart(c)) {
            while (i < n && identChar(s[static_cast<std::size_t>(i)])) {
                ++i;
            }
            const std::string_view w(s.data() + start, static_cast<std::size_t>(i - start));
            out.push_back({start, i, isLuauKeyword(w) ? Tok::Keyword : classify ? classify(w) : Tok::Plain});
            continue;
        }
        i = nextCol(s, i);
        out.push_back({start, i, Tok::Punct});
    }
    return out;
}

} // namespace

std::vector<TokSpan> lexLine(CodeLanguage language, std::string_view line, std::uint8_t& state, const std::function<Tok(std::string_view)>& classify)
{
    return language == CodeLanguage::Luau ? lexLuau(line, state, classify) : lexNoxScript(line, state, classify);
}

std::string_view lineComment(CodeLanguage language)
{
    return language == CodeLanguage::Luau ? "--" : "//";
}

bool luauOpensBlock(std::string_view line, std::uint8_t state)
{
    // The blocks and brackets the line leaves open: "if a then b() end" leaves none.
    int open = 0;
    std::string_view last;
    for (const TokSpan& r : lexLuau(line, state, {})) {
        if (r.tok == Tok::Comment || r.tok == Tok::String) {
            continue;
        }
        const std::string_view w = line.substr(static_cast<std::size_t>(r.begin), static_cast<std::size_t>(r.end - r.begin));
        if (r.tok == Tok::Keyword) {
            open += w == "function" || w == "then" || w == "do" || w == "repeat" ? 1 : w == "end" || w == "until" ? -1 : 0;
        } else if (r.tok == Tok::Punct) {
            open += w == "{" || w == "(" || w == "[" ? 1 : w == "}" || w == ")" || w == "]" ? -1 : 0;
        }
        last = w;
    }
    if (state != 0) {
        return false; // (the line ends inside a long string or comment)
    }
    return open > 0 || last == "else";
}

bool luauClosesBlock(std::string_view line)
{
    const std::size_t at = line.find_first_not_of(" \t");
    if (at == std::string_view::npos) {
        return false;
    }
    const std::string_view w = line.substr(at);
    return w == "end" || w == "else" || w == "elseif" || w == "until";
}

} // namespace noxsdl::editor
