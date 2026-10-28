// The map editor's markers and map-wide panels: the Waypoint tool (place, select, move, link one or
// both ways, unlink, chain-linking clicks), the Polygon tool (draw, reshape: corners dragged,
// inserted, deleted, snapped; move whole; the list order is the game's priority), the Waypoints,
// Polygons and Groups panels, waypoint and polygon properties, the Map Info panel, the Minimap
// panel and the string browser (Game Texts).
//
// It works on the MapEditor's document through its ops (a friend, as the object tools are): every
// change is one op in the history (noxcore/mapedit markers.hpp builds them).
#pragma once

#include "editor/held_edit.hpp"
#include "noxcore/mapedit/markers.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct ImDrawList;
struct ImVec2;
struct SDL_Texture;

namespace nox::strings {
class StringTable;
}

namespace noxsdl {

class MapEditor;
class QuestPictures; // (marker_tools.cpp: the quest picture picker's list and thumbnails)

class MarkerTools {
public:
    explicit MarkerTools(MapEditor& editor);
    ~MarkerTools();
    MarkerTools(const MarkerTools&) = delete;
    MarkerTools& operator=(const MarkerTools&) = delete;

    void registerCommands();
    /// Once a frame inside the ImGui frame: the Waypoints, Polygons, Groups, Map Info, Minimap and
    /// Game Texts panels (each when shown).
    void panels();
    /// The Waypoint or Polygon tool's options (Tool Options panel), by the editor's current tool.
    void toolPanel();
    /// The Waypoint or Polygon tool's mouse in the map.
    void input(float wx, float wy, bool hovered, bool rightClick);
    /// A waypoint, polygon or corner drag under way (shown live: MapEditor::previewDrag).
    bool dragging() const { return drag_ != Drag::None; }
    /// The drag dropped, nothing done (the editor takes its preview back).
    void cancelDrag() { drag_ = Drag::None; }
    /// The tools' overlays and the marker layers' extras (selected polygon's corners, group
    /// members highlighted...), over the map panel.
    void drawOverlay(ImDrawList* dl, const ImVec2& origin);
    /// The map's polygon layer (outlines, names, the ambient preview fill) and its waypoint layer
    /// (waypoints, their links one way and both ways), drawn under the other overlays.
    void drawPolygons(ImDrawList* dl, const ImVec2& origin);
    void drawWaypoints(ImDrawList* dl, const ImVec2& origin, const ImVec2& size);
    /// Properties for a selection of waypoints only / polygons only (in the Properties panel).
    void waypointProperties();
    void polygonProperties();
    /// --editor-run's marker steps; false if `step` isn't one.
    bool runStep(const std::string& step);
    /// The Groups panel shown, open on this group (its members highlighted on the map): a
    /// "Linked from" group row's Go to.
    void openGroup(std::uint32_t key);
    /// The string table (nox.csf) when it could be read (the readable panel's preview), or null.
    const nox::strings::StringTable* strings() const { return strings_.get(); }
    /// The string browser opened to pick a key; `done` gets it (the readable's text field).
    void browseStrings(std::string current, std::function<void(const std::string&)> done);

    bool showWaypoints = false;
    bool showPolygons = false;
    bool showGroups = false;
    bool showMapInfo = false;
    bool showMinimap = false;
    bool showStrings = false;

private:
    enum class WpMode { Place, Connect };
    enum class PolyMode { Edit, Draw };
    enum class Drag { None, Waypoints, Corner, Polygon };

    // The tools.
    float pickRadius() const; ///< 10 screen pixels in world units
    nox::mapedit::WorldPoint gridSnap(nox::mapedit::WorldPoint p) const;
    /// A polygon corner's place: another polygon's corner within the snap radius (the option),
    /// else the toolbar's grid snap.
    nox::mapedit::WorldPoint polySnap(nox::mapedit::WorldPoint p, std::optional<std::uint32_t> except) const;
    std::optional<std::uint32_t> waypointAt(float x, float y) const;
    /// The waypoint links start from: the last one clicked while it's selected, else the only one selected.
    std::optional<std::uint32_t> currentWaypoint() const;
    std::optional<std::uint32_t> selectedPolygon() const;
    void selectWaypoints(std::set<std::uint32_t> ids, std::optional<std::uint32_t> current = std::nullopt);
    void selectPolygon(std::optional<std::uint32_t> key, bool add = false);
    void placeWaypoint(nox::mapedit::WorldPoint at);
    void linkTo(std::uint32_t target, bool unlink);
    void waypointInput(float wx, float wy, bool hovered, bool rightClick);
    void polygonInput(float wx, float wy, bool hovered, bool rightClick);
    void startDrawing();
    void finishDrawing();
    void cancelDrawing();
    bool deleteActiveCorner();
    void locate(float x, float y, bool zoom = false);
    void frameKeys(const std::set<std::uint32_t>& polygonKeys);

    // The panels.
    void syncHolds();
    void waypointsPanel();
    void polygonsPanel();
    void groupsPanel();
    void groupDetails(std::size_t index);
    void mapInfoPanel();
    /// The quest intro's picture picker (Map Info): thing.bin's named images with thumbnails, the
    /// full-screen ones (and the stock maps' choices) first, a filter; a click sets the field.
    void picturePicker();
    /// Sets the quest picture (one op); false (with the reason in the status bar) for a name past 31 bytes.
    bool setQuestPicture(const std::string& name);
    void minimapPanel();
    void stringsPanel();
    void rebuildMinimap();
    std::uint32_t floorColour(int floor);
    std::string memberLabel(const nox::map::Group& g, std::size_t i) const;
    std::optional<nox::mapedit::WorldPoint> memberPlace(const nox::map::Group& g, std::size_t i) const;
    void pickMembers(std::uint32_t key);

    // Fields: they edit a copy while active and answer true once, with the value, when the edit
    // ends (one op per edit).
    struct Edit {
        std::string text;
        float f = 0;
        editor::HeldValue<int> held;   ///< an int field typed into or its - / + held
        editor::PickedColour colour;   ///< a colour being picked
        bool active = false;
        int frame = -2; ///< the frame it was last drawn
    };
    /// A field's edit state by ImGui id. One not drawn last frame (its panel showed something else,
    /// or was hidden) starts over from the document: an edit left behind never comes back later to
    /// be committed over what the document has since (an undo, another editor's change).
    Edit& editState(const char* id);
    float labelWidth() const;
    void row(const char* label, const char* help = nullptr);
    bool textField(const char* id, const std::string& current, std::size_t maxBytes, std::string& out, bool multiline = false, float height = 0);
    bool floatField(const char* id, float current, float& out, float speed, float lo, float hi);
    bool intField(const char* id, int current, int& out, int lo, int hi);
    bool colourField(const char* id, std::array<std::uint8_t, 3> current, std::array<std::uint8_t, 3>& out);
    /// Checkboxes for the named bits (unknown bits kept, shown in hex); `mixed`: bits that differ
    /// across a multi-selection. True when one was clicked: `bit` and `on` say which.
    bool bitsField(const char* id, const std::vector<nox::mapedit::FlagBit>& bits, std::uint32_t value, std::uint32_t mixed, std::uint32_t& bit, bool& on);
    /// A link flags byte as a button opening its eight route classes; true with `out` when changed.
    bool linkFlagsButton(const char* id, std::uint8_t value, std::uint8_t& out);

    MapEditor& ed_;
    std::unique_ptr<nox::strings::StringTable> strings_;
    std::function<void(const std::string&)> stringPicked_;
    std::string stringCurrent_;
    std::map<unsigned int, Edit> edits_;
    std::array<int, 6> focus_{-1, -1, -1, -1, -1, -1}; ///< frames a panel just opened is still brought to the front (-1: hidden)

    // Waypoint tool.
    WpMode wpMode_ = WpMode::Place;
    bool bothWays_ = false;
    bool placeLinked_ = false;
    bool wpRightDeletes_ = true;
    std::uint8_t linkFlags_ = nox::mapedit::kDefaultLinkFlags;
    std::optional<std::uint32_t> lastWp_;

    // Polygon tool.
    PolyMode polyMode_ = PolyMode::Edit;
    std::vector<nox::mapedit::WorldPoint> drawing_;
    std::optional<std::pair<std::uint32_t, std::size_t>> activeCorner_; ///< (polygon key, corner)
    bool polygonSnap_ = true;
    float polygonSnapPx_ = 14; ///< the old editor's sqrt(200) screen pixels
    bool polyRightDeletes_ = true;
    bool ambientPreview_ = false;

    // A drag in progress (either tool).
    Drag drag_ = Drag::None;
    nox::mapedit::WorldPoint dragStart_{};
    nox::mapedit::WorldPoint dragAnchor_{};
    nox::mapedit::WorldPoint dragTo_{};
    float dragSx_ = 0, dragSy_ = 0;
    bool dragMoved_ = false;
    std::uint32_t dragKey_ = 0;

    // Panels.
    char wpSearch_[64] = {};
    char polySearch_[64] = {};
    char groupSearch_[64] = {};
    char stringSearch_[96] = {};
    char newGroupName_[80] = {};
    int newGroupKind_ = 0;
    std::optional<std::size_t> wpAnchorRow_; ///< the Waypoints list's Shift-click anchor
    std::optional<std::uint32_t> openGroup_; ///< the Groups panel's open group (key)
    std::optional<std::pair<std::uint32_t, int>> kindChange_; ///< a kind change waiting for its confirmation
    bool pickingMembers_ = false;
    int stringSel_ = -1;
    std::vector<std::pair<std::string, std::string>> stringIndex_; ///< lower-cased label, text
    bool stringScroll_ = false;

    // The quest picture picker.
    std::unique_ptr<QuestPictures> pictures_;
    bool showPictures_ = false;
    bool picturesAll_ = false; ///< every named image, not only the full-screen ones
    char pictureFilter_[64] = {};

    // Minimap.
    SDL_Texture* minimap_ = nullptr;
    std::uint64_t minimapRevision_ = ~0ull;
    std::uint64_t minimapPendingSerial_ = ~0ull; ///< the session's pending plan it was made with
    int minimapZoom_ = 2;
    int minimapShownZoom_ = 0;
    bool minimapCentre_ = true; ///< scroll the minimap to the main view next frame
    bool minimapObjects_ = false;
    bool minimapWaypoints_ = false;
    bool minimapPolygons_ = true;
    bool minimapGuides_ = true;
    std::map<int, std::uint32_t> floorColours_;
};

} // namespace noxsdl
