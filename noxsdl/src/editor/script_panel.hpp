// The map editor's scripting (the Script menu): the Script panel (the map's NoxScript source with
// colouring, line numbers, find, diagnostics in the text and in the Problems panel, go to
// definition), the Script Reference panel (the builtins with their prototypes and tooltips, click
// to insert, a context menu with Function Info, Insert and a priority, sorted and filtered by
// priority; the script's functions and globals), the Function Info panel (a builtin's long
// description: noxcore script/builtin_docs.hpp), the play-test (the map hosted here in its real
// game mode, the sandbox's "players can't die" unless asked), hot reload into it (state kept:
// noxcore server/script_reload.hpp) or a restart of its scripts or its map, a watch on the .ns file
// for outside editors, and the Test* runner. The builtins' string arguments that take names from a
// known set (noxcore script/arg_values.hpp) get an autocomplete drop-down in the source (Ctrl+Space
// too), a Values section in Function Info and the lint.
//
// The source is the document's (MapDocument::scriptSource, edited by SetScriptSource ops, so it
// undoes with everything else); it and its debug info are saved in the .nxe sidecar, never the
// .map, whose script section is written from a compile (an op) on Compile, Save and Play.
//
// Object variables (NoX Core-only: the original game crashes on them) compile only with Editor
// Settings' "Allow NoX Core-only script features" on. Off, a map whose script already has them
// shows its source read-only, says why and offers to turn the setting on; its script is kept.
#pragma once

#include "editor/builtin_list.hpp"
#include "editor/code_editor.hpp"
#include "editor/play_test.hpp"

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/script/arg_values.hpp"
#include "noxcore/script/compiler.hpp"
#include "noxcore/server/script_reload.hpp"
#include "noxcore/server/script_tests.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class MapEditor;

class ScriptPanel {
public:
    explicit ScriptPanel(MapEditor& editor);
    ~ScriptPanel();
    ScriptPanel(const ScriptPanel&) = delete;
    ScriptPanel& operator=(const ScriptPanel&) = delete;

    /// Its commands, into the editor's registry (before the user's key bindings are read).
    void registerCommands();
    /// A document opened or made: its source from the sidecar (or none yet); a play-test ends.
    void documentOpened();
    /// Before a save: the typing committed, the source compiled into the map (an op) when it
    /// compiles, the source and debug info into the sidecar.
    void beforeSave(nox::mapedit::Sidecar& side);
    /// The typing committed as a SetScriptSource op (before an undo or a save).
    void commit();
    /// Every frame, without ImGui (also while the play-test has the window): the idle commit and
    /// compile, the .ns file watch.
    void tick();
    /// The Script and Script Reference panels.
    void draw();
    /// The Script and Play menus (the toolbar's Play / Pause / Stop: play_view.hpp).
    void menus();
    /// Its rows at the Problems panel's end: the script's diagnostics, the last reload's notes.
    void problems();
    /// A function picker (the compiled script's functions, or a typed name): true when changed.
    /// For the property panels (polygons, generators...).
    bool functionPicker(const char* label, std::string& function);
    /// The script editor has the keyboard (the map editor's shortcuts wait).
    bool hasKeyboard() const;
    /// An --editor-run step of its own (tests): "script-goto:LINE", "script-type:TEXT" (\n a
    /// new line, \c a semicolon), "script-find:TEXT", "focus:PANEL", "script-noxcore:on|off"
    /// (the NoX Core-only features setting, for the run), "builtin-info:NAME" (Function Info on
    /// it), "builtin-menu:NAME" (its context menu opened in the Builtins list),
    /// "builtin-priority:NAME,LEVEL" (0 low ... 3 favourite), "builtin-sort:N",
    /// "builtin-favourites:on|off", "builtin-search:TEXT", "script-complete" (the autocomplete
    /// at the caret, as Ctrl+Space), "script-complete-down:N" (its selection N rows down),
    /// "values-search:TEXT" (Function Info's Values search), "builtin-values:NAME" (Function Info
    /// on it, scrolled to its Values), "script-hover-caret" (the mouse on the caret's character);
    /// "script-goto:LINE,COL" puts the caret at a column too. False for any other step.
    bool runStep(const std::string& step);
    /// Jump in at a world point: a play-test started with its player there, or a running one's
    /// player put there (the map's context menu "Play From Here", the play.here command).
    void playFrom(SDL_FPoint at);
    /// A mod's play-test on the editor's map (the Mod menu's Play Mod with the open map): the map
    /// written and hosted as Play does, the NoX Core mode `modeId` running it.
    void playMod(const std::string& modeId, const std::filesystem::path& modsDir = {});

private:
    enum class Reload { Hot, RestartScripts, RestartMap };

    void syncFromDocument();
    void ensureSource();
    void compileNow();
    bool compileIntoMap();
    void saveAndReload();
    void reload(Reload mode);
    /// The test started (or, running, back to it), its player put at `at` once in (jump in).
    void startPlayTest(std::optional<SDL_FPoint> at = std::nullopt);
    /// The point Play From Here uses: under the mouse on the map (or on the in-panel game), else
    /// the view's centre; none when the in-panel game has no point under the mouse.
    std::optional<SDL_FPoint> cursorPoint() const;
    /// The class the test's player plays (-1: the player's profile's).
    int playClass() const;
    void stopPlayTest();
    void runTests();
    void gotoDefinition(std::string_view word);
    void showLine(int line, int col = 1);
    void exportNs();
    void exportObj();
    void importObj(const std::filesystem::path& path);
    void setWatch(bool on);
    void pollWatch();
    std::filesystem::path watchPath() const;
    std::filesystem::path playTestDir() const;
    std::vector<std::string> functionNames() const;
    bool onShortcut(int chord);
    void referencePanel();
    /// The Builtins tab: its sort, filter and rows.
    void builtinsTab(const std::string& query);
    /// A builtin's tooltip: its prototype, its short text, what it's used for.
    void builtinTooltip(std::uint32_t id) const;
    /// A builtin's context menu's items (the Builtins list's and the source's).
    void builtinMenuItems(std::uint32_t id);
    /// The Function Info panel.
    void functionInfoPanel();
    /// Function Info's Values section: each of the builtin's parameters that takes values from a
    /// set (script/arg_values.hpp), its values searchable, Insert at Cursor and Copy.
    void valuesSection(std::uint32_t id);

    // ---- The builtins' argument values ---------------------------------------
    /// The game's sets (made once from thing.bin, nox.csf...): the lint's and the lists'.
    const nox::script::ArgValueSets& gameArgValues();
    /// The game's sets with the open map's names (made again when the document changes).
    const nox::script::ArgValueSets& argValues();
    /// A value's icon (a thing type's thumbnail) into `dl` at `at`, `size` square; false when it has
    /// none (or it's still being drawn).
    bool valueIcon(ImDrawList* dl, const nox::script::ArgValue& v, const ImVec2& at, float size);
    /// The autocomplete opened at the caret: inside a parameter's string literal, or (`explicitly`:
    /// Ctrl+Space) at an argument with nothing written yet.
    void openCompletion(bool explicitly);
    /// After a key or a caret move: the drop-down kept (filtered again), opened (`typedNow`, the
    /// caret inside such a literal) or closed (the caret left it).
    void updateCompletion(bool typedNow);
    /// Up / Down / Page keys, Enter or Tab, Escape while the drop-down is open: true if taken.
    bool completionKey(int key);
    /// The drop-down's row put into the source (the literal's text replaced).
    void acceptCompletion(std::size_t row);
    /// The drop-down, under the caret (inside the Script window, after the source).
    void drawCompletion();
    /// A value typed at the caret for Function Info's Insert at Cursor: into the literal the caret
    /// is in when it takes this set, else with its quotes.
    void insertValue(nox::script::ValueSetId set, const std::string& value);

    std::optional<nox::script::ArgValueSets> gameValues_;
    nox::script::ArgValueSets mapValues_;
    std::optional<std::uint64_t> mapValuesKey_; ///< the document state the map's sets were made for
    struct Completion {
        bool open = false;
        bool explicitly = false;            ///< opened by Ctrl+Space (kept at an empty argument)
        nox::script::ArgSite site;
        std::vector<std::size_t> rows;      ///< indices into the set's values
        int selected = 0;
        bool scrollTo = false;
        bool hovered = false;               ///< the mouse was on it last frame (a click on it keeps it)
        std::optional<std::size_t> dismissed; ///< Escape: the literal's start, not reopened while the caret stays in it
    };
    Completion complete_;
    bool codeWasFocused_ = false; ///< the source had the keyboard last frame (losing it closes the drop-down)
    // Function Info's Values.
    char valuesSearch_[64] = {};
    std::string valuesSelected_;  ///< the value picked in the list (Insert at Cursor / Copy)
    nox::script::ValueSetId valuesSelectedSet_ = nox::script::ValueSetId::Effect;
    std::uint32_t valuesFor_ = 0; ///< the builtin they're for (a new one clears them)
    int infoScrollValues_ = 0;    ///< frames Function Info scrolls to its Values (its "Values" button)
    /// Function Info on a builtin (`back`: the one shown before is kept for Back).
    void openFunctionInfo(std::uint32_t id, bool back = false);
    /// The name and its parentheses at the caret (the caret between them).
    void insertBuiltin(std::uint32_t id);
    /// Text at the caret, as typed (false: the source is read-only).
    bool insertAtCaret(const std::string& text);
    editor::BuiltinPriority builtinPriority(std::uint32_t id) const;
    void setBuiltinPriority(std::uint32_t id, editor::BuiltinPriority p);
    /// The Builtins list's rows (made once from the docs).
    const std::vector<editor::BuiltinRow>& builtinRows();
    void findBar();
    std::uint32_t defaultMode() const;
    /// The editor's setting for NoX Core-only script features (object variables).
    bool allowNoxCore() const;
    /// Why the source is read-only (me::scriptReadOnlyReason; cached by the document's revision
    /// and the setting), or null when it's editable.
    const std::string* readOnlyReason();
    /// The bar under the buttons when object variables are refused: why, and a button that turns
    /// the setting on.
    void noxCoreBanner(const std::string* readOnly);
    /// A key or an insert in the source: typing from now on (in a session, the script's lock taken
    /// first; another editor holding it, the text is put back).
    void typed();
    /// In a session, who else holds the script (one editor at a time); nullopt when it's free or ours.
    std::optional<std::string> scriptHolder() const;
    /// The session's script lock kept with the typing: lost, the unsent typing dropped; let go a
    /// few seconds after a pause.
    void keepScriptLock(std::uint64_t now);

    MapEditor& ed_;
    editor::CodeEditor code_;
    bool show_ = true;
    bool showReference_ = true;
    int focusWindow_ = 0; ///< frames the Script panel is brought to the front
    std::string focusName_; ///< a panel a test step brings to the front
    // The text against the document.
    std::optional<std::string> synced_; ///< the document's source the editor shows
    bool typing_ = false;               ///< edited since the last commit
    std::uint64_t typedMs_ = 0;
    bool decompileTried_ = false;
    std::vector<std::string> decompileNotes_;
    // The last compile.
    nox::script::CompileResult last_;
    std::optional<nox::script::DebugInfo> good_; ///< the last good compile's names (navigation)
    std::string compiledText_;
    bool compiledAllow_ = false; ///< the setting the last compile had
    bool compiled_ = false;
    // The read-only state (readOnlyReason).
    std::optional<std::string> readOnly_;
    std::optional<std::uint64_t> readOnlyRev_; ///< the history revision it was worked out at
    bool readOnlyAllow_ = false;
    std::uint64_t compileDueMs_ = 0;
    // Find.
    bool findOpen_ = false, findFocus_ = false, findCase_ = false;
    char findBuf_[128] = {};
    bool gotoOpen_ = false;
    char gotoBuf_[16] = {};
    // Reference panel.
    char refSearch_[64] = {};
    std::vector<editor::BuiltinRow> builtinRows_;
    std::optional<std::uint32_t> menuBuiltin_;     ///< a test step's context menu to open (builtin-menu:)
    std::optional<std::uint32_t> codeMenuBuiltin_; ///< the source's right-click on a builtin, its menu to open
    std::uint32_t codeMenuShown_ = 0;              ///< the builtin the source's open menu is for
    // Function Info.
    bool showInfo_ = false;
    bool infoFocus_ = false;        ///< brought to the front next frame
    std::uint32_t infoId_ = 0;
    std::vector<std::uint32_t> infoBack_; ///< the builtins shown before (Back)
    char newFnBuf_[64] = {};  ///< the function picker's "New function..." name
    bool newFnOpen_ = false;
    /// The source's size against the panels' (Ctrl+wheel; EditorSettings::scriptFontScale, saved
    /// a moment after the last change so a wheel's steps write the settings once).
    void setFontScale(float scale);
    std::uint64_t fontSaveDueMs_ = 0;
    std::filesystem::path lastObj_; ///< the last .obj imported (re-import and save)
    // Play-test and reload.
    std::uint32_t playMode_ = 0;   ///< 0: the map's default
    std::string playModeId_;       ///< (playMod's: the next test's NoX Core mode, once)
    std::filesystem::path playModsDir_; ///< (playMod's: where that mode is read from; empty: mods\)
    int playClass_ = -1;           ///< the Play menu's class (-1: the map's by its name, else the player's)
    bool playersCanDie_ = false;
    bool playing_ = false;         ///< a test this editor started (the hooks say if it's still up)
    nox::script::DebugInfo running_; ///< the names of the script the test runs
    std::vector<nox::server::dev::ReloadNote> reloadNotes_;
    std::string reloadSummary_;
    std::optional<nox::server::ScriptTestRun> tests_;
    // The .ns watch.
    bool watching_ = false;
    std::filesystem::path watched_;
    std::filesystem::file_time_type watchedTime_{};
    std::uintmax_t watchedSize_ = 0;
    std::uint64_t watchSeenMs_ = 0;   ///< when a change was first seen (the 200 ms settle)
    std::uint64_t watchPollMs_ = 0;
    // A file picked in the system's dialog (its callback may come from another thread).
    std::mutex pickedMutex_;
    std::optional<std::string> picked_;
};

} // namespace noxsdl
