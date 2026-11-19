// The map editor's panels and dialogs: the docked layout, the menus and toolbar, the status bar,
// Layers, History, Problems, Properties, Tool Options, the command palette, the keyboard
// shortcuts, the settings, and the New / Open / Save As / unsaved-changes dialogs.
#include "editor/map_editor.hpp"
#include "editor/editor_text.hpp"
#include "editor/marker_tools.hpp"
#include "editor/mod_panel.hpp"
#include "editor/name_label.hpp"
#include "editor/object_tools.hpp"
#include "editor/play_view.hpp"
#include "editor/property_panel.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/mapedit/markers.hpp"
#include "noxcore/mapedit/op_label.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (the dock builder, for the default layout)

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
namespace me = nox::mapedit;

namespace noxsdl {

namespace {

using ObjId = me::ObjId;
using editor::pathText;
using nox::util::lowered;

using editor::help;

/// MapInfo's game types, by flag.
constexpr std::pair<std::uint32_t, nox::l10n::Key> kGameTypes[] = {
    {1, NOX_TRK("editor.game_type.solo", "Solo")},
    {2, NOX_TRK("editor.game_type.quest", "Quest")},
    {4, NOX_TRK("editor.game_type.arena", "Arena")},
    {8, NOX_TRK("editor.game_type.capture_the_flag", "Capture the Flag")},
    {0x10, NOX_TRK("editor.game_type.king_of_the_realm", "King of the Realm")},
    {0x20, NOX_TRK("editor.game_type.elimination", "Elimination")},
    {0x40, NOX_TRK("editor.game_type.flag_ball", "Flag Ball")},
    {0x80000000u, NOX_TRK("editor.game_type.chat", "Chat")}};

/// A command's category as shown (the English stays the command's own: menus and panels compare it).
const std::string& categoryText(const std::string& c)
{
    if (c == "File") return NOX_TR("editor.category.file", "File");
    if (c == "Edit") return NOX_TR("editor.category.edit", "Edit");
    if (c == "Select") return NOX_TR("editor.category.select", "Select");
    if (c == "View") return NOX_TR("editor.category.view", "View");
    if (c == "Tools") return NOX_TR("editor.category.tools", "Tools");
    if (c == "Window") return NOX_TR("editor.category.window", "Window");
    if (c == "Help") return NOX_TR("editor.category.help", "Help");
    if (c == "Script") return NOX_TR("editor.category.script", "Script");
    if (c == "Play") return NOX_TR("editor.category.play", "Play");
    if (c == "Mod") return NOX_TR("editor.category.mod", "Mod");
    return c;
}

} // namespace

void MapEditor::menuItem(const char* id)
{
    const editor::Command* c = commands_.find(id);
    if (!c) {
        return;
    }
    const std::string key = commands_.shortcut(id).text();
    const bool checked = c->checked && c->checked();
    if (ImGui::MenuItem(c->label.c_str(), key.empty() ? nullptr : key.c_str(), checked, commands_.enabled(id))) {
        commands_.run(id);
    }
    help(c->help);
}

void MapEditor::buildUi()
{
    applyFloatRequest(); // (tests: a panel put out of the main window)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##editor", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    mainMenu();
    toolbar();
    const float statusH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    const ImGuiID dock = ImGui::GetID("EditorDock");
    if (resetLayout_ || !ImGui::DockBuilderGetNode(dock)) {
        resetLayout_ = false;
        defaultLayout(dock);
    }
    ImGui::DockSpace(dock, ImVec2(0, -statusH));
    statusBar();
    ImGui::End();

    mapPanel();
    objects_->panels(); // (the Content Browser and Object List)
    markers_->panels(); // (Waypoints, Polygons, Groups, Map Info, Minimap, Game Texts)
    if (showLayers_) layersPanel();
    if (showProperties_) propertiesPanel();
    if (showTool_) toolPanel();
    if (showHistory_) historyPanel();
    if (showProblems_) problemsPanel();
    scriptPanel_->draw();
    modPanel_->draw(); // (a mod project's windows, when one is open)
    dialogs();
    palette();
    if (showKeys_) keysWindow();
    if (showSettings_) settingsWindow();
    if (showEdgeRules_) edgeRulesWindow();
    if (session_) {
        if (showSession_) sessionPanel(); // (the users, requests, locks, publish, play-test)
        requestToast();
    }
    if (session_ && showChat_) {
        chatPanel(); // (the game's chat, a box to chat into it)
    } else {
        chat_.setViewing(false);
    }
    if (setupDialog_.isOpen()) {
        editor::SessionSetup chosen;
        if (setupDialog_.draw(chosen) && setupHost_) {
            chosen.modFolder = modPanel_->folderForSession(); // (an open mod goes with the session)
            setupHost_(chosen);
        }
        if (setupDialog_.takeCancelled() && setupCancelled_) {
            setupCancelled_();
        }
    }
}

void MapEditor::defaultLayout(unsigned int dockspace)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImVec2(std::max(avail.x, 400.0f), std::max(avail.y - 30, 300.0f)));
    ImGuiID centre = dockspace;
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f, nullptr, &centre);
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.40f, nullptr, &centre); // (room for the Script panel and the Content Browser)
    ImGuiID rightLow = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGuiID browser = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.64f, nullptr, &bottom);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.map", "Map"), centre);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.layers", "Layers"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.object_list", "Object List"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.content_browser", "Content Browser"), browser);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.properties", "Properties"), right);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.tool_options", "Tool Options"), rightLow);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.problems", "Problems"), bottom);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.history", "History"), bottom);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.script", "Script"), bottom); // (with the diagnostics)
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.script_reference", "Script Reference"), rightLow);
    // (The marker lists beside the Object List, map-wide ones with Properties.)
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.waypoints", "Waypoints"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.polygons", "Polygons"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.groups", "Groups"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.minimap", "Minimap"), rightLow);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.map_info", "Map Info"), right);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.game_texts", "Game Texts"), bottom);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.session", "Session"), right); // (a collaborative session's people, requests, publish)
    ImGui::DockBuilderDockWindow("###Chat", right); // (tabbed with Session; its title carries the unread count)
    // A mod project's windows (mod_panel.hpp), where they open: its code with the Script panel, its
    // form with Properties, its files with the lists, its preview over the map.
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.mod", "Mod"), right);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.mod_files", "Mod Files"), left);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.mod_code", "Mod Code"), bottom);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.mod_reference", "Mod Reference"), rightLow);
    ImGui::DockBuilderDockWindow(NOX_TRI("editor.window.mod_preview", "Mod Preview"), centre);
    ImGui::DockBuilderFinish(dockspace);
    showLayers_ = showProperties_ = showTool_ = showProblems_ = showHistory_ = true;
}

void MapEditor::mainMenu()
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.file", "File"))) {
        menuItem("file.new");
        menuItem("file.open");
        if (ImGui::BeginMenu(NOX_TRI("editor.menu.open_recent", "Open Recent"), !settings_.recent.empty())) {
            for (const std::string& r : settings_.recent) {
                if (ImGui::MenuItem(r.c_str())) {
                    const fs::path p(reinterpret_cast<const char8_t*>(r.c_str()));
                    guarded(Pending::OpenRecent, [this, p] { open(p); });
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(NOX_TRI("editor.menu.clear_recent", "Clear Recent"))) {
                settings_.recent.clear();
                saveSettings();
            }
            ImGui::EndMenu();
        }
        menuItem("file.recover");
        ImGui::Separator();
        menuItem("file.save");
        menuItem("file.saveAs");
        ImGui::Separator();
        menuItem("file.validate");
        ImGui::Separator();
        if (ctx_.hostSession && ImGui::MenuItem(NOX_TRI("editor.menu.host_session", "Host a Session..."), nullptr, false, !inSession())) {
            ctx_.hostSession(); // (a collaborative session of a map: who may edit, the sandbox...)
        }
        ImGui::Separator();
        menuItem("file.close");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.edit", "Edit"))) {
        for (const char* id : {"edit.undo", "edit.redo"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete"}) menuItem(id);
        ImGui::Separator();
        menuItem("edit.areaCopy");
        if (ImGui::BeginMenu(NOX_TRI("editor.menu.copy_as_text", "Copy as Text"))) {
            for (const char* id : {"edit.copyCoords", "edit.copyExtents", "edit.copyWaypoints"}) menuItem(id);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.select", "Select"))) {
        for (const char* id : {"select.all", "select.none", "select.invert", "select.sameType"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"select.shapeBox", "select.shape45", "select.shapeLasso"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"select.filterObjects", "select.filterWalls", "select.filterTiles", "select.filterWaypoints"}) menuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.view", "View"))) {
        for (const char* id : {"view.frame", "view.zoomIn", "view.zoomOut", "view.zoomReset"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"view.grid", "view.floor", "view.edges", "view.walls", "view.objects", "view.waypoints", "view.polygons",
                               "view.extents", "view.facing", "view.markDisabled", "view.specialWalls", "view.lighting", "view.seeThroughWalls", "view.schematic"}) {
            menuItem(id);
        }
        if (ImGui::BeginMenu(NOX_TRI("editor.menu.labels", "Labels"))) {
            for (const char* id : {"view.labelTypes", "view.labelScriptNames", "view.labelExtents"}) menuItem(id);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(NOX_TRI("editor.menu.pending_changes", "Pending Changes"))) {
            for (const char* id : {"view.pendingShaded", "view.pendingFinal", "view.pendingHidden", "view.pendingPublished"}) menuItem(id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menuItem("view.palette");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.tools", "Tools"))) {
        for (const char* id : {"tool.select", "tool.wall", "tool.floor", "tool.edge", "tool.object", "tool.waypoint", "tool.polygon", "tool.toggle"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"tool.eyedropper", "brush.bigger", "brush.smaller", "edges.recompute", "window.edgeRules"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"object.rotateLeft", "object.rotateRight", "object.scatter", "object.random", "prefab.save"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"object.changeType", "object.resetData", "extents.reorder", "waypoint.enableAll"}) menuItem(id);
        ImGui::EndMenu();
    }
    modPanel_->menu();     // (Mod: mod projects)
    scriptPanel_->menus(); // (Script, Play)
    playView_->menus();    // (more of Play: Pause, Release Mouse, Play in Full Window)
    if (ImGui::BeginMenu(NOX_TRI("editor.category.window", "Window"))) {
        for (const char* id : {"window.layers", "window.properties", "window.tool", "window.history", "window.problems", "window.browser", "window.objectList"}) menuItem(id);
        menuItem("window.chat");
        ImGui::Separator();
        for (const char* id : {"window.mapInfo", "window.waypoints", "window.polygons", "window.groups", "window.minimap", "window.strings"}) menuItem(id);
        ImGui::Separator();
        if (ImGui::BeginMenu(NOX_TRI("editor.menu.layouts", "Layouts"))) {
            std::error_code ec;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(ctx_.prefDir / "layouts", ec)) {
                if (e.path().extension() == ".ini") names.push_back(e.path().stem().string());
            }
            std::ranges::sort(names);
            for (const auto& n : names) {
                if (ImGui::MenuItem(n.c_str())) loadLayout_ = n;
            }
            if (names.empty()) ImGui::TextDisabled("%s", NOX_TRC("editor.menu.no_layouts", "No saved layouts"));
            ImGui::Separator();
            ImGui::SetNextItemWidth(140 * imgui_->scale());
            ImGui::InputTextWithHint("##layoutname", NOX_TRC("editor.menu.layout_name_hint", "name"), layoutBuf_, sizeof layoutBuf_);
            ImGui::SameLine();
            if (ImGui::Button(NOX_TRI("editor.menu.save_layout", "Save Layout")) && layoutBuf_[0] != 0 && !ctx_.screenshotRun) {
                std::size_t n = 0;
                const char* ini = ImGui::SaveIniSettingsToMemory(&n);
                fs::create_directories(ctx_.prefDir / "layouts", ec);
                std::ofstream(ctx_.prefDir / "layouts" / (std::string(layoutBuf_) + ".ini"), std::ios::binary).write(ini, static_cast<std::streamsize>(n));
                status(nox::l10n::format(NOX_TR("editor.status.layout_saved", "Layout saved as {name}"), {{"name", layoutBuf_}}));
            }
            ImGui::EndMenu();
        }
        menuItem("window.resetLayout");
        ImGui::Separator();
        menuItem("window.settings");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("editor.category.help", "Help"))) {
        menuItem("help.palette");
        menuItem("help.keys");
        ImGui::Separator();
        menuItem("help.about");
        ImGui::EndMenu();
    }
    // The theme, at the bar's right end (after the menus if the window is too narrow for both).
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float comboW = std::max(ImGui::CalcTextSize(editor::themeLabel(editor::Theme::Medium), nullptr, true).x + ImGui::GetFrameHeight() + st.FramePadding.x * 2,
                                      90 * imgui_->scale());
        const char* themeText = NOX_TRC("editor.menu.theme", "Theme");
        const float labelW = ImGui::CalcTextSize(themeText).x;
        const float x = ImGui::GetWindowWidth() - st.FramePadding.x * 2 - comboW - st.ItemSpacing.x - labelW;
        if (x > ImGui::GetCursorPosX() + st.ItemSpacing.x * 2) {
            ImGui::SetCursorPosX(x);
        }
        ImGui::TextDisabled("%s", themeText);
        ImGui::SameLine();
        themeCombo("##theme", comboW);
        help(NOX_TR("editor.menu.theme_help", "The editor's colours: Light, Medium (NoX Core's stone greys and gold) or Dark (near-black with blue accents)."));
    }
    ImGui::EndMenuBar();
}

bool MapEditor::themeCombo(const char* id, float width)
{
    if (width > 0) {
        ImGui::SetNextItemWidth(width);
    }
    if (openThemeMenu_ && std::string_view(id) == "##theme") {
        openThemeMenu_ = false;
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(id))); // (BeginCombo's popup, as a click opens it)
    }
    bool picked = false;
    if (ImGui::BeginCombo(id, editor::themeLabel(settings_.theme))) {
        for (const editor::Theme t : editor::kThemeMenuOrder) {
            const bool on = settings_.theme == t;
            if (ImGui::Selectable(editor::themeLabel(t), on) && !on) {
                setTheme(t);
                picked = true;
            }
            if (on) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return picked;
}

void MapEditor::toolbar()
{
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetCursorPosY() + 3));
    const auto button = [&](const char* label, const char* id, bool active = false) {
        const editor::Command* c = commands_.find(id);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::BeginDisabled(!commands_.enabled(id));
        if (ImGui::Button(label)) commands_.run(id);
        ImGui::EndDisabled();
        if (active) ImGui::PopStyleColor();
        if (c) {
            const std::string key = commands_.shortcut(id).text();
            help(c->label + (key.empty() ? "" : "  (" + key + ")") + (c->help.empty() ? "" : "\n" + c->help));
        }
        ImGui::SameLine();
    };
    const auto gap = [] {
        ImGui::SameLine(0, 4);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, 4);
    };
    button(NOX_TRI("editor.toolbar.new", "New"), "file.new");
    button(NOX_TRI("editor.common.open", "Open"), "file.open");
    button(NOX_TRI("editor.common.save", "Save"), "file.save");
    gap();
    button(NOX_TRI("editor.common.undo", "Undo"), "edit.undo");
    button(NOX_TRI("editor.common.redo", "Redo"), "edit.redo");
    gap();
    if (ctx_.playTest.available()) {
        playView_->toolbarButtons(); // (Play / Pause / Stop, the play state)
        gap();
    }
    button(NOX_TRI("editor.tool.select", "Select"), "tool.select", tool_ == Tool::Select);
    button(NOX_TRI("editor.tool.wall", "Wall"), "tool.wall", tool_ == Tool::Wall);
    button(NOX_TRI("editor.tool.floor", "Floor"), "tool.floor", tool_ == Tool::Floor);
    button(NOX_TRI("editor.tool.edge", "Edge"), "tool.edge", tool_ == Tool::Edge);
    button(NOX_TRI("editor.tool.object", "Object"), "tool.object", tool_ == Tool::Object);
    button(NOX_TRI("editor.tool.waypoint", "Waypoint"), "tool.waypoint", tool_ == Tool::Waypoint);
    button(NOX_TRI("editor.tool.polygon", "Polygon"), "tool.polygon", tool_ == Tool::Polygon);
    gap();
    button(NOX_TRI("editor.toolbar.box", "Box"), "select.shapeBox", settings_.regionShape == 0);
    button(NOX_TRI("editor.toolbar.box45", "45\xC2\xB0 Box"), "select.shape45", settings_.regionShape == 1);
    button(NOX_TRI("editor.toolbar.lasso", "Lasso"), "select.shapeLasso", settings_.regionShape == 2);
    gap();
    ImGui::TextUnformatted(NOX_TRC("editor.toolbar.pick", "Pick:"));
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.toolbar.pick_objects", "Objects"), &settings_.pick.objects);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.toolbar.pick_walls", "Walls"), &settings_.pick.walls);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.toolbar.pick_tiles", "Tiles"), &settings_.pick.tiles);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.toolbar.pick_waypoints", "Waypoints"), &settings_.pick.waypoints);
    gap();
    ImGui::TextUnformatted(NOX_TRC("editor.toolbar.snap", "Snap:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * imgui_->scale());
    const char* snaps[] = {NOX_TRC("editor.common.off", "Off"), NOX_TRC("editor.toolbar.snap_grid", "Grid (23)"),
                           NOX_TRC("editor.toolbar.snap_half", "Half grid"), NOX_TRC("editor.toolbar.snap_custom", "Custom")};
    ImGui::Combo("##snap", &settings_.snap, snaps, 4);
    help(NOX_TR("editor.toolbar.snap_help", "Where dragged objects and waypoints land. Walls and floor tiles always move by whole cells."));
    if (settings_.snap == 3) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60 * imgui_->scale());
        ImGui::DragFloat("##snapn", &settings_.snapCustom, 0.5f, 1, 230, "%.0f");
    }
    gap();
    button(NOX_TRI("editor.toolbar.validate", "Validate"), "file.validate");
    if (session_) {
        // The session bar: back to the game, publish (the host), ask to edit (others).
        gap();
        if (ImGui::Button(NOX_TRI("editor.toolbar.play_f2", "Play (F2)")) && sessionHooks_.toGame) sessionHooks_.toGame();
        help(NOX_TR("editor.toolbar.play_f2_help", "Back to the running game (the editor waits; F2 comes back)."));
        ImGui::SameLine();
        if (session_->isOwner()) {
            if (ImGui::Button(NOX_TRI("editor.toolbar.publish", "Publish"))) sessionPublish();
            help(NOX_TR("editor.toolbar.publish_help",
                        "Write the working copy as the session map's new version and change the map to it (walls, floors and the script's bytecode reach the game; stock clients download it)."));
        } else if (session_->role() < nox::mapedit::session::Role::Editor) {
            if (ImGui::Button(NOX_TRI("editor.toolbar.request_edit", "Request edit"))) session_->requestEdit(requestReason_);
            help(NOX_TR("editor.toolbar.request_edit_help", "Ask the host for edit rights (or type /edit <reason> in the game's chat)."));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", session_->isOwner() ? NOX_TRC("editor.toolbar.role_host", "Host")
                                  : session_->role() >= nox::mapedit::session::Role::Editor ? NOX_TRC("editor.toolbar.role_editor", "Editor")
                                                                                             : NOX_TRC("editor.toolbar.role_viewer", "Viewer"));
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void MapEditor::statusBar()
{
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetStyle().ItemSpacing.x);
    const std::string* const kTools[] = {&NOX_TR("editor.tool.select", "Select"), &NOX_TR("editor.tool.wall", "Wall"),
                                         &NOX_TR("editor.tool.floor", "Floor"),   &NOX_TR("editor.tool.edge", "Edge"),
                                         &NOX_TR("editor.tool.object", "Object"), &NOX_TR("editor.tool.waypoint", "Waypoint"),
                                         &NOX_TR("editor.tool.polygon", "Polygon")};
    std::string line = *kTools[static_cast<int>(tool_)];
    if (areaCopyArmed_) line += " " + NOX_TR("editor.status.copy_area", "(copy area)");
    if (const auto w = mouseWorld()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "  %.0f, %.0f", w->x, w->y);
        line += buf;
        if (const auto c = me::wallCellAt(w->x, w->y)) {
            line += "  " + nox::l10n::format(NOX_TR("editor.status.wall_cell", "wall {x},{y}"), {{"x", c->x}, {"y", c->y}});
        }
        if (const auto t = me::tileAt(w->x, w->y)) {
            line += "  " + nox::l10n::format(NOX_TR("editor.status.tile_cell", "tile {x},{y}"), {{"x", t->gx}, {"y", t->gy}});
        }
        const std::string under = underCursor(w->x, w->y);
        if (!under.empty()) line += "  |  " + under;
    }
    ImGui::TextUnformatted(line.c_str());
    // Right-aligned: the selection, the save state, the last message.
    std::string right;
    if (!selection_.empty()) right += nox::l10n::format(NOX_TR("editor.common.n_selected", "{n} selected"), {{"n", selection_.size()}}) + "   ";
    if (session_) {
        right += nox::l10n::format(NOX_TR("editor.status.session", "Session {name}"), {{"name", docName_}});
        if (session_->pendingOps()) {
            right += "  " + nox::l10n::format(NOX_TR("editor.status.sending", "({n} sending)"), {{"n", session_->pendingOps()}});
        }
    } else {
        right += history_.dirty() ? NOX_TR("editor.status.unsaved", "Unsaved changes")
                                  : (docPath_.empty() ? NOX_TR("editor.status.new_map", "New map") : NOX_TR("editor.status.no_unsaved", "No unsaved changes"));
    }
    if (validating_.valid()) right += "   " + NOX_TR("editor.status.validating", "Validating...");
    if (!statusText_.empty() && SDL_GetTicks() - statusMs_ < 8000) right = statusText_ + "     " + right;
    const float w = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 12));
    ImGui::TextUnformatted(right.c_str());
}

void MapEditor::layersPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.layers", "Layers"), &showLayers_)) {
        ImGui::End();
        return;
    }
    editor::Layers& L = settings_.layers;
    const auto rebuild = [&](const char* label, bool* v, const char* tip) {
        if (ImGui::Checkbox(label, v)) ++layerRevision_;
        help(tip);
    };
    ImGui::SeparatorText(NOX_TRC("editor.layers.map", "Map"));
    rebuild(NOX_TRI("editor.layers.floor", "Floor"), &L.floor, NOX_TRC("editor.layers.floor_help", "The floor tiles."));
    rebuild(NOX_TRI("editor.layers.edges", "Floor edges"), &L.edges, NOX_TRC("editor.layers.edges_help", "The edges blended over tiles."));
    rebuild(NOX_TRI("editor.layers.walls", "Walls"), &L.walls, "");
    rebuild(NOX_TRI("editor.layers.objects", "Objects"), &L.objects, "");
    ImGui::Checkbox(NOX_TRI("editor.layers.waypoints", "Waypoints"), &L.waypoints);
    ImGui::Checkbox(NOX_TRI("editor.layers.polygons", "Polygons"), &L.polygons);
    ImGui::Checkbox(NOX_TRI("editor.layers.lighting", "Lighting preview"), &L.lighting);
    help(NOX_TR("editor.layers.lighting_help", "The map's lights and ambient colour, as the game lights it."));
    ImGui::Checkbox(NOX_TRI("editor.layers.see_through", "See-through walls near the view centre"), &L.seeThroughWalls);
    help(NOX_TR("editor.layers.see_through_help", "Walls below the view's centre drawn half see-through over everything, as the game draws them. Off, every wall is "
                                                  "drawn whole and opaque."));
    ImGui::SeparatorText(NOX_TRC("editor.layers.overlays", "Overlays"));
    const char* grids[] = {NOX_TRC("editor.common.off", "Off"), NOX_TRC("editor.layers.grid_cells", "Wall cells"), NOX_TRC("editor.layers.grid_tiles", "Floor tiles")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##grid", &L.grid, grids, 3);
    help(NOX_TR("editor.layers.grid_help", "The grid: wall cells (23 x 23) or the floor's diamonds."));
    const char* extents[] = {NOX_TRC("editor.layers.extents_hidden", "Extents: hidden"), NOX_TRC("editor.layers.extents_colliding", "Extents: colliding"),
                             NOX_TRC("editor.layers.extents_all", "Extents: all")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##extents", &L.extents, extents, 3);
    help(NOX_TR("editor.layers.extents_help", "Objects' collision shapes (thing.bin EXTENT)."));
    ImGui::Checkbox(NOX_TRI("editor.layers.facing", "Facing arrows"), &L.facing);
    help(NOX_TR("editor.layers.facing_help", "Creatures' and NPCs' facing, doors' hanging and sentries' beams."));
    ImGui::Checkbox(NOX_TRI("editor.layers.mark_disabled", "Mark disabled objects"), &L.markDisabled);
    ImGui::Checkbox(NOX_TRI("editor.layers.schematic", "Schematic"), &L.schematic);
    help(NOX_TR("editor.layers.schematic_help", "Flat colours over the map: walls by kind (secret blue, breakable red, window cyan), objects as dots."));
    ImGui::Checkbox(NOX_TRI("editor.layers.special_walls", "Tint special walls"), &L.specialWalls);
    help(NOX_TR("editor.layers.special_walls_help", "Secret walls blue, breakable red, windows cyan, over the drawn map."));
    ImGui::Checkbox(NOX_TRI("editor.layers.triggers", "Trigger and plate boxes"), &L.triggers);
    help(NOX_TR("editor.layers.triggers_help", "Triggers' and pressure plates' areas (their size, turned 45 degrees), in their colours."));
    ImGui::Checkbox(NOX_TRI("editor.layers.light_rings", "Light radius rings"), &L.lightRings);
    help(NOX_TR("editor.layers.light_rings_help", "Each invisible light's radius, in its colour."));
    ImGui::Checkbox(NOX_TRI("editor.layers.links", "Links"), &L.links);
    help(NOX_TR("editor.layers.links_help", "Lines from teleporters, elevators and shafts to their partners, movers to their waypoints and what they carry, glyphs "
                                            "to their targets, exits and pits to their points."));
    ImGui::Checkbox(NOX_TRI("editor.layers.extents3d", "3D extents"), &L.extents3d);
    ImGui::Checkbox(NOX_TRI("editor.layers.live_players", "Live players"), &L.livePlayers);
    help(NOX_TR("editor.layers.live_players_help", "In a session: the players in the running game where they are now (name, team colour, stock or NoX Core client). View only."));
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.layers.live_creatures", "Live creatures"), &L.liveCreatures);
    help(NOX_TR("editor.layers.live_creatures_help", "In a session: the running game's creatures and moving things. View only."));
    help(NOX_TR("editor.layers.extents3d_help", "Extents drawn with a raised copy at the type's height (with Extents shown)."));
    const char* pendingModes[] = {NOX_TRC("editor.layers.pending_shaded", "Pending changes: shaded"), NOX_TRC("editor.layers.pending_final", "Pending changes: as final"),
                                  NOX_TRC("editor.layers.pending_hidden", "Pending changes: hidden")};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##pending", &L.pendingView, pendingModes, 3);
    help(NOX_TR("editor.layers.pending_help", "In a session: walls, floor and client-only things not yet published. Shaded: greyed, and what they remove as see-through "
                                              "hatched ghosts; As final: as they will look once published; Hidden: only what the game has (G flips to it and back)."));
    ImGui::SeparatorText(NOX_TRC("editor.layers.labels", "Labels"));
    ImGui::Checkbox(NOX_TRI("editor.layers.label_types", "Type names"), &L.labelTypes);
    ImGui::Checkbox(NOX_TRI("editor.layers.label_script_names", "Script names"), &L.labelScriptNames);
    ImGui::Checkbox(NOX_TRI("editor.layers.label_extents", "Extents"), &L.labelExtents);
    ImGui::SeparatorText(NOX_TRC("editor.layers.locked", "Locked layers"));
    ImGui::TextDisabled("%s", NOX_TRC("editor.layers.locked_note", "Shown, but not picked or changed by the tools:"));
    ImGui::Checkbox(NOX_TRI("editor.layers.lock_floor", "Floor##lock"), &L.lockFloor);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.layers.lock_walls", "Walls##lock"), &L.lockWalls);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.layers.lock_objects", "Objects##lock"), &L.lockObjects);
    ImGui::Checkbox(NOX_TRI("editor.layers.lock_waypoints", "Waypoints##lock"), &L.lockWaypoints);
    ImGui::SameLine();
    ImGui::Checkbox(NOX_TRI("editor.layers.lock_polygons", "Polygons##lock"), &L.lockPolygons);
    ImGui::End();
}

void MapEditor::historyPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.history", "History"), &showHistory_)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button(NOX_TRI("editor.common.undo", "Undo"))) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button(NOX_TRI("editor.common.redo", "Redo"))) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.history.changes", "{n} changes"), {{"n", history_.entries().size()}}).c_str());
    ImGui::BeginChild("##hist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const auto& entries = history_.entries();
    if (ImGui::Selectable(history_.dropped() ? NOX_TRI("editor.history.dropped", "(older changes dropped)") : NOX_TRI("editor.history.opened", "(opened)"),
                          history_.position() == 0)) {
        jumpHistory(0);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool undone = i >= history_.position();
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string label = std::to_string(history_.dropped() + i + 1) + ". " + me::shownOpLabel(entries[i].op.label) + "##h" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), history_.position() == i + 1)) {
            jumpHistory(i + 1);
        }
        if (undone) ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void MapEditor::problemsPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.problems", "Problems"), &showProblems_)) {
        ImGui::End();
        return;
    }
    const auto errors = std::ranges::count_if(issues_, [](const me::Issue& i) { return i.severity == me::Severity::Error; });
    const auto warnings = static_cast<std::ptrdiff_t>(issues_.size()) - errors;
    if (validating_.valid()) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.status.validating", "Validating..."));
    } else if (issues_.empty()) {
        ImGui::TextColored(editor::themeColours().ok, "%s", NOX_TRC("editor.problems.none", "No problems: the game will load this map."));
    } else {
        ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.problems.counts", "{errors} errors, {warnings} warnings"),
                                                 {{"errors", errors}, {"warnings", warnings}})
                                   .c_str());
        if (errors > 0) {
            ImGui::SameLine();
            ImGui::TextColored(editor::themeColours().error, "%s", NOX_TRC("editor.problems.save_refused", "(saving is refused until the errors are fixed)"));
        }
    }
    scriptPanel_->problems(); // (the script's diagnostics, reload notes, tests)
    modPanel_->problems();    // (an open mod project's mode.json and Luau diagnostics)
    if (session_ && !session_->problems().empty()) {
        // The session's problems: what a live change couldn't keep for the running game (a script
        // holding an object made again...), till it's fixed or published.
        ImGui::SeparatorText(NOX_TRC("editor.problems.session", "Session"));
        for (const auto& [key, pr] : session_->problems()) {
            ImGui::TextColored(editor::themeColours().warning, "%s", NOX_TRC("editor.problems.session", "Session"));
            ImGui::SameLine();
            if (ImGui::Selectable((pr.text + "##sp" + std::to_string(key)).c_str())) {
                if (const auto it = doc_.objects.find(key); it != doc_.objects.end()) {
                    me::Selection s;
                    s.objects.insert(key);
                    setSelection(s);
                    camera_.x = it->second.header.x;
                    camera_.y = it->second.header.y;
                    cameraSet_ = true;
                }
            }
            help(NOX_TR("editor.problems.show_object_help", "Click to show the object on the map."));
        }
    }
    if (ImGui::BeginTable("##issues", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * imgui_->scale());
        ImGui::TableSetupColumn(NOX_TRI("editor.problems.where", "Where"), ImGuiTableColumnFlags_WidthFixed, 200 * imgui_->scale());
        ImGui::TableSetupColumn(NOX_TRI("editor.problems.problem", "Problem"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < issues_.size(); ++i) {
            const auto& issue = issues_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool error = issue.severity == me::Severity::Error;
            ImGui::TextColored(error ? editor::themeColours().error : editor::themeColours().warning, "%s",
                               error ? NOX_TRC("editor.problems.error", "Error") : NOX_TRC("editor.problems.warning", "Warning"));
            ImGui::TableNextColumn();
            if (ImGui::Selectable((issue.where + "##p" + std::to_string(i)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                locate(issue);
            }
            help(NOX_TR("editor.problems.show_help", "Click to show it on the map."));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(issue.message.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void MapEditor::propertiesPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.properties", "Properties"), &showProperties_)) {
        propsTyping_ = false;
        ImGui::End();
        return;
    }
    // A field typed in last frame whose selection a click elsewhere changed: this frame the click
    // ends the typing and the field commits what was typed (ImGui hands the text back as the field
    // is drawn), so the panel shows what it belonged to once more. Drawn for the new selection,
    // the edit went to the newly selected object (the same field's id) or was lost.
    const bool previous = propsTyping_ && !(propsShown_ == selection_);
    if (previous) std::swap(selection_, propsShown_);
    propertiesContent();
    if (previous) {
        std::swap(selection_, propsShown_);
        selection_.prune(doc_);
    }
    propsShown_ = selection_;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    propsTyping_ = g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId && g.ActiveIdWindow &&
                   g.ActiveIdWindow->RootWindow == ImGui::GetCurrentWindow()->RootWindow;
    ImGui::End();
}

void MapEditor::propertiesContent()
{
    const me::Selection& s = selection_;
    const bool onlyObjects = !s.objects.empty() && s.walls.empty() && s.tiles.empty() && s.waypoints.empty() && s.polygons.empty();
    const bool onlyWalls = !s.walls.empty() && s.objects.empty() && s.tiles.empty() && s.waypoints.empty() && s.polygons.empty();
    const bool onlyWaypoints = !s.waypoints.empty() && s.objects.empty() && s.walls.empty() && s.tiles.empty() && s.polygons.empty();
    const bool onlyPolygons = !s.polygons.empty() && s.objects.empty() && s.walls.empty() && s.tiles.empty() && s.waypoints.empty();
    if (s.empty()) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.selinfo.nothing", "Nothing selected."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", nox::l10n::format(NOX_TR("editor.selinfo.map", "Map: {title}"), {{"title", me::fromMapText(doc_.info.title.view())}}).c_str());
        ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.selinfo.counts", "{walls} walls, {tiles} floor tiles, {objects} objects, {waypoints} waypoints, "
                                                                                    "{polygons} polygons, {groups} groups"),
                                                    {{"walls", doc_.walls.size()},
                                                     {"tiles", doc_.floor.tileCount()},
                                                     {"objects", doc_.objects.size()},
                                                     {"waypoints", doc_.waypoints.waypoints.size()},
                                                     {"polygons", doc_.polygons.polygons.size()},
                                                     {"groups", doc_.groups.groups.size()}})
                                      .c_str());
        ImGui::Spacing();
        if (ImGui::Button(NOX_TRI("editor.selinfo.map_info", "Map Info..."))) commands_.run("window.mapInfo");
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.selinfo.groups", "Groups..."))) commands_.run("window.groups");
    } else if (onlyObjects) {
        props_->objects(s.objects); // (one object or many of a kind: every field, as ops)
    } else if (onlyWalls) {
        wallProperties(s.walls); // (one wall or many: every field applies to all)
    } else if (onlyWaypoints) {
        markers_->waypointProperties();
    } else if (onlyPolygons) {
        markers_->polygonProperties();
    } else if (s.size() == 1 && !s.tiles.empty()) {
        const float labelW = 90 * imgui_->scale();
        const auto row = [&](const char* label) { editor::labelRow(label, labelW); };
        const me::TileCell t = *s.tiles.begin();
        if (const auto* tile = doc_.floor.tile(t.gx, t.gy)) {
            ImGui::SeparatorText(tile->image < things_.floors.size() ? things_.floors[tile->image].name.c_str() : NOX_TRC("editor.selinfo.floor", "Floor"));
            row(NOX_TRC("editor.selinfo.tile", "Tile"));
            ImGui::Text("%d, %d", t.gx, t.gy);
            row(NOX_TRC("editor.selinfo.variant", "Variant"));
            ImGui::Text("%d", tile->variant);
            row(NOX_TRC("editor.selinfo.edges", "Edges"));
            ImGui::Text("%zu", tile->edges.size());
            for (const auto& e : tile->edges) {
                ImGui::BulletText("%s", nox::l10n::format(NOX_TR("editor.selinfo.edge_over", "{edge} over {floor}, direction {direction}"),
                                                          {{"edge", e.edge < things_.edges.size() ? things_.edges[e.edge].name.c_str() : "?"},
                                                           {"floor", e.floor < things_.floors.size() ? things_.floors[e.floor].name.c_str() : "?"},
                                                           {"direction", static_cast<int>(e.direction)}})
                                            .c_str());
            }
        }
    } else {
        ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.common.n_selected", "{n} selected"), {{"n", s.size()}}).c_str());
        const auto bullet = [](const std::string& pattern, std::size_t n) { ImGui::BulletText("%s", nox::l10n::format(pattern, {{"n", n}}).c_str()); };
        if (!s.objects.empty()) bullet(NOX_TR("editor.selinfo.n_objects", "{n} objects"), s.objects.size());
        if (!s.walls.empty()) bullet(NOX_TR("editor.selinfo.n_walls", "{n} walls"), s.walls.size());
        if (!s.tiles.empty()) bullet(NOX_TR("editor.selinfo.n_tiles", "{n} floor tiles"), s.tiles.size());
        if (!s.waypoints.empty()) bullet(NOX_TR("editor.selinfo.n_waypoints", "{n} waypoints"), s.waypoints.size());
        if (!s.polygons.empty()) bullet(NOX_TR("editor.selinfo.n_polygons", "{n} polygons"), s.polygons.size());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", NOX_TRC("editor.selinfo.one_kind", "Properties edit one kind at a time: select only objects, walls, waypoints or polygons."));
        if (!s.objects.empty() && ImGui::Button(NOX_TRI("editor.selinfo.keep_objects", "Keep only the objects"))) {
            me::Selection o;
            o.objects = s.objects;
            setSelection(std::move(o));
        }
    }
}

void MapEditor::toolPanel()
{
    if (!ImGui::Begin(NOX_TRI("editor.window.tool_options", "Tool Options"), &showTool_)) {
        ImGui::End();
        return;
    }
    switch (tool_) {
    case Tool::Select: {
        ImGui::SeparatorText(NOX_TRC("editor.tool.select", "Select"));
        ImGui::TextWrapped("%s", NOX_TRC("editor.select_tool.help", "Click to select; Shift-click adds or toggles, Ctrl-click removes. Drag on empty map to select an area, "
                                                                    "drag a selected item to move the selection. Middle- or right-drag pans, the wheel zooms."));
        ImGui::RadioButton(NOX_TRI("editor.toolbar.box", "Box"), &settings_.regionShape, 0);
        ImGui::SameLine();
        ImGui::RadioButton(NOX_TRI("editor.select_tool.box45", "45\xC2\xB0 box"), &settings_.regionShape, 1);
        ImGui::SameLine();
        ImGui::RadioButton(NOX_TRI("editor.toolbar.lasso", "Lasso"), &settings_.regionShape, 2);
        ImGui::SeparatorText(NOX_TRC("editor.select_tool.copy_area", "Copy area"));
        ImGui::TextWrapped("%s", NOX_TRC("editor.select_tool.copy_area_note", "Takes everything in a dragged box, of the layers ticked:"));
        ImGui::Checkbox(NOX_TRI("editor.select_tool.copy_walls", "Walls##ca"), &settings_.copyLayers.walls);
        ImGui::SameLine();
        ImGui::Checkbox(NOX_TRI("editor.select_tool.copy_floor", "Floor##ca"), &settings_.copyLayers.tiles);
        ImGui::SameLine();
        ImGui::Checkbox(NOX_TRI("editor.select_tool.copy_objects", "Objects##ca"), &settings_.copyLayers.objects);
        ImGui::SameLine();
        ImGui::Checkbox(NOX_TRI("editor.select_tool.copy_waypoints", "Waypoints##ca"), &settings_.copyLayers.waypoints);
        if (ImGui::Button(areaCopyArmed_ ? NOX_TRI("editor.select_tool.drag_box", "Drag a box on the map...")
                                         : NOX_TRI("editor.select_tool.copy_area_button", "Copy Area"))) {
            commands_.run("edit.areaCopy");
        }
        help(NOX_TR("editor.select_tool.copy_area_help", "The copy goes to the system clipboard too, so it pastes into another map (Ctrl+V)."));
        ImGui::SeparatorText(NOX_TRC("editor.select_tool.move", "Move"));
        ImGui::TextWrapped("%s", NOX_TRC("editor.select_tool.move_help", "Arrows nudge one unit, Shift+arrows one cell. Objects and waypoints snap as the toolbar's Snap says; "
                                                                         "walls and floor tiles move by whole cells, keeping their grid."));
        ImGui::TextDisabled("%s", NOX_TRC("editor.select_tool.gizmos", "Move and rotate gizmos come with the property panels (P2d)."));
        break;
    }
    case Tool::Wall: wallToolPanel(); break;
    case Tool::Floor: floorToolPanel(); break;
    case Tool::Edge: edgeToolPanel(); break;
    case Tool::Object: objects_->toolPanel(); break;
    case Tool::Waypoint:
    case Tool::Polygon: markers_->toolPanel(); break;
    }
    ImGui::End();
}

// --- Dialogs ----------------------------------------------------------------------------------

void MapEditor::refreshMapList()
{
    mapList_.clear();
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(ctx_.mapsDir, ec)) {
        if (!dir.is_directory(ec)) {
            continue;
        }
        const std::string name = dir.path().filename().string();
        for (const auto& f : fs::directory_iterator(dir.path(), ec)) {
            if (lowered(f.path().filename().string()) == lowered(name + ".map")) {
                mapList_.push_back({name, f.path(), me::isStockMapName(name)});
                break;
            }
        }
    }
    std::ranges::sort(mapList_, [](const MapEntry& a, const MapEntry& b) {
        return a.stock != b.stock ? !a.stock : lowered(a.name) < lowered(b.name);
    });
}

void MapEditor::dialogs()
{
    startDialog();
    newDialog();
    openDialog();
    saveAsDialog();
    unsavedDialog();
    endSessionDialog();
    if (!errorText_.empty()) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.error.title", "Problem##err"));
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.dialog.error.title", "Problem##err"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(520 * imgui_->scale());
        ImGui::TextUnformatted(errorText_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(NOX_TRI("editor.common.ok", "OK"), ImVec2(100 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            errorText_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (showAbout_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.about.title", "About the Map Editor"));
        showAbout_ = false;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.dialog.about.title", "About the Map Editor"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(NOX_TRC("editor.dialog.about.name", "NoX Core Map Editor (shell, P2a)"));
        ImGui::TextDisabled("%s", NOX_TRC("editor.dialog.about.maps", "Maps are written as the game reads them; stock maps are saved under new names."));
        ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.dialog.about.panels", "Panels: Dear ImGui {version} (docking), MIT."), {{"version", IMGUI_VERSION}}).c_str());
        if (ImGui::Button(NOX_TRI("editor.common.close", "Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void MapEditor::startDialog()
{
    if (startOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.start.title", "Map Editor##start"));
        startOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440 * imgui_->scale(), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.start.title", "Map Editor##start"), nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }
    const ImVec2 wide(-FLT_MIN, 0);
    if (ImGui::Button(NOX_TRI("editor.dialog.start.new", "New Map..."), wide)) {
        ImGui::CloseCurrentPopup();
        newOpen_ = true;
    }
    if (ImGui::Button(NOX_TRI("editor.dialog.start.open", "Open Map..."), wide)) {
        ImGui::CloseCurrentPopup();
        refreshMapList();
        openOpen_ = true;
    }
    if (!settings_.recent.empty()) {
        ImGui::SeparatorText(NOX_TRC("editor.dialog.start.recent", "Recent"));
        for (int i = 0; i < static_cast<int>(settings_.recent.size()); ++i) {
            const std::string& r = settings_.recent[static_cast<std::size_t>(i)];
            if (editor::verbatimSelectable(i, r, false)) {
                ImGui::CloseCurrentPopup();
                open(fs::path(reinterpret_cast<const char8_t*>(r.c_str())));
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button(NOX_TRI("editor.dialog.start.blank", "Start with a blank map"))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::newDialog()
{
    if (newOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.new_map.title", "New Map"));
        newOpen_ = false;
    }
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.new_map.title", "New Map"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    static char title[64] = "Untitled";
    ImGui::InputText(NOX_TRI("editor.dialog.new_map.map_title", "Title"), title, sizeof title);
    static const int sizes[] = {0, 16, 32, 48, 64, 96, 128, 192, 254};
    int sizeIndex = static_cast<int>(std::ranges::find(sizes, newOptions_.size) - std::begin(sizes));
    const auto sizeText = [](int n) {
        return n == 0 ? NOX_TR("editor.dialog.new_map.no_floor", "No floor")
                      : nox::l10n::format(NOX_TR("editor.dialog.new_map.cells", "{n} x {n} cells"), {{"n", n}});
    };
    const std::string sizeLabel = sizeText(newOptions_.size);
    if (ImGui::BeginCombo(NOX_TRI("editor.dialog.new_map.floored_area", "Floored area"), sizeLabel.c_str())) {
        for (int i = 0; i < static_cast<int>(std::size(sizes)); ++i) {
            const std::string l = sizeText(sizes[i]);
            if (ImGui::Selectable(l.c_str(), i == sizeIndex)) newOptions_.size = sizes[i];
        }
        ImGui::EndCombo();
    }
    help(NOX_TR("editor.dialog.new_map.floored_area_help", "A square of floor in the middle of the map (the whole map is 256 x 256 wall cells)."));
    const char* floorName = newOptions_.floor < things_.floors.size() ? things_.floors[newOptions_.floor].name.c_str() : "?";
    if (ImGui::BeginCombo(NOX_TRI("editor.dialog.new_map.base_floor", "Base floor"), floorName, ImGuiComboFlags_HeightLarge)) {
        static char filter[64];
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##ff", NOX_TRC("editor.common.search_hint", "search"), filter, sizeof filter);
        for (std::size_t i = 0; i < things_.floors.size(); ++i) {
            const auto& name = things_.floors[i].name;
            if (filter[0] && lowered(name).find(lowered(filter)) == std::string::npos) continue;
            if (ImGui::Selectable(name.c_str(), i == newOptions_.floor)) newOptions_.floor = static_cast<std::uint8_t>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText(NOX_TRC("editor.dialog.new_map.game_types", "Game types"));
    for (const auto& [bit, name] : kGameTypes) {
        bool on = (newOptions_.gameFlags & bit) != 0;
        if (ImGui::Checkbox(nox::l10n::trImGui(name), &on)) newOptions_.gameFlags = on ? (newOptions_.gameFlags | bit) : (newOptions_.gameFlags & ~bit);
    }
    ImGui::Checkbox(NOX_TRI("editor.dialog.new_map.player_start", "Player start in the middle"), &newOptions_.playerStart);
    help(NOX_TR("editor.dialog.new_map.player_start_help", "A multiplayer map needs at least one PlayerStart."));
    ImGui::Separator();
    if (ImGui::Button(NOX_TRI("editor.dialog.new_map.create", "Create"), ImVec2(110 * imgui_->scale(), 0))) {
        newOptions_.title = title;
        newDocument(newOptions_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(110 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::openDialog()
{
    static int chosen = -1; // (a row of mapList_, refreshed as the dialog opens: none chosen then)
    if (openOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.open_map.title", "Open Map"));
        openOpen_ = false;
        searchBuf_[0] = 0;
        chosen = -1;
    }
    ImGui::SetNextWindowSize(ImVec2(460 * imgui_->scale(), 520 * imgui_->scale()), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.open_map.title", "Open Map"), nullptr)) {
        return;
    }
    ImGui::TextDisabled("%s", pathText(ctx_.mapsDir).c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", NOX_TRC("editor.dialog.open_map.search_hint", "Search maps"), searchBuf_, sizeof searchBuf_);
    std::optional<fs::path> go;
    ImGui::BeginChild("##maps", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const std::string q = lowered(searchBuf_);
    bool chosenShown = false; // (Open opens only a row the search shows)
    for (int i = 0; i < static_cast<int>(mapList_.size()); ++i) {
        const auto& e = mapList_[static_cast<std::size_t>(i)];
        if (!q.empty() && lowered(e.name).find(q) == std::string::npos) continue;
        chosenShown = chosenShown || chosen == i;
        const std::string label = e.name + (e.stock ? "   " + NOX_TR("editor.dialog.open_map.stock_note", "(stock: saved under a new name)") : std::string());
        // (Drawn verbatim: a map's name can hold '#'; each row its own id, names repeating or not.)
        if (editor::verbatimSelectable(i, label, chosen == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            chosen = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = e.path;
        }
    }
    if (mapList_.empty()) ImGui::TextDisabled("%s", NOX_TRC("editor.dialog.open_map.empty", "No maps in this folder."));
    ImGui::EndChild();
    ImGui::BeginDisabled(!chosenShown || chosen < 0 || chosen >= static_cast<int>(mapList_.size()));
    if (ImGui::Button(NOX_TRI("editor.common.open", "Open"), ImVec2(100 * imgui_->scale(), 0))) go = mapList_[static_cast<std::size_t>(chosen)].path;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.browse", "Browse..."))) {
        // The system's file dialog, for a map anywhere (.map or the download's .nxz).
        static const SDL_DialogFileFilter filters[] = {{NOX_TRC("editor.dialog.open_map.filter_maps", "Nox maps"), "map;nxz"},
                                                       {NOX_TRC("editor.common.filter_all_files", "All files"), "*"}};
        const std::string start = pathText(ctx_.mapsDir);
        editor::openFileDialog(filePick_, ctx_.window, filters, 2, start.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (go) {
        ImGui::CloseCurrentPopup();
        open(*go);
    }
    ImGui::EndPopup();
}

void MapEditor::saveAsDialog()
{
    if (saveAsOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.save_as.title", "Save As"));
        saveAsOpen_ = false;
    }
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.save_as.title", "Save As"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.dialog.save_as.into", "Into {folder}"), {{"folder", pathText(ctx_.mapsDir)}}).c_str());
    if (doc_.origin.stock) {
        ImGui::TextWrapped("%s", nox::l10n::format(NOX_TR("editor.dialog.save_as.stock",
                                                          "{name} is a stock map: its edits are saved under a new name, so nobody's own copy is overwritten."),
                                                   {{"name", doc_.origin.name}})
                                     .c_str());
    } else if (const std::string& own = !docName_.empty() ? docName_ : doc_.origin.name; !own.empty() && me::mapNameProblem(own)) {
        // (A map named before the limit opens as it is, but is saved only under a name that fits.)
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped("%s", nox::l10n::format(NOX_TR("editor.dialog.save_as.own_name_bad", "\"{name}\" can't be saved under its own name: {why}."),
                                                   {{"name", own}, {"why", *me::mapNameProblem(own)}})
                                     .c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText(NOX_TRI("editor.dialog.save_as.map_name", "Map name"), nameBuf_, sizeof nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    const auto problem = me::mapNameProblem(nameBuf_);
    std::error_code ec;
    if (problem) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextColored(editor::themeColours().error, "%s", problem->c_str());
        ImGui::PopTextWrapPos();
    } else if (fs::exists(ctx_.mapsDir / nameBuf_, ec) && nameBuf_ != docName_) {
        ImGui::TextColored(editor::themeColours().warning, "%s", NOX_TRC("editor.dialog.save_as.exists", "A map with this name is there: saving replaces it."));
    } else {
        ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.dialog.save_as.writes", "Writes {name}\\{name}.map, .nxz and .nxe"), {{"name", nameBuf_}}).c_str());
    }
    ImGui::BeginDisabled(problem.has_value());
    if (ImGui::Button(NOX_TRI("editor.common.save", "Save"), ImVec2(100 * imgui_->scale(), 0)) || (enter && !problem)) {
        ImGui::CloseCurrentPopup();
        saveAs(nameBuf_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(100 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        afterSave_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::unsavedDialog()
{
    if (unsavedOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.unsaved.title", "Unsaved Changes"));
        unsavedOpen_ = false;
    }
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.unsaved.title", "Unsaved Changes"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const std::string name = !docName_.empty() ? docName_ : NOX_TR("editor.dialog.unsaved.this_map", "this map");
    ImGui::TextUnformatted(nox::l10n::format(NOX_TR("editor.dialog.unsaved.question", "Save the changes to {name} first?"), {{"name", name}}).c_str());
    const float bw = 110 * imgui_->scale();
    if (ImGui::Button(NOX_TRI("editor.common.save", "Save"), ImVec2(bw, 0))) {
        ImGui::CloseCurrentPopup();
        auto then = std::move(pendingThen_);
        pending_ = Pending::None;
        if (saveNeedsName()) {
            afterSave_ = std::move(then); // (Save As asks for a name, then goes on)
            save();
        } else if (save() && then) {
            then();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.dialog.unsaved.dont_save", "Don't Save"), ImVec2(bw, 0))) {
        ImGui::CloseCurrentPopup();
        auto then = std::move(pendingThen_);
        pending_ = Pending::None;
        if (then) then();
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pendingThen_ = nullptr;
        pending_ = Pending::None;
    }
    ImGui::EndPopup();
}

void MapEditor::endSessionDialog()
{
    if (endSessionOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.dialog.end_session.title", "End the Session?"));
        endSessionOpen_ = false;
        endSessionAsked_ = true;
    }
    if (!ImGui::BeginPopupModal(NOX_TRI("editor.dialog.end_session.title", "End the Session?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        endSessionAsked_ = false;
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(endSessionQuits_ ? NOX_TRC("editor.dialog.end_session.quit_text",
                                                      "Quitting ends your map editor session for everyone in it: their game ends and they go back to their menus.")
                                            : NOX_TRC("editor.dialog.end_session.close_text",
                                                      "Closing the editor ends your map editor session for everyone in it: their game ends and they go back to their menus."));
    ImGui::TextDisabled("%s", NOX_TRC("editor.dialog.end_session.publish_first", "Publish first to keep the working copy's changes in the session's map."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const float bw = 130 * imgui_->scale();
    const int answer = std::exchange(endSessionAnswer_, 0); // (a test's session-end: step)
    if (ImGui::Button(endSessionQuits_ ? NOX_TRI("editor.dialog.end_session.end_quit", "End and Quit") : NOX_TRI("editor.dialog.end_session.end", "End Session"),
                      ImVec2(bw, 0)) ||
        answer == 1) {
        ImGui::CloseCurrentPopup();
        endSessionAsked_ = false;
        SDL_Log("map editor: End the Session? End %s", endSessionQuits_ ? "and Quit" : "Session");
        if (endSessionQuits_) {
            quitConfirmed_ = true;
        } else {
            wantsClose_ = true; // (main.cpp's closeEditor: the session ended, then the main menu)
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || answer == 2) {
        ImGui::CloseCurrentPopup();
        endSessionAsked_ = false;
        SDL_Log("map editor: End the Session? Cancel");
    }
    ImGui::EndPopup();
}

void MapEditor::palette()
{
    if (!paletteOpen_) {
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + 60 * imgui_->scale()), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(520 * imgui_->scale(), 0));
    if (paletteFocus_) {
        ImGui::OpenPopup("##palette");
    }
    if (!ImGui::BeginPopup("##palette")) {
        paletteOpen_ = false;
        return;
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", paletteQuery_.c_str());
    if (paletteFocus_) {
        ImGui::SetKeyboardFocusHere();
        paletteFocus_ = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##q", NOX_TRC("editor.palette.hint", "Type a command"), buf, sizeof buf)) {
        paletteQuery_ = buf;
        paletteIndex_ = 0;
    }
    const auto found = commands_.search(paletteQuery_);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteIndex_ = std::min(paletteIndex_ + 1, static_cast<int>(found.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteIndex_ = std::max(paletteIndex_ - 1, 0);
    std::string run;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && paletteIndex_ >= 0 && paletteIndex_ < static_cast<int>(found.size())) {
        run = found[static_cast<std::size_t>(paletteIndex_)]->id;
    }
    ImGui::BeginChild("##list", ImVec2(0, std::min(static_cast<float>(found.size()), 12.0f) * ImGui::GetTextLineHeightWithSpacing() + 8));
    for (int i = 0; i < static_cast<int>(found.size()); ++i) {
        const editor::Command* c = found[static_cast<std::size_t>(i)];
        const bool on = commands_.enabled(c->id);
        if (!on) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string label = categoryText(c->category) + ": " + c->label + "##" + c->id;
        if (ImGui::Selectable(label.c_str(), i == paletteIndex_)) run = c->id;
        if (i == paletteIndex_) ImGui::SetScrollHereY();
        const std::string key = commands_.shortcut(c->id).text();
        if (!key.empty()) {
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(key.c_str()).x - 16);
            ImGui::TextDisabled("%s", key.c_str());
        }
        if (!on) ImGui::PopStyleColor();
    }
    if (found.empty()) ImGui::TextDisabled("%s", NOX_TRC("editor.palette.none", "No command matches."));
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || !run.empty()) {
        ImGui::CloseCurrentPopup();
        paletteOpen_ = false;
    }
    ImGui::EndPopup();
    if (!run.empty()) {
        commands_.run(run);
    }
}

void MapEditor::keysWindow()
{
    ImGui::SetNextWindowSize(ImVec2(620 * imgui_->scale(), 560 * imgui_->scale()), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(NOX_TRI("editor.window.keyboard_shortcuts", "Keyboard Shortcuts"), &showKeys_)) {
        ImGui::End();
        return;
    }
    static char filter[64];
    ImGui::SetNextItemWidth(240 * imgui_->scale());
    ImGui::InputTextWithHint("##kf", NOX_TRC("editor.keys.filter_hint", "Filter"), filter, sizeof filter);
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("editor.keys.reset_all", "Reset All"))) {
        commands_.resetAll();
        saveSettings();
    }
    const auto conflicts = commands_.conflicts();
    if (!conflicts.empty()) {
        ImGui::TextColored(editor::themeColours().error, "%s",
                           nox::l10n::format(NOX_TR("editor.keys.conflicts", "{n} shortcut conflicts (the first command listed wins)"), {{"n", conflicts.size()}}).c_str());
    }
    if (rebinding_) {
        ImGui::TextColored(editor::themeColours().warning, "%s",
                           nox::l10n::format(NOX_TR("editor.keys.press_new", "Press the new shortcut for \"{command}\" (Escape cancels, Backspace clears)"),
                                             {{"command", commands_.find(*rebinding_)->label}})
                               .c_str());
    }
    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(NOX_TRI("editor.keys.menu", "Menu"), ImGuiTableColumnFlags_WidthFixed, 70 * imgui_->scale());
        ImGui::TableSetupColumn(NOX_TRI("editor.keys.command", "Command"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(NOX_TRI("editor.keys.shortcut", "Shortcut"), ImGuiTableColumnFlags_WidthFixed, 150 * imgui_->scale());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * imgui_->scale());
        ImGui::TableHeadersRow();
        const std::string f = lowered(filter);
        for (const editor::Command& c : commands_.commands()) {
            const std::string& category = categoryText(c.category);
            if (!f.empty() && lowered(category + " " + c.label).find(f) == std::string::npos) continue;
            ImGui::PushID(c.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", category.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.label.c_str());
            help(c.help);
            ImGui::TableNextColumn();
            const editor::Shortcut s = commands_.shortcut(c.id);
            const bool clash = std::ranges::any_of(conflicts, [&](const auto& p) { return p.first == c.id || p.second == c.id; });
            if (clash) ImGui::PushStyleColor(ImGuiCol_Text, editor::themeColours().error);
            const std::string shown = rebinding_ == c.id ? std::string("...") : s.empty() ? NOX_TR("editor.common.none", "(none)") : s.text();
            if (ImGui::Button(shown.c_str(), ImVec2(-FLT_MIN, 0))) rebinding_ = c.id;
            if (clash) {
                ImGui::PopStyleColor();
                for (const auto& [a, b] : conflicts) {
                    if (a == c.id || b == c.id) {
                        help(nox::l10n::format(NOX_TR("editor.keys.also_on", "Also on {command}"), {{"command", commands_.find(a == c.id ? b : a)->label}}));
                    }
                }
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!commands_.overrides().contains(c.id));
            if (ImGui::SmallButton(NOX_TRI("editor.keys.reset", "Reset"))) {
                commands_.resetBinding(c.id);
                saveSettings();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void MapEditor::settingsWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420 * imgui_->scale(), 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(NOX_TRI("editor.window.editor_settings", "Editor Settings"), &showSettings_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::SliderFloat(NOX_TRI("editor.settings.ui_scale", "Interface scale"), &settings_.uiScale, 0.75f, 2.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) styleDirty_ = true;
    help(NOX_TR("editor.settings.ui_scale_help", "On top of the display's own scale."));
    themeCombo(NOX_TRI("editor.settings.theme", "Theme##settings"), 0);
    help(NOX_TR("editor.settings.theme_help", "Light, Medium (NoX Core's stone greys and gold) or Dark (near-black with blue accents). Also at the menu bar's right end."));
    ImGui::SliderInt(NOX_TRI("editor.settings.autosave", "Autosave (minutes)"), &settings_.autosaveMinutes, 0, 30);
    help(NOX_TR("editor.settings.autosave_help", "A recovery copy in your settings folder (File > Open Autosave); 0 turns it off."));
    ImGui::BeginDisabled(!imgui_->floatingSupported());
    if (ImGui::Checkbox(NOX_TRI("editor.settings.floating", "Allow floating panels"), &settings_.floatingPanels)) {
        floatingDirty_ = true; // (applied before the next frame)
    }
    ImGui::EndDisabled();
    if (imgui_->floatingSupported()) {
        help(NOX_TR("editor.settings.floating_help", "Drag a panel's tab or title bar out of the window and it becomes a window of its own, for another monitor. "
                                                     "Floating panels keep their places with the layout. The Map panel stays in the main window. Off: panels stay inside it."));
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", NOX_TRC("editor.settings.floating_unsupported", "(not on this system: e.g. Wayland can't place our windows)"));
    }
    ImGui::SeparatorText(NOX_TRC("editor.settings.scripting", "Scripting"));
    if (ImGui::Checkbox(NOX_TRI("editor.settings.noxcore_script", "Allow NoX Core-only script features (object variables)"), &settings_.allowNoxCoreScript)) {
        saveSettings(); // (the Script panel compiles again with it)
    }
    help(NOX_TR("editor.settings.noxcore_script_help", "Lets scripts declare and use object variables (int object.hp;). Maps that use them crash the original game "
                                                       "and need a NoX Core server. Off: the compiler refuses them, and a map whose script already has them is shown read-only."));
    ImGui::TextDisabled("%s", NOX_TRC("editor.settings.noxcore_script_note", "Maps that use them crash the original game and need a NoX Core server."));
    ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.settings.folder", "Settings: {folder}"), {{"folder", pathText(ctx_.prefDir)}}).c_str());
    if (ImGui::Button(NOX_TRI("editor.common.close", "Close"))) {
        showSettings_ = false;
        saveSettings();
    }
    ImGui::End();
}

} // namespace noxsdl
