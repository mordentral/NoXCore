// The map editor's waypoints, polygons and groups: the ops
// that place, link, reshape, reorder and fill them, the lookups the tools pick with, and the
// meanings of their bits as the game reads them.
//
// Polygons and groups are named by their stable keys (MapDocument::polygonKeys / groupKeys), so
// a lock, a selection or an open panel keeps naming the same one while the list changes. Group
// members name objects by extent (as the file does); any op that changes an extent rewrites the
// members and links naming it (object_edit.hpp), so groups stay correct.
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/selection.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::mapedit {

/// A named bit of a flags field, with what the game does with it.
struct FlagBit {
    std::uint32_t bit = 0;
    const char* name = "";
    const char* help = "";
};

// --- Waypoints -----------------------------------------------------------------------------------

/// The waypoint flags' bits as the game reads them.
const std::vector<FlagBit>& waypointFlagBits();
/// The link flags' bits (each link's byte) as the game reads them: eight route classes.
const std::vector<FlagBit>& waypointLinkFlagBits();
/// The flag stock maps give every link (and new links get).
constexpr std::uint8_t kDefaultLinkFlags = 0x80;
/// Waypoint flag: enabled.
constexpr std::uint32_t kWaypointEnabled = 1;
/// Waypoint flag 2: the game's own "registered in the map grid" mark, set at run time. A file
/// storing it keeps the waypoint out of the grid, so the nearest-waypoint search never finds it.
constexpr std::uint32_t kWaypointRegistered = 2;
/// The longest waypoint (and polygon) name the file holds (its u8 length).
constexpr std::size_t kMarkerNameMax = 255;

const map::Waypoint* findWaypoint(const MapDocument& doc, std::uint32_t id);
/// The route classes a waypoint offers the game's searches: the OR of its own links' flags and
/// the flags of every link pointing at it (as the load builds each waypoint's mask).
std::uint8_t waypointRouteMask(const MapDocument& doc, std::uint32_t id);
/// The links pointing at `id` (waypoint, link flags), in list order.
std::vector<std::pair<std::uint32_t, std::uint8_t>> incomingLinks(const MapDocument& doc, std::uint32_t id);
/// The lowest waypoint id no waypoint uses (from 1), as the old editor numbered new ones.
std::uint32_t lowestFreeWaypointId(const MapDocument& doc, const std::set<std::uint32_t>& alsoTaken = {});
/// A new, enabled waypoint at `at` (kept on the map).
Op placeWaypointOp(const MapDocument& doc, WorldPoint at, std::string name = {}, std::uint32_t* placed = nullptr);
/// A new waypoint linked from `from` (and back, with bothWays) in the same op: the Waypoint
/// tool's "place linked" chain.
Op placeLinkedWaypointOp(const MapDocument& doc, WorldPoint at, std::uint32_t from, bool bothWays, std::uint8_t flags = kDefaultLinkFlags,
                         std::uint32_t* placed = nullptr);
/// Links `from` to `to` (and `to` back to `from` with bothWays) with `flags`; a link already there
/// takes the flags. Nothing (an empty op) for a waypoint linked to itself or missing.
Op linkWaypointsOp(const MapDocument& doc, std::uint32_t from, std::uint32_t to, bool bothWays, std::uint8_t flags = kDefaultLinkFlags);
/// Removes the link from `from` to `to` (and back, with bothWays).
Op unlinkWaypointsOp(const MapDocument& doc, std::uint32_t from, std::uint32_t to, bool bothWays);
/// Sets one link's flags (every listed link from `from` to `to`).
Op setLinkFlagsOp(const MapDocument& doc, std::uint32_t from, std::uint32_t to, std::uint8_t flags);
/// Enables or disables the waypoints (all of them for "Enable all").
Op setWaypointsEnabledOp(const MapDocument& doc, const std::set<std::uint32_t>& ids, bool on);
/// Replaces one waypoint (name, position, flags, links), keeping its id and place in the list.
Op setWaypointOp(const MapDocument& doc, const map::Waypoint& w, std::string label = {});
/// Replaces several waypoints in one op (those that differ).
Op setWaypointsOp(const MapDocument& doc, const std::vector<map::Waypoint>& changed, std::string label);
/// Sets (on) or clears (off) flag bits on the waypoints, in one op.
Op setWaypointFlagOp(const MapDocument& doc, const std::set<std::uint32_t>& ids, std::uint32_t bits, bool on);

// --- Polygons ------------------------------------------------------------------------------------

/// The polygon flags' bits as the game reads them.
const std::vector<FlagBit>& polygonFlagBits();
/// The minimap group a wall outside every polygon gets (and the default for a new polygon).
constexpr std::uint8_t kNoMinimapGroup = 100;

/// A polygon's corners in world units, in order (a corner whose point is missing is left out).
std::vector<WorldPoint> polygonCorners(const MapDocument& doc, std::size_t index);
/// Whether (x, y) is inside the polygon at `index` (the even-odd rule).
bool polygonContains(const MapDocument& doc, std::size_t index, float x, float y);
/// The polygon the game takes for a point (its lookup's order when polygons overlap), by key.
std::optional<std::uint32_t> polygonAt(const MapDocument& doc, float x, float y);
/// The corner nearest (x, y) within `radius`: (polygon key, corner index). `only`: that polygon's.
std::optional<std::pair<std::uint32_t, std::size_t>> polygonCornerAt(const MapDocument& doc, float x, float y, float radius,
                                                                     std::optional<std::uint32_t> only = std::nullopt);
/// The nearest corner of another polygon within `radius` (the old editor's polygon snap), or `p`.
WorldPoint snapToPolygonCorners(const MapDocument& doc, WorldPoint p, float radius, std::optional<std::uint32_t> exceptPolygon = std::nullopt);
/// The polygon edge nearest (x, y) within `radius`: the corner it starts at (a new corner goes
/// after it) and the nearest point on it. `only`: that polygon's edges.
struct EdgeHit {
    std::uint32_t key = 0;
    std::size_t after = 0;
    WorldPoint at;
};
std::optional<EdgeHit> polygonEdgeAt(const MapDocument& doc, float x, float y, float radius, std::optional<std::uint32_t> only = std::nullopt);
/// A new polygon's properties: "Polygon N", no minimap group, the map's ambient light, no
/// handlers, no flags.
map::Polygon defaultPolygon(const MapDocument& doc);

/// A new polygon at the end of the list (the old editor put new ones at the top) with these
/// corners (3 or more), its own new points; `props` gives its name, light, group, handlers and
/// flags (its point list is ignored).
Op addPolygonOp(const MapDocument& doc, const std::vector<WorldPoint>& corners, const map::Polygon& props, std::uint32_t* key = nullptr);
/// Sets a polygon's name, ambient light, minimap group, handlers and flags (its corners stay).
Op setPolygonPropsOp(const MapDocument& doc, std::uint32_t key, const map::Polygon& props, std::string label = {});
/// Moves one corner (its point: a point two polygons share moves for both, as the game reads it).
Op movePolygonCornerOp(const MapDocument& doc, std::uint32_t key, std::size_t corner, WorldPoint to);
/// A new corner after `after` (a new point of its own).
Op insertPolygonCornerOp(const MapDocument& doc, std::uint32_t key, std::size_t after, WorldPoint at);
/// Removes a corner (a polygon keeps at least 3); its point goes when nothing else uses it.
Op deletePolygonCornerOp(const MapDocument& doc, std::uint32_t key, std::size_t corner);
/// Moves the polygon to list position `to`.
Op reorderPolygonOp(const MapDocument& doc, std::uint32_t key, std::size_t to);
/// Removes the polygons (and the points only they used).
Op deletePolygonsOp(const MapDocument& doc, const std::set<std::uint32_t>& keys);
/// Moves the polygons' points by (dx, dy).
Op movePolygonsOp(const MapDocument& doc, const std::set<std::uint32_t>& keys, float dx, float dy);

// --- Groups -------------------------------------------------------------------------------------

/// The group kinds' names (Objects, Waypoints, Walls, Groups: kind 3 is a group of groups, its
/// members other groups' ids, walked recursively by the scripts' for-each).
const char* groupKindName(map::Group::Kind kind);
/// The longest group name the game's reader holds (its buffer, the NUL included in the 76).
constexpr std::size_t kGroupNameMax = 75;
/// An id no group has (the least above the others'). Scripts name groups by id (the name turns
/// into an id when the script is compiled), so ids must be unique.
std::int32_t freeGroupId(const MapDocument& doc);
/// The group with this id (the last in the list with it: the game's lookup keeps the last read).
const map::Group* findGroupById(const MapDocument& doc, std::int32_t id);
/// Whether making `memberId` a member of the kind-3 group `groupId` would make a cycle (a group
/// reaching itself through its members), or names the group itself.
bool groupCycle(const MapDocument& doc, std::int32_t groupId, std::int32_t memberId);
/// Adds or removes members by their file ids (kind 3: groups' ids; Objects: extents; Waypoints:
/// waypoint ids). Ids already in (or not in) are skipped.
Op addGroupMemberIdsOp(const MapDocument& doc, std::uint32_t key, const std::vector<std::int32_t>& ids);
Op removeGroupMemberIdsOp(const MapDocument& doc, std::uint32_t key, const std::vector<std::int32_t>& ids);
/// A new, empty group at the end of the list.
Op addGroupOp(const MapDocument& doc, std::string name, map::Group::Kind kind, std::uint32_t* key = nullptr);
/// Replaces a group (name, kind, members), keeping its key and place.
Op setGroupOp(const MapDocument& doc, std::uint32_t key, const map::Group& g, std::string label = {});
Op deleteGroupOp(const MapDocument& doc, std::uint32_t key);
/// Adds or removes members picked on the map (objects become their extents, waypoints their
/// ids, walls their cells); what doesn't fit the group's kind is left out.
Op addGroupMembersOp(const MapDocument& doc, std::uint32_t key, const Selection& picked);
Op removeGroupMembersOp(const MapDocument& doc, std::uint32_t key, const Selection& picked);
/// The group's members on the map as a selection (objects by extent; waypoints; walls).
Selection groupSelection(const MapDocument& doc, const map::Group& g);
/// What's wrong with a group: members that don't exist, an extent several objects share, a name
/// another group has, a name the game's buffer can't hold.
std::vector<std::string> groupProblems(const MapDocument& doc, std::size_t index);

// --- Map info ------------------------------------------------------------------------------------

/// Text as the map stores it (the game's 8-bit Windows-1252 text) from UTF-8: characters it can't
/// hold become '?' (and `lossy` is set).
std::string toMapText(std::string_view utf8, bool* lossy = nullptr);
/// The map's 8-bit text as UTF-8.
std::string fromMapText(std::string_view text);
/// UTF-8 text as the map stores it, cut to at most `maxBytes` bytes. `lossy`: characters became
/// '?'; `cut`: it was longer.
std::string fitMapText(std::string_view utf8, std::size_t maxBytes, bool* lossy = nullptr, bool* cut = nullptr);
/// The game types (MapInfo::gameFlags) by bit, with their names.
const std::vector<FlagBit>& gameTypeBits();
/// The longest quest intro title and graphic name (MapInfo version 3): the game reads each into a
/// 32-byte buffer without a check, so 31 bytes and the NUL.
constexpr std::size_t kQuestIntroMax = 31;

} // namespace nox::mapedit
