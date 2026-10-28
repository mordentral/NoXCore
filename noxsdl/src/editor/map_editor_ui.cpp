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
constexpr std::pair<std::uint32_t, const char*> kGameTypes[] = {{1, "Solo"},        {2, "Quest"},     {4, "Arena"},
                                                                 {8, "Capture the Flag"}, {0x10, "King of the Realm"},
                                                                 {0x20, "Elimination"}, {0x40, "Flag Ball"}, {0x80000000u, "Chat"}};

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
    ImGui::DockBuilderDockWindow("Map", centre);
    ImGui::DockBuilderDockWindow("Layers", left);
    ImGui::DockBuilderDockWindow("Object List", left);
    ImGui::DockBuilderDockWindow("Content Browser", browser);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderDockWindow("Tool Options", rightLow);
    ImGui::DockBuilderDockWindow("Problems", bottom);
    ImGui::DockBuilderDockWindow("History", bottom);
    ImGui::DockBuilderDockWindow("Script", bottom); // (with the diagnostics)
    ImGui::DockBuilderDockWindow("Script Reference", rightLow);
    // (The marker lists beside the Object List, map-wide ones with Properties.)
    ImGui::DockBuilderDockWindow("Waypoints", left);
    ImGui::DockBuilderDockWindow("Polygons", left);
    ImGui::DockBuilderDockWindow("Groups", left);
    ImGui::DockBuilderDockWindow("Minimap", rightLow);
    ImGui::DockBuilderDockWindow("Map Info", right);
    ImGui::DockBuilderDockWindow("Game Texts", bottom);
    ImGui::DockBuilderDockWindow("Session", right); // (a collaborative session's people, requests, publish)
    ImGui::DockBuilderDockWindow("###Chat", right); // (tabbed with Session; its title carries the unread count)
    // A mod project's windows (mod_panel.hpp), where they open: its code with the Script panel, its
    // form with Properties, its files with the lists, its preview over the map.
    ImGui::DockBuilderDockWindow("Mod", right);
    ImGui::DockBuilderDockWindow("Mod Files", left);
    ImGui::DockBuilderDockWindow("Mod Code", bottom);
    ImGui::DockBuilderDockWindow("Mod Reference", rightLow);
    ImGui::DockBuilderDockWindow("Mod Preview", centre);
    ImGui::DockBuilderFinish(dockspace);
    showLayers_ = showProperties_ = showTool_ = showProblems_ = showHistory_ = true;
}

void MapEditor::mainMenu()
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        menuItem("file.new");
        menuItem("file.open");
        if (ImGui::BeginMenu("Open Recent", !settings_.recent.empty())) {
            for (const std::string& r : settings_.recent) {
                if (ImGui::MenuItem(r.c_str())) {
                    const fs::path p(reinterpret_cast<const char8_t*>(r.c_str()));
                    guarded(Pending::OpenRecent, [this, p] { open(p); });
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Recent")) {
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
        if (ctx_.hostSession && ImGui::MenuItem("Host a Session...", nullptr, false, !inSession())) {
            ctx_.hostSession(); // (a collaborative session of a map: who may edit, the sandbox...)
        }
        ImGui::Separator();
        menuItem("file.close");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        for (const char* id : {"edit.undo", "edit.redo"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"edit.cut", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete"}) menuItem(id);
        ImGui::Separator();
        menuItem("edit.areaCopy");
        if (ImGui::BeginMenu("Copy as Text")) {
            for (const char* id : {"edit.copyCoords", "edit.copyExtents", "edit.copyWaypoints"}) menuItem(id);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Select")) {
        for (const char* id : {"select.all", "select.none", "select.invert", "select.sameType"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"select.shapeBox", "select.shape45", "select.shapeLasso"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"select.filterObjects", "select.filterWalls", "select.filterTiles", "select.filterWaypoints"}) menuItem(id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (const char* id : {"view.frame", "view.zoomIn", "view.zoomOut", "view.zoomReset"}) menuItem(id);
        ImGui::Separator();
        for (const char* id : {"view.grid", "view.floor", "view.edges", "view.walls", "view.objects", "view.waypoints", "view.polygons",
                               "view.extents", "view.facing", "view.markDisabled", "view.specialWalls", "view.lighting", "view.seeThroughWalls", "view.schematic"}) {
            menuItem(id);
        }
        if (ImGui::BeginMenu("Labels")) {
            for (const char* id : {"view.labelTypes", "view.labelScriptNames", "view.labelExtents"}) menuItem(id);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Pending Changes")) {
            for (const char* id : {"view.pendingShaded", "view.pendingFinal", "view.pendingHidden", "view.pendingPublished"}) menuItem(id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        menuItem("view.palette");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
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
    if (ImGui::BeginMenu("Window")) {
        for (const char* id : {"window.layers", "window.properties", "window.tool", "window.history", "window.problems", "window.browser", "window.objectList"}) menuItem(id);
        menuItem("window.chat");
        ImGui::Separator();
        for (const char* id : {"window.mapInfo", "window.waypoints", "window.polygons", "window.groups", "window.minimap", "window.strings"}) menuItem(id);
        ImGui::Separator();
        if (ImGui::BeginMenu("Layouts")) {
            std::error_code ec;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(ctx_.prefDir / "layouts", ec)) {
                if (e.path().extension() == ".ini") names.push_back(e.path().stem().string());
            }
            std::ranges::sort(names);
            for (const auto& n : names) {
                if (ImGui::MenuItem(n.c_str())) loadLayout_ = n;
            }
            if (names.empty()) ImGui::TextDisabled("No saved layouts");
            ImGui::Separator();
            ImGui::SetNextItemWidth(140 * imgui_->scale());
            ImGui::InputTextWithHint("##layoutname", "name", layoutBuf_, sizeof layoutBuf_);
            ImGui::SameLine();
            if (ImGui::Button("Save Layout") && layoutBuf_[0] != 0 && !ctx_.screenshotRun) {
                std::size_t n = 0;
                const char* ini = ImGui::SaveIniSettingsToMemory(&n);
                fs::create_directories(ctx_.prefDir / "layouts", ec);
                std::ofstream(ctx_.prefDir / "layouts" / (std::string(layoutBuf_) + ".ini"), std::ios::binary).write(ini, static_cast<std::streamsize>(n));
                status(std::string("Layout saved as ") + layoutBuf_);
            }
            ImGui::EndMenu();
        }
        menuItem("window.resetLayout");
        ImGui::Separator();
        menuItem("window.settings");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        menuItem("help.palette");
        menuItem("help.keys");
        ImGui::Separator();
        menuItem("help.about");
        ImGui::EndMenu();
    }
    // The theme, at the bar's right end (after the menus if the window is too narrow for both).
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float comboW = std::max(ImGui::CalcTextSize("Medium").x + ImGui::GetFrameHeight() + st.FramePadding.x * 2, 90 * imgui_->scale());
        const float labelW = ImGui::CalcTextSize("Theme").x;
        const float x = ImGui::GetWindowWidth() - st.FramePadding.x * 2 - comboW - st.ItemSpacing.x - labelW;
        if (x > ImGui::GetCursorPosX() + st.ItemSpacing.x * 2) {
            ImGui::SetCursorPosX(x);
        }
        ImGui::TextDisabled("Theme");
        ImGui::SameLine();
        themeCombo("##theme", comboW);
        help("The editor's colours: Light, Medium (NoX Core's stone greys and gold) or Dark (near-black with blue accents).");
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
    button("New", "file.new");
    button("Open", "file.open");
    button("Save", "file.save");
    gap();
    button("Undo", "edit.undo");
    button("Redo", "edit.redo");
    gap();
    if (ctx_.playTest.available()) {
        playView_->toolbarButtons(); // (Play / Pause / Stop, the play state)
        gap();
    }
    button("Select", "tool.select", tool_ == Tool::Select);
    button("Wall", "tool.wall", tool_ == Tool::Wall);
    button("Floor", "tool.floor", tool_ == Tool::Floor);
    button("Edge", "tool.edge", tool_ == Tool::Edge);
    button("Object", "tool.object", tool_ == Tool::Object);
    button("Waypoint", "tool.waypoint", tool_ == Tool::Waypoint);
    button("Polygon", "tool.polygon", tool_ == Tool::Polygon);
    gap();
    button("Box", "select.shapeBox", settings_.regionShape == 0);
    button("45\xC2\xB0 Box", "select.shape45", settings_.regionShape == 1);
    button("Lasso", "select.shapeLasso", settings_.regionShape == 2);
    gap();
    ImGui::TextUnformatted("Pick:");
    ImGui::SameLine();
    ImGui::Checkbox("Objects", &settings_.pick.objects);
    ImGui::SameLine();
    ImGui::Checkbox("Walls", &settings_.pick.walls);
    ImGui::SameLine();
    ImGui::Checkbox("Tiles", &settings_.pick.tiles);
    ImGui::SameLine();
    ImGui::Checkbox("Waypoints", &settings_.pick.waypoints);
    gap();
    ImGui::TextUnformatted("Snap:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * imgui_->scale());
    const char* snaps[] = {"Off", "Grid (23)", "Half grid", "Custom"};
    ImGui::Combo("##snap", &settings_.snap, snaps, 4);
    help("Where dragged objects and waypoints land. Walls and floor tiles always move by whole cells.");
    if (settings_.snap == 3) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60 * imgui_->scale());
        ImGui::DragFloat("##snapn", &settings_.snapCustom, 0.5f, 1, 230, "%.0f");
    }
    gap();
    button("Validate", "file.validate");
    if (session_) {
        // The session bar: back to the game, publish (the host), ask to edit (others).
        gap();
        if (ImGui::Button("Play (F2)") && sessionHooks_.toGame) sessionHooks_.toGame();
        help("Back to the running game (the editor waits; F2 comes back).");
        ImGui::SameLine();
        if (session_->isOwner()) {
            if (ImGui::Button("Publish")) sessionPublish();
            help("Write the working copy as the session map's new version and change the map to it (walls, floors and the script's bytecode reach the game; stock clients download it).");
        } else if (session_->role() < nox::mapedit::session::Role::Editor) {
            if (ImGui::Button("Request edit")) session_->requestEdit(requestReason_);
            help("Ask the host for edit rights (or type /edit <reason> in the game's chat).");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", session_->isOwner() ? "Host" : session_->role() >= nox::mapedit::session::Role::Editor ? "Editor" : "Viewer");
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void MapEditor::statusBar()
{
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetStyle().ItemSpacing.x);
    static constexpr const char* kTools[] = {"Select", "Wall", "Floor", "Edge", "Object", "Waypoint", "Polygon"};
    std::string line = kTools[static_cast<int>(tool_)];
    if (areaCopyArmed_) line += " (copy area)";
    if (const auto w = mouseWorld()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "  %.0f, %.0f", w->x, w->y);
        line += buf;
        if (const auto c = me::wallCellAt(w->x, w->y)) line += "  wall " + std::to_string(c->x) + "," + std::to_string(c->y);
        if (const auto t = me::tileAt(w->x, w->y)) line += "  tile " + std::to_string(t->gx) + "," + std::to_string(t->gy);
        const std::string under = underCursor(w->x, w->y);
        if (!under.empty()) line += "  |  " + under;
    }
    ImGui::TextUnformatted(line.c_str());
    // Right-aligned: the selection, the save state, the last message.
    std::string right;
    if (!selection_.empty()) right += std::to_string(selection_.size()) + " selected   ";
    if (session_) {
        right += "Session " + docName_ + (session_->pendingOps() ? "  (" + std::to_string(session_->pendingOps()) + " sending)" : "");
    } else {
        right += history_.dirty() ? "Unsaved changes" : (docPath_.empty() ? "New map" : "No unsaved changes");
    }
    if (validating_.valid()) right += "   Validating...";
    if (!statusText_.empty() && SDL_GetTicks() - statusMs_ < 8000) right = statusText_ + "     " + right;
    const float w = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 12));
    ImGui::TextUnformatted(right.c_str());
}

void MapEditor::layersPanel()
{
    if (!ImGui::Begin("Layers", &showLayers_)) {
        ImGui::End();
        return;
    }
    editor::Layers& L = settings_.layers;
    const auto rebuild = [&](const char* label, bool* v, const char* tip) {
        if (ImGui::Checkbox(label, v)) ++layerRevision_;
        help(tip);
    };
    ImGui::SeparatorText("Map");
    rebuild("Floor", &L.floor, "The floor tiles.");
    rebuild("Floor edges", &L.edges, "The edges blended over tiles.");
    rebuild("Walls", &L.walls, "");
    rebuild("Objects", &L.objects, "");
    ImGui::Checkbox("Waypoints", &L.waypoints);
    ImGui::Checkbox("Polygons", &L.polygons);
    ImGui::Checkbox("Lighting preview", &L.lighting);
    help("The map's lights and ambient colour, as the game lights it.");
    ImGui::Checkbox("See-through walls near the view centre", &L.seeThroughWalls);
    help("Walls below the view's centre drawn half see-through over everything, as the game draws them. Off, every wall is "
         "drawn whole and opaque.");
    ImGui::SeparatorText("Overlays");
    const char* grids[] = {"Off", "Wall cells", "Floor tiles"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##grid", &L.grid, grids, 3);
    help("The grid: wall cells (23 x 23) or the floor's diamonds.");
    const char* extents[] = {"Extents: hidden", "Extents: colliding", "Extents: all"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##extents", &L.extents, extents, 3);
    help("Objects' collision shapes (thing.bin EXTENT).");
    ImGui::Checkbox("Facing arrows", &L.facing);
    help("Creatures' and NPCs' facing, doors' hanging and sentries' beams.");
    ImGui::Checkbox("Mark disabled objects", &L.markDisabled);
    ImGui::Checkbox("Schematic", &L.schematic);
    help("Flat colours over the map: walls by kind (secret blue, breakable red, window cyan), objects as dots.");
    ImGui::Checkbox("Tint special walls", &L.specialWalls);
    help("Secret walls blue, breakable red, windows cyan, over the drawn map.");
    ImGui::Checkbox("Trigger and plate boxes", &L.triggers);
    help("Triggers' and pressure plates' areas (their size, turned 45 degrees), in their colours.");
    ImGui::Checkbox("Light radius rings", &L.lightRings);
    help("Each invisible light's radius, in its colour.");
    ImGui::Checkbox("Links", &L.links);
    help("Lines from teleporters, elevators and shafts to their partners, movers to their waypoints and what they carry, glyphs "
         "to their targets, exits and pits to their points.");
    ImGui::Checkbox("3D extents", &L.extents3d);
    ImGui::Checkbox("Live players", &L.livePlayers);
    help("In a session: the players in the running game where they are now (name, team colour, stock or NoX Core client). View only.");
    ImGui::SameLine();
    ImGui::Checkbox("Live creatures", &L.liveCreatures);
    help("In a session: the running game's creatures and moving things. View only.");
    help("Extents drawn with a raised copy at the type's height (with Extents shown).");
    const char* pendingModes[] = {"Pending changes: shaded", "Pending changes: as final", "Pending changes: hidden"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##pending", &L.pendingView, pendingModes, 3);
    help("In a session: walls, floor and client-only things not yet published. Shaded: greyed, and what they remove as see-through "
         "hatched ghosts; As final: as they will look once published; Hidden: only what the game has (G flips to it and back).");
    ImGui::SeparatorText("Labels");
    ImGui::Checkbox("Type names", &L.labelTypes);
    ImGui::Checkbox("Script names", &L.labelScriptNames);
    ImGui::Checkbox("Extents", &L.labelExtents);
    ImGui::SeparatorText("Locked layers");
    ImGui::TextDisabled("Shown, but not picked or changed by the tools:");
    ImGui::Checkbox("Floor##lock", &L.lockFloor);
    ImGui::SameLine();
    ImGui::Checkbox("Walls##lock", &L.lockWalls);
    ImGui::SameLine();
    ImGui::Checkbox("Objects##lock", &L.lockObjects);
    ImGui::Checkbox("Waypoints##lock", &L.lockWaypoints);
    ImGui::SameLine();
    ImGui::Checkbox("Polygons##lock", &L.lockPolygons);
    ImGui::End();
}

void MapEditor::historyPanel()
{
    if (!ImGui::Begin("History", &showHistory_)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu changes", history_.entries().size());
    ImGui::BeginChild("##hist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const auto& entries = history_.entries();
    if (ImGui::Selectable(history_.dropped() ? "(older changes dropped)" : "(opened)", history_.position() == 0)) {
        jumpHistory(0);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool undone = i >= history_.position();
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const std::string label = std::to_string(history_.dropped() + i + 1) + ". " + entries[i].op.label + "##h" + std::to_string(i);
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
    if (!ImGui::Begin("Problems", &showProblems_)) {
        ImGui::End();
        return;
    }
    const auto errors = std::ranges::count_if(issues_, [](const me::Issue& i) { return i.severity == me::Severity::Error; });
    const auto warnings = static_cast<std::ptrdiff_t>(issues_.size()) - errors;
    if (validating_.valid()) {
        ImGui::TextDisabled("Validating...");
    } else if (issues_.empty()) {
        ImGui::TextColored(editor::themeColours().ok, "No problems: the game will load this map.");
    } else {
        ImGui::Text("%td errors, %td warnings", errors, warnings);
        if (errors > 0) {
            ImGui::SameLine();
            ImGui::TextColored(editor::themeColours().error, "(saving is refused until the errors are fixed)");
        }
    }
    scriptPanel_->problems(); // (the script's diagnostics, reload notes, tests)
    modPanel_->problems();    // (an open mod project's mode.json and Luau diagnostics)
    if (session_ && !session_->problems().empty()) {
        // The session's problems: what a live change couldn't keep for the running game (a script
        // holding an object made again...), till it's fixed or published.
        ImGui::SeparatorText("Session");
        for (const auto& [key, pr] : session_->problems()) {
            ImGui::TextColored(editor::themeColours().warning, "Session");
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
            help("Click to show the object on the map.");
        }
    }
    if (ImGui::BeginTable("##issues", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * imgui_->scale());
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, 200 * imgui_->scale());
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < issues_.size(); ++i) {
            const auto& issue = issues_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool error = issue.severity == me::Severity::Error;
            ImGui::TextColored(error ? editor::themeColours().error : editor::themeColours().warning, error ? "Error" : "Warning");
            ImGui::TableNextColumn();
            if (ImGui::Selectable((issue.where + "##p" + std::to_string(i)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                locate(issue);
            }
            help("Click to show it on the map.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(issue.message.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void MapEditor::propertiesPanel()
{
    if (!ImGui::Begin("Properties", &showProperties_)) {
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
        ImGui::TextDisabled("Nothing selected.");
        ImGui::Spacing();
        ImGui::TextWrapped("Map: %s", me::fromMapText(doc_.info.title.view()).c_str());
        ImGui::TextDisabled("%zu walls, %zu floor tiles, %zu objects, %zu waypoints, %zu polygons, %zu groups", doc_.walls.size(),
                            doc_.floor.tileCount(), doc_.objects.size(), doc_.waypoints.waypoints.size(), doc_.polygons.polygons.size(),
                            doc_.groups.groups.size());
        ImGui::Spacing();
        if (ImGui::Button("Map Info...")) commands_.run("window.mapInfo");
        ImGui::SameLine();
        if (ImGui::Button("Groups...")) commands_.run("window.groups");
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
            ImGui::SeparatorText(tile->image < things_.floors.size() ? things_.floors[tile->image].name.c_str() : "Floor");
            row("Tile");
            ImGui::Text("%d, %d", t.gx, t.gy);
            row("Variant");
            ImGui::Text("%d", tile->variant);
            row("Edges");
            ImGui::Text("%zu", tile->edges.size());
            for (const auto& e : tile->edges) {
                ImGui::BulletText("%s over %s, direction %d", e.edge < things_.edges.size() ? things_.edges[e.edge].name.c_str() : "?",
                                  e.floor < things_.floors.size() ? things_.floors[e.floor].name.c_str() : "?", e.direction);
            }
        }
    } else {
        ImGui::Text("%zu selected", s.size());
        if (!s.objects.empty()) ImGui::BulletText("%zu objects", s.objects.size());
        if (!s.walls.empty()) ImGui::BulletText("%zu walls", s.walls.size());
        if (!s.tiles.empty()) ImGui::BulletText("%zu floor tiles", s.tiles.size());
        if (!s.waypoints.empty()) ImGui::BulletText("%zu waypoints", s.waypoints.size());
        if (!s.polygons.empty()) ImGui::BulletText("%zu polygons", s.polygons.size());
        ImGui::Spacing();
        ImGui::TextWrapped("Properties edit one kind at a time: select only objects, walls, waypoints or polygons.");
        if (!s.objects.empty() && ImGui::Button("Keep only the objects")) {
            me::Selection o;
            o.objects = s.objects;
            setSelection(std::move(o));
        }
    }
}

void MapEditor::toolPanel()
{
    if (!ImGui::Begin("Tool Options", &showTool_)) {
        ImGui::End();
        return;
    }
    switch (tool_) {
    case Tool::Select: {
        ImGui::SeparatorText("Select");
        ImGui::TextWrapped("Click to select; Shift-click adds or toggles, Ctrl-click removes. Drag on empty map to select an area, "
                           "drag a selected item to move the selection. Middle- or right-drag pans, the wheel zooms.");
        ImGui::RadioButton("Box", &settings_.regionShape, 0);
        ImGui::SameLine();
        ImGui::RadioButton("45\xC2\xB0 box", &settings_.regionShape, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Lasso", &settings_.regionShape, 2);
        ImGui::SeparatorText("Copy area");
        ImGui::TextWrapped("Takes everything in a dragged box, of the layers ticked:");
        ImGui::Checkbox("Walls##ca", &settings_.copyLayers.walls);
        ImGui::SameLine();
        ImGui::Checkbox("Floor##ca", &settings_.copyLayers.tiles);
        ImGui::SameLine();
        ImGui::Checkbox("Objects##ca", &settings_.copyLayers.objects);
        ImGui::SameLine();
        ImGui::Checkbox("Waypoints##ca", &settings_.copyLayers.waypoints);
        if (ImGui::Button(areaCopyArmed_ ? "Drag a box on the map..." : "Copy Area")) commands_.run("edit.areaCopy");
        help("The copy goes to the system clipboard too, so it pastes into another map (Ctrl+V).");
        ImGui::SeparatorText("Move");
        ImGui::TextWrapped("Arrows nudge one unit, Shift+arrows one cell. Objects and waypoints snap as the toolbar's Snap says; "
                           "walls and floor tiles move by whole cells, keeping their grid.");
        ImGui::TextDisabled("Move and rotate gizmos come with the property panels (P2d).");
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
        ImGui::OpenPopup("Problem##err");
    }
    if (ImGui::BeginPopupModal("Problem##err", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(520 * imgui_->scale());
        ImGui::TextUnformatted(errorText_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK", ImVec2(100 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            errorText_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (showAbout_) {
        ImGui::OpenPopup("About the Map Editor");
        showAbout_ = false;
    }
    if (ImGui::BeginPopupModal("About the Map Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("NoX Core Map Editor (shell, P2a)");
        ImGui::TextDisabled("Maps are written as the game reads them; stock maps are saved under new names.");
        ImGui::TextDisabled("Panels: Dear ImGui %s (docking), MIT.", IMGUI_VERSION);
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void MapEditor::startDialog()
{
    if (startOpen_) {
        ImGui::OpenPopup("Map Editor##start");
        startOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(440 * imgui_->scale(), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Map Editor##start", nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }
    const ImVec2 wide(-FLT_MIN, 0);
    if (ImGui::Button("New Map...", wide)) {
        ImGui::CloseCurrentPopup();
        newOpen_ = true;
    }
    if (ImGui::Button("Open Map...", wide)) {
        ImGui::CloseCurrentPopup();
        refreshMapList();
        openOpen_ = true;
    }
    if (!settings_.recent.empty()) {
        ImGui::SeparatorText("Recent");
        for (int i = 0; i < static_cast<int>(settings_.recent.size()); ++i) {
            const std::string& r = settings_.recent[static_cast<std::size_t>(i)];
            if (editor::verbatimSelectable(i, r, false)) {
                ImGui::CloseCurrentPopup();
                open(fs::path(reinterpret_cast<const char8_t*>(r.c_str())));
            }
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Start with a blank map")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::newDialog()
{
    if (newOpen_) {
        ImGui::OpenPopup("New Map");
        newOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("New Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    static char title[64] = "Untitled";
    ImGui::InputText("Title", title, sizeof title);
    static const int sizes[] = {0, 16, 32, 48, 64, 96, 128, 192, 254};
    int sizeIndex = static_cast<int>(std::ranges::find(sizes, newOptions_.size) - std::begin(sizes));
    const std::string sizeLabel = newOptions_.size == 0 ? std::string("No floor") : std::to_string(newOptions_.size) + " x " + std::to_string(newOptions_.size) + " cells";
    if (ImGui::BeginCombo("Floored area", sizeLabel.c_str())) {
        for (int i = 0; i < static_cast<int>(std::size(sizes)); ++i) {
            const std::string l = sizes[i] == 0 ? std::string("No floor") : std::to_string(sizes[i]) + " x " + std::to_string(sizes[i]) + " cells";
            if (ImGui::Selectable(l.c_str(), i == sizeIndex)) newOptions_.size = sizes[i];
        }
        ImGui::EndCombo();
    }
    help("A square of floor in the middle of the map (the whole map is 256 x 256 wall cells).");
    const char* floorName = newOptions_.floor < things_.floors.size() ? things_.floors[newOptions_.floor].name.c_str() : "?";
    if (ImGui::BeginCombo("Base floor", floorName, ImGuiComboFlags_HeightLarge)) {
        static char filter[64];
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##ff", "search", filter, sizeof filter);
        for (std::size_t i = 0; i < things_.floors.size(); ++i) {
            const auto& name = things_.floors[i].name;
            if (filter[0] && lowered(name).find(lowered(filter)) == std::string::npos) continue;
            if (ImGui::Selectable(name.c_str(), i == newOptions_.floor)) newOptions_.floor = static_cast<std::uint8_t>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText("Game types");
    for (const auto& [bit, name] : kGameTypes) {
        bool on = (newOptions_.gameFlags & bit) != 0;
        if (ImGui::Checkbox(name, &on)) newOptions_.gameFlags = on ? (newOptions_.gameFlags | bit) : (newOptions_.gameFlags & ~bit);
    }
    ImGui::Checkbox("Player start in the middle", &newOptions_.playerStart);
    help("A multiplayer map needs at least one PlayerStart.");
    ImGui::Separator();
    if (ImGui::Button("Create", ImVec2(110 * imgui_->scale(), 0))) {
        newOptions_.title = title;
        newDocument(newOptions_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::openDialog()
{
    static int chosen = -1; // (a row of mapList_, refreshed as the dialog opens: none chosen then)
    if (openOpen_) {
        ImGui::OpenPopup("Open Map");
        openOpen_ = false;
        searchBuf_[0] = 0;
        chosen = -1;
    }
    ImGui::SetNextWindowSize(ImVec2(460 * imgui_->scale(), 520 * imgui_->scale()), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Open Map", nullptr)) {
        return;
    }
    ImGui::TextDisabled("%s", pathText(ctx_.mapsDir).c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search maps", searchBuf_, sizeof searchBuf_);
    std::optional<fs::path> go;
    ImGui::BeginChild("##maps", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
    const std::string q = lowered(searchBuf_);
    bool chosenShown = false; // (Open opens only a row the search shows)
    for (int i = 0; i < static_cast<int>(mapList_.size()); ++i) {
        const auto& e = mapList_[static_cast<std::size_t>(i)];
        if (!q.empty() && lowered(e.name).find(q) == std::string::npos) continue;
        chosenShown = chosenShown || chosen == i;
        const std::string label = e.name + (e.stock ? "   (stock: saved under a new name)" : "");
        // (Drawn verbatim: a map's name can hold '#'; each row its own id, names repeating or not.)
        if (editor::verbatimSelectable(i, label, chosen == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            chosen = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = e.path;
        }
    }
    if (mapList_.empty()) ImGui::TextDisabled("No maps in this folder.");
    ImGui::EndChild();
    ImGui::BeginDisabled(!chosenShown || chosen < 0 || chosen >= static_cast<int>(mapList_.size()));
    if (ImGui::Button("Open", ImVec2(100 * imgui_->scale(), 0))) go = mapList_[static_cast<std::size_t>(chosen)].path;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        // The system's file dialog, for a map anywhere (.map or the download's .nxz).
        static const SDL_DialogFileFilter filters[] = {{"Nox maps", "map;nxz"}, {"All files", "*"}};
        static std::pair<std::mutex*, std::optional<std::string>*> target;
        target = {&pickedMutex_, &picked_};
        const std::string start = pathText(ctx_.mapsDir);
        SDL_ShowOpenFileDialog(editor::storePickedFile, &target, ctx_.window, filters, 2, start.c_str(), false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (go) {
        ImGui::CloseCurrentPopup();
        open(*go);
    }
    ImGui::EndPopup();
}

void MapEditor::saveAsDialog()
{
    if (saveAsOpen_) {
        ImGui::OpenPopup("Save As");
        saveAsOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Save As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextDisabled("Into %s", pathText(ctx_.mapsDir).c_str());
    if (doc_.origin.stock) {
        ImGui::TextWrapped("%s is a stock map: its edits are saved under a new name, so nobody's own copy is overwritten.",
                           doc_.origin.name.c_str());
    } else if (const std::string& own = !docName_.empty() ? docName_ : doc_.origin.name; !own.empty() && me::mapNameProblem(own)) {
        // (A map named before the limit opens as it is, but is saved only under a name that fits.)
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped("\"%s\" can't be saved under its own name: %s.", own.c_str(), me::mapNameProblem(own)->c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("Map name", nameBuf_, sizeof nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    const auto problem = me::mapNameProblem(nameBuf_);
    std::error_code ec;
    if (problem) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextColored(editor::themeColours().error, "%s", problem->c_str());
        ImGui::PopTextWrapPos();
    } else if (fs::exists(ctx_.mapsDir / nameBuf_, ec) && nameBuf_ != docName_) {
        ImGui::TextColored(editor::themeColours().warning, "A map with this name is there: saving replaces it.");
    } else {
        ImGui::TextDisabled("Writes %s\\%s.map, .nxz and .nxe", nameBuf_, nameBuf_);
    }
    ImGui::BeginDisabled(problem.has_value());
    if (ImGui::Button("Save", ImVec2(100 * imgui_->scale(), 0)) || (enter && !problem)) {
        ImGui::CloseCurrentPopup();
        saveAs(nameBuf_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100 * imgui_->scale(), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        afterSave_ = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MapEditor::unsavedDialog()
{
    if (unsavedOpen_) {
        ImGui::OpenPopup("Unsaved Changes");
        unsavedOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const std::string name = !docName_.empty() ? docName_ : "this map";
    ImGui::Text("Save the changes to %s first?", name.c_str());
    const float bw = 110 * imgui_->scale();
    if (ImGui::Button("Save", ImVec2(bw, 0))) {
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
    if (ImGui::Button("Don't Save", ImVec2(bw, 0))) {
        ImGui::CloseCurrentPopup();
        auto then = std::move(pendingThen_);
        pending_ = Pending::None;
        if (then) then();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pendingThen_ = nullptr;
        pending_ = Pending::None;
    }
    ImGui::EndPopup();
}

void MapEditor::endSessionDialog()
{
    if (endSessionOpen_) {
        ImGui::OpenPopup("End the Session?");
        endSessionOpen_ = false;
        endSessionAsked_ = true;
    }
    if (!ImGui::BeginPopupModal("End the Session?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        endSessionAsked_ = false;
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(endSessionQuits_ ? "Quitting ends your map editor session for everyone in it: their game ends and they go back to their menus."
                                            : "Closing the editor ends your map editor session for everyone in it: their game ends and they go back to their menus.");
    ImGui::TextDisabled("Publish first to keep the working copy's changes in the session's map.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const float bw = 130 * imgui_->scale();
    const int answer = std::exchange(endSessionAnswer_, 0); // (a test's session-end: step)
    if (ImGui::Button(endSessionQuits_ ? "End and Quit" : "End Session", ImVec2(bw, 0)) || answer == 1) {
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
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || answer == 2) {
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
    if (ImGui::InputTextWithHint("##q", "Type a command", buf, sizeof buf)) {
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
        const std::string label = c->category + ": " + c->label + "##" + c->id;
        if (ImGui::Selectable(label.c_str(), i == paletteIndex_)) run = c->id;
        if (i == paletteIndex_) ImGui::SetScrollHereY();
        const std::string key = commands_.shortcut(c->id).text();
        if (!key.empty()) {
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(key.c_str()).x - 16);
            ImGui::TextDisabled("%s", key.c_str());
        }
        if (!on) ImGui::PopStyleColor();
    }
    if (found.empty()) ImGui::TextDisabled("No command matches.");
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
    if (!ImGui::Begin("Keyboard Shortcuts", &showKeys_)) {
        ImGui::End();
        return;
    }
    static char filter[64];
    ImGui::SetNextItemWidth(240 * imgui_->scale());
    ImGui::InputTextWithHint("##kf", "Filter", filter, sizeof filter);
    ImGui::SameLine();
    if (ImGui::Button("Reset All")) {
        commands_.resetAll();
        saveSettings();
    }
    const auto conflicts = commands_.conflicts();
    if (!conflicts.empty()) {
        ImGui::TextColored(editor::themeColours().error, "%zu shortcut conflicts (the first command listed wins)", conflicts.size());
    }
    if (rebinding_) {
        ImGui::TextColored(editor::themeColours().warning, "Press the new shortcut for \"%s\" (Escape cancels, Backspace clears)",
                           commands_.find(*rebinding_)->label.c_str());
    }
    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Menu", ImGuiTableColumnFlags_WidthFixed, 70 * imgui_->scale());
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 150 * imgui_->scale());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * imgui_->scale());
        ImGui::TableHeadersRow();
        const std::string f = lowered(filter);
        for (const editor::Command& c : commands_.commands()) {
            if (!f.empty() && lowered(c.category + " " + c.label).find(f) == std::string::npos) continue;
            ImGui::PushID(c.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", c.category.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.label.c_str());
            help(c.help);
            ImGui::TableNextColumn();
            const editor::Shortcut s = commands_.shortcut(c.id);
            const bool clash = std::ranges::any_of(conflicts, [&](const auto& p) { return p.first == c.id || p.second == c.id; });
            if (clash) ImGui::PushStyleColor(ImGuiCol_Text, editor::themeColours().error);
            const std::string shown = rebinding_ == c.id ? std::string("...") : s.empty() ? std::string("(none)") : s.text();
            if (ImGui::Button(shown.c_str(), ImVec2(-FLT_MIN, 0))) rebinding_ = c.id;
            if (clash) {
                ImGui::PopStyleColor();
                for (const auto& [a, b] : conflicts) {
                    if (a == c.id || b == c.id) help("Also on " + commands_.find(a == c.id ? b : a)->label);
                }
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!commands_.overrides().contains(c.id));
            if (ImGui::SmallButton("Reset")) {
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
    if (!ImGui::Begin("Editor Settings", &showSettings_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::SliderFloat("Interface scale", &settings_.uiScale, 0.75f, 2.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) styleDirty_ = true;
    help("On top of the display's own scale.");
    themeCombo("Theme##settings", 0);
    help("Light, Medium (NoX Core's stone greys and gold) or Dark (near-black with blue accents). Also at the menu bar's right end.");
    ImGui::SliderInt("Autosave (minutes)", &settings_.autosaveMinutes, 0, 30);
    help("A recovery copy in your settings folder (File > Open Autosave); 0 turns it off.");
    ImGui::BeginDisabled(!imgui_->floatingSupported());
    if (ImGui::Checkbox("Allow floating panels", &settings_.floatingPanels)) {
        floatingDirty_ = true; // (applied before the next frame)
    }
    ImGui::EndDisabled();
    if (imgui_->floatingSupported()) {
        help("Drag a panel's tab or title bar out of the window and it becomes a window of its own, for another monitor. "
             "Floating panels keep their places with the layout. The Map panel stays in the main window. Off: panels stay inside it.");
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(not on this system: e.g. Wayland can't place our windows)");
    }
    ImGui::SeparatorText("Scripting");
    if (ImGui::Checkbox("Allow NoX Core-only script features (object variables)", &settings_.allowNoxCoreScript)) {
        saveSettings(); // (the Script panel compiles again with it)
    }
    help("Lets scripts declare and use object variables (int object.hp;). Maps that use them crash the original game "
         "and need a NoX Core server. Off: the compiler refuses them, and a map whose script already has them is shown read-only.");
    ImGui::TextDisabled("Maps that use them crash the original game and need a NoX Core server.");
    ImGui::TextDisabled("Settings: %s", pathText(ctx_.prefDir).c_str());
    if (ImGui::Button("Close")) {
        showSettings_ = false;
        saveSettings();
    }
    ImGui::End();
}

} // namespace noxsdl
