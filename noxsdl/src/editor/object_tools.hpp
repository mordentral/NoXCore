// The map editor's objects: the Content Browser panel (every thing type by folder with thumbnails,
// search, class filters, favourites, recently used, the user's categories, prefabs, and the floors,
// walls and edges the floor, wall and edge tools paint with), the Object tool (placement with a ghost, snapping,
// facing, multi-place, random from the folder, the scatter brush), prefabs, the Object List panel,
// the object layers (door and sentry facing, trigger and plate boxes, light radius rings) and the
// read-only object summary in Properties.
//
// It works on the MapEditor's document through its ops (a friend, as the script panel is): every
// placement is one op in the history.
#pragma once

#include "editor/thing_thumbnails.hpp"
#include "map_view.hpp"
#include "player_art.hpp"

#include "noxcore/mapedit/content.hpp"
#include "noxcore/mapedit/placement.hpp"
#include "noxcore/mapedit/prefab.hpp"
#include "noxcore/things/monster_db.hpp"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace noxsdl {

class MapEditor;

class ObjectTools {
public:
    explicit ObjectTools(MapEditor& editor);
    ~ObjectTools();
    ObjectTools(const ObjectTools&) = delete;
    ObjectTools& operator=(const ObjectTools&) = delete;

    void registerCommands();
    /// Once a frame inside the ImGui frame, after the Map panel: the Content Browser and Object
    /// List panels, a drop from the browser onto the map, the thumbnails' uploads.
    void panels();
    /// The Object tool's options (Tool Options panel).
    void toolPanel();
    /// The Object tool's mouse in the map.
    void input(float wx, float wy, bool hovered, bool rightClick);
    /// The ghost of what a click (or a drop) would place, for the scene's draw.
    std::vector<MapView::SceneSprite> ghosts();
    /// The object layers and the tool's overlay, over the map panel (world -> screen by S).
    void drawOverlay(ImDrawList* dl, const ImVec2& origin);
    /// A read-only summary of the object's type fields (Properties).
    void summary(nox::mapedit::ObjId id);
    /// --editor-run's object steps; false if `step` isn't one.
    bool runStep(const std::string& step);
    /// The browser opened on a folder ("Floors", "Walls", "Edges" from the tools' pickers).
    void browse(const std::string& folder);
    /// What the properties panel builds on: the factory (new objects, type changes), the catalog,
    /// the browser's selected type (an inventory's "add from the browser"), monster.bin (solo).
    const nox::mapedit::ObjectFactory& factory() const { return *factory_; }
    const nox::mapedit::ContentCatalog& catalog() const { return catalog_; }
    const std::string& browserType() const { return type_; }
    /// The type's thumbnail as an ImGui image of `size` (a blank square while it's being made).
    void drawThumbnail(const std::string& type, float size);
    /// The same into a draw list at `at` (over what's drawn there): false while it's being made,
    /// or for a type without one.
    bool drawThumbnailAt(ImDrawList* dl, const std::string& type, const ImVec2& at, float size);
    bool showBrowser = true;
    bool showList = true;

private:
    /// One row of the Object List.
    struct ListRow {
        nox::mapedit::ObjId id = 0;
        std::uint32_t extent = 0;
        std::string type;
        std::string scriptName;
        std::string folder;
        std::string displayName; ///< the game's name for its type
        float x = 0, y = 0;
        int team = 0;
        std::size_t items = 0;
        std::string search; ///< lower case: what the search matches
    };

    void browserPanel();
    void objectListPanel();
    void folderTree();
    void itemGrid();
    void detailsCard(bool tall);
    void scatterPanel();
    void prefabDialog();
    void pickType(const std::string& type);
    void pickPaint(const std::string& folder, int index);
    bool thumbnail(std::size_t thing, float size);
    nox::mapedit::WorldPoint placePoint(float wx, float wy) const;
    std::optional<nox::mapedit::DocObject> makeAt(const std::string& type, nox::mapedit::WorldPoint at);
    bool placeAt(nox::mapedit::WorldPoint at, std::string type = {});
    void rotate(int steps);
    void deleteAt(float wx, float wy);
    std::string randomType();
    void noteRecent(const std::string& type);
    void loadState();
    void saveState();
    void loadUserCategories();
    /// The categories editor (the User Categories folder): names and rules, new and deleted
    /// categories, saved to the user folder's categories.xml as they change.
    void categoriesEditor();
    void saveUserCategories();
    std::string uniqueCategoryName(const std::string& base) const;
    void savePrefab(const std::string& name);
    void reloadPrefabs();
    std::vector<const nox::mapedit::ContentEntry*> shown();
    std::string displayName(const nox::mapedit::ContentEntry& e) const;
    /// How the game will draw `o` (its colours, an NPC's worn things among `items`, its inventory).
    RecordLook lookOf(const nox::mapedit::DocObject& o, const std::vector<const nox::mapedit::DocObject*>& items) const;

    MapEditor& ed_;
    std::filesystem::path readDir_;  ///< where favourites, prefabs, categories and thumbnails are read from
    std::filesystem::path writeDir_; ///< ... and written to (empty: nothing written, a test's run)
    std::optional<nox::things::MonsterDb> monsters_;
    std::unique_ptr<nox::mapedit::ObjectFactory> factory_;
    std::unique_ptr<editor::ThingThumbnails> thumbs_;
    nox::mapedit::ContentCatalog catalog_;
    std::vector<nox::mapedit::UserCategory> userCats_;
    std::vector<nox::mapedit::Prefab> prefabs_;

    // The browser.
    std::string folder_ = "Monsters";
    char search_[128] = {};
    bool showAll_ = false;  ///< the hidden (not placeable) types too
    std::uint32_t classMask_ = 0; ///< class filter chips (none: all)
    std::set<std::string> favourites_;
    std::deque<std::string> recent_; ///< newest first, at most kRecent
    static constexpr std::size_t kRecent = 30;
    float cell_ = 72;
    bool recursive_ = true; ///< a folder shows its sub-folders' things too
    std::string type_;      ///< the type the Object tool places (the browser's selection)
    int prefab_ = -1;       ///< the prefab the tool places instead (index into prefabs_), -1 none
    bool focusFolder_ = false;

    // The Object tool.
    enum class Mode { Place, Scatter };
    Mode mode_ = Mode::Place;
    float facing_ = 90;     ///< degrees: the ghost's and new objects' facing (0 east, 90 south)
    bool randomFromFolder_ = false;
    bool rightDeletes_ = true; ///< right click deletes the object under the mouse (the old place mode's)
    std::uint32_t placements_ = 0; ///< counts placements: the random picks' seed (never a clock)
    nox::mapedit::ScatterBrush scatter_;
    std::optional<nox::mapedit::ScatterStroke> stroke_;
    nox::mapedit::WorldPoint lastDab_{};
    char prefabName_[64] = {};
    bool prefabOpen_ = false;
    std::string dragType_;  ///< a type being dragged from the browser (its ghost follows the mouse)
    int dragPrefab_ = -1;   ///< or a prefab

    // The object list.
    char listSearch_[128] = {};
    bool listInView_ = true; ///< starts on: a long list is easier to read cut to what's on screen
    std::vector<ListRow> rows_;
    std::uint64_t rowsRevision_ = ~0ull;
    int sortColumn_ = 0;
    bool sortAscending_ = true;
};

} // namespace noxsdl
