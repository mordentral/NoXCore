// The map editor's object properties: the Properties panel for a selection of objects, contextual
// and non-modal. The common header (type, position, extent, header details, create flags with the
// authoring ones apart from the runtime-only ones, xStatus, team, script name, pickup handler,
// decay), the inventory as an inline list (add from the content browser, reorder, clone, equip, an
// item's own properties in place), and every xfer's fields from noxcore/mapedit properties.hpp's
// tables, with the lists the tables don't cover (spells, buffs, shop items, glyph spells, reward
// lists, generator templates with their monsters' fields and inventories inline, the light's key
// sequences). Several objects of one kind edit together (mixed values shown as "-"); every edit is
// one op.
//
// It also owns the move gizmo and the rotate ring for the selection, and the link lines (V6:
// teleporters, elevators and shafts, movers with their path, glyph targets, exits' and pits'
// points, lights' targets; drawn bright when either end is selected), and the selected light's and
// sentry's previews. An object (or waypoint) other links or groups name lists them under "Linked
// from", each with a Go to (the reverse of a link field's Go; a group opens in the Groups panel),
// from a link index rebuilt once per change.
#pragma once

#include "light_cone.hpp"

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/properties.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace nox::things {
struct SoundSets;
struct ThingDb;
}

namespace noxsdl {

class MapEditor;

/// An invisible light's cone at game frame `frame`, animated as the game does (light_cone.hpp),
/// aiming at the document's static object with its target extent; nullopt: all round (or not an
/// invisible light).
std::optional<lightcone::Cone> lightConeNow(const nox::mapedit::MapDocument& doc, const nox::things::ThingDb& things,
                                            const nox::mapedit::DocObject& o, std::uint32_t frame);
/// Draws a light's cone (its kite, the halves the game lights, and where it points) on the map
/// panel; `toScreen` maps world to screen.
void drawLightCone(ImDrawList* dl, const std::function<ImVec2(float, float)>& toScreen, float x, float y, float radius,
                   lightcone::Cone cone, std::uint32_t colour, bool filled);

class PropertyPanel {
public:
    explicit PropertyPanel(MapEditor& editor);
    ~PropertyPanel();
    PropertyPanel(const PropertyPanel&) = delete;
    PropertyPanel& operator=(const PropertyPanel&) = delete;

    void registerCommands();
    /// The Properties panel's body for these objects (the selection's).
    void objects(const std::set<nox::mapedit::ObjId>& ids);
    /// The gizmos' mouse (Select tool): true while one is being dragged (it has the input).
    bool gizmoInput(const nox::mapedit::WorldPoint& w, bool hovered);
    /// A gizmo being dragged (shown live: MapEditor::previewDrag).
    bool gizmoDragging() const { return gizmo_ != Gizmo::None; }
    /// The gizmo's drag dropped, nothing done (the editor takes its preview back).
    void cancelGizmo()
    {
        gizmo_ = Gizmo::None;
        gizmoDx_ = gizmoDy_ = gizmoTurn_ = 0;
    }
    /// The link lines, previews and the gizmos over the map panel.
    void drawOverlay(ImDrawList* dl, const ImVec2& origin);
    /// --editor-run's property steps; false if `step` isn't one.
    bool runStep(const std::string& step);
    /// The map's links by what they name, rebuilt when the document changed (History revision).
    const nox::mapedit::LinkIndex& links();
    /// A "Linked from" section: one row per link naming the shown object or waypoint, each with a
    /// Go to button (selects the source and centres on it; Shift adds it to the selection).
    void linkedFrom(const std::vector<nox::mapedit::Link>& in, const char* id);

private:
    /// What a field's edit does: the selection's op (setFieldOp...), or a nested object's change
    /// (an inventory item's, a generator template's).
    using Apply = std::function<void(const nox::mapedit::FieldDesc&, const nox::mapedit::FieldValue&, std::uint64_t mask)>;

    void header(const std::vector<nox::mapedit::ObjId>& ids);
    void fieldList(const std::vector<nox::mapedit::SharedField>& fields, const Apply& apply, const std::string& itemType,
                   std::optional<nox::mapedit::WorldPoint> origin, bool multi, const std::string& scope);
    void fieldWidget(const nox::mapedit::SharedField& f, const Apply& apply, const std::string& itemType,
                     std::optional<nox::mapedit::WorldPoint> origin);
    // fieldWidget's parts.
    /// (Tests' props-focus:) the keyboard to this field's text box, when a step asked for it.
    void focusIfAsked(const std::string& fieldId);
    /// A text box committed when let go (typing is no op until then); a mixed value shows empty.
    std::optional<std::string> textInput(const char* id, const nox::mapedit::SharedField& sf, const std::string& now, std::size_t maxBytes, bool multiline);
    /// A number committed when let go, kept in [lo, hi] (the field's range in the units shown: an
    /// angle's in degrees).
    std::optional<double> numberInput(const char* id, const nox::mapedit::SharedField& sf, double now, bool integer, double lo, double hi);
    void flagsField(const nox::mapedit::SharedField& sf, const Apply& apply);
    void stringKeyField(const nox::mapedit::SharedField& sf, const Apply& apply);
    /// An object's extent, a waypoint, a point: typed, picked on the map (from `origin`), or gone to.
    void extentField(const nox::mapedit::SharedField& sf, const Apply& apply, std::optional<nox::mapedit::WorldPoint> origin);
    void waypointField(const nox::mapedit::SharedField& sf, const Apply& apply, std::optional<nox::mapedit::WorldPoint> origin);
    void pointField(const nox::mapedit::SharedField& sf, const Apply& apply, std::optional<nox::mapedit::WorldPoint> origin);
    void angleField(const nox::mapedit::SharedField& sf, const Apply& apply);
    void directionField(const nox::mapedit::SharedField& sf, const Apply& apply);
    Apply selectionApply(const std::vector<nox::mapedit::ObjId>& ids);
    void inventory(nox::mapedit::ObjId owner);
    void lists(nox::mapedit::ObjId id);
    void monsterSpells(nox::mapedit::ObjId id);
    void buffs(nox::mapedit::ObjId id);
    void shop(nox::mapedit::ObjId id);
    void glyphSpells(nox::mapedit::ObjId id);
    void rewardLists(nox::mapedit::ObjId id);
    void generator(nox::mapedit::ObjId id);
    /// A generator template's inventory, inline like an object's (through templateEditOp: each
    /// edit one op on the generator).
    void templateInventory(nox::mapedit::ObjId id, std::size_t group, std::size_t slot);
    void lightKeys(nox::mapedit::ObjId id);
    void linkTools(nox::mapedit::ObjId id);
    /// A collapsing section (a test's "props-scroll:" brings it to the top).
    bool section(const std::string& title, const char* id, int flags);
    /// Edits a copy of one object and performs the change as an op (when it differs).
    bool edit(nox::mapedit::ObjId id, const std::string& label, const std::function<void(nox::mapedit::DocObject&)>& change);
    /// A combo of names (with a filter box: the names, and object types' game names); true when
    /// `value` changed. `thumbs`: object types, each row with its mini icon (the inventory rows').
    bool nameCombo(const char* id, std::string& value, const std::vector<std::string>& names, bool allowNone = true, bool thumbs = false);
    /// A list's small number box (`width` unscaled), committed when let go: the value typed then.
    std::optional<int> intOnRelease(const char* id, int value, float width);
    /// A full-width "drop ... here" button taking the content browser's thumbnails: the type
    /// dropped on it this frame.
    std::optional<std::string> droppedThing(const char* label);
    /// An inventory's add row: the browser's type, a name picked, or a thumbnail dropped, each to `add`.
    void addItemRow(const std::function<void(const std::string&)>& add);
    std::vector<std::string> typesOfClass(std::uint32_t classes) const;
    /// What an inventory or a shop can hold (mapedit carriedTypes), made once.
    const std::vector<std::string>& carriedTypes() const;
    /// A type's game name in lower case (the name filters match it too, as the browser's search).
    const std::string& displayLower(const std::string& type) const;
    std::string label(const nox::mapedit::DocObject& o) const;
    /// A link's source as the "Linked from" row names it ("TeleportPentagram #812 (Leads to)").
    std::string linkSource(const nox::mapedit::Link& l) const;
    void goToSource(const nox::mapedit::Link& l, bool add);
    /// The gizmo drag's op as the document stands: the ring's turn, or the arrows' move (`moved`:
    /// the selection after it). Built each frame for the live preview and once more on release.
    nox::mapedit::Op turnOp() const;
    nox::mapedit::Op moveOp(std::optional<nox::mapedit::Selection>& moved) const;

    MapEditor& ed_;
    nox::mapedit::FieldContext ctx_;
    std::unique_ptr<nox::things::SoundSets> sounds_;
    std::set<nox::mapedit::ObjId> itemsOpen_;        ///< inventory items whose own fields are shown inline
    std::set<std::pair<int, int>> templatesOpen_;    ///< (variant, slot) generator templates shown inline
    std::set<std::tuple<int, int, int>> templateItemsOpen_; ///< (variant, slot, item's stream position) shown inline
    char filter_[64] = {};
    std::string openCombo_;   ///< (tests: props-combo:) a picker to open next frame...
    std::string comboFilter_; ///< ...with this in its filter
    std::string focusField_;  ///< (tests: props-focus:) the field whose text box takes the keyboard next frame
    bool showRuntimeFlags_ = false;
    std::string scrollTo_;                           ///< a section to scroll to (a test's step)
    bool twoWay_ = true;                             ///< link pickers set the partner's link back
    std::map<unsigned int, std::string> textEdits_;  ///< text fields being typed in (by ImGui id)
    std::map<unsigned int, double> numberEdits_;     ///< number fields being typed in
    unsigned int heldNumber_ = 0;                    ///< the number field whose text or - / + button is active
    mutable std::map<std::uint32_t, std::vector<std::string>> typesByClass_;
    mutable std::optional<std::vector<std::string>> carried_;
    mutable std::unordered_map<std::string, std::string> displayLower_;
    nox::mapedit::LinkIndex links_;                  ///< (links(): built for linksRevision_)
    std::uint64_t linksRevision_ = ~0ull;

    // The gizmos.
    enum class Gizmo { None, MoveA, MoveB, MoveFree, Rotate };
    Gizmo gizmo_ = Gizmo::None;
    nox::mapedit::WorldPoint gizmoStart_{};
    nox::mapedit::WorldPoint gizmoCentre_{};
    float gizmoDx_ = 0, gizmoDy_ = 0, gizmoTurn_ = 0;
    bool gizmoVisible_ = false;
    bool gizmoTurns_ = false; ///< the selection has something that turns (the ring is shown)
};

} // namespace noxsdl
