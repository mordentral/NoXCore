// The map editor: a docked Dear ImGui shell round the map, drawn live from the document by noxsdl's
// MapView. The document is noxcore/mapedit's MapDocument; every change is an Op through the History
// (undo/redo); the selection is a lock request through the LockTable (always granted in
// single-player; the host's in a session).
//
// The shell: documents (new, open .map / .nxz, save, save as, recent, autosave), the viewport (pan,
// zoom, grid, layers), selection (click, Shift / Ctrl, box, 45-degree box, lasso), move, copy /
// paste / duplicate / area copy, delete, the command registry with its palette and rebindable keys,
// the History, Problems, Layers, Properties and Tool Options panels, and the status bar. The tools
// and their panels have files of their own (map_editor_tools.cpp, object_tools.hpp,
// marker_tools.hpp, property_panel.hpp, script_panel.hpp).
//
// Alone, it edits a map document. (A play-test's game shows in its Map panel, drawn by main.cpp
// into a texture: play_view.hpp.) In a collaborative session (attachSession) the document is the
// session's working copy, its ops go through the host, and the running game's players show in the
// live layer; the game itself runs on behind it (F2 toggles the game and the editor).
#pragma once

#include "camera.hpp"
#include "editor/chat_log.hpp"
#include "editor/commands.hpp"
#include "editor/editor_settings.hpp"
#include "editor/held_edit.hpp"
#include "editor/imgui_host.hpp"
#include "editor/play_test.hpp"
#include "editor/session_setup.hpp"
#include "lighting.hpp"
#include "map_view.hpp"

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/drag_preview.hpp"
#include "noxcore/mapedit/locks.hpp"
#include "noxcore/mapedit/new_map.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/paint.hpp"
#include "noxcore/mapedit/selection.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/mapedit/validate.hpp"
#include "noxcore/mapedit/walls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace nox::things {
struct ThingDb;
struct ModifierDb;
struct GameData;
} // namespace nox::things

namespace nox::client {
class EditSessionClient;
class World;
}

namespace nox::strings {
class StringTable;
}

namespace noxsdl {

class ObjectTools; // (the content browser and the object tools: object_tools.hpp)
class MarkerTools; // (waypoints, polygons, groups, map info, minimap, strings: marker_tools.hpp)
class PropertyPanel; // (the objects' properties: property_panel.hpp)
class SpriteCache;
class ThingArt;
class ScriptPanel;
class ModPanel;
class PlayView;

class MapEditor {
public:
    struct Context {
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        const nox::things::ThingDb* things = nullptr;
        ThingArt* art = nullptr;
        const nox::things::ModifierDb* modifiers = nullptr;
        const nox::strings::StringTable* strings = nullptr; ///< nox.csf (the content browser's display names); may be null
        SpriteCache* sprites = nullptr;
        const nox::things::GameData* gameData = nullptr; ///< gamedata.bin (new objects' ammunition)
        std::filesystem::path gameDir;   ///< the game's folder (video.bag, monster.bin: thumbnails, new creatures)
        std::filesystem::path imagePack; ///< the HD pack the game draws from (empty: video.bag): the thumbnails'
        std::filesystem::path userDir;   ///< favourites, prefabs, categories and the thumbnail cache (empty: prefDir; read-only in a test's run)
        std::filesystem::path mapsDir;  ///< where maps are listed, opened from and saved (the game's maps\)
        std::filesystem::path prefDir;  ///< the user's editor settings folder
        bool screenshotRun = false;     ///< a test's hidden run: no layout or settings written
        editor::PlayTestHooks playTest; ///< the play-test's host (main.cpp); unset: none (script_panel.hpp)
        std::filesystem::path playTestDir; ///< where a play-test's map is written (empty: prefDir\playtest)
        std::filesystem::path modsDir;     ///< the mods folder mod projects live in (mods\ beside noxcore.exe; empty: no Mod menu)
        /// File > Host a Session...: the session dialog, then the session hosted, listed and
        /// joined (main.cpp); unset: not offered.
        std::function<void()> hostSession;
    };

    explicit MapEditor(Context context);
    ~MapEditor();
    MapEditor(const MapEditor&) = delete;
    MapEditor& operator=(const MapEditor&) = delete;

    /// Opens a map file (.map, or .nxz); false (with the reason shown) if it can't be read.
    bool open(const std::filesystem::path& path);
    /// Opens the start dialog (New / Open / Recent) over the current, untouched blank map.
    void showStart() { startOpen_ = true; }
    void handleEvent(const SDL_Event& event);
    /// One frame onto the window (the screen target): the map into its panel, the panels over it.
    void frame();
    /// The window's close button: true to quit now; else the unsaved-changes prompt (or, hosting
    /// a session, "End the session?") is up.
    bool requestQuit();
    /// Whether the editor asked to be closed (File > Close, confirmed: main.cpp's closeEditor
    /// does what close_plan.hpp says for where it stands).
    bool wantsClose() const { return wantsClose_; }
    /// Asks to be closed at the end of the frame (the main loop closes it): never destroy the
    /// editor from inside one of its own callbacks.
    void requestClose() { wantsClose_ = true; }
    /// Whether the unsaved-changes prompt a requestQuit opened ended in quitting.
    bool quitConfirmed() const { return quitConfirmed_; }
    /// Runs commands by id, ';'-separated, as if picked from the palette ("select.all;edit.copy").
    /// Tests and --editor-run use it; "wait:N" holds the rest for N frames.
    void runCommands(const std::string& list);
    /// While the play-test (or a session's game) has the window: the script panel's file watch and
    /// idle work (no drawing); the floating panels' windows put away till the editor's next frame.
    void backgroundTick();

    /// What a "pick in view" click found (links, points and members picked on the map).
    struct PickResult {
        nox::mapedit::WorldPoint at;                ///< the click (snapped as the toolbar's Snap says, when asked)
        std::optional<nox::mapedit::ObjId> object;  ///< the world object under it (Kind::Object / Any)
        std::optional<std::uint32_t> waypoint;      ///< the waypoint under it (Kind::Waypoint / Any)
        std::optional<nox::mapedit::Cell> wall;     ///< the wall under it (Kind::Wall / Any)
        bool shift = false;                         ///< Shift held (e.g. "link both ways", "remove member")
    };
    /// A field asking for a click on the map: the next left click in the Map panel answers it (a
    /// click on nothing of the kind asked is ignored, but for Point); right click or Escape
    /// cancels. `repeat`: it stays armed after a pick (members picked one after another).
    struct PickRequest {
        enum class Kind { Point, Object, Waypoint, Wall, Any };
        Kind kind = Kind::Point;
        std::string prompt;                         ///< shown over the map ("Click the transporter it leads to")
        std::function<void(const PickResult&)> done;
        bool repeat = false;
        bool snap = false;                          ///< Point: snapped as the toolbar's Snap
        std::optional<nox::mapedit::WorldPoint> from; ///< a line drawn from here to the mouse (a link being picked)
        std::string owner;                          ///< which field asked (a second request from it replaces this one)
    };
    /// One thing of a session's running game: a player, or a creature or other moving thing, where
    /// it is now. View only: never a document object (it can't be picked or edited).
    struct LiveEntity {
        std::string name;               ///< a player's name, or the creature's type
        float x = 0, y = 0;
        std::uint8_t team = 0;
        bool player = true;             ///< false: a creature or moving thing
        bool stockClient = false;       ///< a player on a stock GAME.EXE client (else noxsdl)
        std::uint16_t id = 0;           ///< its net code (a session's: follow it, jump in at it)
        // What it's drawn with, as the game draws it (none known: the marker alone).
        std::uint16_t typeId = 0;       ///< its thing type (THNG index + 1; 0: unknown)
        std::uint8_t direction = 7;     ///< the client's AnimDir (0-3, 5-8)
        std::uint8_t animState = 0;     ///< a player's or creature's animation state
        std::optional<std::uint8_t> frame; ///< the server's animation frame, when it sends one
        std::uint32_t armorEquip = 0;   ///< (players) worn armour class bits
        std::uint32_t weaponEquip = 0;  ///< (players) worn weapon class bits
        bool observer = false;          ///< (players) observing: no body in the game, the marker alone
    };
    /// The live layer's things, replaced whole (the session feeds it each frame; empty outside a
    /// session). Each one's drawn position moves smoothly from where it was drawn to the new one
    /// over about the time between two of these (the game view's interpolation).
    void setLiveEntities(std::vector<LiveEntity> entities);

    // ---- The collaborative session (session_panel.cpp)
    /// What the editor asks of its host program in a session.
    struct SessionHooks {
        /// Play (F2): the session's game (in the Map panel, or over the window with Play in Full
        /// Window); our character back from where it stood aside, at the camera's spot.
        std::function<void()> toGame;
        /// The same, our character left where the session put it (after "Jump in here").
        std::function<void()> toGameAsIs;
        /// Play From Here: Play, our character in at this world point (the cursor's) instead of
        /// the camera's spot.
        std::function<void(float x, float y)> toGameAt;
        /// The Chat panel's line into the game, as a player chatting sends it (MSG_TEXT_MESSAGE
        /// through our game connection; `team`: to our team only).
        std::function<void(const std::string& text, bool team)> say;
        /// Our game connection's world (the players' colours and worn things' modifiers for the
        /// live layer's bodies); null while there is none.
        std::function<const nox::client::World*()> gameWorld;
        /// Our own session (we host it): closing the editor, or the window, ends it for everyone,
        /// so both ask first (close_plan.hpp). Else Close Editor puts the session's game in front.
        bool hosting = false;
    };
    /// A line for the Chat panel (main.cpp: the session's game events, chat_log.hpp's chatLineFrom
    /// with the game's strings).
    void addChat(editor::ChatLine line);
    /// Edits go through `session` (the client side of the session main.cpp's game connection
    /// runs): the document becomes the session's working copy, ops are sent to the host (and
    /// rolled back if it refuses them), the selection is a lock request to the host, and the live
    /// view, the other editors' presence and locks, the session panel and the edit request toast
    /// show. Null detaches (the session ended: the document stays, as an unsaved copy).
    void attachSession(nox::client::EditSessionClient* session, SessionHooks hooks);
    // (An overload, not a default argument: GCC can't take a nested struct's default member
    // initializers in a default argument inside the class.)
    void attachSession(nox::client::EditSessionClient* session) { attachSession(session, SessionHooks{}); }
    bool inSession() const { return session_ != nullptr; }
    /// Play From Here in a session: the session's Play, our character in at `x, y`.
    void sessionPlayFrom(float x, float y);
    /// The view's centre in world units (where Play brings our character in a session).
    std::pair<float, float> cameraCentre() const { return {camera_.x, camera_.y}; }
    /// Play in Full Window (the Play menu): a session's Play over the window instead of the panel.
    bool playFullWindow() const { return settings_.playFullWindow; }
    /// The players a session's host lets edit whenever they join ("Approve always"), kept in the
    /// editor's settings for the host's next sessions.
    const std::vector<std::string>& sessionAlwaysEditors() const { return settings_.sessionAlwaysEditors; }
    void setSessionAlwaysEditors(std::vector<std::string> names);
    /// Host Custom > Map Editor, File > Host a Session: the session setup dialog (session_setup.hpp);
    /// `host` gets the settings when Host is pressed (main.cpp hosts and joins). `cancelled`: the
    /// dialog closed.
    void showSessionSetup(std::function<void(const editor::SessionSetup&)> host, std::function<void()> cancelled = {});
    /// What the session dialog says of where Host lists the session (main.cpp: online with an
    /// XWIS login, else the LAN only), from its next opening.
    void setSessionListingNote(editor::SessionListingNote note) { setupListing_ = std::move(note); }

    void beginPick(PickRequest request);
    void cancelPick();
    bool picking() const { return pick_.has_value(); }
    const PickRequest* pickRequest() const { return pick_ ? &*pick_ : nullptr; }

    /// The Map panel's rectangle in window coordinates, as last drawn (where an in-panel
    /// play-test's game goes: play_view.hpp); empty while the panel isn't shown.
    SDL_FRect playViewport() const;
    /// A key pressed while the play-test's game has the keyboard: true when it's one of the
    /// editor's Play keys (Stop, Pause, Release Mouse), run and kept from the game.
    bool playKey(const SDL_Event& event);

private:
    friend class PlayView;    // (the play-test in the Map panel and the toolbar: play_view.hpp)
    friend class ScriptPanel; // (the scripting: script_panel.hpp)
    friend class ModPanel;    // (mod projects: mod_panel.hpp)
    friend class ObjectTools; // (the content browser and object tools: object_tools.hpp)
    friend class MarkerTools; // (waypoints, polygons, groups, map info: marker_tools.hpp)
    friend class PropertyPanel; // (the objects' properties: property_panel.hpp)

    enum class Tool { Select, Wall, Floor, Edge, Object, Waypoint, Polygon };
    /// The wall tool's modes (the old editor's place, brush, line, rectangle, bucket and
    /// properties modes).
    enum class WallMode { Draw, Place, Line, Rect, Outline, Erase, Pick };
    /// The edge tool's modes: auto-edge where clicked or dragged, add the chosen edge, remove.
    enum class EdgeMode { Auto, Add, Remove };
    enum class Drag { None, Pan, Region, Move, AreaCopy };
    enum class Pending { None, New, Open, OpenRecent, Close, Quit };

    // map_editor.cpp: documents, commands, ops
    void registerCommands();
    /// One of runCommands' steps (a frame's): a command by id, or a test's step.
    void runStep(std::string step);
    void newDocument(const nox::mapedit::NewMapOptions& options);
    void resetDocumentState();
    bool save();
    bool saveAs(const std::string& name);
    /// Whether Save asks for a name: none yet (a stock map's edits, a new map), or one the game's
    /// map lists can't show (over 8 characters: a map named before the limit still opens).
    bool saveNeedsName() const;
    /// The name Save As starts with: the map's own, else a copy's (suggestedMapName) that fits.
    std::string saveAsOffer() const;
    void autosave();
    bool perform(const nox::mapedit::Op& op);
    void undo();
    void redo();
    void jumpHistory(std::size_t position);
    void setSelection(nox::mapedit::Selection s, bool quiet = false);
    void copySelection(bool cut);
    void paste(std::optional<nox::mapedit::Cell> at);
    void duplicate();
    void deleteSelection();
    void nudge(int dx, int dy, bool cell);
    void frameSelection();
    void frameWorld(float x0, float y0, float x1, float y1);
    void locate(const nox::mapedit::Issue& issue);
    void status(std::string text);
    void guarded(Pending action, std::function<void()> then);
    void saveSettings();
    std::string title() const;
    std::string elementName(const nox::mapedit::ElementKey& key) const;
    std::string peerName(nox::mapedit::PeerId peer) const;
    /// The editor holding key when it's another than us (the lock table's holder), else nothing.
    std::optional<nox::mapedit::PeerId> otherHolder(const nox::mapedit::ElementKey& key) const;

    // map_editor_view.cpp: the viewport
    void rebuildView();
    void renderMap();
    void mapPanel();
    void mapInput(const ImVec2& origin, bool hovered);
    void drawOverlays(ImDrawList* dl, const ImVec2& origin, const ImVec2& size);
    void mapContextMenu();
    float snapValue(float v) const;
    /// How far from the mouse a click takes things, in world units: 10 screen pixels, at least 6.
    float clickRadius() const { return std::max(6.0f, 10.0f / camera_.zoom); }
    std::string underCursor(float x, float y) const;
    std::optional<nox::mapedit::WorldPoint> mouseWorld() const;

    // map_editor_tools.cpp: the wall, floor and edge tools
    void initTools();
    void loadPaintSettings();
    void storePaintSettings();
    /// The view after an op the history applied: walls and tiles updated in place (no rebuild).
    void viewAfterOp(const nox::mapedit::Op& op);
    void viewTiles(const std::vector<nox::mapedit::TileCell>& tiles);
    void viewWalls();
    /// A tool's mouse in the map panel (Wall, Floor, Edge); true if it handled the frame's input.
    bool toolInput(const nox::mapedit::WorldPoint& w, bool hovered);
    void wallInput(const nox::mapedit::WorldPoint& w, bool hovered);
    void floorInput(const nox::mapedit::WorldPoint& w, bool hovered);
    void edgeInput(const nox::mapedit::WorldPoint& w, bool hovered);
    /// A wall stroke's preview: the op for the stroke's cells applied to the document (its inverse
    /// kept), replaced as the stroke grows; committed or rolled back at its end.
    void wallPreview(const std::vector<nox::mapedit::Cell>& cells, bool erase);
    void endWallStroke(bool commit);
    void endFloorStroke(bool commit);
    nox::mapedit::WallBrush wallBrush() const;
    nox::mapedit::FloorBrush floorBrush() const;
    nox::mapedit::AutoEdgeOptions edgeOptions() const;
    void eyedropper(const nox::mapedit::WorldPoint& w, bool full);
    void drawToolOverlay(ImDrawList* dl, const ImVec2& origin);
    void wallToolPanel();
    void floorToolPanel();
    void edgeToolPanel();
    void wallProperties(const std::set<nox::mapedit::Cell>& cells);
    /// An InputInt (with - / +) for a panel's number that's committed as an op: typed into or
    /// held, its own count till let go; then the value to commit, once (editor/held_edit.hpp).
    std::optional<int> heldInputInt(const char* id, int current, int lo, int hi);
    std::map<unsigned int, std::pair<editor::HeldValue<int>, int>> heldInts_; ///< by ImGui id: the state, the frame last drawn
    editor::LiveDrag variantDrag_; ///< the wall Variant slider, previewed live while dragged
    void edgeRulesWindow();
    /// A thumbnail of an image (decoded original), made once; null if it has none.
    SDL_Texture* thumb(const std::string& key, const std::function<std::vector<std::uint8_t>(int& w, int& h)>& build);
    SDL_Texture* floorThumb(int floor, int variant);
    SDL_Texture* wallThumb(int material, int facing, int variant);
    SDL_Texture* edgeThumb(int edge, int direction, int overFloor, int baseFloor);
    /// A list to pick a floor / edge set / wall material from, with thumbnails and a filter.
    bool floorPicker(const char* id, int& floor, bool allowNone = false);
    bool edgePicker(const char* id, int& edge);
    bool wallPicker(const char* id, int& material);
    /// --editor-run's tool steps (floor:, wall:, brush: ...); false if `step` isn't one.
    bool toolStep(const std::string& step);

    // map_editor_ui.cpp: the panels and dialogs
    void buildUi();
    void mainMenu();
    void toolbar();
    void statusBar();
    void defaultLayout(unsigned int dockspace);
    void layersPanel();
    void historyPanel();
    void problemsPanel();
    void propertiesPanel();
    void propertiesContent(); ///< (what propertiesPanel shows for selection_)
    void toolPanel();
    void dialogs();
    void palette();
    void keysWindow();
    void settingsWindow();
    void openDialog();
    void newDialog();
    void saveAsDialog();
    void startDialog();
    void unsavedDialog();
    /// "End the session?" (hosting a session: Close Editor, the window's close button): End
    /// Session closes the editor (main.cpp ends the session and goes to the main menu) or quits.
    void endSessionDialog();
    void menuItem(const char* id);
    void refreshMapList();
    void runValidation();

    Context ctx_;
    const nox::things::ThingDb& things_;
    std::filesystem::path settingsPath_;
    editor::EditorSettings settings_;
    std::unique_ptr<editor::ImGuiHost> imgui_;
    editor::CommandRegistry commands_;

    // The document.
    nox::mapedit::MapDocument doc_;
    nox::mapedit::History history_;
    nox::mapedit::Sidecar sidecar_;
    nox::mapedit::Selection selection_;
    /// The selection at each history position (dropped + position), so undo and redo bring back
    /// what was selected there.
    std::map<std::size_t, nox::mapedit::Selection> selectionAt_;
    /// What Properties showed last frame, and whether one of its fields was being typed in then: a
    /// click elsewhere that changes the selection ends the typing, and the edit goes to what the
    /// field belonged to (the panel shows it one more frame), never to the new selection.
    nox::mapedit::Selection propsShown_;
    bool propsTyping_ = false;
    nox::mapedit::LockTable locks_;
    nox::mapedit::PeerId self_ = 0;
    std::filesystem::path docPath_; ///< the file it came from or was saved to (empty: new)
    std::string docName_;           ///< the name Save writes under (empty: Save asks; a stock map's edits)
    std::optional<nox::mapedit::Clip> clip_;
    std::uint64_t autosaveAt_ = 0;

    // The viewport.
    std::unique_ptr<MapView> view_;
    std::uint64_t layerRevision_ = 0;
    std::uint64_t viewBuiltFor_ = ~0ull;
    /// The HD pack's images of the document asked of the decode workers ahead (SpriteCache::
    /// beginMap with the view's preload list, while no game is up): the document generation it
    /// was done for (docGeneration_ counts documents opened or made).
    std::uint64_t docGeneration_ = 1;
    std::uint64_t prefetchedFor_ = 0;
    /// No game shares the sprite cache now (no play-test, no session): the editor's draws may
    /// defer HD decodes and ask for the map's images ahead.
    bool spritesOwned() const;
    void prefetchImages(const nox::map::MapFile& file);
    Camera camera_;
    LightMap light_;
    SDL_Texture* mapTarget_ = nullptr;
    int targetW_ = 0, targetH_ = 0;
    float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0; ///< the map panel's screen rectangle
    bool mapHovered_ = false;
    bool cameraSet_ = false;
    Drag drag_ = Drag::None;
    nox::mapedit::WorldPoint dragStart_{};
    float dragStartSx_ = 0, dragStartSy_ = 0;
    bool dragMoved_ = false;
    nox::mapedit::Region region_;
    nox::mapedit::Selection dragPick_;
    nox::mapedit::WorldPoint moveAnchor_{};
    float moveDx_ = 0, moveDy_ = 0;
    int dragMods_ = 0; ///< 1 shift, 2 ctrl
    bool areaCopyArmed_ = false;
    bool rightDragging_ = false;
    float rightDownX_ = -1, rightDownY_ = 0; ///< where the right button went down in the map (-1: not)
    // A drag's live preview (noxcore/mapedit/drag_preview.hpp): the op a move drag (the selection,
    // the gizmo, waypoints, polygons and their corners) would perform on release, applied in place
    // while the mouse is held, so the map draws the things moving. Still one op, on release; a
    // session's news wait meanwhile, as for a stroke (strokeActive), and nothing is sent mid-drag.
    nox::mapedit::DragPreview preview_;
    std::optional<std::array<float, 4>> previewKey_; ///< what the preview was built for (the drag's offset)
    std::optional<nox::mapedit::Selection> previewSelectionBefore_; ///< the selection the preview's move replaced
    int moveCx_ = 0, moveCy_ = 0; ///< the selection drag's wall-cell step, as its preview made it
    /// The drag's preview for this frame: `build` makes the op the release would perform, from the
    /// document as it was (the shown one taken back first); not again while `key` is unchanged. A
    /// move's `moved` selection (if `build` gives one) stands for the selection while it's shown. An
    /// op touching what another editor holds shows nothing (its release is refused as ever).
    void previewDrag(const std::array<float, 4>& key,
                     const std::function<nox::mapedit::Op(std::optional<nox::mapedit::Selection>& moved)>& build);
    /// The preview taken back (a drag's release, before its op; or its cancel), the view redrawn.
    void endPreview();
    /// Whether a move drag of any kind is under way (the selection, the gizmo, the marker tools').
    bool moveDragging() const;
    /// Escape or a right click mid-drag: every drag dropped and its preview taken back, nothing
    /// done. Returns whether one was under way.
    bool cancelDrags();
    bool styleDirty_ = false;                 ///< the scale or theme changed: the style made again next frame
    bool openThemeMenu_ = false;              ///< (tests) the menu bar's Theme drop-down opened next frame
    /// The theme picked (the menu bar's drop-down, Editor Settings): applied between frames, saved.
    void setTheme(editor::Theme theme);
    /// The Theme drop-down (the menu bar's right end; Editor Settings has one too). True if a
    /// theme was picked.
    bool themeCombo(const char* id, float width);
    nox::mapedit::WorldPoint contextAt_{};
    struct Flash {
        nox::mapedit::WorldPoint at;
        std::uint64_t startMs = 0;
    };
    std::optional<Flash> flash_;

    // Panels and dialogs.
    Tool tool_ = Tool::Select;
    Tool lastTool_ = Tool::Select;
    bool showLayers_ = true, showHistory_ = true, showProblems_ = true, showProperties_ = true, showTool_ = true;
    bool showKeys_ = false, showSettings_ = false, showAbout_ = false;
    bool paletteOpen_ = false, paletteFocus_ = false;
    std::string paletteQuery_;
    int paletteIndex_ = 0;
    bool startOpen_ = false, newOpen_ = false, openOpen_ = false, saveAsOpen_ = false, unsavedOpen_ = false;
    bool resetLayout_ = false;
    std::string loadLayout_; ///< a named layout to load before the next frame
    char nameBuf_[64] = {};
    char searchBuf_[128] = {};
    char layoutBuf_[64] = {};
    nox::mapedit::NewMapOptions newOptions_;
    struct MapEntry {
        std::string name;
        std::filesystem::path path;
        bool stock = false;
    };
    std::vector<MapEntry> mapList_;
    std::optional<std::string> rebinding_; ///< a command waiting for its new shortcut
    Pending pending_ = Pending::None;
    std::function<void()> afterSave_;
    std::function<void()> pendingThen_;
    bool wantsClose_ = false;
    bool quitConfirmed_ = false;
    bool endSessionOpen_ = false;  ///< the "End the session?" prompt to open at the next frame
    bool endSessionQuits_ = false; ///< it was the window's close button: End Session quits
    bool endSessionAsked_ = false; ///< the prompt is up (a second Close or close button doesn't stack another)
    int endSessionAnswer_ = 0;     ///< (tests: session-end:confirm|cancel) 1 End Session, 2 Cancel, pressed at the next frame
    std::string statusText_;
    std::uint64_t statusMs_ = 0;
    std::string errorText_; ///< an error to show in a modal
    std::vector<std::string> queued_; ///< runCommands' steps still to do
    int queuedWait_ = 0;
    float testMouseX_ = 0, testMouseY_ = 0; ///< runCommands' mouse (tests)
    std::deque<std::string> testText_;      ///< runCommands' typed text (tests: the events point into it)
    /// Floating panels (imgui_host.hpp): the setting changed (applied before the next frame); a
    /// test's "float:" step, a panel to put out of the main window at the next frame.
    bool floatingDirty_ = false;
    struct FloatRequest {
        std::string window;
        float dx = 0, dy = 0, w = 0, h = 0; ///< from the main window's top left
        int frames = 0;                     ///< frames left to place it (it undocks first)
    };
    std::optional<FloatRequest> floatRequest_;
    void applyFloatRequest();
    /// ImGui's coordinates of the main window's top left (the desktop's while panels can float).
    ImVec2 windowOrigin() const;

    // The wall, floor and edge tools.
    WallMode wallMode_ = WallMode::Draw;
    int wallMaterial_ = 0;
    int wallFacing_ = 0;
    bool wallAutoFacing_ = true;
    bool wallSmart_ = false;
    bool wallAutoVariation_ = true;
    int wallVariantMin_ = 0, wallVariantMax_ = 0;
    bool wallGroupAuto_ = true;
    int wallGroup_ = 100;
    bool wallWindow_ = false, wallBreakable_ = false, wallSecret_ = false;
    nox::map::SecretWall wallSecretProps_{};
    bool wallReplace_ = true;
    bool wallReface_ = true;
    bool wallStroking_ = false;
    bool wallErasing_ = false;
    std::vector<nox::mapedit::Cell> wallStroke_;
    nox::mapedit::Cell wallStart_{};
    std::optional<nox::mapedit::Op> wallPreviewUndo_; ///< undoes the stroke's preview
    nox::mapedit::FloorBrush brush_;
    bool floorOnTop_ = false;
    int floorExclude_ = -1, floorOnly_ = -1, floorCover_ = -1, floorHoleTrim_ = -1;
    bool floorWallBlock_ = true;
    std::optional<nox::mapedit::FloorStroke> floorStroke_;
    nox::mapedit::WorldPoint lastDab_{};
    EdgeMode edgeMode_ = EdgeMode::Add;
    int edgeSet_ = 0;
    int edgeDirection_ = 0;
    int edgeFloor_ = 0;
    bool edgeMarkCustom_ = true;
    std::set<nox::mapedit::TileCell> edgeStroke_;
    std::uint32_t strokeSeed_ = 1; ///< counts strokes: the seed of their random picks (never a clock)
    bool toolRightClick_ = false;  ///< a right click (no drag) for the tool this frame
    bool strokeActive() const { return wallStroking_ || floorStroke_.has_value() || !edgeStroke_.empty() || preview_.active(); }
    nox::mapedit::EdgeRules rules_;
    nox::mapedit::PaintSettings paint_;
    bool showEdgeRules_ = false;
    std::map<std::string, SDL_Texture*> thumbs_;
    char pickerFilter_[64] = {};
    /// A type's collision shape as the Extents layer draws it (its EXTENT, FLAGS and ZSIZE read
    /// once, not per object each frame: ThingDb::findThing walks every type).
    struct TypeExtent {
        bool known = false;     ///< the type is in thing.bin
        bool colliding = true;  ///< FLAGS without NO_COLLIDE (an object's own flags say first)
        bool circle = false, box = false;
        bool haveA = false, haveB = false;
        float a = 0, b = 0;
        std::optional<float> zTop; ///< ZSIZE's second number (the 3D extents' height)
    };
    std::map<std::string, TypeExtent, std::less<>> typeExtents_;
    const TypeExtent& typeExtent(const std::string& type);

    // Problems.
    std::vector<nox::mapedit::Issue> issues_;
    std::future<std::vector<nox::mapedit::Issue>> validating_;
    std::uint64_t validatedRevision_ = ~0ull;
    std::uint64_t changedMs_ = 0;

    // A file picked in the system's dialog (its callback may come from another thread).
    std::mutex pickedMutex_;
    std::optional<std::string> picked_;

    std::unique_ptr<ScriptPanel> scriptPanel_; ///< the Script panels, play-test and hot reload
    std::unique_ptr<ModPanel> modPanel_;       ///< mod projects: the Mod menu and windows
    std::unique_ptr<PlayView> playView_;       ///< the play-test in the Map panel, the toolbar's Play / Pause / Stop
    std::unique_ptr<ObjectTools> objects_;     ///< the content browser, object placement, the object list
    std::unique_ptr<MarkerTools> markers_;     ///< waypoints, polygons, groups, map info, minimap, strings
    std::unique_ptr<PropertyPanel> props_;     ///< the objects' properties
    std::optional<PickRequest> pick_;          ///< a field's "pick in view" waiting for a click
    std::vector<LiveEntity> live_;             ///< a session's live players and creatures (view only)
    void drawLive(ImDrawList* dl, const ImVec2& origin);
    // Each live thing's drawing: its drawn position gliding from where it was drawn to where the
    // last update put it (by net code), and when its animation state began.
    struct LiveTrack {
        float fromX = 0, fromY = 0;   ///< where it was drawn when the update came
        float toX = 0, toY = 0;       ///< where the update put it
        std::uint64_t atMs = 0;       ///< when the update came
        std::uint8_t state = 0;
        double animStart = 0;         ///< game frames: when `state` began (one-shot animations)
        float headY = 0;              ///< the top of its body as last drawn (world units above its feet), for its label
        bool drawn = false;           ///< drawn as its sprite last frame (else the marker)
    };
    std::map<std::uint16_t, LiveTrack> liveTracks_;
    std::uint64_t liveLastMs_ = 0;             ///< when the last update came
    float liveIntervalMs_ = 100;               ///< about how often they come (the glide's length)
    /// Where a live thing is drawn now (its glide; its update's place without a track).
    std::pair<float, float> livePos(const LiveEntity& e) const;
    /// The live things in view as the game draws them, for the map's scene (sorted with its
    /// objects): a player's body, armour and weapon layers in their animation and facing, a
    /// creature's image. Players observing, and things without art, are left to drawLive's marker.
    std::vector<MapView::SceneSprite> liveSprites();
    /// liveSprites' images again, drawn faint over the whole scene (renderMap).
    struct LiveOver {
        nox::things::ImageRef image;
        std::optional<MaterialColours> colours;
        float x = 0, y = 0;
    };
    std::vector<LiveOver> liveOver_;
    // The Chat panel (chat_panel.cpp).
    editor::ChatLog chat_;
    bool showChat_ = true;
    char chatInput_[256] = {};
    bool chatTeam_ = false;
    bool chatFocus_ = false;                   ///< (the chat-focus test step) the panel to the front next frame
    std::uint64_t chatSeenTotal_ = 0;          ///< lines the panel had when it last drew (it scrolls to new ones)
    void chatPanel();
    /// The live player the camera follows (a click on one), by net code; panning stops it.
    std::optional<std::uint16_t> follow_;
    std::optional<LiveEntity> liveMenu_;       ///< the live player a right click opened the menu for
    /// The live player under the mouse (players only, within a few screen pixels).
    const LiveEntity* liveAt(const ImVec2& origin, const ImVec2& mouse) const;
    /// The live layer's clicks: follow a player (left), its menu (right: Follow, Jump in here).
    /// True when it took the click.
    bool liveInput(const ImVec2& origin, bool hovered);
    void liveMenu();
    /// Our character into the session's game at a live player's spot, and Play.
    void jumpInAt(const LiveEntity& e);
    /// What clicks and boxes take: the toolbar's kinds without the locked layers'.
    nox::mapedit::KindFilter pickFilter() const;
    /// Elements held by an open panel (a group's, the map info's) besides the selection's: kept
    /// in every lock request the selection makes.
    std::set<nox::mapedit::ElementKey> panelLocks_;
    /// Holds (or lets go of) an element for an open panel: false when another editor holds it.
    bool holdPanel(const nox::mapedit::ElementKey& key, bool on);
    /// The session host's Take Over: an element held for a panel, taken from whoever has it.
    void takeOverPanel(const nox::mapedit::ElementKey& key);
    void drawPick(ImDrawList* dl, const ImVec2& origin);
    /// Picking's mouse: true when a pick request took the frame's input.
    bool pickInput(const nox::mapedit::WorldPoint& w, bool hovered);
    bool wallEdges_ = true;                    ///< placing or removing walls recomputes the floor edges at them
    bool moveDoor_ = false;                    ///< the move being dragged grabbed a door: it moves on the grid
    void viewObjects();                        ///< the view's objects made again from the document (no rebuild)
    void viewAmbient();                        ///< the view's ambient and polygons from the document (no rebuild)
    nox::mapedit::Op withWallEdges(nox::mapedit::Op op) const; ///< a wall op with the floor edges at its walls (wallEdges_)

    // The session (session_panel.cpp).
    nox::client::EditSessionClient* session_ = nullptr;
    SessionHooks sessionHooks_;
    std::uint64_t sessionRevision_ = 0;      ///< the session's revision the editor last matched
    std::uint32_t sessionLiveVersion_ = 0;
    std::map<std::uint32_t, std::size_t> sessionSeqs_; ///< our ops in flight: seq -> history entry (absolute)
    bool showSession_ = true;
    char requestReason_[128] = {};
    std::string sessionRoleText_;            ///< what the host last said about our role
    int playTestModeIndex_ = 0;
    bool playTestCanDie_ = false;
    editor::SessionSetupDialog setupDialog_;
    std::function<void(const editor::SessionSetup&)> setupHost_;
    std::function<void()> setupCancelled_;
    editor::SessionListingNote setupListing_;
    // A session's pending edits in the view: what the view was last given (its serial, the
    // document's revision, the published version, the setting and layers).
    struct PendingFor {
        std::uint64_t view = 0; ///< MapView::serial
        std::uint64_t revision = ~0ull;
        std::uint32_t version = ~0u;
        int mode = -1;
        int layers = -1;
        bool any = false;
    } pendingFor_;
    /// The plan last given to the view (the Minimap panel draws its walls: red pending, dark red
    /// removed) and a counter bumped each time it's replaced.
    nox::mapedit::PendingPlan pendingPlan_;
    std::uint64_t pendingPlanSerial_ = 0;
    int pendingKeptView_ = 0; ///< G (view.pendingPublished): the setting to go back to from Hidden
    /// The working copy's differences from the published map into view_ as the "Pending changes"
    /// setting draws them (renderMap, each frame: worked out again only when something changed).
    void updatePendingView();
    /// Each frame in a session: the session's events (others' ops into the view, our acks and
    /// refusals), the lock table, the live view, our presence.
    void sessionFrame();
    void sessionPanel();
    void requestToast();
    void drawSession(ImDrawList* dl, const ImVec2& origin, const ImVec2& size);
    /// The lock request a selection (and the open panels) makes, to the host.
    void sendLocks();
    /// The script panel's reload in a session: the host compiles and reloads it for everyone.
    void sessionReloadScript(bool hot);
    /// A full publish (the owner's Save in a session).
    void sessionPublish();
    /// --editor-run's session steps (tests): session-wait-synced, session-wait-editor, session-wait-request, session-show,
    /// session-request:REASON, session-approve-all, session-publish, session-playtest:start|end.
    /// False if `step` isn't one; a wait puts itself back.
    bool sessionStep(const std::string& step);
};

} // namespace noxsdl
