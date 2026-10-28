// The map editor's scripting: the Script and Script Reference panels, compile, play-test, hot
// reload, the .ns watch and the tests (script_panel.hpp).
#include "editor/script_panel.hpp"

#include "editor/editor_text.hpp"
#include "editor/map_editor.hpp"
#include "editor/object_tools.hpp"
#include "editor/play_view.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/script_doc.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/script/builtin_docs.hpp"
#include "noxcore/script/builtins.hpp"
#include "noxcore/script/decompiler.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (BringWindowToDisplayFront: the autocomplete over the Script window)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <utility>

namespace fs = std::filesystem;
namespace me = nox::mapedit;
namespace sc = nox::script;

namespace noxsdl {

namespace {

using editor::pathText;
using nox::util::lowered;

using editor::help;

/// A builtin's prototype, as the reference lists it ("int Object(string)").
std::string prototype(std::uint32_t id)
{
    const auto& sig = sc::builtinSignature(id);
    std::string s = std::string(sc::typeName(sig.returns)) + " " + std::string(sig.name) + "(";
    for (std::size_t i = 0; i < sig.args.size(); ++i) {
        s += (i ? ", " : "") + std::string(sc::typeName(sig.args[i]));
    }
    return s + ")";
}

/// A builtin's prototype with its parameters' names when it's documented
/// ("int SecondTimer(int seconds, function callback)").
std::string namedPrototype(std::uint32_t id)
{
    const sc::BuiltinDoc* d = sc::builtinDoc(id);
    return d && !d->prototype.empty() ? d->prototype : prototype(id);
}

/// A five-pointed star (a favourite's mark), drawn so it needs no glyph in the font.
void drawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled)
{
    constexpr float kPi = 3.14159265f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -kPi / 2 + static_cast<float>(i) * kPi / 5;
        const float rr = i % 2 == 0 ? r : r * 0.42f;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    if (filled) {
        dl->AddConcavePolyFilled(pts, 10, col);
    } else {
        dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
    }
}

/// The favourites' star (the theme's: gold, or orange on Dark, deeper on Light).
ImU32 starColour()
{
    return ImGui::ColorConvertFloat4ToU32(editor::themeColours().star);
}

/// A star the size of a line of text at the cursor, as a button: true when clicked.
bool starButton(const char* id, bool on, const char* tip)
{
    const float h = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(h, h));
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImU32 col = on ? starColour() : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    drawStar(ImGui::GetWindowDrawList(), ImVec2(a.x + h * 0.5f, a.y + h * 0.5f), h * 0.36f, col, on);
    help(tip);
    return clicked;
}

/// Text of the docs' kind: paragraphs, "- " lines as bullets, wrapped to the window.
void docText(const std::string& text)
{
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, nl - at);
        if (line.empty()) {
            ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35f));
        } else if (line.starts_with("- ")) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line.c_str() + 2);
        } else {
            ImGui::TextWrapped("%s", line.c_str());
        }
        at = nl + 1;
    }
}

/// A heading in Function Info.
void docHeading(const char* text)
{
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

/// The play-test's game modes: HostSettings::mode for a network game (solo and quest keyed by the
/// game flags they set, 0x800 and 0x1000), with the MapInfo game-type bit that offers it, in the
/// order the map's own is picked: the network modes first (as before solo and quest could be
/// played), then solo, then quest.
struct Mode {
    std::uint32_t host;
    std::uint32_t mapBit;
    const char* name;
    editor::PlayKind kind = editor::PlayKind::Network;
};
constexpr std::uint32_t kSoloMode = 0x800;
constexpr std::uint32_t kQuestMode = 0x1000;
constexpr Mode kModes[] = {{0x100, 4, "Arena"},
                           {0x20, 8, "Capture the Flag"},
                           {0x10, 0x10, "King of the Realm"},
                           {0x40, 0x40, "Flag Ball"},
                           {0x400, 0x20, "Elimination"},
                           {kSoloMode, 1, "Solo", editor::PlayKind::Solo},
                           {kQuestMode, 2, "Quest", editor::PlayKind::Quest}};

const Mode& modeOf(std::uint32_t host)
{
    for (const Mode& m : kModes) {
        if (m.host == host) return m;
    }
    return kModes[0];
}

const char* modeName(std::uint32_t host)
{
    return modeOf(host).name;
}

constexpr const char* kClassNames[] = {"Warrior", "Wizard", "Conjurer"};

/// A solo map's class by its name, as the campaign names them (War / Wiz / Con + a digit); -1 for
/// any other.
int classOfMapName(const std::string& name)
{
    if (name.size() < 4 || !std::isdigit(static_cast<unsigned char>(name[3]))) {
        return -1;
    }
    const std::string p = lowered(name.substr(0, 3));
    return p == "war" ? 0 : p == "wiz" ? 1 : p == "con" ? 2 : -1;
}

} // namespace

ScriptPanel::ScriptPanel(MapEditor& editor)
    : ed_(editor)
{
    code_.classify = [this](std::string_view w) {
        if (sc::findBuiltin(w)) return editor::CodeEditor::Tok::Builtin;
        if (good_ && good_->function(w)) return editor::CodeEditor::Tok::Function;
        return editor::CodeEditor::Tok::Plain;
    };
    code_.shortcut = [this](int chord) { return onShortcut(chord); };
    code_.gotoDefinition = [this](std::string_view w) { gotoDefinition(w); };
    code_.hoverText = [this](std::string_view w) -> std::string {
        if (const auto id = sc::findBuiltin(w)) {
            const sc::BuiltinDoc* d = sc::builtinDoc(*id);
            return namedPrototype(*id) + "\n\n" + (d ? d->shortText + "\n\n" : std::string()) + "(builtin " + std::to_string(*id) +
                   "; right-click it for Function Info)";
        }
        if (good_) {
            if (const auto* f = good_->function(w)) {
                return std::string(sc::typeName(f->returns)) + " " + f->name + "  line " + std::to_string(f->loc.line) +
                       " (F12 / Ctrl+click to go there)";
            }
            for (const auto& g : good_->globals) {
                if (g.name == w && g.symbol >= 4) {
                    return "global " + std::string(sc::typeName(g.type)) + (g.size > 1 ? "[" + std::to_string(g.size) + "]" : "") + " " + g.name +
                           "  line " + std::to_string(g.loc.line);
                }
            }
            for (const auto& v : good_->objectVars) {
                if (v.name == w && !v.name.empty()) {
                    return "object variable " + std::string(sc::typeName(v.type)) + (v.size > 1 ? "[" + std::to_string(v.size) + "]" : "") +
                           " object." + v.name + "  line " + std::to_string(v.loc.line) + " (each object's own; NoX Core only)";
                }
            }
        }
        return {};
    };
    code_.contextMenu = [this](std::string_view w) {
        if (const auto id = sc::findBuiltin(w)) {
            codeMenuBuiltin_ = *id; // (its menu opened after the source is drawn)
        }
    };
    code_.popupKey = [this](int key) { return completionKey(key); };
}

ScriptPanel::~ScriptPanel()
{
    if (playing_ && ed_.ctx_.playTest.running && ed_.ctx_.playTest.running() && ed_.ctx_.playTest.stop) {
        ed_.ctx_.playTest.stop();
    }
}

// --- Commands ----------------------------------------------------------------------------------

void ScriptPanel::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    const auto key = [](const char* text) { return Shortcut::parse(text).value_or(Shortcut{}); };
    const auto add = [&](const char* id, const char* label, const char* category, const char* shortcut, const char* helpText,
                         std::function<void()> run, std::function<bool()> enabled = {}, std::function<bool()> checked = {}) {
        ed_.commands_.add(Command{id, label, category, key(shortcut), helpText, std::move(run), std::move(enabled), std::move(checked)});
    };
    const auto playable = [this] { return ed_.ctx_.playTest.available(); };
    // (In a collaborative session the reloads go to the session's running game: the host's.)
    const auto testRunning = [this] { return (ed_.ctx_.playTest.running && ed_.ctx_.playTest.running()) || ed_.inSession(); };
    add("window.script", "Script", "Window", "", "Shows or hides the Script panel.", [this] { show_ = !show_; }, {},
        [this] { return show_; });
    add("script.focus", "Go to Script Panel", "Script", "Ctrl+Shift+E", "Show the Script panel and put the keyboard in its source.", [this] {
        show_ = true;
        focusWindow_ = 2;
        code_.focus();
    });
    add("window.scriptReference", "Script Reference", "Window", "", "Shows or hides the builtins, functions and globals.",
        [this] { showReference_ = !showReference_; }, {}, [this] { return showReference_; });
    add("window.functionInfo", "Function Info", "Window", "",
        "Shows or hides Function Info: a builtin's full description, parameters, edge cases and example (right-click a builtin).",
        [this] { showInfo_ = !showInfo_; }, {}, [this] { return showInfo_; });
    const auto editable = [this] { return readOnlyReason() == nullptr; };
    add("script.compile", "Compile Script", "Script", "Ctrl+B",
        "Compile the source into the map's script (an undoable change); errors and warnings go to the Problems panel.",
        [this] {
            commit();
            if (compileIntoMap()) {
                ed_.status("Script compiled" + std::string(last_.diagnostics.empty() ? "" : " (" + std::to_string(last_.diagnostics.size()) + " warnings)"));
            }
        }, editable);
    add("script.reload", "Hot Reload", "Script", "Ctrl+R",
        "Compile and swap the new script into the running play-test, keeping the globals' values, timers and dialogs (matched by name).",
        [this] { reload(Reload::Hot); }, [=] { return testRunning() && editable(); });
    add("script.restartScripts", "Restart Scripts", "Script", "Ctrl+Shift+R",
        "Compile, then run GLOBAL, MapInitialize and MapEntry again in the play-test (objects stay, timers are cleared).",
        [this] { reload(Reload::RestartScripts); }, [=] { return testRunning() && editable(); });
    add("script.restartMap", "Restart Map", "Script", "", "Restart the play-test on a fresh load of the edited map.",
        [this] { reload(Reload::RestartMap); }, testRunning);
    add("script.runTests", "Run Script Tests", "Script", "Ctrl+T",
        "Run the script's Test* functions headless in the server, each on a fresh load of the map; asserts that fail are listed in Problems.",
        [this] { runTests(); });
    add("script.decompile", "Decompile Map Script", "Script", "",
        "Replace the source with the map's compiled script decompiled (undoable).", [this] {
            const auto d = me::decompileDocumentScript(ed_.doc_, good_ ? &*good_ : nullptr);
            if (d.source.empty()) {
                ed_.status("The map has no compiled script to decompile");
                return;
            }
            commit();
            ed_.perform(me::Op{"Decompile script", {me::SetScriptSource{d.source}}});
            decompileNotes_ = d.notes;
        });
    add("script.exportNs", "Export .ns", "Script", "", "Write the source beside the map as <name>.ns (the file the watch follows).",
        [this] { exportNs(); });
    add("script.importObj", "Import nsc .obj...", "Script", "", "Replace the map's script with a compiled .obj (nsc's, or the game's nc.obj) and decompile it.",
        [this] {
            static std::pair<std::mutex*, std::optional<std::string>*> target;
            target = {&pickedMutex_, &picked_};
            static const SDL_DialogFileFilter filters[] = {{"Compiled NoxScript", "obj"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(editor::storePickedFile, &target, ed_.ctx_.window, filters, 2, nullptr, false);
        });
    add("script.reimportSave", "Re-import .obj and Save", "Script", "",
        "Import the last imported .obj again (after compiling it outside) and save the map (the old editor's F10).",
        [this] {
            if (lastObj_.empty()) {
                ed_.status("No .obj imported yet");
                return;
            }
            importObj(lastObj_);
            ed_.save();
        }, [this] { return !lastObj_.empty(); });
    add("script.exportObj", "Export .obj", "Script", "",
        "Compile and write the map's script as <name>.obj beside the map (the ScriptObject's bytes, nsc's format).", [this] { exportObj(); });
    add("script.fontBigger", "Script Font Bigger", "Script", "Ctrl+=", "A bigger font in the Script panel (also Ctrl+wheel there).",
        [this] { setFontScale(ed_.settings_.scriptFontScale * 1.1f); });
    add("script.fontSmaller", "Script Font Smaller", "Script", "Ctrl+-", "A smaller font in the Script panel (also Ctrl+wheel there).",
        [this] { setFontScale(ed_.settings_.scriptFontScale / 1.1f); });
    add("script.fontReset", "Script Font Normal Size", "Script", "Ctrl+0", "The Script panel's font back to the panels' size.",
        [this] { setFontScale(1.0f); });
    add("script.watch", "Watch .ns File", "Script", "",
        "Follow the .ns file beside the map: saving it in an outside editor updates the source and hot-reloads a running play-test.",
        [this] { setWatch(!watching_); }, {}, [this] { return watching_; });
    add("play.start", "Play-Test", "Play", "F5",
        "Host the edited map here in its game mode and play it in the Map panel (click it to play, Shift+F1 frees the mouse), or "
        "over the whole window with Play in Full Window (F2 in the game comes back). While it runs: the keyboard and mouse back "
        "to the game in the Map panel, or back to the game over the whole window.",
        [this] { startPlayTest(); }, playable);
    add("play.here", "Play From Here", "Play", "Ctrl+F5",
        "Jump in at the cursor: the play-test starts with the player at the map point under the mouse (the view's centre when "
        "the mouse isn't on the map). While a test runs, its player is put at the point under the mouse, on the editor's map "
        "or on the game in the Map panel.",
        [this] {
            if (const auto at = cursorPoint()) {
                playFrom(*at);
            }
        },
        playable);
    add("play.stop", "Stop Play-Test", "Play", "Shift+F5", "End the play-test.", [this] { stopPlayTest(); }, testRunning);
    add("play.playersCanDie", "Players Can Die", "Play", "",
        "For testing traps: players die as in the map's mode. Off (the sandbox's rule): their health stops at 1.",
        [this] { playersCanDie_ = !playersCanDie_; }, {}, [this] { return playersCanDie_; });
}

void ScriptPanel::setFontScale(float scale)
{
    scale = std::clamp(scale, 0.5f, 3.0f);
    if (std::abs(scale - 1.0f) < 0.02f) {
        scale = 1.0f; // (steps of 1.1 back and forth come home exactly)
    }
    if (scale == ed_.settings_.scriptFontScale) {
        return;
    }
    ed_.settings_.scriptFontScale = scale;
    fontSaveDueMs_ = SDL_GetTicks() + 1000;
}

bool ScriptPanel::onShortcut(int chord)
{
    const int key = chord & ~ImGuiMod_Mask_;
    const bool ctrl = (chord & ImGuiMod_Ctrl) != 0;
    const bool shift = (chord & ImGuiMod_Shift) != 0;
    const bool alt = (chord & ImGuiMod_Alt) != 0;
    if (ctrl && !alt) {
        switch (key) {
        case ImGuiKey_S:
            if (!shift) {
                saveAndReload();
                return true;
            }
            break;
        case ImGuiKey_Z:
            commit();
            shift ? ed_.redo() : ed_.undo();
            return true;
        case ImGuiKey_Y:
            commit();
            ed_.redo();
            return true;
        case ImGuiKey_F:
            findOpen_ = true;
            findFocus_ = true;
            if (const std::string sel = code_.selectedText(); !sel.empty() && sel.find('\n') == std::string::npos) {
                std::snprintf(findBuf_, sizeof findBuf_, "%s", sel.c_str());
            }
            return true;
        case ImGuiKey_G:
            gotoOpen_ = true;
            return true;
        case ImGuiKey_Space:
            if (!shift) {
                openCompletion(true); // (the values a builtin's argument takes, at the caret)
                return true;
            }
            break;
        // The text's own chords stay the editor's.
        case ImGuiKey_A: case ImGuiKey_C: case ImGuiKey_V: case ImGuiKey_X: case ImGuiKey_Slash: case ImGuiKey_Insert:
        case ImGuiKey_Backspace: case ImGuiKey_Delete: case ImGuiKey_LeftArrow: case ImGuiKey_RightArrow: case ImGuiKey_UpArrow:
        case ImGuiKey_DownArrow: case ImGuiKey_Home: case ImGuiKey_End: case ImGuiKey_PageUp: case ImGuiKey_PageDown:
            return false;
        default:
            break;
        }
    }
    if (!ctrl && !alt && key == ImGuiKey_F3) {
        code_.find(findBuf_, !shift, findCase_);
        return true;
    }
    if (!ctrl && !alt && !shift && key == ImGuiKey_F12) {
        return false; // (go to definition: the editor's)
    }
    // Else the registry's command on that key, for the categories that make sense while typing.
    editor::Shortcut s;
    s.ctrl = ctrl;
    s.shift = shift;
    s.alt = alt;
    s.key = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
    if (const editor::Command* c = ed_.commands_.match(s)) {
        const std::string& cat = c->category;
        if (cat == "File" || cat == "Window" || cat == "Help" || cat == "Script" || cat == "Play" || c->id == "view.palette") {
            ed_.commands_.run(c->id);
            return true;
        }
    }
    return false;
}

bool ScriptPanel::hasKeyboard() const
{
    return show_ && code_.focused();
}

bool ScriptPanel::runStep(const std::string& step)
{
    if (step.starts_with("script-goto:")) {
        // "script-goto:LINE" or "script-goto:LINE,COL" (1-based).
        const auto comma = step.find(',', 12);
        showLine(std::max(1, std::atoi(step.c_str() + 12)), comma == std::string::npos ? 1 : std::max(1, std::atoi(step.c_str() + comma + 1)));
        return true;
    }
    if (step == "script-hover-caret") {
        // (tests) The mouse put on the caret's character (a diagnostic's hover text shows there).
        // (As the "mouse:" step, at the caret's place from the last frame.)
        const ImVec2 o = ed_.windowOrigin();
        const ImVec2 at = code_.caretScreenPos();
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(ed_.ctx_.window);
        e.motion.x = at.x - o.x + 3;
        e.motion.y = at.y - o.y - code_.lineHeight() * 0.5f;
        ed_.testMouseX_ = e.motion.x;
        ed_.testMouseY_ = e.motion.y;
        SDL_PushEvent(&e);
        return true;
    }
    if (step.starts_with("builtin-values:")) {
        // (tests) Function Info on a builtin, scrolled to its Values.
        if (const auto bid = sc::findBuiltin(step.substr(15))) {
            openFunctionInfo(*bid);
            infoScrollValues_ = 3;
        }
        return true;
    }
    if (step.starts_with("script-type:")) {
        ensureSource();
        std::string text;
        for (std::size_t i = 12; i < step.size(); ++i) {
            if (step[i] == '\\' && i + 1 < step.size() && (step[i + 1] == 'n' || step[i + 1] == 'c')) {
                text += step[i + 1] == 'n' ? '\n' : ';'; // (\c: a semicolon, which would end the step)
                ++i;
            } else {
                text += step[i];
            }
        }
        code_.insertText(text);
        typed();
        return true;
    }
    if (step.starts_with("focus:")) {
        focusName_ = step.substr(6); // (any panel by its title, brought to the front in the next frame)
        return true;
    }
    if (step == "script-noxcore:on" || step == "script-noxcore:off") {
        ed_.settings_.allowNoxCoreScript = step.ends_with("on"); // (a test's run never saves its settings)
        return true;
    }
    if (step == "play-here") {
        ed_.commands_.run("play.here"); // (tests: as the key, the mouse where it is)
        return true;
    }
    if (step.starts_with("play-here:")) {
        // (tests) Play From Here at a world point: "play-here:x,y".
        float x = 0, y = 0;
        if (std::sscanf(step.c_str() + 10, "%f,%f", &x, &y) == 2) {
            playFrom(SDL_FPoint{x, y});
        }
        return true;
    }
    if (step.starts_with("play-mode:")) {
        // (tests) The Play menu's Game Mode by name ("solo", "quest", "arena"...; "map" the map's own).
        const std::string want = lowered(step.substr(10));
        playMode_ = 0;
        for (const Mode& m : kModes) {
            if (lowered(m.name) == want) playMode_ = m.host;
        }
        return true;
    }
    if (step.starts_with("play-class:")) {
        playClass_ = std::clamp(std::atoi(step.c_str() + 11), -1, 2); // (tests: 0 warrior, 1 wizard, 2 conjurer, -1 the map's)
        return true;
    }
    if (step.starts_with("builtin-info:") || step.starts_with("builtin-menu:")) {
        // (tests) Function Info on a builtin, or its context menu in the Builtins list.
        if (const auto id = sc::findBuiltin(step.substr(13))) {
            if (step[8] == 'i') {
                openFunctionInfo(*id);
            } else {
                showReference_ = true;
                menuBuiltin_ = *id;
            }
        } else {
            ed_.status("No builtin " + step.substr(13));
        }
        return true;
    }
    if (step.starts_with("builtin-priority:")) {
        // (tests) "builtin-priority:NAME,LEVEL" (0 low, 1 normal, 2 high, 3 favourite).
        const std::string rest = step.substr(17);
        const auto comma = rest.rfind(',');
        if (const auto id = comma == std::string::npos ? std::nullopt : sc::findBuiltin(rest.substr(0, comma))) {
            setBuiltinPriority(*id, static_cast<editor::BuiltinPriority>(std::clamp(std::atoi(rest.c_str() + comma + 1), 0, 3)));
        }
        return true;
    }
    if (step.starts_with("builtin-sort:")) {
        ed_.settings_.builtinSort = std::clamp(std::atoi(step.c_str() + 13), 0, editor::kBuiltinSortCount - 1); // (tests: BuiltinSort)
        return true;
    }
    if (step == "builtin-favourites:on" || step == "builtin-favourites:off") {
        ed_.settings_.builtinFavouritesOnly = step.ends_with("on");
        return true;
    }
    if (step.starts_with("builtin-search:")) {
        std::snprintf(refSearch_, sizeof refSearch_, "%s", step.c_str() + 15); // (tests: Script Reference's search box)
        return true;
    }
    if (step == "script-complete") {
        show_ = true;
        code_.focus();
        openCompletion(true); // (tests: as Ctrl+Space)
        return true;
    }
    if (step.starts_with("script-complete-down:")) {
        // (tests) The drop-down's selection moved down N rows, as the Down key.
        for (int n = std::max(0, std::atoi(step.c_str() + 21)); n > 0; --n) {
            completionKey(ImGuiKey_DownArrow);
        }
        return true;
    }
    if (step.starts_with("values-search:")) {
        std::snprintf(valuesSearch_, sizeof valuesSearch_, "%s", step.c_str() + 14); // (tests: Function Info's Values search)
        valuesFor_ = infoId_;
        return true;
    }
    if (step.starts_with("script-find:")) {
        show_ = true;
        findOpen_ = true;
        std::snprintf(findBuf_, sizeof findBuf_, "%s", step.c_str() + 12);
        code_.find(findBuf_, true, findCase_);
        return true;
    }
    return false;
}

// --- The text and the document ---------------------------------------------------------------------

void ScriptPanel::documentOpened()
{
    if (playing_) {
        stopPlayTest();
    }
    if (const auto src = me::sidecarScriptSource(ed_.sidecar_); src && !ed_.doc_.scriptSource) {
        ed_.doc_.scriptSource = *src; // (the saved source: not a change)
    }
    good_ = me::sidecarScriptDebug(ed_.sidecar_);
    readOnlyRev_.reset();
    synced_.reset();
    typing_ = false;
    decompileTried_ = false;
    decompileNotes_.clear();
    last_ = {};
    compiled_ = false;
    compiledText_.clear();
    reloadNotes_.clear();
    reloadSummary_.clear();
    tests_.reset();
    code_.setText(ed_.doc_.scriptSource.value_or(std::string()), false);
    code_.setMarkers({});
    synced_ = ed_.doc_.scriptSource;
    compileDueMs_ = SDL_GetTicks();
    if (watching_) {
        setWatch(false);
        setWatch(true);
    }
}

void ScriptPanel::commit()
{
    if (!typing_) {
        return;
    }
    typing_ = false;
    std::string text = code_.text();
    if (synced_ && *synced_ == text) {
        return;
    }
    if (!synced_ && text.empty()) {
        return;
    }
    synced_ = text;
    ed_.perform(me::Op{"Edit script", {me::SetScriptSource{std::move(text)}}});
    if (watching_) {
        // The watched file follows the editor's text (its own write isn't a change from outside).
        std::ofstream(watched_, std::ios::binary | std::ios::trunc) << *synced_;
        std::error_code ec;
        watchedTime_ = fs::last_write_time(watched_, ec);
        watchedSize_ = fs::file_size(watched_, ec);
    }
}

void ScriptPanel::typed()
{
    typing_ = true;
    typedMs_ = SDL_GetTicks();
    compileDueMs_ = typedMs_ + 350;
    if (ed_.inSession() && !ed_.holdPanel(me::ElementKey::script(), true)) {
        // (Another editor got the script first: holdPanel said who. The text as the document has it.)
        typing_ = false;
        synced_ = ed_.doc_.scriptSource;
        code_.setText(synced_.value_or(std::string()));
    }
}

std::optional<std::string> ScriptPanel::scriptHolder() const
{
    if (!ed_.inSession()) {
        return std::nullopt;
    }
    const auto h = ed_.otherHolder(me::ElementKey::script());
    if (!h) {
        return std::nullopt;
    }
    return ed_.peerName(*h);
}

void ScriptPanel::keepScriptLock(std::uint64_t now)
{
    // A session's script, one editor at a time: the lock taken at the first key (typed), kept
    // while the typing goes on, let go 4 s after its last key once it's sent.
    constexpr std::uint64_t kLetGoMs = 4000;
    const me::ElementKey key = me::ElementKey::script();
    if (!ed_.inSession() || !ed_.panelLocks_.contains(key)) {
        if (ed_.inSession() && typing_) {
            // Lost (the host took it over, or it refused us): what wasn't sent yet goes.
            const auto who = scriptHolder();
            typing_ = false;
            synced_ = ed_.doc_.scriptSource;
            code_.setText(synced_.value_or(std::string()));
            ed_.status((who ? *who : std::string("Another editor")) + " has the script: your last unsent typing was dropped");
        }
        return;
    }
    if (!typing_ && now - typedMs_ > kLetGoMs) {
        ed_.holdPanel(key, false);
    }
}

void ScriptPanel::syncFromDocument()
{
    // An undo, a redo, a decompile or the watch changed the document's source: the editor shows it.
    if (typing_ || ed_.doc_.scriptSource == synced_) {
        return;
    }
    synced_ = ed_.doc_.scriptSource;
    code_.setText(synced_.value_or(std::string()));
    compileDueMs_ = SDL_GetTicks();
}

void ScriptPanel::ensureSource()
{
    // A stock map's script, decompiled on first open (named from the sidecar's debug info when
    // it has some). Not a change: the map's bytes are the same.
    if (ed_.doc_.scriptSource || decompileTried_) {
        return;
    }
    decompileTried_ = true;
    if (ed_.doc_.script.code.empty()) {
        return;
    }
    const auto d = me::decompileDocumentScript(ed_.doc_, good_ ? &*good_ : nullptr);
    decompileNotes_ = d.notes;
    if (d.source.empty()) {
        return;
    }
    if (!d.identical) {
        decompileNotes_.insert(decompileNotes_.begin(), "the decompiled source doesn't compile back to the map's exact bytes");
    }
    ed_.doc_.scriptSource = d.source;
    synced_ = d.source;
    code_.setText(d.source, false);
    compileDueMs_ = SDL_GetTicks();
    ed_.status("Decompiled the map's script" + std::string(d.identical ? " (it compiles back to the same bytes)" : ""));
}

void ScriptPanel::compileNow()
{
    const std::string text = typing_ ? code_.text() : ed_.doc_.scriptSource.value_or(std::string());
    const bool allow = allowNoxCore();
    if (compiled_ && text == compiledText_ && allow == compiledAllow_) {
        return;
    }
    compiledText_ = text;
    compiledAllow_ = allow;
    compiled_ = true;
    last_ = me::compileForDocument(ed_.doc_, text, allow, &gameArgValues()); // (the lint checks the builtins' values too)
    if (last_.ok()) {
        good_ = last_.debug;
    }
    std::vector<editor::CodeEditor::Marker> markers;
    for (const auto& d : last_.diagnostics) {
        if (d.loc.line > 0) {
            markers.push_back({static_cast<int>(d.loc.line), static_cast<int>(d.loc.column), d.severity == sc::Diagnostic::Severity::Error, d.message});
        }
    }
    code_.setMarkers(std::move(markers));
}

bool ScriptPanel::compileIntoMap()
{
    if (!ed_.doc_.scriptSource) {
        ed_.status("No script source to compile");
        return false;
    }
    if (readOnlyReason()) {
        // (The map's script stays as it is: its object variables are never stripped.)
        ed_.status("Not compiled: the script uses object variables (NoX Core-only), which Editor Settings doesn't allow");
        return false;
    }
    compiled_ = false;
    compileNow();
    if (!last_.ok()) {
        const auto errors = std::ranges::count_if(last_.diagnostics, [](const auto& d) { return d.severity == sc::Diagnostic::Severity::Error; });
        ed_.status("The script has " + std::to_string(errors) + (errors == 1 ? " error" : " errors") + ": not compiled (see Problems)");
        ed_.showProblems_ = true;
        return false;
    }
    const me::Op op = me::compiledScriptOp(ed_.doc_, *last_.file);
    if (!op.empty()) {
        ed_.perform(op);
    }
    return true;
}

void ScriptPanel::beforeSave(me::Sidecar& side)
{
    commit();
    ensureSource();
    const sc::DebugInfo* debug = nullptr;
    if (ed_.doc_.scriptSource && compileIntoMap()) {
        debug = &last_.debug;
    }
    me::storeScriptInSidecar(side, ed_.doc_.scriptSource, debug ? debug : (good_ ? &*good_ : nullptr));
    me::markNoxCoreNeeds(side, ed_.doc_); // (saveDocument writes it too; this keeps the editor's copy the same)
}

bool ScriptPanel::allowNoxCore() const
{
    return ed_.settings_.allowNoxCoreScript;
}

const std::string* ScriptPanel::readOnlyReason()
{
    const std::uint64_t rev = ed_.history_.revision();
    const bool allow = allowNoxCore();
    if (readOnlyRev_ != rev || readOnlyAllow_ != allow) {
        readOnlyRev_ = rev;
        readOnlyAllow_ = allow;
        readOnly_ = me::scriptReadOnlyReason(ed_.doc_, allow);
    }
    return readOnly_ ? &*readOnly_ : nullptr;
}

void ScriptPanel::noxCoreBanner(const std::string* readOnly)
{
    if (!readOnly && !(last_.objectVariablesRefused && !allowNoxCore())) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, editor::themeColours().unknown);
    ImGui::TextWrapped("%s", readOnly ? readOnly->c_str()
                                      : "The script uses object variables, a NoX Core-only feature: the original game crashes on them, "
                                        "and a map with them needs a NoX Core server. They are off in Editor Settings, so the compiler "
                                        "refuses them (see Problems).");
    ImGui::PopStyleColor();
    if (ImGui::Button("Allow NoX Core-only script features")) {
        ed_.settings_.allowNoxCoreScript = true;
        ed_.saveSettings();
        compiled_ = false;
        compileDueMs_ = SDL_GetTicks();
        ed_.status("NoX Core-only script features allowed (Editor Settings): maps that use them need a NoX Core server");
    }
    help("Turns on Editor Settings > \"Allow NoX Core-only script features (object variables)\". Maps that use them crash the "
         "original game and need a NoX Core server.");
}

void ScriptPanel::saveAndReload()
{
    commit();
    if (!compileIntoMap()) {
        return;
    }
    if (ed_.ctx_.playTest.running && ed_.ctx_.playTest.running()) {
        reload(Reload::Hot);
    }
    ed_.save();
}

void ScriptPanel::tick()
{
    const std::uint64_t now = SDL_GetTicks();
    if (typing_ && now - typedMs_ > 800 && !code_.focused()) {
        commit();
    }
    if (typing_ && now - typedMs_ > 1500) {
        commit();
    }
    keepScriptLock(now);
    syncFromDocument();
    code_.setReadOnly(readOnlyReason() != nullptr || scriptHolder().has_value());
    if (compiled_ && compiledAllow_ != allowNoxCore() && compileDueMs_ == 0) {
        compileDueMs_ = now; // (the setting changed: compiled again with it)
    }
    if (now >= compileDueMs_ && compileDueMs_ != 0 && (!typing_ || now - typedMs_ > 350)) {
        compileDueMs_ = 0;
        compileNow();
    }
    if (fontSaveDueMs_ != 0 && now >= fontSaveDueMs_) {
        fontSaveDueMs_ = 0;
        ed_.saveSettings(); // (the script font's size, kept per user)
    }
    if (watching_ && now - watchPollMs_ >= 250) {
        watchPollMs_ = now;
        pollWatch();
    }
    if (playing_ && ed_.ctx_.playTest.running && !ed_.ctx_.playTest.running()) {
        playing_ = false; // (the test ended in the game: its quit menu, a failure)
        ed_.status("The play-test ended");
    }
    std::optional<std::string> picked;
    {
        std::lock_guard lock(pickedMutex_);
        picked = std::exchange(picked_, std::nullopt);
    }
    if (picked) {
        importObj(fs::path(reinterpret_cast<const char8_t*>(picked->c_str())));
    }
}

// --- Play-test and reload -------------------------------------------------------------------------

std::filesystem::path ScriptPanel::playTestDir() const
{
    return !ed_.ctx_.playTestDir.empty() ? ed_.ctx_.playTestDir : ed_.ctx_.prefDir / "playtest";
}

std::uint32_t ScriptPanel::defaultMode() const
{
    for (const Mode& m : kModes) {
        if (ed_.doc_.info.gameFlags & m.mapBit) return m.host;
    }
    return 0x100;
}

int ScriptPanel::playClass() const
{
    if (playClass_ >= 0) {
        return playClass_;
    }
    return classOfMapName(ed_.doc_.origin.name); // (-1: the player's own)
}

std::optional<SDL_FPoint> ScriptPanel::cursorPoint() const
{
    auto& hooks = ed_.ctx_.playTest;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (ed_.playView_->inPanel()) {
        // The game in the Map panel: the point under the mouse on its frame.
        if (hooks.gameWorldAt) {
            const ImVec2 o = ed_.windowOrigin(); // (the hook takes window coordinates)
            if (const auto p = hooks.gameWorldAt(m.x - o.x, m.y - o.y)) {
                return p;
            }
        }
        ed_.status("Put the mouse on the game in the Map panel to jump there");
        return std::nullopt;
    }
    if (const auto w = ed_.mouseWorld()) {
        return SDL_FPoint{w->x, w->y};
    }
    return SDL_FPoint{ed_.camera_.x, ed_.camera_.y}; // (the mouse on a menu or a panel: the view's centre)
}

void ScriptPanel::playFrom(SDL_FPoint at)
{
    if (ed_.inSession()) {
        // In a session, Play From Here is the session's Play (as F2: the game in front, our
        // character back in play), our character at the cursor's point instead of the view's.
        ed_.sessionPlayFrom(at.x, at.y);
        return;
    }
    auto& hooks = ed_.ctx_.playTest;
    if (!hooks.available()) {
        ed_.status("Play-test isn't available here");
        return;
    }
    char where[48];
    std::snprintf(where, sizeof where, "%.0f, %.0f", at.x, at.y);
    if (hooks.running && hooks.running()) {
        if (hooks.jumpTo) {
            hooks.jumpTo(at);
            ed_.status(std::string("Play-test: the player put at ") + where);
        }
        if (hooks.resume) hooks.resume();
        return;
    }
    startPlayTest(at);
}

void ScriptPanel::playMod(const std::string& modeId, const std::filesystem::path& modsDir)
{
    playModeId_ = modeId;
    playModsDir_ = modsDir;
    startPlayTest();
    playModeId_.clear(); // (a test that didn't start leaves the next Play as ever)
    playModsDir_.clear();
}

void ScriptPanel::startPlayTest(std::optional<SDL_FPoint> at)
{
    auto& hooks = ed_.ctx_.playTest;
    if (!hooks.available()) {
        ed_.status("Play-test isn't available here");
        return;
    }
    const bool privateTest = ed_.inSession(); // (a session's play-test is a private local one)
    if (privateTest && !hooks.startPrivate) {
        ed_.status("A private test isn't available here");
        return;
    }
    if (!privateTest && hooks.running && hooks.running()) {
        if (at && hooks.jumpTo) hooks.jumpTo(*at);
        if (hooks.resume) hooks.resume();
        return;
    }
    commit();
    ensureSource();
    // The test runs what the source compiles to (so a hot reload's names match it).
    me::MapDocument doc = ed_.doc_;
    if (privateTest) {
        // The working copy as it stands, compiled here alone (nothing sent to the session: the
        // test is ours).
        if (doc.scriptSource) {
            const auto r = me::compileForDocument(doc, *doc.scriptSource, ed_.settings_.allowNoxCoreScript);
            if (!r.ok() || !r.file) {
                ed_.errorText_ = "The script doesn't compile: fix its errors (Problems) before the private test.";
                return;
            }
            if (const me::Op op = me::compiledScriptOp(doc, *r.file); !op.empty()) {
                me::applyOp(doc, op);
            }
        }
    } else if (ed_.doc_.scriptSource) {
        if (!compileIntoMap()) {
            const std::string* why = readOnlyReason();
            ed_.errorText_ = why ? *why : "The script doesn't compile: fix its errors (Problems) before the play-test.";
            return;
        }
        doc = ed_.doc_;
        running_ = last_.debug;
    } else {
        running_ = {};
    }
    const std::string name = privateTest ? "EDPRIV" : "EDTEST";
    const fs::path dir = playTestDir() / name;
    const fs::path mapPath = dir / (name + ".map");
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        const auto files = me::encodeFiles(doc, ed_.things_);
        std::ofstream out(mapPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(files.map.data()), static_cast<std::streamsize>(files.map.size()));
        if (!out) {
            throw std::runtime_error("can't write " + pathText(mapPath));
        }
        // (and its .nxz, as a saved map has: what the host loads for downloads at each map's load)
        std::ofstream(fs::path(mapPath).replace_extension(".nxz"), std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(files.nxz.data()), static_cast<std::streamsize>(files.nxz.size()));
    } catch (const std::exception& ex) {
        ed_.errorText_ = std::string("The play-test's map can't be written:\n") + ex.what();
        return;
    }
    editor::PlayTestRequest req;
    req.map = mapPath;
    req.cacheDir = playTestDir();
    const Mode& mode = modeOf(playMode_ ? playMode_ : defaultMode());
    req.kind = mode.kind;
    req.mode = mode.kind == editor::PlayKind::Network ? mode.host : 0x100;
    req.playerClass = playClass();
    req.playersCanDie = playersCanDie_;
    req.fullWindow = ed_.settings_.playFullWindow;
    req.startAt = at;
    req.modeId = std::exchange(playModeId_, {});
    req.modsDir = std::exchange(playModsDir_, {});
    if (!req.modeId.empty()) {
        req.playersCanDie = true; // (a mod's own rules: no sandbox)
    }
    if (privateTest) {
        // Its own game in a window of its own; the session's game and our place in it untouched.
        req.playersCanDie = true; // (the map's real mode, as any game of it)
        const std::string error = hooks.startPrivate(req);
        if (!error.empty()) {
            ed_.errorText_ = "The private test didn't start:\n" + error;
            return;
        }
        ed_.status(std::string("Private test of the working copy: ") + modeName(req.mode) + ", in its own window (the session goes on here)");
        return;
    }
    const std::string error = hooks.start(req);
    if (!error.empty()) {
        ed_.errorText_ = "The play-test didn't start:\n" + error;
        return;
    }
    playing_ = true;
    reloadNotes_.clear();
    reloadSummary_.clear();
    std::string from;
    if (at) {
        char where[48];
        std::snprintf(where, sizeof where, ", from %.0f, %.0f", at->x, at->y);
        from = where;
    }
    ed_.status(std::string("Play-test: ") + mode.name + (req.playerClass >= 0 ? std::string(", ") + kClassNames[req.playerClass] : std::string()) +
               (playersCanDie_ ? "" : ", players can't die") + from +
               (req.fullWindow ? " (F2 in the game comes back)" : " (" + ed_.commands_.shortcut("play.release").text() + " frees the mouse)"));
}

void ScriptPanel::stopPlayTest()
{
    auto& hooks = ed_.ctx_.playTest;
    if (hooks.running && hooks.running() && hooks.stop) {
        hooks.stop();
    }
    playing_ = false;
}

void ScriptPanel::reload(Reload mode)
{
    if (ed_.inSession()) {
        // A session: the source is the working copy's (committed as an op); the host compiles it,
        // commits the bytecode and reloads the running game's script for every player.
        commit();
        if (mode == Reload::RestartMap) {
            ed_.status("In a session the map restarts with a publish");
            return;
        }
        ed_.sessionReloadScript(mode == Reload::Hot);
        return;
    }
    auto& hooks = ed_.ctx_.playTest;
    if (!hooks.running || !hooks.running()) {
        ed_.status("No play-test is running");
        return;
    }
    if (mode == Reload::RestartMap) {
        stopPlayTest();
        startPlayTest();
        return;
    }
    commit();
    if (!compileIntoMap()) {
        return;
    }
    const sc::ScriptFile file = *last_.file;
    const sc::DebugInfo debug = last_.debug;
    nox::server::dev::ReloadReport report;
    hooks.withGame([&](nox::server::GameHost& game) {
        report = mode == Reload::Hot ? nox::server::dev::hotReload(game.server(), file, running_, debug)
                                     : nox::server::dev::restartScripts(game.server(), file);
    });
    if (report.applied) {
        running_ = debug;
    }
    reloadNotes_ = report.notes;
    reloadSummary_ = mode == Reload::Hot ? "Hot reload: " + report.summary()
                                         : "Restart scripts: " + (report.notes.empty() ? report.summary() : report.notes.back().message);
    if (!report.applied && !report.notes.empty()) {
        reloadSummary_ = "Not reloaded: " + report.notes.front().message;
    }
    ed_.status(reloadSummary_);
    if (hooks.notify) {
        hooks.notify(reloadSummary_);
    }
    if (std::ranges::any_of(report.notes, [](const auto& n) { return n.severity == nox::server::dev::ReloadNote::Severity::Warning; })) {
        ed_.showProblems_ = true;
    }
}

void ScriptPanel::runTests()
{
    commit();
    ensureSource();
    me::MapDocument doc = ed_.doc_;
    if (doc.scriptSource) {
        compiled_ = false;
        compileNow();
        if (!last_.ok()) {
            ed_.status("The script has errors: no tests run");
            ed_.showProblems_ = true;
            return;
        }
        doc.script.code = last_.file->serialize();
        doc.ensureSection(nox::map::ScriptObject::kName);
    }
    try {
        const auto bytes = me::writeMapBytes(doc, ed_.things_);
        const auto map = nox::map::parseMapFile(bytes);
        nox::server::ScriptTestOptions o;
        tests_ = nox::server::runScriptTests(map, "EDTEST.map", ed_.things_, o);
    } catch (const std::exception& ex) {
        tests_ = nox::server::ScriptTestRun{};
        tests_->error = ex.what();
    }
    ed_.showProblems_ = true;
    ed_.status(tests_->error.empty() ? "Script tests: " + std::to_string(tests_->passed()) + " passed, " + std::to_string(tests_->failed()) + " failed"
                                     : "Script tests didn't run: " + tests_->error);
}

// --- Files -----------------------------------------------------------------------------------------

std::filesystem::path ScriptPanel::watchPath() const
{
    if (!ed_.docName_.empty() && !ed_.docPath_.empty()) {
        return ed_.docPath_.parent_path() / (ed_.docName_ + ".ns");
    }
    const std::string name = !ed_.doc_.origin.name.empty() ? ed_.doc_.origin.name : "untitled";
    return playTestDir() / (name + ".ns");
}

void ScriptPanel::exportNs()
{
    commit();
    ensureSource();
    const fs::path path = watchPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << ed_.doc_.scriptSource.value_or(std::string());
    ed_.status(out ? "Exported " + pathText(path) : "Can't write " + pathText(path));
}

void ScriptPanel::exportObj()
{
    commit();
    ensureSource();
    std::vector<std::uint8_t> bytes = ed_.doc_.script.code;
    if (ed_.doc_.scriptSource) {
        compiled_ = false;
        compileNow();
        if (!last_.ok()) {
            ed_.status("The script has errors: not exported");
            return;
        }
        bytes = last_.file->serialize();
    }
    const fs::path path = fs::path(watchPath()).replace_extension(".obj");
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ed_.status(out ? "Exported " + pathText(path) : "Can't write " + pathText(path));
}

void ScriptPanel::importObj(const fs::path& path)
{
    lastObj_ = path;
    try {
        const auto bytes = nox::readFileBytes(path);
        const sc::ScriptFile file = sc::ScriptFile::parse(bytes);
        commit();
        nox::map::ScriptObject so = ed_.doc_.script;
        so.code = bytes;
        me::MapDocument probe = ed_.doc_;
        probe.script = so;
        const auto d = me::decompileDocumentScript(probe);
        ed_.perform(me::Op{"Import script", {me::SetScript{so}, me::SetScriptSource{d.source}}});
        decompileNotes_ = d.notes;
        ed_.status("Imported " + pathText(path) + " (" + std::to_string(file.functions.size()) + " functions)");
    } catch (const std::exception& ex) {
        ed_.errorText_ = "Can't import " + pathText(path) + ":\n" + ex.what();
    }
}

void ScriptPanel::setWatch(bool on)
{
    watching_ = on;
    if (!on) {
        return;
    }
    watched_ = watchPath();
    std::error_code ec;
    if (!fs::exists(watched_, ec)) {
        ensureSource();
        fs::create_directories(watched_.parent_path(), ec);
        std::ofstream(watched_, std::ios::binary) << ed_.doc_.scriptSource.value_or(std::string());
    }
    watchedTime_ = fs::last_write_time(watched_, ec);
    watchedSize_ = fs::file_size(watched_, ec);
    watchSeenMs_ = 0;
    ed_.status("Watching " + pathText(watched_));
}

void ScriptPanel::pollWatch()
{
    std::error_code ec;
    const auto t = fs::last_write_time(watched_, ec);
    if (ec) {
        return;
    }
    const auto size = fs::file_size(watched_, ec);
    if (t == watchedTime_ && size == watchedSize_) {
        watchSeenMs_ = 0;
        return;
    }
    // Settled 200 ms (an editor's save may come in pieces), then read.
    const std::uint64_t now = SDL_GetTicks();
    if (watchSeenMs_ == 0) {
        watchSeenMs_ = now;
        return;
    }
    if (now - watchSeenMs_ < 200) {
        return;
    }
    watchSeenMs_ = 0;
    watchedTime_ = t;
    watchedSize_ = size;
    std::string text;
    try {
        const auto bytes = nox::readFileBytes(watched_);
        text.assign(bytes.begin(), bytes.end());
    } catch (const std::exception&) {
        return;
    }
    if (ed_.doc_.scriptSource && *ed_.doc_.scriptSource == text) {
        return;
    }
    typing_ = false;
    ed_.perform(me::Op{"Script changed on disk", {me::SetScriptSource{text}}});
    syncFromDocument();
    if (ed_.ctx_.playTest.running && ed_.ctx_.playTest.running()) {
        reload(Reload::Hot);
    } else {
        compiled_ = false;
        compileNow();
    }
}

// --- Navigation --------------------------------------------------------------------------------------

void ScriptPanel::showLine(int line, int col)
{
    show_ = true;
    focusWindow_ = 2;
    code_.gotoLine(line, col);
}

void ScriptPanel::gotoDefinition(std::string_view word)
{
    if (word.empty()) {
        return;
    }
    if (good_) {
        if (const auto* f = good_->function(word)) {
            showLine(static_cast<int>(f->loc.line), static_cast<int>(f->loc.column));
            return;
        }
        for (const auto& g : good_->globals) {
            if (g.name == word && g.loc.line > 0) {
                showLine(static_cast<int>(g.loc.line), static_cast<int>(g.loc.column));
                return;
            }
        }
        // A local of the function the caret is in.
        const int caretLine = code_.caret().line + 1;
        const sc::DebugFunction* in = nullptr;
        for (const auto& f : good_->functions) {
            if (static_cast<int>(f.loc.line) <= caretLine && (!in || f.loc.line > in->loc.line)) in = &f;
        }
        if (in) {
            for (const auto& v : in->symbols) {
                if (v.name == word && v.loc.line > 0) {
                    showLine(static_cast<int>(v.loc.line), static_cast<int>(v.loc.column));
                    return;
                }
            }
        }
        // An object variable (the word after `object.`).
        for (const auto& v : good_->objectVars) {
            if (v.name == word && v.loc.line > 0) {
                showLine(static_cast<int>(v.loc.line), static_cast<int>(v.loc.column));
                return;
            }
        }
    }
    if (sc::findBuiltin(word)) {
        ed_.status(prototype(*sc::findBuiltin(word)) + " is a builtin");
        return;
    }
    ed_.status("No definition of " + std::string(word) + " (compile the script for its names)");
}

std::vector<std::string> ScriptPanel::functionNames() const
{
    std::vector<std::string> out;
    if (good_) {
        for (const auto& f : good_->functions) out.push_back(f.name);
    } else if (!ed_.doc_.script.code.empty()) {
        try {
            const auto file = sc::ScriptFile::parse(ed_.doc_.script.code);
            for (std::size_t i = 2; i < file.functions.size(); ++i) out.push_back(file.functions[i].name);
        } catch (const std::exception&) {
        }
    }
    std::ranges::sort(out);
    return out;
}

bool ScriptPanel::functionPicker(const char* label, std::string& function)
{
    bool changed = false;
    const auto names = functionNames();
    const bool known = function.empty() || std::ranges::find(names, function) != names.end();
    const std::string preview = function.empty() ? "(none)" : known ? function : function + "  (not in the script)";
    ImGui::PushID(label);
    if (!known) ImGui::PushStyleColor(ImGuiCol_Text, editor::themeColours().unknown);
    if (ImGui::BeginCombo("##fn", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (!known) ImGui::PopStyleColor();
        if (ImGui::Selectable("(none)", function.empty())) {
            function.clear();
            changed = true;
        }
        for (const auto& n : names) {
            if (ImGui::Selectable(n.c_str(), n == function)) {
                function = n;
                changed = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("New function...")) {
            // A name from the field's ("Enemy sighted" -> "OnEnemySighted").
            std::string suggestion = "On";
            bool upper = true;
            for (const char* c = label; *c; ++c) {
                if (std::isalnum(static_cast<unsigned char>(*c))) {
                    suggestion += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(*c))) : *c;
                    upper = false;
                } else {
                    upper = true;
                }
            }
            std::snprintf(newFnBuf_, sizeof newFnBuf_, "%s", suggestion.c_str());
            newFnOpen_ = true;
        }
        ImGui::EndCombo();
    } else if (!known) {
        ImGui::PopStyleColor();
    }
    help("The script function this handler calls (from the compiled script). Right-click: go to it.");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !function.empty()) {
        gotoDefinition(function);
    }
    if (std::exchange(newFnOpen_, false)) {
        ImGui::OpenPopup("New function");
    }
    if (ImGui::BeginPopup("New function")) {
        ImGui::TextUnformatted("A new void function in the script, for this handler:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##newfn", newFnBuf_, sizeof newFnBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = newFnBuf_;
        const bool valid = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
                           std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }) &&
                           !sc::findBuiltin(name);
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create") || (enter && valid)) {
            if (readOnlyReason()) {
                ed_.status("The script is read-only: it uses object variables, which Editor Settings doesn't allow");
            } else {
                bool have = good_ && good_->function(name);
                if (!have) {
                    commit();
                    ensureSource();
                    std::string src = ed_.doc_.scriptSource.value_or(std::string());
                    if (!src.empty() && src.back() != '\n') src += '\n';
                    src += "\nvoid " + name + "()\n{\n}\n";
                    // (Refused, as when another editor holds the script: the handler isn't pointed
                    // at a function that was never made.)
                    have = ed_.perform(me::Op{"Add function " + name, {me::SetScriptSource{src}}});
                    syncFromDocument();
                    compiled_ = false;
                    compileNow();
                }
                if (have) {
                    function = name;
                    changed = true;
                    gotoDefinition(name);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// --- Drawing -------------------------------------------------------------------------------------------

void ScriptPanel::findBar()
{
    if (!findOpen_) {
        return;
    }
    const float s = ed_.imgui_->scale();
    ImGui::SetNextItemWidth(220 * s);
    if (findFocus_) {
        ImGui::SetKeyboardFocusHere();
        findFocus_ = false;
    }
    const bool enter = ImGui::InputTextWithHint("##find", "Find (Enter: next, Shift+Enter: previous)", findBuf_, sizeof findBuf_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter) {
        if (!code_.find(findBuf_, !ImGui::GetIO().KeyShift, findCase_)) ed_.status(std::string("Not found: ") + findBuf_);
        findFocus_ = true; // (keep typing there)
    }
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        findOpen_ = false;
        code_.focus();
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Up)) code_.find(findBuf_, false, findCase_);
    help("Previous match (Shift+F3)");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Down)) code_.find(findBuf_, true, findCase_);
    help("Next match (F3)");
    ImGui::SameLine();
    ImGui::Checkbox("Match case", &findCase_);
    ImGui::SameLine();
    code_.setHighlight(findBuf_, findCase_);
    ImGui::TextDisabled("%d matches", code_.highlightCount());
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        findOpen_ = false;
    }
}

void ScriptPanel::draw()
{
    if (!focusName_.empty()) {
        ImGui::SetWindowFocus(focusName_.c_str());
        focusName_.clear();
    }
    if (show_) {
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        if (focusWindow_ > 0) {
            --focusWindow_; // (two frames: a docked window's tab is picked after its node is up)
            ImGui::SetNextWindowFocus();
        }
        if (ImGui::Begin("Script", &show_)) {
            ensureSource();
            // The bar: compile state, reload, play, the function list.
            const auto errors = std::ranges::count_if(last_.diagnostics, [](const auto& d) { return d.severity == sc::Diagnostic::Severity::Error; });
            const auto warnings = static_cast<std::ptrdiff_t>(last_.diagnostics.size()) - errors;
            const auto button = [&](const char* label, const char* id) {
                const editor::Command* c = ed_.commands_.find(id);
                ImGui::BeginDisabled(!ed_.commands_.enabled(id));
                if (ImGui::Button(label)) ed_.commands_.run(id);
                ImGui::EndDisabled();
                if (c) {
                    const std::string k = ed_.commands_.shortcut(id).text();
                    help((c->label + (k.empty() ? "" : "  (" + k + ")") + "\n" + c->help).c_str());
                }
                ImGui::SameLine();
            };
            button("Compile", "script.compile");
            const bool running = ed_.ctx_.playTest.running && ed_.ctx_.playTest.running();
            // Running: in the Map panel F5 gives the game the keyboard and mouse (it's already in
            // view); over the whole window it goes back to the game behind the editor.
            const editor::PlayTestView pv = running && ed_.ctx_.playTest.view ? ed_.ctx_.playTest.view() : editor::PlayTestView{};
            button(!running ? "Play" : pv.fullWindow ? "Back to Game" : pv.captured ? "Playing" : "Focus Game", "play.start");
            button("Hot Reload", "script.reload");
            button("Restart Scripts", "script.restartScripts");
            button("Stop", "play.stop");
            button("Tests", "script.runTests");
            if (!compiled_) {
                ImGui::TextDisabled("not compiled yet");
            } else if (errors > 0) {
                ImGui::TextColored(editor::themeColours().error, "%td %s, %td warnings", errors, errors == 1 ? "error" : "errors", warnings);
            } else {
                ImGui::TextColored(editor::themeColours().ok, warnings ? "compiles (%td warnings)" : "compiles", warnings);
            }
            if (good_ && !good_->functions.empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x));
                if (ImGui::BeginCombo("##functions", "Go to function...", ImGuiComboFlags_HeightLarge)) {
                    std::vector<const sc::DebugFunction*> fs;
                    for (const auto& f : good_->functions) fs.push_back(&f);
                    std::ranges::sort(fs, [](auto* a, auto* b) { return a->loc.line < b->loc.line; });
                    for (const auto* f : fs) {
                        const std::string label = f->name + "##" + std::to_string(f->index);
                        if (ImGui::Selectable(label.c_str())) showLine(static_cast<int>(f->loc.line), static_cast<int>(f->loc.column));
                    }
                    ImGui::EndCombo();
                }
            }
            if (running) {
                ImGui::TextColored(editor::themeColours().info, "Play-test running: Ctrl+S or Ctrl+R hot-reloads into it.%s",
                                   reloadSummary_.empty() ? "" : ("  Last: " + reloadSummary_).c_str());
            }
            if (const auto who = scriptHolder()) {
                // A session's script, one editor at a time.
                ImGui::TextColored(editor::themeColours().caution, "%s is editing the script: read-only till they pause.", who->c_str());
                if (ed_.session_->isOwner()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Take Over")) {
                        ed_.takeOverPanel(me::ElementKey::script());
                        typedMs_ = SDL_GetTicks(); // (kept a while for the first key: keepScriptLock)
                    }
                    help("Take the script from them now (what they typed and haven't sent yet is dropped).");
                }
            }
            noxCoreBanner(readOnlyReason());
            findBar();
            if (gotoOpen_) {
                ImGui::OpenPopup("Go to line");
                gotoOpen_ = false;
                gotoBuf_[0] = 0;
            }
            if (ImGui::BeginPopup("Go to line")) {
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("Line", gotoBuf_, sizeof gotoBuf_, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                    showLine(std::max(1, std::atoi(gotoBuf_)));
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0) {
                setFontScale(ed_.settings_.scriptFontScale * (ImGui::GetIO().MouseWheel > 0 ? 1.1f : 1.0f / 1.1f));
            }
            ImGui::PushFont(ed_.imgui_->monoFont(), ImGui::GetStyle().FontSizeBase * ed_.settings_.scriptFontScale);
            code_.render("##source", ImVec2(0, 0), nullptr);
            const bool changed = code_.takeChanged();
            if (changed) {
                typed();
            }
            // The autocomplete: kept with the caret, opened by typing in a builtin's value argument.
            if (const bool moved = code_.takeCaretMoved(); changed || moved) {
                updateCompletion(changed);
            }
            if (complete_.open && codeWasFocused_ && !code_.focused() && !complete_.hovered && !ed_.ctx_.screenshotRun) {
                complete_.open = false; // (the keyboard went elsewhere; a test's hidden window never has it)
            }
            codeWasFocused_ = code_.focused();
            if (complete_.open) {
                drawCompletion();
            }
            ImGui::PopFont();
            // A builtin right-clicked in the source: its menu (Function Info, priority).
            if (codeMenuBuiltin_) {
                ImGui::OpenPopup("##codeBuiltinMenu");
                codeMenuShown_ = *std::exchange(codeMenuBuiltin_, std::nullopt);
            }
            if (ImGui::BeginPopup("##codeBuiltinMenu")) {
                builtinMenuItems(codeMenuShown_);
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    }
    if (showReference_) {
        referencePanel();
    }
    if (showInfo_) {
        functionInfoPanel();
    }
}

void ScriptPanel::referencePanel()
{
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Reference", &showReference_)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##refsearch", "Search", refSearch_, sizeof refSearch_);
    const std::string q = lowered(refSearch_);
    if (ImGui::BeginTabBar("##ref")) {
        if (ImGui::BeginTabItem("Builtins")) {
            builtinsTab(q);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Functions")) {
            if (!good_) {
                ImGui::TextDisabled("Compile the script for its functions.");
            } else {
                ImGui::BeginChild("##fns");
                for (const auto& f : good_->functions) {
                    if (!q.empty() && lowered(f.name).find(q) == std::string::npos) continue;
                    const std::string label = std::string(sc::typeName(f.returns)) + " " + f.name + "##f" + std::to_string(f.index);
                    if (ImGui::Selectable(label.c_str())) showLine(static_cast<int>(f.loc.line), static_cast<int>(f.loc.column));
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("line %u, function %u", f.loc.line, f.index);
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Disassembly")) {
            // The compiled code of the function the caret is in (read-only: the old editor's
            // line assembler view).
            const sc::DebugFunction* in = nullptr;
            if (good_) {
                const int caretLine = code_.caret().line + 1;
                for (const auto& f : good_->functions) {
                    if (static_cast<int>(f.loc.line) <= caretLine && (!in || f.loc.line > in->loc.line)) in = &f;
                }
            }
            if (!in || !last_.file || !compiled_ || !last_.ok()) {
                ImGui::TextDisabled("Compile the script and put the caret in a function.");
            } else if (in->index < last_.file->functions.size()) {
                ImGui::Text("%s (function %u)", in->name.c_str(), in->index);
                ImGui::BeginChild("##disasm", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::PushFont(ed_.imgui_->monoFont(), 0.0f);
                ImGui::TextUnformatted(sc::disassemble(*last_.file, in->index).c_str());
                ImGui::PopFont();
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Globals")) {
            if (!good_) {
                ImGui::TextDisabled("Compile the script for its globals.");
            } else {
                ImGui::BeginChild("##globals");
                for (const auto& g : good_->globals) {
                    if (g.symbol < 4 || (!q.empty() && lowered(g.name).find(q) == std::string::npos)) continue;
                    const std::string label = std::string(sc::typeName(g.type)) + (g.size > 1 ? "[" + std::to_string(g.size) + "]" : "") + " " +
                                              g.name + "##g" + std::to_string(g.symbol);
                    if (ImGui::Selectable(label.c_str())) showLine(static_cast<int>(g.loc.line), static_cast<int>(g.loc.column));
                }
                for (const auto& v : good_->objectVars) {
                    if (v.name.empty() || (!q.empty() && lowered(v.name).find(q) == std::string::npos)) continue;
                    const std::string label = std::string(sc::typeName(v.type)) + (v.size > 1 ? "[" + std::to_string(v.size) + "]" : "") +
                                              " object." + v.name + "##o" + std::to_string(v.symbol);
                    if (ImGui::Selectable(label.c_str())) showLine(static_cast<int>(v.loc.line), static_cast<int>(v.loc.column));
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// --- Builtins: the list, its menu, Function Info ----------------------------------------------------

namespace {

/// "used in 53 of 129 stock map scripts (627 calls), e.g. ..." (examples: up to five maps).
std::string usageText(const sc::BuiltinDoc& d, bool examples)
{
    if (d.usageMaps == 0) {
        return "Not used by any stock map.";
    }
    std::string s = "Used in " + std::to_string(d.usageMaps) + " of the " + std::to_string(sc::builtinDocs().usageScripts) +
                    " stock map scripts (" + std::to_string(d.usageCalls) + (d.usageCalls == 1 ? " call)" : " calls)");
    if (examples && !d.usageExamples.empty()) {
        s += ", e.g. ";
        for (std::size_t i = 0; i < d.usageExamples.size(); ++i) {
            s += (i ? ", " : "") + d.usageExamples[i];
        }
    }
    return s + ".";
}

} // namespace

const std::vector<editor::BuiltinRow>& ScriptPanel::builtinRows()
{
    if (builtinRows_.empty()) {
        for (std::uint32_t id = 0; id < sc::kBuiltinCount; ++id) {
            editor::BuiltinRow r;
            r.id = id;
            r.name = std::string(sc::builtinSignature(id).name);
            const sc::BuiltinDoc* d = sc::builtinDoc(id);
            r.category = d ? d->category : std::string();
            r.usageCalls = d ? d->usageCalls : 0;
            // The search: the prototype (named and typed), the tooltip, the category, an old name.
            std::string text = namedPrototype(id) + "\n" + prototype(id);
            if (d) text += "\n" + d->shortText + "\n" + d->category;
            if (const auto alias = sc::builtinAlias(id)) text += "\n" + std::string(*alias);
            r.searchText = lowered(std::move(text));
            builtinRows_.push_back(std::move(r));
        }
    }
    return builtinRows_;
}

editor::BuiltinPriority ScriptPanel::builtinPriority(std::uint32_t id) const
{
    return editor::priorityOf(ed_.settings_.builtinPriorities, sc::builtinSignature(id).name);
}

void ScriptPanel::setBuiltinPriority(std::uint32_t id, editor::BuiltinPriority p)
{
    editor::setPriority(ed_.settings_.builtinPriorities, sc::builtinSignature(id).name, p);
    ed_.saveSettings();
    ed_.status(std::string(sc::builtinSignature(id).name) + ": " + (p == editor::BuiltinPriority::Favourite ? "a favourite" : "priority " + std::string(editor::builtinPriorityName(p))));
}

bool ScriptPanel::insertAtCaret(const std::string& text)
{
    ensureSource();
    if (const std::string* why = readOnlyReason()) {
        ed_.status(*why);
        return false;
    }
    code_.insertText(text);
    typed();
    show_ = true;
    code_.focus();
    return true;
}

void ScriptPanel::insertBuiltin(std::uint32_t id)
{
    // The name and its parentheses, the caret between them.
    const auto& sig = sc::builtinSignature(id);
    insertAtCaret(std::string(sig.name) + "(" + (sig.args.empty() ? ")" : ""));
}

void ScriptPanel::builtinTooltip(std::uint32_t id) const
{
    if (!ImGui::BeginTooltip()) {
        return;
    }
    const sc::BuiltinDoc* d = sc::builtinDoc(id);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::PushFont(ed_.imgui_->monoFont(), 0.0f);
    ImGui::TextUnformatted(namedPrototype(id).c_str());
    ImGui::PopFont();
    if (d) {
        ImGui::Spacing();
        ImGui::TextUnformatted(d->shortText.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("%s, builtin %u. %s", d->category.c_str(), id, usageText(*d, false).c_str());
    }
    ImGui::TextDisabled("Click: insert at the caret. Right-click: Function Info, priority.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void ScriptPanel::builtinMenuItems(std::uint32_t id)
{
    using editor::BuiltinPriority;
    const std::string name(sc::builtinSignature(id).name);
    ImGui::TextDisabled("%s", namedPrototype(id).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Function Info")) {
        openFunctionInfo(id);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The full description: parameters, return value, edge cases, related builtins and an example.");
    if (ImGui::MenuItem("Insert", nullptr, false, readOnlyReason() == nullptr)) {
        insertBuiltin(id);
    }
    if (ImGui::MenuItem("Copy Name")) {
        ImGui::SetClipboardText(name.c_str());
    }
    ImGui::Separator();
    const BuiltinPriority p = builtinPriority(id);
    if (ImGui::MenuItem(p == BuiltinPriority::Favourite ? "Remove from Favourites" : "Add to Favourites")) {
        setBuiltinPriority(id, p == BuiltinPriority::Favourite ? BuiltinPriority::Normal : BuiltinPriority::Favourite);
    }
    if (ImGui::BeginMenu("Priority")) {
        for (const BuiltinPriority level : {BuiltinPriority::Favourite, BuiltinPriority::High, BuiltinPriority::Normal, BuiltinPriority::Low}) {
            if (ImGui::MenuItem(editor::builtinPriorityName(level), nullptr, p == level)) {
                setBuiltinPriority(id, level);
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Sort the list by Priority: favourites first, low last.");
        ImGui::EndMenu();
    }
}

void ScriptPanel::builtinsTab(const std::string& query)
{
    using editor::BuiltinPriority;
    using editor::BuiltinSort;
    auto& s = ed_.settings_;
    const auto sort = static_cast<BuiltinSort>(std::clamp(s.builtinSort, 0, editor::kBuiltinSortCount - 1));
    // The order and the filter.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sort");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 9.5f, ImGui::GetContentRegionAvail().x * 0.5f));
    if (ImGui::BeginCombo("##bsort", editor::builtinSortName(sort))) {
        for (int i = 0; i < editor::kBuiltinSortCount; ++i) {
            if (ImGui::Selectable(editor::builtinSortName(static_cast<BuiltinSort>(i)), s.builtinSort == i)) {
                s.builtinSort = i;
                ed_.saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    help("Priority: favourites first, then high, normal and low, each by name (right-click a builtin to set its priority). "
         "Use in stock maps: the most called first.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Favourites only", &s.builtinFavouritesOnly)) {
        ed_.saveSettings();
    }
    const auto ids = editor::builtinListOrder(builtinRows(), s.builtinPriorities, sort, s.builtinFavouritesOnly, query);
    if (ids.empty()) {
        ImGui::TextDisabled(s.builtinFavouritesOnly ? "No favourites here: right-click a builtin, Add to Favourites." : "No builtin matches.");
    }
    ImGui::BeginChild("##builtins");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string lastCategory;
    bool firstRow = true;
    for (const std::uint32_t id : ids) {
        const editor::BuiltinRow& row = builtinRows()[id];
        const bool first = std::exchange(firstRow, false);
        if (sort == BuiltinSort::Category && (first || row.category != lastCategory)) {
            ImGui::SeparatorText(row.category.empty() ? "(none)" : row.category.c_str());
            lastCategory = row.category;
        }
        const BuiltinPriority p = builtinPriority(id);
        ImGui::PushID(static_cast<int>(id));
        // The row: room for its mark, then the prototype in the source's font (low ones dimmed).
        ImGui::PushFont(ed_.imgui_->monoFont(), 0.0f);
        const float markW = ImGui::CalcTextSize("  ").x;
        if (p == BuiltinPriority::Low) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool clicked = ImGui::Selectable(("  " + namedPrototype(id)).c_str());
        if (p == BuiltinPriority::Low) ImGui::PopStyleColor();
        ImGui::PopFont();
        const ImVec2 a = ImGui::GetItemRectMin();
        const float h = ImGui::GetItemRectSize().y;
        if (p == BuiltinPriority::Favourite || p == BuiltinPriority::High) {
            drawStar(dl, ImVec2(a.x + markW * 0.45f, a.y + h * 0.5f), h * (p == BuiltinPriority::Favourite ? 0.40f : 0.34f), starColour(),
                     p == BuiltinPriority::Favourite);
        }
        if (clicked) {
            insertBuiltin(id);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsPopupOpen("##menu")) {
            builtinTooltip(id);
        }
        if (menuBuiltin_ == id) {
            // (tests: builtin-menu) its menu opened as a right-click would, just under the row.
            ImGui::OpenPopup("##menu");
            ImGui::SetNextWindowPos(ImVec2(a.x + markW * 3, a.y + h));
            menuBuiltin_.reset();
        }
        if (ImGui::BeginPopupContextItem("##menu")) {
            builtinMenuItems(id);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void ScriptPanel::openFunctionInfo(std::uint32_t id, bool back)
{
    if (id >= sc::kBuiltinCount) {
        return;
    }
    if (!back) {
        infoBack_.clear();
    } else if (showInfo_ && infoId_ != id) {
        infoBack_.push_back(infoId_);
        if (infoBack_.size() > 64) infoBack_.erase(infoBack_.begin());
    }
    infoId_ = id;
    showInfo_ = true;
    infoFocus_ = true;
}

void ScriptPanel::functionInfoPanel()
{
    using editor::BuiltinPriority;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 40), ImGuiCond_FirstUseEver);
    if (infoFocus_) {
        ImGui::SetNextWindowFocus();
        infoFocus_ = false;
    }
    if (!ImGui::Begin("Function Info", &showInfo_)) {
        ImGui::End();
        return;
    }
    const std::uint32_t id = std::min(infoId_, sc::kBuiltinCount - 1);
    const sc::BuiltinDoc* d = sc::builtinDoc(id);
    ImFont* mono = ed_.imgui_->monoFont();

    // The bar: Back, the favourite star, the name, the priority.
    ImGui::BeginDisabled(infoBack_.empty());
    if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
        infoId_ = infoBack_.back();
        infoBack_.pop_back();
    }
    ImGui::EndDisabled();
    help("Back to the builtin shown before.");
    ImGui::SameLine();
    const BuiltinPriority p = builtinPriority(id);
    if (starButton("##fav", p == BuiltinPriority::Favourite, p == BuiltinPriority::Favourite ? "A favourite: click to take it out of the favourites." : "Add to the favourites.")) {
        setBuiltinPriority(id, p == BuiltinPriority::Favourite ? BuiltinPriority::Normal : BuiltinPriority::Favourite);
    }
    ImGui::SameLine();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(sc::builtinSignature(id).name.data(), sc::builtinSignature(id).name.data() + sc::builtinSignature(id).name.size());
    ImGui::PopFont();
    const float comboW = ImGui::GetFontSize() * 7.5f;
    ImGui::SameLine();
    if (const float room = ImGui::GetContentRegionAvail().x; room > comboW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - comboW);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##prio", editor::builtinPriorityName(p))) {
        for (const BuiltinPriority level : {BuiltinPriority::Favourite, BuiltinPriority::High, BuiltinPriority::Normal, BuiltinPriority::Low}) {
            if (ImGui::Selectable(editor::builtinPriorityName(level), p == level)) setBuiltinPriority(id, level);
        }
        ImGui::EndCombo();
    }
    help("Its priority: sort Script Reference's Builtins by Priority for the favourites first.");
    // The prototype and the facts.
    ImGui::PushFont(mono, 0.0f);
    ImGui::TextWrapped("%s", namedPrototype(id).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        ImGui::SetClipboardText(namedPrototype(id).c_str());
    }
    help("Copy the prototype.");
    {
        std::string facts = (d ? d->category + ", builtin " : std::string("builtin ")) + std::to_string(id);
        if (const auto alias = sc::builtinAlias(id)) facts += " (the community tools' " + std::string(*alias) + ")";
        ImGui::TextDisabled("%s", facts.c_str());
        const auto& args = sc::builtinSignature(id).args;
        bool hasValues = false;
        for (std::size_t i = 0; i < args.size(); ++i) hasValues = hasValues || sc::argValueSet(id, i).has_value();
        if (hasValues && d) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Values")) {
                infoScrollValues_ = 3;
            }
            help("Go to the list of every value its parameters accept (also offered as you type the argument, or with Ctrl+Space).");
        }
    }
    if (!d) {
        ImGui::Separator();
        ImGui::TextWrapped("No description for this builtin yet.");
        ImGui::End();
        return;
    }
    ImGui::Separator();
    ImGui::BeginChild(("##infobody" + std::to_string(id)).c_str()); // (a builtin of its own: scrolled from the top)
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("%s", d->shortText.c_str());
    docHeading("Description");
    docText(d->description);
    docHeading("Parameters");
    if (d->params.empty()) {
        ImGui::TextDisabled("None.");
    }
    for (const auto& prm : d->params) {
        ImGui::PushFont(mono, 0.0f);
        ImGui::TextColored(editor::themeColours().param, "%s %s", prm.type.c_str(), prm.name.c_str());
        ImGui::PopFont();
        ImGui::Indent();
        docText(prm.text);
        ImGui::Unindent();
    }
    valuesSection(id); // (the parameters' accepted values, from the game's data and the map)
    docHeading("Returns");
    docText(d->returns);
    if (!d->edge.empty()) {
        docHeading("Edge cases");
        docText(d->edge);
    }
    if (!d->related.empty()) {
        docHeading("Related");
        const ImGuiStyle& style = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        float x = 0;
        for (std::size_t i = 0; i < d->related.size(); ++i) {
            const std::string& rel = d->related[i];
            const float w = ImGui::CalcTextSize(rel.c_str()).x + style.FramePadding.x * 2;
            if (i > 0 && x + style.ItemSpacing.x + w <= avail) {
                ImGui::SameLine();
                x += style.ItemSpacing.x + w;
            } else {
                x = w;
            }
            const auto rid = sc::findBuiltin(rel);
            if (ImGui::SmallButton((rel + "##rel").c_str()) && rid) {
                openFunctionInfo(*rid, true);
            }
            if (rid && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) builtinTooltip(*rid);
        }
    }
    docHeading("Example");
    ImGui::BeginDisabled(readOnlyReason() != nullptr);
    if (ImGui::SmallButton("Insert at Cursor")) {
        insertAtCaret(d->example + "\n");
    }
    ImGui::EndDisabled();
    help("Type the example into the Script panel at the caret.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy Example")) {
        ImGui::SetClipboardText(d->example.c_str());
    }
    {
        ImGui::PushFont(mono, 0.0f);
        const int lines = 1 + static_cast<int>(std::ranges::count(d->example, '\n'));
        const float hgt = static_cast<float>(lines) * ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2 + ImGui::GetStyle().ScrollbarSize;
        ImGui::BeginChild("##example", ImVec2(0, hgt), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushTextWrapPos(-1.0f); // (as written: no wrapping)
        ImGui::TextUnformatted(d->example.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopFont();
    }
    docHeading("In the stock maps");
    ImGui::TextWrapped("%s", usageText(*d, true).c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();
}

// --- The builtins' argument values: autocomplete and Function Info's Values ----------------------

const sc::ArgValueSets& ScriptPanel::gameArgValues()
{
    if (!gameValues_) {
        sc::ArgValueSources src;
        src.things = &ed_.things_;
        src.strings = ed_.ctx_.strings;
        src.modifiers = ed_.ctx_.modifiers;
        gameValues_ = sc::ArgValueSets::build(src);
    }
    return *gameValues_;
}

const sc::ArgValueSets& ScriptPanel::argValues()
{
    // The map's names made again when the document changed (its history's revision, or in a
    // session the parts the sets come from).
    const auto& d = ed_.doc_;
    const std::uint64_t key = ed_.history_.revision() * 1000003u ^ (d.objects.size() << 40) ^ (d.waypoints.waypoints.size() << 20) ^ d.groups.groups.size();
    if (mapValuesKey_ != key) {
        mapValuesKey_ = key;
        mapValues_ = gameArgValues();
        me::addMapArgValues(mapValues_, d, &ed_.things_);
    }
    return mapValues_;
}

bool ScriptPanel::valueIcon(ImDrawList* dl, const sc::ArgValue& v, const ImVec2& at, float size)
{
    if (v.thing < 0 || static_cast<std::size_t>(v.thing) >= ed_.things_.things.size() || !ed_.objects_) {
        return false;
    }
    // (The content browser's thumbnails: registered for the floating panels' copies.)
    return ed_.objects_->drawThumbnailAt(dl, ed_.things_.things[static_cast<std::size_t>(v.thing)].name, at, size);
}

void ScriptPanel::openCompletion(bool explicitly)
{
    ensureSource();
    const std::string text = code_.text();
    const auto site = sc::argSiteAt(text, code_.offsetOf(code_.caret()), true);
    if (!site) {
        ed_.status("No value list here: the caret isn't in an argument that takes names from a known set (an effect, a sound, a thing type...)");
        complete_.open = false;
        return;
    }
    complete_.open = true;
    complete_.explicitly = explicitly;
    complete_.dismissed.reset();
    complete_.site = *site;
    complete_.site.typed.push_back('\x01'); // (differs from any text typed: filtered below)
    updateCompletion(false);
}

void ScriptPanel::updateCompletion(bool typedNow)
{
    const std::string text = code_.text();
    const auto site = sc::argSiteAt(text, code_.offsetOf(code_.caret()), complete_.open && complete_.explicitly);
    if (!site) {
        complete_.open = false;
        complete_.dismissed.reset();
        return;
    }
    if (site->inString && complete_.dismissed == site->begin) {
        complete_.open = false; // (Escape closed it in this literal)
        return;
    }
    complete_.dismissed.reset();
    if (!complete_.open) {
        if (!typedNow || !site->inString) {
            return; // (it opens by itself only for typing inside the literal)
        }
        complete_.explicitly = false;
        complete_.site.typed.push_back('\x01');
    }
    const bool refilter = !complete_.open || site->typed != complete_.site.typed || site->set != complete_.site.set;
    complete_.open = true;
    complete_.site = *site;
    if (refilter) {
        const sc::ValueSet* set = argValues().get(site->set);
        complete_.rows = set ? set->filter(site->typed, 1000) : std::vector<std::size_t>{};
        complete_.selected = 0;
        complete_.scrollTo = true;
    }
}

bool ScriptPanel::completionKey(int key)
{
    if (!complete_.open) {
        return false;
    }
    const int n = static_cast<int>(complete_.rows.size());
    const auto move = [&](int by) {
        if (n > 0) {
            complete_.selected = std::clamp(complete_.selected + by, 0, n - 1);
            complete_.scrollTo = true;
        }
    };
    switch (key) {
    case ImGuiKey_UpArrow: move(-1); return true;
    case ImGuiKey_DownArrow: move(1); return true;
    case ImGuiKey_PageUp: move(-10); return true;
    case ImGuiKey_PageDown: move(10); return true;
    case ImGuiKey_Enter:
    case ImGuiKey_KeypadEnter:
    case ImGuiKey_Tab:
        if (n == 0) {
            complete_.open = false;
            return false; // (nothing to put in: the key does what it always does)
        }
        acceptCompletion(static_cast<std::size_t>(complete_.selected));
        return true;
    case ImGuiKey_Escape:
        complete_.open = false;
        if (complete_.site.inString) complete_.dismissed = complete_.site.begin;
        return true;
    default:
        return false;
    }
}

void ScriptPanel::acceptCompletion(std::size_t row)
{
    const sc::ValueSet* set = argValues().get(complete_.site.set);
    if (!set || row >= complete_.rows.size() || complete_.rows[row] >= set->values().size()) {
        complete_.open = false;
        return;
    }
    if (const std::string* why = readOnlyReason()) {
        ed_.status(*why);
        complete_.open = false;
        return;
    }
    const sc::ArgSite site = complete_.site;
    const std::string& value = set->values()[complete_.rows[row]].value;
    const std::string insert = sc::argInsertText(site, value);
    code_.replaceRange(site.begin, site.end, insert);
    // The caret past the literal's closing quote.
    const std::size_t after = site.begin + insert.size() + (site.inString && site.closed ? 1 : 0);
    const editor::TextPos p = code_.posOf(after);
    code_.gotoLine(p.line + 1, p.col + 1);
    complete_.open = false;
    typed();
}

void ScriptPanel::insertValue(sc::ValueSetId setId, const std::string& value)
{
    ensureSource();
    if (const std::string* why = readOnlyReason()) {
        ed_.status(*why);
        return;
    }
    const auto site = sc::argSiteAt(code_.text(), code_.offsetOf(code_.caret()), true);
    if (site && site->set == setId) {
        // Into the argument the caret is in.
        const std::string insert = sc::argInsertText(*site, value);
        code_.replaceRange(site->begin, site->end, insert);
        const editor::TextPos p = code_.posOf(site->begin + insert.size() + (site->inString && site->closed ? 1 : 0));
        code_.gotoLine(p.line + 1, p.col + 1);
        typed();
        show_ = true;
        return;
    }
    insertAtCaret("\"" + value + "\"");
}

void ScriptPanel::drawCompletion()
{
    const sc::ValueSet* set = argValues().get(complete_.site.set);
    const bool icons = complete_.site.set == sc::ValueSetId::ThingType || complete_.site.set == sc::ValueSetId::ObjectName;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = std::floor(icons ? lineH * 1.7f : lineH);
    const int n = static_cast<int>(complete_.rows.size());
    const int shown = std::clamp(n, 1, 12);
    const float headerH = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 size(ImGui::GetFontSize() * 38.0f, headerH + static_cast<float>(shown) * (rowH + style.ItemSpacing.y) + style.WindowPadding.y * 2 + 2);
    // Under the caret's line, or over it when it would run past the window's bottom.
    ImVec2 pos = code_.caretScreenPos();
    const ImGuiViewport* vp = ImGui::GetWindowViewport();
    if (pos.y + size.y > vp->Pos.y + vp->Size.y) {
        pos.y -= code_.lineHeight() + size.y;
    }
    pos.x = std::min(pos.x, vp->Pos.x + vp->Size.x - size.x);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(vp->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (ImGui::Begin("##argComplete", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // (over the Script window, which has the keyboard)
        complete_.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const std::string_view title = sc::valueSetTitle(complete_.site.set);
        ImGui::TextDisabled("%.*s  %d%s  (Enter/Tab insert, Esc close)", static_cast<int>(title.size()), title.data(), n, n >= 1000 ? "+" : "");
        ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        if (!set || set->empty()) {
            ImGui::TextDisabled(sc::isMapValueSet(complete_.site.set) ? "None on this map." : "Not available: the game's data isn't loaded.");
        } else if (n == 0) {
            ImGui::TextDisabled("No %s starts with or contains \"%s\".", std::string(sc::valueSetName(complete_.site.set)).c_str(), complete_.site.typed.c_str());
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 noteCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        std::optional<int> clicked;
        ImGuiListClipper clipper;
        clipper.Begin(n, rowH + style.ItemSpacing.y);
        if (complete_.scrollTo && complete_.selected < n) {
            clipper.IncludeItemByIndex(complete_.selected);
        }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const sc::ArgValue& v = set->values()[complete_.rows[static_cast<std::size_t>(i)]];
                ImGui::PushID(i);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", i == complete_.selected, ImGuiSelectableFlags_None, ImVec2(0, rowH))) {
                    clicked = i;
                }
                if (icons) {
                    valueIcon(dl, v, at, rowH);
                }
                if (i == complete_.selected && complete_.scrollTo) {
                    ImGui::SetScrollHereY(0.5f);
                    complete_.scrollTo = false;
                }
                const float tx = at.x + (icons ? rowH + style.ItemSpacing.x : style.FramePadding.x);
                const float ty = at.y + (rowH - lineH) * 0.5f;
                dl->AddText(ImVec2(tx, ty), textCol, v.value.c_str());
                if (!v.note.empty()) {
                    const float vx = tx + ImGui::CalcTextSize(v.value.c_str()).x + ImGui::GetFontSize();
                    dl->PushClipRect(ImVec2(vx, at.y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), at.y + rowH), true);
                    dl->AddText(ImVec2(vx, ty), noteCol, v.note.c_str());
                    dl->PopClipRect();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            code_.focus(); // (the keyboard stays the source's)
        }
        if (clicked) {
            complete_.selected = *clicked;
            acceptCompletion(static_cast<std::size_t>(*clicked));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void ScriptPanel::valuesSection(std::uint32_t id)
{
    // Each set once, with the parameters that take it ("spell1, spell2, spell3").
    std::vector<std::pair<sc::ValueSetId, std::string>> sets;
    const sc::BuiltinDoc* d = sc::builtinDoc(id);
    const auto& sig = sc::builtinSignature(id);
    for (std::size_t i = 0; i < sig.args.size(); ++i) {
        const auto set = sc::argValueSet(id, i);
        if (!set) continue;
        const std::string name = d && i < d->params.size() ? d->params[i].name : "argument " + std::to_string(i + 1);
        const auto it = std::ranges::find_if(sets, [&](const auto& s) { return s.first == *set; });
        if (it == sets.end()) {
            sets.emplace_back(*set, name);
        } else {
            it->second += ", " + name;
        }
    }
    if (sets.empty()) {
        return;
    }
    if (valuesFor_ != id) {
        valuesFor_ = id;
        valuesSearch_[0] = 0;
        valuesSelected_.clear();
    }
    docHeading("Values");
    if (infoScrollValues_ > 0) {
        ImGui::SetScrollHereY(0.0f); // (its "Values" button: a few frames, while the layout settles)
        --infoScrollValues_;
    }
    ImGui::TextDisabled("Every value it accepts (the game's own data%s). Double-click one, or pick it and Insert at Cursor.",
                        std::ranges::any_of(sets, [](const auto& s) { return sc::isMapValueSet(s.first); }) ? " and this map's names" : "");
    ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 16.0f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##valsearch", "Search values and notes", valuesSearch_, sizeof valuesSearch_);
    ImGui::SameLine();
    const bool editable = readOnlyReason() == nullptr;
    ImGui::BeginDisabled(valuesSelected_.empty() || !editable);
    if (ImGui::SmallButton("Insert at Cursor")) {
        insertValue(valuesSelectedSet_, valuesSelected_);
    }
    ImGui::EndDisabled();
    help("Type the picked value into the Script panel at the caret: into the string the caret is in when it takes these values, else in quotes.");
    ImGui::SameLine();
    ImGui::BeginDisabled(valuesSelected_.empty());
    if (ImGui::SmallButton("Copy##value")) {
        ImGui::SetClipboardText(valuesSelected_.c_str());
    }
    ImGui::EndDisabled();
    help("Copy the picked value.");
    const ImGuiStyle& style = ImGui::GetStyle();
    for (const auto& [setId, params] : sets) {
        const sc::ValueSet* set = argValues().get(setId);
        const std::string_view title = sc::valueSetTitle(setId);
        ImGui::Spacing();
        ImGui::PushFont(ed_.imgui_->monoFont(), 0.0f);
        ImGui::TextColored(editor::themeColours().param, "%s", params.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextUnformatted(title.data(), title.data() + title.size());
        if (!set || set->empty()) {
            ImGui::TextDisabled(sc::isMapValueSet(setId) ? "None on this map yet." : "Not available: the game's data isn't loaded.");
            continue;
        }
        const auto rows = set->filter(valuesSearch_, SIZE_MAX);
        ImGui::SameLine();
        ImGui::TextDisabled(" %zu of %zu", rows.size(), set->values().size());
        const bool icons = setId == sc::ValueSetId::ThingType || setId == sc::ValueSetId::ObjectName;
        const float lineH = ImGui::GetTextLineHeight();
        const float rowH = std::floor(icons ? lineH * 1.7f : lineH);
        const int shown = std::clamp(static_cast<int>(rows.size()), 1, 12);
        ImGui::BeginChild(("##values" + std::to_string(static_cast<int>(setId))).c_str(),
                          ImVec2(0, static_cast<float>(shown) * (rowH + style.ItemSpacing.y) + style.WindowPadding.y * 2), ImGuiChildFlags_Borders);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 noteCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        ImFont* mono = ed_.imgui_->monoFont();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()), rowH + style.ItemSpacing.y);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const sc::ArgValue& v = set->values()[rows[static_cast<std::size_t>(i)]];
                ImGui::PushID(i);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const bool selected = valuesSelectedSet_ == setId && valuesSelected_ == v.value;
                const bool clicked = ImGui::Selectable("##v", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH));
                if (icons) {
                    valueIcon(dl, v, at, rowH);
                }
                if (clicked) {
                    valuesSelected_ = v.value;
                    valuesSelectedSet_ = setId;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editable) {
                        insertValue(setId, v.value);
                    }
                }
                if (!v.note.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s\n%s", v.value.c_str(), v.note.c_str());
                }
                const float tx = at.x + (icons ? rowH + style.ItemSpacing.x : style.FramePadding.x);
                const float ty = at.y + (rowH - lineH) * 0.5f;
                dl->AddText(mono, ImGui::GetFontSize(), ImVec2(tx, ty), textCol, v.value.c_str());
                if (!v.note.empty()) {
                    const float vx = tx + mono->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, v.value.c_str()).x + ImGui::GetFontSize();
                    dl->PushClipRect(ImVec2(vx, at.y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), at.y + rowH), true);
                    dl->AddText(ImVec2(vx, ty), noteCol, v.note.c_str());
                    dl->PopClipRect();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
}

void ScriptPanel::menus()
{
    if (ImGui::BeginMenu("Script")) {
        for (const char* id : {"script.compile", "script.runTests"}) ed_.menuItem(id);
        ImGui::Separator();
        for (const char* id : {"script.reload", "script.restartScripts", "script.restartMap"}) ed_.menuItem(id);
        ImGui::Separator();
        for (const char* id : {"script.decompile", "script.importObj", "script.reimportSave", "script.exportNs", "script.exportObj", "script.watch"}) {
            ed_.menuItem(id);
        }
        ImGui::Separator();
        for (const char* id : {"script.fontBigger", "script.fontSmaller", "script.fontReset"}) ed_.menuItem(id);
        ImGui::Separator();
        for (const char* id : {"window.script", "window.scriptReference", "window.functionInfo"}) ed_.menuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        ed_.menuItem("play.start");
        ed_.menuItem("play.here");
        ed_.menuItem("play.stop");
        ImGui::Separator();
        ed_.menuItem("play.playersCanDie");
        if (ImGui::BeginMenu("Game Mode")) {
            const std::uint32_t def = defaultMode();
            if (ImGui::MenuItem((std::string("The map's own (") + modeName(def) + ")").c_str(), nullptr, playMode_ == 0)) playMode_ = 0;
            ImGui::Separator();
            for (const Mode& m : kModes) {
                if (m.kind == editor::PlayKind::Solo) ImGui::Separator();
                const bool offered = (ed_.doc_.info.gameFlags & m.mapBit) != 0;
                if (ImGui::MenuItem(m.name, offered ? "map's" : nullptr, playMode_ == m.host)) playMode_ = m.host;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    if (m.kind == editor::PlayKind::Solo) {
                        ImGui::SetTooltip("The campaign's solo game: the map hosted as the main menu's Solo hosts one, a new character "
                                          "of the class chosen below, its saves in the play-test's folder.");
                    } else if (m.kind == editor::PlayKind::Quest) {
                        ImGui::SetTooltip("A quest, as Solo Quest plays one: the chat map, then OTQuest.rul begins the quest, whose "
                                          "only map is this one (its saves in the play-test's folder).");
                    }
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Character Class")) {
            const int byName = classOfMapName(ed_.doc_.origin.name);
            const std::string own = byName >= 0 ? std::string("The map's (") + kClassNames[byName] + ")" : std::string("The player's own");
            if (ImGui::MenuItem(own.c_str(), nullptr, playClass_ < 0)) playClass_ = -1;
            ImGui::Separator();
            for (int c = 0; c < 3; ++c) {
                if (ImGui::MenuItem(kClassNames[c], nullptr, playClass_ == c)) playClass_ = c;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
}

void ScriptPanel::problems()
{
    const bool any = !last_.diagnostics.empty() || !reloadNotes_.empty() || !decompileNotes_.empty() || (tests_ && (!tests_->tests.empty() || !tests_->error.empty()));
    if (!any) {
        return;
    }
    ImGui::SeparatorText("Script");
    if (ImGui::BeginTable("##scriptissues", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        const float s = ed_.imgui_->scale();
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * s);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 200 * s);
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        int row = 0;
        const auto line = [&](int kind, const std::string& where, const std::string& message, int goLine, int goCol) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const editor::ThemeColours& tc = editor::themeColours();
            const ImVec4 colours[] = {tc.error, tc.warning, tc.info, tc.ok};
            static const char* names[] = {"Error", "Warning", "Info", "Pass"};
            ImGui::TextColored(colours[kind], "%s", names[kind]);
            ImGui::TableNextColumn();
            if (ImGui::Selectable((where + "##s" + std::to_string(row++)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && goLine > 0) {
                showLine(goLine, goCol);
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", message.c_str());
        };
        for (const auto& d : last_.diagnostics) {
            const int kind = d.severity == sc::Diagnostic::Severity::Error ? 0 : 1;
            line(kind, d.loc.line > 0 ? "script " + std::to_string(d.loc.line) + ":" + std::to_string(d.loc.column) : "script (the map)", d.message,
                 static_cast<int>(d.loc.line), static_cast<int>(d.loc.column));
        }
        for (const auto& n : decompileNotes_) {
            line(2, "decompiler", n, 0, 0);
        }
        for (const auto& n : reloadNotes_) {
            line(n.severity == nox::server::dev::ReloadNote::Severity::Warning ? 1 : 2, "hot reload", n.message, static_cast<int>(n.loc.line),
                 static_cast<int>(n.loc.column));
        }
        if (tests_) {
            if (!tests_->error.empty()) line(0, "tests", tests_->error, 0, 0);
            for (const auto& f : tests_->startFailures) line(0, "tests: map start", f, 0, 0);
            for (const auto& t : tests_->tests) {
                const int defLine = good_ && good_->function(t.function) ? static_cast<int>(good_->function(t.function)->loc.line) : 0;
                if (t.passed()) {
                    line(3, "test " + t.function, "passed", defLine, 1);
                    continue;
                }
                if (t.stopped) line(0, "test " + t.function, "stopped: it ran too long (a loop that never ends?)", defLine, 1);
                for (const auto& f : t.failures) {
                    const int at = std::atoi(f.c_str() + (f.starts_with("line ") ? 5 : 0));
                    line(0, "test " + t.function, f, at > 0 ? at : defLine, 1);
                }
            }
        }
        ImGui::EndTable();
    }
}

} // namespace noxsdl
