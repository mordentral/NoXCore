// Element locks for collaborative editing: an element an editor is working on (selected, or open in
// a property, inventory or script editor) is locked to that editor; others can't select, move,
// edit, delete or open it, and the host refuses their ops on it.
//
// The host owns the LockTable and grants requests first come, first served; a client keeps a
// copy (late joiners get it with the session state). In single-player the one local editor is
// granted everything. Selection is a lock request: the editor's selection holds only what the
// table granted it.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace nox::mapedit {

/// An editor in a session (0 is the host's own editor, and the single-player editor).
using PeerId = std::uint32_t;

/// One lockable element.
struct ElementKey {
    enum class Kind : std::uint8_t {
        Object,         ///< a = ObjId (its inventory items are locked with it)
        Wall,           ///< a = x, b = y
        Tile,           ///< a = gx, b = gy (a floor area being painted: its tiles)
        Waypoint,       ///< a = waypoint id
        Polygon,        ///< a = the polygon's key (MapDocument::polygonKeys)
        Group,          ///< a = the group's key (MapDocument::groupKeys)
        ScriptFunction, ///< name = the function's name (open in the script editor)
        MapInfo,        ///< the map info panel (one lock for the whole value)
    };
    Kind kind = Kind::Object;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::string name;

    auto operator<=>(const ElementKey& o) const { return std::tie(kind, a, b, name) <=> std::tie(o.kind, o.a, o.b, o.name); }
    bool operator==(const ElementKey&) const = default;

    static ElementKey object(ObjId id) { return {Kind::Object, static_cast<std::int32_t>(id), 0, {}}; }
    static ElementKey wall(Cell c) { return {Kind::Wall, c.x, c.y, {}}; }
    static ElementKey tile(TileCell t) { return {Kind::Tile, t.gx, t.gy, {}}; }
    static ElementKey waypoint(std::uint32_t id) { return {Kind::Waypoint, static_cast<std::int32_t>(id), 0, {}}; }
    static ElementKey polygon(std::uint32_t key) { return {Kind::Polygon, static_cast<std::int32_t>(key), 0, {}}; }
    static ElementKey group(std::uint32_t key) { return {Kind::Group, static_cast<std::int32_t>(key), 0, {}}; }
    static ElementKey mapInfo() { return {Kind::MapInfo, 0, 0, {}}; }
    static ElementKey script() { return {Kind::ScriptFunction, 0, 0, "*"}; } ///< the whole script
    /// "object 12", "wall 30,40"... for messages.
    std::string text() const;
};

/// The elements a selection locks: its objects with their inventories, walls, tiles, waypoints.
std::set<ElementKey> elementsOf(const MapDocument& doc, const Selection& s);
/// The elements an op would change in `doc` (what the host checks against others' locks, and
/// what undo checks before it runs). New objects and waypoints touch nothing yet held.
std::set<ElementKey> elementsTouched(const MapDocument& doc, const Op& op);
/// The selection without what another editor holds; `skipped` counts what was left out.
Selection withoutLocked(const MapDocument& doc, const Selection& s, const class LockTable& locks, PeerId self,
                        std::size_t* skipped = nullptr);

class LockTable {
public:
    struct Holder {
        PeerId peer = 0;
        std::uint64_t lastUse = 0; ///< the time the holder last used it (the idle timeout's)
    };

    /// Grants `peer` every key no one else holds (first come, first served) and returns the
    /// keys refused (held by another).
    std::set<ElementKey> request(PeerId peer, const std::set<ElementKey>& keys, std::uint64_t now);
    /// Makes `peer`'s locks exactly `keys` (minus what others hold): a selection change. Returns
    /// the keys refused.
    std::set<ElementKey> setHeld(PeerId peer, const std::set<ElementKey>& keys, std::uint64_t now);
    void release(PeerId peer, const std::set<ElementKey>& keys);
    /// Everything `peer` holds (it left, or the host forced it).
    void releaseAll(PeerId peer);
    /// The host's force-release of one element, whoever holds it.
    void forceRelease(const ElementKey& key) { locks_.erase(key); }
    /// Marks `peer`'s locks used now (any op or selection activity).
    void touch(PeerId peer, std::uint64_t now);
    /// Releases locks idle since before now - timeout; returns the peers that lost some.
    std::set<PeerId> expire(std::uint64_t now, std::uint64_t timeout);

    std::optional<PeerId> holder(const ElementKey& key) const;
    /// The first of `keys` another editor holds, with its holder, or nothing.
    std::optional<std::pair<ElementKey, PeerId>> conflict(PeerId peer, const std::set<ElementKey>& keys) const;
    std::set<ElementKey> heldBy(PeerId peer) const;
    const std::map<ElementKey, Holder>& all() const { return locks_; }

    /// The table for a late joiner (and back).
    std::vector<std::uint8_t> serialize() const;
    static LockTable parse(std::span<const std::uint8_t> bytes);

private:
    std::map<ElementKey, Holder> locks_;
};

} // namespace nox::mapedit
