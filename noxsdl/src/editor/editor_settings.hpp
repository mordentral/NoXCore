// The map editor's per-user settings: recent maps, the interface's scale and theme, the layers
// shown, the selection's options and the key bindings. Kept in the user's settings folder
// (SDL's pref path: %APPDATA%\noxcpp\noxsdl\editor), never in the game folder. (No SDL here:
// editorPrefDir is pref_dir.cpp's, so the tests can read and write the settings.)
#pragma once

#include "editor/builtin_list.hpp"
#include "editor/theme.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace noxsdl::editor {

struct Layers {
    bool floor = true;
    bool edges = true;
    bool walls = true;
    bool objects = true;
    bool waypoints = true;
    bool polygons = false;
    bool lighting = false;
    bool seeThroughWalls = false; ///< walls below the view's centre half see-through, as the game draws them
    int grid = 1;    ///< 0 off, 1 wall cells, 2 floor tiles
    int extents = 0; ///< 0 hidden, 1 colliding objects, 2 all
    bool labelTypes = false;
    bool labelScriptNames = false;
    bool labelExtents = false;
    bool facing = false;
    bool markDisabled = true;
    bool schematic = false;
    bool specialWalls = true; ///< secret, breakable and window walls tinted
    bool triggers = true;     ///< triggers' and pressure plates' boxes (their xfer's size), 45 degrees turned
    bool lightRings = false;  ///< invisible lights' radius rings
    bool links = true;        ///< teleporter, elevator, mover, glyph, exit and pit links as lines (V6)
    bool extents3d = false;   ///< extents with a raised copy at the type's height (the old "3D extents")
    bool livePlayers = true;    ///< a session's players where they are now (view only; the session feeds it)
    bool liveCreatures = false; ///< a session's creatures and moving things where they are now
    /// A session's edits not yet published ("Pending changes"): 0 Shaded (greyed; what they remove
    /// as ghosts), 1 As final, 2 Hidden (the published map).
    int pendingView = 0;
    // Locked layers: what can't be picked or changed by the tools (the layer still shows).
    bool lockFloor = false;
    bool lockWalls = false;
    bool lockObjects = false;
    bool lockWaypoints = false;
    bool lockPolygons = false;
};

struct EditorSettings {
    static constexpr std::size_t kMaxRecent = 10;

    std::vector<std::string> recent; ///< map paths, newest first
    float uiScale = 1.0f;
    /// "ui.theme=dark|medium|light". Settings from before it kept only "lightTheme=1|0": light
    /// stays Light, the old dark (now Medium) or nothing at all becomes the new default, Dark.
    Theme theme = Theme::Dark;
    Layers layers;
    nox::mapedit::KindFilter pick{.objects = true, .walls = true, .tiles = false, .waypoints = true};
    int regionShape = 0;      ///< nox::mapedit::Region::Shape
    int snap = 0;             ///< 0 off, 1 grid (23), 2 half grid, 3 custom
    float snapCustom = 8.0f;
    nox::mapedit::CopyLayers copyLayers;
    int autosaveMinutes = 3;  ///< 0: off
    bool playFullWindow = false; ///< play-tests over the whole window, not in the Map panel (play_view.hpp)
    /// Panels can be dragged out of the main window as windows of their own, onto another monitor
    /// (imgui_host.hpp). Off: they stay inside it, as before.
    bool floatingPanels = true;
    /// NoX Core-only script features (object variables): off, the Script panel's compiles refuse
    /// them (CompileOptions::allowObjectVariables). Maps that use them crash the original game.
    bool allowNoxCoreScript = false;
    float scriptFontScale = 1.0f; ///< the Script panel's source size against the panels' (Ctrl+wheel, Ctrl+= / Ctrl+-)
    /// Script Reference's Builtins list: each builtin's priority the user set (its context menu;
    /// Normal ones aren't kept), the list's order (BuiltinSort) and its favourites-only filter.
    BuiltinPriorities builtinPriorities;
    int builtinSort = 0;
    bool builtinFavouritesOnly = false;
    /// A collaborative session host's "approve always" players: editors whenever they join this
    /// user's sessions, by account name.
    std::vector<std::string> sessionAlwaysEditors;
    std::string bindings;     ///< CommandRegistry::saveBindings

    void addRecent(const std::string& path);
    void load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;
};

/// The user's settings folder for the editor (made if missing).
std::filesystem::path editorPrefDir();

} // namespace noxsdl::editor
