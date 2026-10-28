// A small source-code editor drawn with Dear ImGui (the map editor's script panel): lines with
// numbers, syntax colouring through a classifier, a caret and a selection by keys and mouse,
// clipboard, auto-indent, comment toggling, find with highlighted matches, diagnostics underlined
// with their messages on hover (a string literal's whole), hover help for words, Ctrl+click / F12
// for go to definition, and the keys an owner's drop-down at the caret takes first (popupKey).
//
// Text is UTF-8; columns are byte offsets that never split a character. A tab draws to the next
// stop of four. Undo is the owner's (the editor's document history): the editor reports changes
// and hands the owner every Ctrl / Alt / function-key chord first. The language (NoxScript, the
// map scripts', by default; Luau, the mods') sets the colouring and the line comment
// (code_lexer.hpp).
#pragma once

#include "editor/code_lexer.hpp"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl::editor {

struct TextPos {
    int line = 0; ///< 0-based
    int col = 0;  ///< 0-based byte offset in the line
    auto operator<=>(const TextPos&) const = default;
};

class CodeEditor {
public:
    using Tok = editor::Tok;

    /// A diagnostic shown in the text (1-based line and column, as the compiler gives them).
    struct Marker {
        int line = 0;
        int col = 0;
        bool error = true;
        std::string message;
    };

    /// What colour an identifier gets (keywords and types are the editor's own).
    std::function<Tok(std::string_view word)> classify;
    /// A chord (ImGuiKey | ImGuiMod_*) with Ctrl, Alt or a function key: true if the owner took it.
    std::function<bool(int chord)> shortcut;
    /// Ctrl+click or F12 on a word.
    std::function<void(std::string_view word)> gotoDefinition;
    /// A word's hover help ("" for none): a builtin's prototype... (shown wrapped)
    std::function<std::string(std::string_view word)> hoverText;
    /// A right-click on a word (the caret moved there first, unless it's in the selection).
    std::function<void(std::string_view word)> contextMenu;
    /// A key a drop-down at the caret may want first (the owner's autocomplete: Up, Down, Page Up,
    /// Page Down, Enter, Tab, Escape, without Ctrl or Alt): true if it took it.
    std::function<bool(int key)> popupKey;

    CodeEditor();

    /// Replaces the text; the caret kept where it was (clamped), or put at the start.
    void setText(std::string_view text, bool keepCaret = true);
    std::string text() const;
    /// Whether the user changed the text since the last call.
    bool takeChanged();
    void setMarkers(std::vector<Marker> markers);
    void setReadOnly(bool readOnly) { readOnly_ = readOnly; }
    /// The text's language (its colouring and line comment); NoxScript until set.
    void setLanguage(CodeLanguage language);
    CodeLanguage language() const noexcept { return language_; }

    /// Draws it into a child region of `size` (0: the rest) in the font `mono`.
    void render(const char* id, ImVec2 size, ImFont* mono);
    /// Whether it has the keyboard (focused last frame).
    bool focused() const { return focused_; }
    void focus() { wantFocus_ = true; }

    /// The caret to 1-based `line` (and column), scrolled into view, with that line's word or
    /// nothing selected.
    void gotoLine(int line, int col = 1);
    /// Typed at the caret (over the selection).
    void insertText(std::string_view s);
    TextPos caret() const { return caret_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    std::string_view line(int i) const { return lines_[static_cast<std::size_t>(i)]; }
    /// The identifier under the caret ("" for none).
    std::string wordAtCaret() const;
    std::string selectedText() const;
    /// A position as a byte offset into text() (lines joined by '\n'), and back.
    std::size_t offsetOf(TextPos p) const;
    TextPos posOf(std::size_t offset) const;
    /// Replaces the text between two byte offsets of text() with `s`, the caret after it (an
    /// edit, as typing is: takeChanged says so). Nothing when read-only.
    void replaceRange(std::size_t from, std::size_t to, std::string_view s);
    /// The caret's place on the screen at the last render: its line's bottom left, and the line's
    /// height (a drop-down at the caret goes under it, or over it near the window's bottom).
    ImVec2 caretScreenPos() const { return caretScreen_; }
    float lineHeight() const { return lineH_; }
    /// Whether the caret moved (keys, mouse, edits) since the last call.
    bool takeCaretMoved();

    /// The next (or previous) match of `needle` from the caret, selected; false if none (it
    /// wraps around).
    bool find(std::string_view needle, bool forward, bool matchCase);
    /// Every match of this text drawn highlighted ("" for none).
    void setHighlight(std::string needle, bool matchCase);
    /// How many times the highlighted text occurs.
    int highlightCount() const;

private:
    using Span = TokSpan;

    /// `line`'s runs; `state`: what it starts inside, then what the next one does (lexLine).
    std::vector<Span> tokenize(int line, std::uint8_t& state) const;
    void restate();
    void edited();
    bool hasSelection() const { return anchor_ != caret_; }
    std::pair<TextPos, TextPos> selection() const;
    void deleteSelection();
    void insertRaw(std::string_view s);
    void newline();
    /// Luau: the caret's line, just typed to a word that closes a block, back a step.
    void dedentClosing();
    void backspace(bool word);
    void deleteForward(bool word);
    void moveCaret(TextPos to, bool select);
    TextPos clamp(TextPos p) const;
    int prevCol(int line, int col) const;
    int nextCol(int line, int col) const;
    TextPos wordLeft(TextPos p) const;
    TextPos wordRight(TextPos p) const;
    std::pair<int, int> wordAt(int line, int col) const;
    int visualCol(int line, int col) const;
    int colAtVisual(int line, int visual) const;
    void indentLines(bool in);
    void toggleComment();
    void keyboard();
    void copy(bool cut);

    std::vector<std::string> lines_{std::string()};
    std::vector<std::uint8_t> commentAtStart_; ///< what each line starts inside (a /* */ comment; Luau: a long comment or string)
    CodeLanguage language_ = CodeLanguage::NoxScript;
    TextPos caret_;
    TextPos anchor_;
    int wantVisual_ = -1; ///< the column up / down keep to
    bool changed_ = false;
    bool readOnly_ = false;
    bool focused_ = false;
    bool wantFocus_ = false;
    bool ensureVisible_ = false;
    bool dragging_ = false;
    /// Hover help's delay: when the mouse last moved over the text (IsWindowHovered takes no
    /// delay flags; ImGui reports misuse at run time).
    double hoverStillSince_ = 0;
    std::vector<Marker> markers_;
    std::string highlight_;
    bool highlightCase_ = false;
    std::uint64_t blinkStart_ = 0;
    float charW_ = 7;
    float lineH_ = 15;
    ImVec2 caretScreen_{0, 0};
    TextPos lastCaret_{-1, -1}; ///< (takeCaretMoved)
    int longest_ = 0; ///< the widest line's visual columns
};

} // namespace noxsdl::editor
