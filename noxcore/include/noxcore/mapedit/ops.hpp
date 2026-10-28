// The map editor's edit operations: every change to a MapDocument is an Op, applied by applyOp,
// which returns the Op that undoes it.
//
// The same ops serve single-player undo/redo, the collaborative session's network messages and
// its host log, so they are:
//   - values: an Op holds the new state of what it changes, never pointers into a document;
//   - deterministic: applying the same ops in the same order to the same document gives the same
//     document (no clocks, no random numbers, no container-order dependence: ids are explicit,
//     list positions are explicit or "append");
//   - ordered: an Op is a list of parts applied first to last; its inverse lists the parts'
//     inverses last to first;
//   - serialisable: serializeOp / parseOp give a compact, versioned byte form that round-trips
//     exactly.
//
// Applying an op that doesn't fit the document (an id already in use, a removed object that
// isn't there) throws FormatError and leaves the document as it was.
#pragma once

#include "noxcore/mapedit/document.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

/// Everything a wall cell holds: the wall, and its entries in the secret, breakable and window
/// lists.
struct WallState {
    map::Wall wall;
    std::optional<map::SecretWall> secret;
    bool destructible = false;
    bool window = false;
    bool operator==(const WallState&) const = default;
};

/// The wall cell's state now, or nothing if it has no wall.
std::optional<WallState> wallState(const MapDocument& doc, Cell cell);

/// One wall cell set (or cleared, with no state). The list positions say where its secret,
/// breakable and window entries go (-1: at the end); an inverse sets them so undo restores the
/// lists' order exactly.
struct WallChange {
    Cell cell;
    std::optional<WallState> state;
    std::int32_t secretAt = -1;
    std::int32_t breakableAt = -1;
    std::int32_t windowAt = -1;
};
struct SetWalls {
    std::vector<WallChange> cells;
};

/// One full-grid floor tile (gx + gy even) set, or cleared.
struct TileChange {
    std::int16_t gx = 0;
    std::int16_t gy = 0;
    std::optional<map::FloorTile> tile;
};
struct SetTiles {
    std::vector<TileChange> tiles;
};

/// An object record with its document id.
struct ObjectEntry {
    ObjId id = 0;
    DocObject object;
};
/// Adds objects with the ids, parents and record order keys given (an owner before its items).
struct AddObjects {
    std::vector<ObjectEntry> objects;
};
/// Removes objects, each with its inventory.
struct RemoveObjects {
    std::vector<ObjId> ids;
};
/// Replaces objects' records whole (their inventories stay).
struct SetObjects {
    std::vector<ObjectEntry> objects;
};
struct ObjectMove {
    ObjId id = 0;
    float x = 0;
    float y = 0;
};
/// Moves objects to absolute positions.
struct MoveObjects {
    std::vector<ObjectMove> moves;
};

/// One waypoint (by id) set, or removed. A new waypoint goes at list position `at` (-1: at the
/// end); a changed one stays where it is unless `at` says otherwise.
struct WaypointChange {
    std::uint32_t id = 0;
    std::optional<map::Waypoint> waypoint;
    std::int32_t at = -1;
    /// A new waypoint (its id was free where the op was made): a collaborative session's host may
    /// move it off an id another editor took meanwhile (freshenNewIds). Applying ignores it.
    bool fresh = false;
};
struct SetWaypoints {
    std::vector<WaypointChange> waypoints;
};

// Whole-value sets, for the parts the editor edits as one value.
struct SetMapInfo {
    map::MapInfo info;
};
struct SetAmbient {
    map::AmbientData ambient;
};
struct SetIntro {
    map::MapIntro intro;
};
struct SetDebug {
    map::DebugData debug;
};
/// The polygons whole, with their stable keys (MapDocument::polygonKeys, one per polygon in list
/// order). Empty keys: the polygons keep the keys of their list positions and new positions get
/// new keys (an op made without them, or read from an older byte form).
/// `before` (a collaborative session's ops on the way to the host only; applying ignores it): the
/// list as its sender had it, so the host can merge the op per entry onto its own list when
/// another editor changed other entries meanwhile (session_doc.hpp mergeConcurrent).
struct SetPolygons {
    map::Polygons polygons;
    std::vector<std::uint32_t> keys;
    std::optional<map::Polygons> before;
    std::vector<std::uint32_t> beforeKeys;
};
/// The groups whole, with their stable keys (as SetPolygons).
struct SetGroups {
    map::GroupData groups;
    std::vector<std::uint32_t> keys;
    std::optional<map::GroupData> before;
    std::vector<std::uint32_t> beforeKeys;
};
struct SetScript {
    map::ScriptObject script;
};
/// The document's section list (the inverse of an op that added a section).
struct SetSections {
    std::vector<std::string> sections;
};
/// The script's NoxScript source, whole (MapDocument::scriptSource: kept in the sidecar, never
/// in the .map). Nothing: no source (a stock map's until it's decompiled).
struct SetScriptSource {
    std::optional<std::string> source;
};

using OpPart = std::variant<SetWalls, SetTiles, AddObjects, RemoveObjects, SetObjects, MoveObjects, SetWaypoints,
                            SetMapInfo, SetAmbient, SetIntro, SetDebug, SetPolygons, SetGroups, SetScript, SetSections,
                            SetScriptSource>;

/// One edit: its parts, applied first to last, and a label for the history.
struct Op {
    std::string label;
    std::vector<OpPart> parts;

    bool empty() const;
};

/// Applies `op` and returns its inverse (labelled as `op` is). Throws FormatError, leaving the
/// document unchanged, when a part doesn't fit the document.
Op applyOp(MapDocument& doc, const Op& op);

/// The op's byte form: format version, label, parts. Objects are stored as their record bytes,
/// so both directions need thing.bin (an object type's xfer).
std::vector<std::uint8_t> serializeOp(const Op& op, const things::ThingDb& things);
/// Throws FormatError for bytes serializeOp didn't write.
Op parseOp(std::span<const std::uint8_t> bytes, const things::ThingDb& things);

/// A rough count of the memory an op holds (the history's cap).
std::size_t approxBytes(const Op& op);

/// The single-player undo/redo history: the ops done, in order, with their inverses.
class History {
public:
    struct Entry {
        Op op;      ///< as done (redo applies it again)
        Op inverse; ///< undoes it
        std::size_t bytes = 0;
    };

    /// `capBytes`: the oldest entries are dropped while the history holds more than this.
    explicit History(std::size_t capBytes = std::size_t(256) << 20)
        : capBytes_(capBytes)
    {
    }

    /// Applies `op` to `doc` and records it, dropping anything undone. An empty op does nothing.
    /// Throws (recording nothing) as applyOp does.
    void perform(MapDocument& doc, const Op& op);
    /// Records an op something else already applied (a collaborative session's: the session
    /// applies it), with its inverse, as perform would.
    void recordApplied(const Op& op, Op inverse);
    /// Moves the position back / on without applying anything (a session sent the undo's or the
    /// redo's op itself); false when there's nothing to undo / redo.
    bool stepBack();
    bool stepForward();
    /// The entry `back` places before the position (1: the last done) replaced: a session's op
    /// that came back from the host changed (new ids), with the inverse it has now.
    void replaceDone(std::size_t back, Op op, Op inverse);
    /// Marks a change made from outside the history (another editor's op): views refresh.
    void touch() { ++revision_; }
    bool canUndo() const { return position_ > 0; }
    bool canRedo() const { return position_ < entries_.size(); }
    /// The op undo / redo would apply (null if none): checked against other editors' locks
    /// (locks.hpp elementsTouched) before running it.
    const Op* nextUndo() const { return canUndo() ? &entries_[position_ - 1].inverse : nullptr; }
    const Op* nextRedo() const { return canRedo() ? &entries_[position_].op : nullptr; }
    /// Undoes the last op done; returns its label (empty if there was none).
    std::string undo(MapDocument& doc);
    std::string redo(MapDocument& doc);
    /// Undoes or redoes until `position` ops are done.
    void jumpTo(MapDocument& doc, std::size_t position);

    const std::vector<Entry>& entries() const { return entries_; }
    /// How many entries are done (the rest can be redone).
    std::size_t position() const { return position_; }
    /// How many entries were dropped off the front by the cap (the first entry's number).
    std::size_t dropped() const { return dropped_; }

    /// The document as it is now counts as saved.
    void markSaved() { saved_ = static_cast<std::ptrdiff_t>(dropped_ + position_); }
    /// Whether the document differs from the last save (or, never saved, from how it started).
    bool dirty() const { return saved_ != static_cast<std::ptrdiff_t>(dropped_ + position_); }
    /// Bumped by every change (perform, undo, redo, clear): views compare it to know to refresh.
    std::uint64_t revision() const { return revision_; }
    /// Forgets everything (a new or opened document); the document counts as saved.
    void clear();

private:
    void trim();

    std::vector<Entry> entries_;
    std::size_t position_ = 0;
    std::size_t dropped_ = 0;
    std::ptrdiff_t saved_ = 0; ///< dropped_ + position_ at the last save (-1: unreachable)
    std::size_t bytes_ = 0;
    std::size_t capBytes_;
    std::uint64_t revision_ = 0;
};

} // namespace nox::mapedit
