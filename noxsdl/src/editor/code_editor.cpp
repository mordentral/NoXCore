// The script panel's source editor (code_editor.hpp).
#include "editor/code_editor.hpp"
#include "editor/theme_colours.hpp"

#include "noxcore/util/text.hpp"

#include <SDL3/SDL.h>
#include <imgui_internal.h> // (ImTextCharToUtf8)

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace noxsdl::editor {

namespace {

constexpr int kTab = 4;

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

/// The palette, by theme: Dark's on its near-black, neutral greys; Medium's on the warm stone
/// greys; Light's on pale panels.
ImU32 colour(CodeEditor::Tok t, Theme theme)
{
    using T = CodeEditor::Tok;
    if (theme == Theme::Dark) {
        switch (t) {
        case T::Keyword: return IM_COL32(200, 140, 240, 255);
        case T::Type: return IM_COL32(90, 170, 255, 255);
        case T::Builtin: return IM_COL32(255, 186, 80, 255);
        case T::Function: return IM_COL32(110, 214, 200, 255);
        case T::Number: return IM_COL32(176, 222, 134, 255);
        case T::String: return IM_COL32(234, 150, 112, 255);
        case T::Comment: return IM_COL32(112, 150, 104, 255);
        case T::Flag: return IM_COL32(214, 172, 120, 255);
        case T::Punct: return IM_COL32(170, 170, 170, 255);
        default: return IM_COL32(214, 214, 214, 255);
        }
    }
    const bool light = theme == Theme::Light;
    switch (t) {
    case T::Keyword: return light ? IM_COL32(150, 40, 140, 255) : IM_COL32(214, 150, 220, 255);
    case T::Type: return light ? IM_COL32(30, 80, 170, 255) : IM_COL32(110, 170, 240, 255);
    case T::Builtin: return light ? IM_COL32(150, 100, 0, 255) : IM_COL32(230, 190, 90, 255);
    case T::Function: return light ? IM_COL32(20, 120, 120, 255) : IM_COL32(120, 210, 200, 255);
    case T::Number: return light ? IM_COL32(20, 120, 40, 255) : IM_COL32(170, 220, 140, 255);
    case T::String: return light ? IM_COL32(170, 60, 30, 255) : IM_COL32(230, 150, 110, 255);
    case T::Comment: return light ? IM_COL32(110, 120, 110, 255) : IM_COL32(120, 135, 115, 255);
    case T::Flag: return light ? IM_COL32(140, 90, 30, 255) : IM_COL32(200, 160, 110, 255);
    case T::Punct: return light ? IM_COL32(70, 70, 70, 255) : IM_COL32(190, 185, 170, 255);
    default: return light ? IM_COL32(30, 30, 30, 255) : IM_COL32(226, 218, 200, 255);
    }
}

using nox::util::lowered;

} // namespace

CodeEditor::CodeEditor()
{
    restate();
}

void CodeEditor::setText(std::string_view text, bool keepCaret)
{
    lines_.clear();
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            lines_.push_back(std::move(cur));
            cur.clear();
        } else if (c == '\n') {
            lines_.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines_.push_back(std::move(cur));
    if (!keepCaret) {
        caret_ = {};
    }
    caret_ = clamp(caret_);
    anchor_ = caret_;
    restate();
}

std::string CodeEditor::text() const
{
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i];
        if (i + 1 < lines_.size()) {
            out += '\n';
        }
    }
    return out;
}

bool CodeEditor::takeChanged()
{
    return std::exchange(changed_, false);
}

void CodeEditor::setMarkers(std::vector<Marker> markers)
{
    markers_ = std::move(markers);
}

void CodeEditor::setLanguage(CodeLanguage language)
{
    if (language != language_) {
        language_ = language;
        restate();
    }
}

void CodeEditor::restate()
{
    commentAtStart_.assign(lines_.size(), 0);
    std::uint8_t in = 0;
    longest_ = 0;
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        commentAtStart_[static_cast<std::size_t>(i)] = in;
        tokenize(i, in);
        longest_ = std::max(longest_, visualCol(i, static_cast<int>(lines_[static_cast<std::size_t>(i)].size())));
    }
}

void CodeEditor::edited()
{
    changed_ = true;
    ensureVisible_ = true;
    blinkStart_ = SDL_GetTicks();
    restate();
}

std::vector<CodeEditor::Span> CodeEditor::tokenize(int lineIndex, std::uint8_t& state) const
{
    return lexLine(language_, lines_[static_cast<std::size_t>(lineIndex)], state, classify);
}

TextPos CodeEditor::clamp(TextPos p) const
{
    p.line = std::clamp(p.line, 0, static_cast<int>(lines_.size()) - 1);
    const std::string& s = lines_[static_cast<std::size_t>(p.line)];
    p.col = std::clamp(p.col, 0, static_cast<int>(s.size()));
    while (p.col > 0 && p.col < static_cast<int>(s.size()) && continuation(s[static_cast<std::size_t>(p.col)])) {
        --p.col;
    }
    return p;
}

int CodeEditor::prevCol(int line, int col) const
{
    const std::string& s = lines_[static_cast<std::size_t>(line)];
    if (col <= 0) {
        return 0;
    }
    --col;
    while (col > 0 && continuation(s[static_cast<std::size_t>(col)])) {
        --col;
    }
    return col;
}

int CodeEditor::nextCol(int line, int col) const
{
    const std::string& s = lines_[static_cast<std::size_t>(line)];
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

int CodeEditor::visualCol(int line, int col) const
{
    const std::string& s = lines_[static_cast<std::size_t>(line)];
    int v = 0;
    for (int i = 0; i < col && i < static_cast<int>(s.size()); ++i) {
        const char c = s[static_cast<std::size_t>(i)];
        if (continuation(c)) {
            continue;
        }
        v = c == '\t' ? (v / kTab + 1) * kTab : v + 1;
    }
    return v;
}

int CodeEditor::colAtVisual(int line, int visual) const
{
    const std::string& s = lines_[static_cast<std::size_t>(line)];
    int v = 0;
    int i = 0;
    const int n = static_cast<int>(s.size());
    while (i < n) {
        const char c = s[static_cast<std::size_t>(i)];
        const int next = c == '\t' ? (v / kTab + 1) * kTab : v + 1;
        if (visual < next && visual - v < next - visual) {
            return i;
        }
        if (visual < next) {
            return nextCol(line, i);
        }
        v = next;
        i = nextCol(line, i);
    }
    return n;
}

std::pair<int, int> CodeEditor::wordAt(int line, int col) const
{
    const std::string& s = lines_[static_cast<std::size_t>(line)];
    int b = col;
    int e = col;
    while (b > 0 && identChar(s[static_cast<std::size_t>(b - 1)])) {
        --b;
    }
    while (e < static_cast<int>(s.size()) && identChar(s[static_cast<std::size_t>(e)])) {
        ++e;
    }
    return {b, e};
}

std::string CodeEditor::wordAtCaret() const
{
    const auto [b, e] = wordAt(caret_.line, caret_.col);
    return lines_[static_cast<std::size_t>(caret_.line)].substr(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b));
}

TextPos CodeEditor::wordLeft(TextPos p) const
{
    if (p.col == 0) {
        return p.line > 0 ? TextPos{p.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(p.line - 1)].size())} : p;
    }
    const std::string& s = lines_[static_cast<std::size_t>(p.line)];
    int c = p.col;
    while (c > 0 && s[static_cast<std::size_t>(c - 1)] == ' ') --c;
    if (c > 0 && identChar(s[static_cast<std::size_t>(c - 1)])) {
        while (c > 0 && identChar(s[static_cast<std::size_t>(c - 1)])) --c;
    } else if (c > 0) {
        c = prevCol(p.line, c);
    }
    return {p.line, c};
}

TextPos CodeEditor::wordRight(TextPos p) const
{
    const std::string& s = lines_[static_cast<std::size_t>(p.line)];
    const int n = static_cast<int>(s.size());
    if (p.col >= n) {
        return p.line + 1 < static_cast<int>(lines_.size()) ? TextPos{p.line + 1, 0} : p;
    }
    int c = p.col;
    if (identChar(s[static_cast<std::size_t>(c)])) {
        while (c < n && identChar(s[static_cast<std::size_t>(c)])) ++c;
    } else {
        c = nextCol(p.line, c);
    }
    while (c < n && s[static_cast<std::size_t>(c)] == ' ') ++c;
    return {p.line, c};
}

std::pair<TextPos, TextPos> CodeEditor::selection() const
{
    return caret_ < anchor_ ? std::pair{caret_, anchor_} : std::pair{anchor_, caret_};
}

std::string CodeEditor::selectedText() const
{
    const auto [a, b] = selection();
    if (a == b) {
        return {};
    }
    if (a.line == b.line) {
        return lines_[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
    }
    std::string out = lines_[static_cast<std::size_t>(a.line)].substr(static_cast<std::size_t>(a.col)) + "\n";
    for (int l = a.line + 1; l < b.line; ++l) {
        out += lines_[static_cast<std::size_t>(l)] + "\n";
    }
    return out + lines_[static_cast<std::size_t>(b.line)].substr(0, static_cast<std::size_t>(b.col));
}

void CodeEditor::deleteSelection()
{
    const auto [a, b] = selection();
    if (a == b) {
        return;
    }
    std::string& first = lines_[static_cast<std::size_t>(a.line)];
    const std::string tail = lines_[static_cast<std::size_t>(b.line)].substr(static_cast<std::size_t>(b.col));
    first.erase(static_cast<std::size_t>(a.col));
    first += tail;
    lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    caret_ = anchor_ = a;
}

void CodeEditor::insertRaw(std::string_view s)
{
    deleteSelection();
    std::string& cur = lines_[static_cast<std::size_t>(caret_.line)];
    const std::string tail = cur.substr(static_cast<std::size_t>(caret_.col));
    cur.erase(static_cast<std::size_t>(caret_.col));
    std::size_t start = 0;
    int line = caret_.line;
    for (;;) {
        const auto nl = s.find('\n', start);
        std::string_view piece = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (!piece.empty() && piece.back() == '\r') {
            piece.remove_suffix(1);
        }
        lines_[static_cast<std::size_t>(line)] += piece;
        if (nl == std::string_view::npos) {
            break;
        }
        lines_.insert(lines_.begin() + line + 1, std::string());
        ++line;
        start = nl + 1;
    }
    caret_ = {line, static_cast<int>(lines_[static_cast<std::size_t>(line)].size())};
    lines_[static_cast<std::size_t>(line)] += tail;
    anchor_ = caret_;
}

void CodeEditor::insertText(std::string_view s)
{
    if (readOnly_) {
        return;
    }
    insertRaw(s);
    edited();
}

void CodeEditor::newline()
{
    deleteSelection();
    const std::string& cur = lines_[static_cast<std::size_t>(caret_.line)];
    std::string indent;
    for (const char c : cur) {
        if (c != ' ' && c != '\t') break;
        indent += c;
    }
    if (static_cast<int>(indent.size()) > caret_.col) {
        indent.resize(static_cast<std::size_t>(caret_.col));
    }
    int k = caret_.col;
    while (k > 0 && cur[static_cast<std::size_t>(k - 1)] == ' ') --k;
    if (language_ == CodeLanguage::Luau) {
        // (a block or a bracket left open: function, then, do, {...)
        const auto at = static_cast<std::size_t>(caret_.line);
        if (luauOpensBlock(std::string_view(cur).substr(0, static_cast<std::size_t>(caret_.col)), at < commentAtStart_.size() ? commentAtStart_[at] : 0)) {
            indent += std::string(kTab, ' ');
        }
    } else if (k > 0 && cur[static_cast<std::size_t>(k - 1)] == '{') {
        indent += std::string(kTab, ' ');
    }
    insertRaw("\n" + indent);
}

void CodeEditor::dedentClosing()
{
    // A line just typed to end, else, elseif or until, as deep as the line above it or deeper,
    // goes back a step (to the block it closes).
    std::string& cur = lines_[static_cast<std::size_t>(caret_.line)];
    if (hasSelection() || caret_.col != static_cast<int>(cur.size()) || !luauClosesBlock(cur)) {
        return;
    }
    const std::size_t indent = cur.find_first_not_of(' ');
    if (indent < static_cast<std::size_t>(kTab)) {
        return;
    }
    int above = caret_.line - 1;
    while (above >= 0 && lines_[static_cast<std::size_t>(above)].find_first_not_of(" \t") == std::string::npos) {
        --above;
    }
    if (above >= 0 && lines_[static_cast<std::size_t>(above)].find_first_not_of(' ') > indent) {
        return; // (taken back by hand already)
    }
    cur.erase(0, static_cast<std::size_t>(kTab));
    caret_.col -= kTab;
    anchor_ = caret_;
}

void CodeEditor::backspace(bool word)
{
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    if (caret_.col == 0) {
        if (caret_.line == 0) return;
        anchor_ = caret_;
        caret_ = {caret_.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(caret_.line - 1)].size())};
    } else {
        anchor_ = caret_;
        caret_ = word ? wordLeft(caret_) : TextPos{caret_.line, prevCol(caret_.line, caret_.col)};
    }
    deleteSelection();
}

void CodeEditor::deleteForward(bool word)
{
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    anchor_ = caret_;
    const int n = static_cast<int>(lines_[static_cast<std::size_t>(caret_.line)].size());
    if (caret_.col >= n) {
        if (caret_.line + 1 >= static_cast<int>(lines_.size())) return;
        caret_ = {caret_.line + 1, 0};
    } else {
        caret_ = word ? wordRight(caret_) : TextPos{caret_.line, nextCol(caret_.line, caret_.col)};
    }
    deleteSelection();
}

void CodeEditor::moveCaret(TextPos to, bool select)
{
    caret_ = clamp(to);
    if (!select) {
        anchor_ = caret_;
    }
    ensureVisible_ = true;
    blinkStart_ = SDL_GetTicks();
}

void CodeEditor::indentLines(bool in)
{
    const auto [a, b] = selection();
    const int last = b.col == 0 && b.line > a.line ? b.line - 1 : b.line;
    for (int l = a.line; l <= last; ++l) {
        std::string& s = lines_[static_cast<std::size_t>(l)];
        int delta = 0;
        if (in) {
            s.insert(0, std::string(kTab, ' '));
            delta = kTab;
        } else {
            int k = 0;
            while (k < kTab && k < static_cast<int>(s.size()) && s[static_cast<std::size_t>(k)] == ' ') ++k;
            if (k == 0 && !s.empty() && s[0] == '\t') k = 1;
            s.erase(0, static_cast<std::size_t>(k));
            delta = -k;
        }
        if (caret_.line == l) caret_.col = std::max(0, caret_.col + delta);
        if (anchor_.line == l) anchor_.col = std::max(0, anchor_.col + delta);
    }
    caret_ = clamp(caret_);
    anchor_ = clamp(anchor_);
}

void CodeEditor::toggleComment()
{
    const auto [a, b] = selection();
    const int last = b.col == 0 && b.line > a.line ? b.line - 1 : b.line;
    const std::string mark(lineComment(language_)); // ("//", Luau's "--")
    const std::string spaced = mark + " ";
    bool all = true;
    for (int l = a.line; l <= last; ++l) {
        const std::string& s = lines_[static_cast<std::size_t>(l)];
        const auto first = s.find_first_not_of(" \t");
        if (first != std::string::npos && s.compare(first, 2, mark) != 0) all = false;
    }
    for (int l = a.line; l <= last; ++l) {
        std::string& s = lines_[static_cast<std::size_t>(l)];
        const auto first = s.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (all) {
            s.erase(first, s.compare(first, 3, spaced) == 0 ? 3 : 2);
        } else {
            s.insert(first, spaced);
        }
    }
    caret_ = clamp(caret_);
    anchor_ = clamp(anchor_);
}

void CodeEditor::copy(bool cut)
{
    std::string t = selectedText();
    if (t.empty()) {
        // No selection: the whole line.
        t = lines_[static_cast<std::size_t>(caret_.line)] + "\n";
        if (cut && !readOnly_) {
            if (lines_.size() > 1) {
                lines_.erase(lines_.begin() + caret_.line);
            } else {
                lines_[0].clear();
            }
            caret_ = anchor_ = clamp({caret_.line, 0});
            edited();
        }
        ImGui::SetClipboardText(t.c_str());
        return;
    }
    ImGui::SetClipboardText(t.c_str());
    if (cut && !readOnly_) {
        deleteSelection();
        edited();
    }
}

std::size_t CodeEditor::offsetOf(TextPos p) const
{
    p = clamp(p);
    std::size_t at = 0;
    for (int l = 0; l < p.line; ++l) {
        at += lines_[static_cast<std::size_t>(l)].size() + 1;
    }
    return at + static_cast<std::size_t>(p.col);
}

TextPos CodeEditor::posOf(std::size_t offset) const
{
    for (int l = 0; l < static_cast<int>(lines_.size()); ++l) {
        const std::size_t n = lines_[static_cast<std::size_t>(l)].size();
        if (offset <= n) {
            return clamp({l, static_cast<int>(offset)});
        }
        offset -= n + 1;
    }
    return {static_cast<int>(lines_.size()) - 1, static_cast<int>(lines_.back().size())};
}

void CodeEditor::replaceRange(std::size_t from, std::size_t to, std::string_view s)
{
    if (readOnly_) {
        return;
    }
    anchor_ = posOf(std::min(from, to));
    caret_ = posOf(std::max(from, to));
    insertRaw(s);
    edited();
}

bool CodeEditor::takeCaretMoved()
{
    const bool moved = caret_ != lastCaret_;
    lastCaret_ = caret_;
    return moved;
}

void CodeEditor::gotoLine(int line, int col)
{
    caret_ = anchor_ = clamp({line - 1, col - 1});
    ensureVisible_ = true;
    wantFocus_ = true;
}

bool CodeEditor::find(std::string_view needle, bool forward, bool matchCase)
{
    if (needle.empty()) {
        return false;
    }
    const std::string n = matchCase ? std::string(needle) : lowered(needle);
    const auto [a, b] = selection();
    const int lines = static_cast<int>(lines_.size());
    for (int k = 0; k <= lines; ++k) {
        const int l = ((forward ? a.line + k : a.line - k) % lines + lines) % lines;
        const std::string hay = matchCase ? lines_[static_cast<std::size_t>(l)] : lowered(lines_[static_cast<std::size_t>(l)]);
        std::size_t at = std::string::npos;
        if (forward) {
            const std::size_t from = k == 0 ? static_cast<std::size_t>(b.col) : 0;
            at = from <= hay.size() ? hay.find(n, from) : std::string::npos;
        } else {
            const std::size_t before = k == 0 ? static_cast<std::size_t>(a.col) : hay.size();
            at = before == 0 ? std::string::npos : hay.rfind(n, before - 1);
            if (at != std::string::npos && at + n.size() > before && k == 0) {
                at = at == 0 ? std::string::npos : hay.rfind(n, at - 1);
            }
        }
        if (k == lines && at != std::string::npos && forward && static_cast<int>(at) >= b.col) {
            at = std::string::npos; // (wrapped round to where it began)
        }
        if (at != std::string::npos) {
            anchor_ = {l, static_cast<int>(at)};
            caret_ = {l, static_cast<int>(at + n.size())};
            ensureVisible_ = true;
            return true;
        }
    }
    return false;
}

void CodeEditor::setHighlight(std::string needle, bool matchCase)
{
    highlight_ = matchCase ? std::move(needle) : lowered(needle);
    highlightCase_ = matchCase;
}

int CodeEditor::highlightCount() const
{
    if (highlight_.empty()) {
        return 0;
    }
    int count = 0;
    for (const auto& s : lines_) {
        const std::string hay = highlightCase_ ? s : lowered(s);
        for (std::size_t at = hay.find(highlight_); at != std::string::npos; at = hay.find(highlight_, at + highlight_.size())) {
            ++count;
        }
    }
    return count;
}

void CodeEditor::keyboard()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;
    // The owner's chords first.
    std::array<bool, ImGuiKey_NamedKey_END> taken{};
    if (shortcut) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            const auto key = static_cast<ImGuiKey>(k);
            const bool fkey = key >= ImGuiKey_F1 && key <= ImGuiKey_F12;
            const bool mod = (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) || (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper);
            if ((ctrl || alt || fkey) && !mod && ImGui::IsKeyPressed(key)) {
                int chord = key;
                if (ctrl) chord |= ImGuiMod_Ctrl;
                if (shift) chord |= ImGuiMod_Shift;
                if (alt) chord |= ImGuiMod_Alt;
                if (shortcut(chord)) taken[static_cast<std::size_t>(k)] = true;
            }
        }
    }
    // Then a drop-down's (the owner's autocomplete).
    if (popupKey && !ctrl && !alt) {
        for (const ImGuiKey k : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter,
                                 ImGuiKey_Tab, ImGuiKey_Escape}) {
            if (!taken[static_cast<std::size_t>(k)] && ImGui::IsKeyPressed(k) && popupKey(k)) {
                taken[static_cast<std::size_t>(k)] = true;
            }
        }
    }
    const auto pressed = [&](ImGuiKey k) { return !taken[static_cast<std::size_t>(k)] && ImGui::IsKeyPressed(k); };
    const int lineCount = static_cast<int>(lines_.size());
    const auto vertical = [&](int by) {
        if (wantVisual_ < 0) wantVisual_ = visualCol(caret_.line, caret_.col);
        const int l = std::clamp(caret_.line + by, 0, lineCount - 1);
        moveCaret({l, colAtVisual(l, wantVisual_)}, shift);
    };
    bool keepVisual = false;
    if (pressed(ImGuiKey_UpArrow)) {
        vertical(-1);
        keepVisual = true;
    } else if (pressed(ImGuiKey_DownArrow)) {
        vertical(1);
        keepVisual = true;
    } else if (pressed(ImGuiKey_PageUp)) {
        vertical(-std::max(1, static_cast<int>(ImGui::GetWindowHeight() / lineH_) - 1));
        keepVisual = true;
    } else if (pressed(ImGuiKey_PageDown)) {
        vertical(std::max(1, static_cast<int>(ImGui::GetWindowHeight() / lineH_) - 1));
        keepVisual = true;
    } else if (pressed(ImGuiKey_LeftArrow)) {
        if (hasSelection() && !shift) {
            moveCaret(selection().first, false);
        } else if (ctrl) {
            moveCaret(wordLeft(caret_), shift);
        } else if (caret_.col > 0) {
            moveCaret({caret_.line, prevCol(caret_.line, caret_.col)}, shift);
        } else if (caret_.line > 0) {
            moveCaret({caret_.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(caret_.line - 1)].size())}, shift);
        }
    } else if (pressed(ImGuiKey_RightArrow)) {
        if (hasSelection() && !shift) {
            moveCaret(selection().second, false);
        } else if (ctrl) {
            moveCaret(wordRight(caret_), shift);
        } else if (caret_.col < static_cast<int>(lines_[static_cast<std::size_t>(caret_.line)].size())) {
            moveCaret({caret_.line, nextCol(caret_.line, caret_.col)}, shift);
        } else if (caret_.line + 1 < lineCount) {
            moveCaret({caret_.line + 1, 0}, shift);
        }
    } else if (pressed(ImGuiKey_Home)) {
        if (ctrl) {
            moveCaret({0, 0}, shift);
        } else {
            const std::string& s = lines_[static_cast<std::size_t>(caret_.line)];
            const int first = static_cast<int>(std::min(s.find_first_not_of(" \t"), s.size()));
            moveCaret({caret_.line, caret_.col == first ? 0 : first}, shift);
        }
    } else if (pressed(ImGuiKey_End)) {
        if (ctrl) {
            moveCaret({lineCount - 1, static_cast<int>(lines_.back().size())}, shift);
        } else {
            moveCaret({caret_.line, static_cast<int>(lines_[static_cast<std::size_t>(caret_.line)].size())}, shift);
        }
    } else if (ctrl && pressed(ImGuiKey_A)) {
        anchor_ = {0, 0};
        caret_ = {lineCount - 1, static_cast<int>(lines_.back().size())};
    } else if (ctrl && (pressed(ImGuiKey_C) || pressed(ImGuiKey_Insert))) {
        copy(false);
    } else if (ctrl && pressed(ImGuiKey_X)) {
        copy(true);
    } else if (!readOnly_ && ((ctrl && pressed(ImGuiKey_V)) || (shift && pressed(ImGuiKey_Insert)))) {
        if (const char* clip = ImGui::GetClipboardText()) {
            insertRaw(clip);
            edited();
        }
    } else if (!readOnly_ && ctrl && pressed(ImGuiKey_Slash)) {
        toggleComment();
        edited();
    } else if (!readOnly_ && pressed(ImGuiKey_Backspace)) {
        backspace(ctrl);
        edited();
    } else if (!readOnly_ && pressed(ImGuiKey_Delete)) {
        if (shift && !hasSelection()) {
            copy(true);
        } else {
            deleteForward(ctrl);
            edited();
        }
    } else if (!readOnly_ && (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))) {
        newline();
        edited();
    } else if (!readOnly_ && pressed(ImGuiKey_Tab)) {
        if (shift || (hasSelection() && selection().first.line != selection().second.line)) {
            indentLines(!shift);
        } else {
            const int v = visualCol(caret_.line, caret_.col);
            insertRaw(std::string(static_cast<std::size_t>(kTab - v % kTab), ' '));
        }
        edited();
    } else if (!ctrl && pressed(ImGuiKey_F12) && gotoDefinition) {
        gotoDefinition(wordAtCaret());
    }
    if (!keepVisual && (io.InputQueueCharacters.Size > 0 || ImGui::IsAnyMouseDown())) {
        wantVisual_ = -1;
    }
    // Typed characters (not with Ctrl, whose chords are commands).
    if (!readOnly_ && !ctrl && io.InputQueueCharacters.Size > 0) {
        std::string typed;
        for (const ImWchar ch : io.InputQueueCharacters) {
            if (ch < 0x20 || ch == 0x7F) continue;
            char buf[5] = {};
            ImTextCharToUtf8(buf, ch);
            typed += buf;
        }
        if (!typed.empty()) {
            // A '}' on a line of only spaces takes back one indent.
            const std::string& s = lines_[static_cast<std::size_t>(caret_.line)];
            if (typed == "}" && !hasSelection() && caret_.col >= kTab && s.find_first_not_of(' ') >= static_cast<std::size_t>(caret_.col)) {
                lines_[static_cast<std::size_t>(caret_.line)].erase(static_cast<std::size_t>(caret_.col - kTab), kTab);
                caret_.col -= kTab;
                anchor_ = caret_;
            }
            insertRaw(typed);
            if (language_ == CodeLanguage::Luau) {
                dedentClosing();
            }
            edited();
        }
        io.InputQueueCharacters.resize(0);
    }
    if (!keepVisual && wantVisual_ >= 0 && !ImGui::IsKeyDown(ImGuiKey_UpArrow) && !ImGui::IsKeyDown(ImGuiKey_DownArrow) &&
        !ImGui::IsKeyDown(ImGuiKey_PageUp) && !ImGui::IsKeyDown(ImGuiKey_PageDown)) {
        wantVisual_ = -1;
    }
}

void CodeEditor::render(const char* id, ImVec2 size, ImFont* mono)
{
    if (mono) {
        ImGui::PushFont(mono, 0.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (wantFocus_) {
        ImGui::SetNextWindowFocus();
        wantFocus_ = false;
    }
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    focused_ = ImGui::IsWindowFocused();
    charW_ = ImGui::CalcTextSize("M").x;
    lineH_ = std::floor(ImGui::GetTextLineHeight() + 2.0f);
    const Theme theme = activeTheme();
    const bool light = theme == Theme::Light;
    const int lineCount = static_cast<int>(lines_.size());
    int digits = 1;
    for (int n = lineCount; n >= 10; n /= 10) ++digits;
    const float gutterW = static_cast<float>(digits + 3) * charW_;
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 origin = ImGui::GetCursorScreenPos(); // (scrolled)
    const float textX = origin.x + gutterW + charW_ * 0.5f;
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered();
    const auto posAt = [&](ImVec2 m) {
        const int l = std::clamp(static_cast<int>(std::floor((m.y - origin.y) / lineH_)), 0, lineCount - 1);
        const int v = std::max(0, static_cast<int>(std::lround((m.x - textX) / charW_)));
        return TextPos{l, colAtVisual(l, v)};
    };

    // The mouse.
    const bool inText = hovered && mouse.x >= winPos.x + gutterW && mouse.x < winPos.x + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize;
    if (inText) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const TextPos p = posAt(mouse);
        if (ImGui::GetIO().KeyCtrl && gotoDefinition) {
            caret_ = anchor_ = p;
            gotoDefinition(wordAtCaret());
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const auto [b, e] = wordAt(p.line, p.col);
            anchor_ = {p.line, b};
            caret_ = {p.line, e};
        } else {
            moveCaret(p, ImGui::GetIO().KeyShift);
            dragging_ = true;
        }
        blinkStart_ = SDL_GetTicks();
    }
    if (inText && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        // The context menu's word: the one clicked (the caret there, unless in the selection).
        const TextPos p = posAt(mouse);
        const auto [selFrom, selTo] = selection();
        if (!(hasSelection() && p >= selFrom && p <= selTo)) {
            caret_ = anchor_ = p;
        }
        if (contextMenu) {
            const auto [b, e] = wordAt(p.line, p.col);
            contextMenu(std::string_view(lines_[static_cast<std::size_t>(p.line)]).substr(static_cast<std::size_t>(b), static_cast<std::size_t>(std::max(0, e - b))));
        }
    }
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            caret_ = posAt(mouse);
            if (mouse.y < winPos.y) ImGui::SetScrollY(ImGui::GetScrollY() - lineH_);
            if (mouse.y > winPos.y + ImGui::GetWindowHeight()) ImGui::SetScrollY(ImGui::GetScrollY() + lineH_);
        } else {
            dragging_ = false;
        }
    }
    if (focused_) {
        keyboard();
    }

    // What's in view.
    const float scrollY = ImGui::GetScrollY();
    const float viewH = ImGui::GetWindowHeight();
    const int first = std::clamp(static_cast<int>(scrollY / lineH_) - 1, 0, std::max(0, lineCount - 1));
    const int last = std::clamp(static_cast<int>((scrollY + viewH) / lineH_) + 1, 0, lineCount - 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto [selA, selB] = selection();
    const ImU32 selCol = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    const ImU32 curLineCol = light ? IM_COL32(0, 0, 0, 14) : IM_COL32(255, 255, 255, 12);
    const ImU32 findCol = light ? IM_COL32(255, 210, 0, 90) : IM_COL32(255, 200, 60, 60);
    const float fontSize = ImGui::GetFontSize();
    ImFont* font = ImGui::GetFont();
    const float textTop = (lineH_ - fontSize) * 0.5f;
    std::uint8_t inComment = first < static_cast<int>(commentAtStart_.size()) ? commentAtStart_[static_cast<std::size_t>(first)] : 0;
    std::string hoverMessage;
    for (int l = first; l <= last; ++l) {
        const float y = origin.y + static_cast<float>(l) * lineH_;
        const std::string& s = lines_[static_cast<std::size_t>(l)];
        const auto x = [&](int col) { return textX + static_cast<float>(visualCol(l, col)) * charW_; };
        if (focused_ && l == caret_.line && !hasSelection()) {
            dl->AddRectFilled(ImVec2(winPos.x, y), ImVec2(winPos.x + ImGui::GetWindowWidth() + ImGui::GetScrollX() + 4000, y + lineH_), curLineCol);
        }
        // The selection.
        if (selA != selB && l >= selA.line && l <= selB.line) {
            const float x0 = l == selA.line ? x(selA.col) : textX;
            const float x1 = l == selB.line ? x(selB.col) : x(static_cast<int>(s.size())) + charW_ * 0.5f;
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x1, x0 + 2), y + lineH_), selCol);
        }
        // Matches of the find text.
        if (!highlight_.empty()) {
            const std::string hay = highlightCase_ ? s : lowered(s);
            for (std::size_t at = hay.find(highlight_); at != std::string::npos; at = hay.find(highlight_, at + highlight_.size())) {
                dl->AddRectFilled(ImVec2(x(static_cast<int>(at)), y), ImVec2(x(static_cast<int>(at + highlight_.size())), y + lineH_), findCol);
            }
        }
        // The text, run by run (a tab's room skipped).
        const auto spans = tokenize(l, inComment);
        for (const Span& sp : spans) {
            const ImU32 col = colour(sp.tok, theme);
            int a = sp.begin;
            while (a < sp.end) {
                int b = a;
                while (b < sp.end && s[static_cast<std::size_t>(b)] != '\t') ++b;
                if (b > a) {
                    dl->AddText(font, fontSize, ImVec2(x(a), y + textTop), col, s.data() + a, s.data() + b);
                }
                a = b + (b < sp.end ? 1 : 0);
            }
        }
        // Diagnostics: a wavy line under the word at the place, the message on hover.
        for (const Marker& m : markers_) {
            if (m.line - 1 != l) continue;
            const int c0 = std::clamp(m.col - 1, 0, static_cast<int>(s.size()));
            auto [wb, we] = wordAt(l, c0);
            if (c0 < static_cast<int>(s.size()) && s[static_cast<std::size_t>(c0)] == '"') {
                // A string literal: the whole of it, quotes and all (the lint's argument warnings).
                int e = c0 + 1;
                while (e < static_cast<int>(s.size()) && s[static_cast<std::size_t>(e)] != '"') {
                    e += s[static_cast<std::size_t>(e)] == '\\' ? 2 : 1;
                }
                we = std::min(e + 1, static_cast<int>(s.size()));
            }
            if (we <= c0) we = std::min(static_cast<int>(s.size()), nextCol(l, c0));
            const float x0 = x(c0);
            const float x1 = std::max(x(we), x0 + charW_);
            const ImU32 wc = m.error ? IM_COL32(240, 80, 70, 255) : IM_COL32(230, 190, 60, 255);
            const float yb = y + lineH_ - 2;
            for (float xx = x0; xx < x1; xx += 4) {
                dl->AddLine(ImVec2(xx, yb), ImVec2(std::min(xx + 2, x1), yb - 2), wc);
                dl->AddLine(ImVec2(std::min(xx + 2, x1), yb - 2), ImVec2(std::min(xx + 4, x1), yb), wc);
            }
            if (hovered && mouse.y >= y && mouse.y < y + lineH_ && ((mouse.x >= x0 && mouse.x <= x1) || mouse.x < winPos.x + gutterW)) {
                hoverMessage += (hoverMessage.empty() ? "" : "\n") + std::string(m.error ? "error: " : "warning: ") + m.message;
            }
        }
        // The caret.
        if (focused_ && l == caret_.line && ((SDL_GetTicks() - blinkStart_) / 530) % 2 == 0) {
            const float cx = x(caret_.col);
            dl->AddLine(ImVec2(cx, y + 1), ImVec2(cx, y + lineH_ - 1), ImGui::GetColorU32(ImGuiCol_Text), 1.5f);
        }
    }
    // The gutter over the text's left (it doesn't scroll sideways): numbers, diagnostic dots.
    const float gutterRight = winPos.x + gutterW;
    dl->AddRectFilled(ImVec2(winPos.x, winPos.y), ImVec2(gutterRight, winPos.y + viewH), ImGui::GetColorU32(ImGuiCol_ChildBg));
    dl->AddLine(ImVec2(gutterRight - 1, winPos.y), ImVec2(gutterRight - 1, winPos.y + viewH), ImGui::GetColorU32(ImGuiCol_Border));
    const ImU32 numCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int l = first; l <= last; ++l) {
        const float y = origin.y + static_cast<float>(l) * lineH_;
        char num[16];
        std::snprintf(num, sizeof num, "%d", l + 1);
        const float w = ImGui::CalcTextSize(num).x;
        dl->AddText(ImVec2(gutterRight - w - charW_ * 1.5f, y + textTop), l == caret_.line ? ImGui::GetColorU32(ImGuiCol_Text) : numCol, num);
        bool err = false, warn = false;
        for (const Marker& m : markers_) {
            if (m.line - 1 == l) (m.error ? err : warn) = true;
        }
        if (err || warn) {
            dl->AddCircleFilled(ImVec2(winPos.x + charW_ * 0.7f, y + lineH_ * 0.5f), charW_ * 0.35f,
                                err ? IM_COL32(240, 80, 70, 255) : IM_COL32(230, 190, 60, 255));
        }
    }
    // Where the caret is on the screen (a drop-down at the caret is put under it).
    caretScreen_ = ImVec2(textX + static_cast<float>(visualCol(caret_.line, caret_.col)) * charW_,
                          origin.y + static_cast<float>(caret_.line + 1) * lineH_);
    // The scrolled area's size.
    ImGui::Dummy(ImVec2(gutterW + static_cast<float>(longest_ + 8) * charW_, static_cast<float>(lineCount) * lineH_ + viewH * 0.5f));
    // Keep the caret in view after keys and edits.
    if (ensureVisible_) {
        ensureVisible_ = false;
        const float cy = static_cast<float>(caret_.line) * lineH_;
        if (cy < scrollY) ImGui::SetScrollY(cy);
        else if (cy + lineH_ * 2 > scrollY + viewH) ImGui::SetScrollY(cy + lineH_ * 2 - viewH);
        const float cx = static_cast<float>(visualCol(caret_.line, caret_.col)) * charW_;
        const float viewW = ImGui::GetWindowWidth() - gutterW - charW_ * 2;
        if (cx < ImGui::GetScrollX()) ImGui::SetScrollX(std::max(0.0f, cx - charW_ * 4));
        else if (cx > ImGui::GetScrollX() + viewW) ImGui::SetScrollX(cx - viewW + charW_ * 4);
    }
    // Hover help: a diagnostic's message, else the word's once the mouse has rested on it
    // (ImGui's normal hover delay).
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || !ImGui::IsWindowHovered()) {
        hoverStillSince_ = ImGui::GetTime();
    }
    const bool rested = ImGui::GetTime() - hoverStillSince_ >= ImGui::GetStyle().HoverDelayNormal;
    if (!hoverMessage.empty()) {
        ImGui::SetTooltip("%s", hoverMessage.c_str());
    } else if (inText && hoverText && !dragging_ && rested && ImGui::IsWindowHovered()) {
        const TextPos p = posAt(mouse);
        if (p.col < static_cast<int>(lines_[static_cast<std::size_t>(p.line)].size())) {
            const auto [b, e] = wordAt(p.line, p.col);
            if (e > b) {
                const std::string help = hoverText(std::string_view(lines_[static_cast<std::size_t>(p.line)]).substr(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b)));
                if (!help.empty() && ImGui::BeginTooltip()) {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.0f);
                    ImGui::TextUnformatted(help.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (mono) {
        ImGui::PopFont();
    }
}

} // namespace noxsdl::editor
