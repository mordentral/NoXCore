// The collaborative session's op transforms:
//
//   - ids the host moves (session_doc.hpp freshenNewIds: objects, extents, waypoints) recorded as
//     IdMoves, so the sender's later ops made before it saw the move follow them (remapIds);
//   - the notes an op carries to the host (annotateForHost: a new waypoint's "fresh", a keyed
//     list's `before`) and the op without them (withoutNotes: what the host logs);
//   - per-entry merges of the polygon and group lists (mergeConcurrent): an op replacing the whole
//     list with one entry changed changes only that entry when another editor changed others
//     meanwhile (the list's whole-value set made per entry: no more whole-record conflicts);
//   - selective undo (selectiveOp): an undo (or redo) of one's own op leaves alone what another
//     editor changed since, and what another's later work depends on;
//   - the compact snapshot (diffOp): one op that takes a document to another exactly, ids kept,
//     for a late joiner when the log is long (checked with sameSessionState).
//
// All deterministic: the same inputs give the same op on every member.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/locks.hpp"
#include "noxcore/mapedit/ops.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

/// What the host moved in one op: old id -> new id.
struct IdMoves {
    std::map<ObjId, ObjId> objects;
    /// New objects' extents moved, by their (final) object id: {old, new}.
    std::map<ObjId, std::pair<std::uint32_t, std::uint32_t>> extents;
    std::map<std::uint32_t, std::uint32_t> waypoints;
    std::map<std::uint32_t, std::uint32_t> polygonKeys;
    std::map<std::uint32_t, std::uint32_t> groupKeys;
    std::map<std::int32_t, std::int32_t> groupIds;
    bool empty() const
    {
        return objects.empty() && extents.empty() && waypoints.empty() && polygonKeys.empty() && groupKeys.empty() && groupIds.empty();
    }
    /// Another op's moves after these (the later wins).
    void merge(const IdMoves& later);
};

/// The references `op` makes to things `moves` moved, made to name them where they are now (an op
/// its sender made before it saw the earlier op's echo). What the op itself adds keeps its ids.
void remapIds(Op& op, const IdMoves& moves);

/// The ids the host gave `sent`'s new things in `committed` (the same op come back with its new
/// ids moved: freshenNewIds, mergeConcurrent's keys).
IdMoves movesBetween(const Op& sent, const Op& committed);

/// Fills in what a session's op tells the host (from `inverse`, what applying `op` locally
/// returned): a waypoint added on a free id is "fresh"; a polygon or group list set carries the
/// list it replaced (`before`).
void annotateForHost(Op& op, const Op& inverse);
/// The op without those notes (as logged and echoed).
Op withoutNotes(Op op);

enum class MergeRule : std::uint8_t {
    Ours,   ///< a concurrent op: its changes win entry by entry (last writer wins per entry)
    Theirs, ///< a selective undo: an entry someone else changed since keeps their version
};

/// Per-entry merges of the op's polygon and group list sets that carry `before` onto `doc`'s
/// lists: entries the op added, changed or removed (against `before`) applied to the lists as
/// they are now; entries others changed meanwhile kept; the op's new keys (and new groups' ids)
/// moved off any another took (into `moves`). The `before` notes are cleared. Returns how many
/// entries were left alone under MergeRule::Theirs.
std::size_t mergeConcurrent(const MapDocument& doc, Op& op, MergeRule rule, IdMoves* moves = nullptr);

/// Selective undo: `apply` (an undo's inverse, or a redo's op) cut to what still holds `expect`'s
/// values in `doc` (the op being undone, or the undo being redone): walls, tiles, objects,
/// waypoints and whole values someone changed since are kept as they are; objects another editor
/// removed are skipped; an object or waypoint whose removal would take another's later work with it
/// (items put in it, links to it, groups naming it) is kept. `skipped` gets the elements left
/// alone, each with why ("changed since", "deleted since", "has items put in it since"...).
struct UndoSkip {
    ElementKey key;
    std::string why;
    std::string what() const { return key.text(); } ///< "wall 30,40", "object 12"...
};
Op selectiveOp(const MapDocument& doc, const Op& apply, const Op& expect, std::vector<UndoSkip>* skipped = nullptr);

/// One op that takes `from` to `to` exactly (walls with their list positions, tiles, objects with
/// their ids, waypoints in order, keyed lists with their keys, every whole value, the section
/// list), or nothing when it can't be built. With the next ids set after it (no op sets them),
/// `from` + the op is `to` as sameSessionState compares.
std::optional<Op> diffOp(const MapDocument& from, const MapDocument& to);
/// The same session state: the same .map bytes, objects (records and ids), keys and next ids,
/// script source and section list.
bool sameSessionState(const MapDocument& a, const MapDocument& b, const things::ThingDb& things);

} // namespace nox::mapedit
