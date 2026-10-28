// The map editor's waypoints, polygons and groups (markers.hpp).
#include "noxcore/mapedit/markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace nox::mapedit {

namespace {

float dist2(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

float clampWorld(float v)
{
    return std::clamp(v, 0.0f, kWorldSize - 1);
}

std::string waypointLabel(std::uint32_t id)
{
    return "waypoint " + std::to_string(id);
}

/// A point id no point in the pool has (above the others: the writer renumbers them anyway).
std::uint32_t freePointId(const map::Polygons& p)
{
    std::uint32_t most = 0;
    for (const auto& pt : p.points) most = std::max(most, pt.id);
    return most + 1;
}

const map::PolygonPoint* point(const map::Polygons& p, std::uint32_t id)
{
    // (Two points sharing an id: the later one, as the writer's renumbering and the game's lookup take it.)
    const map::PolygonPoint* found = nullptr;
    for (const auto& pt : p.points) {
        if (pt.id == id) found = &pt;
    }
    return found;
}

/// Drops points no polygon uses any more.
void dropUnusedPoints(map::Polygons& p)
{
    std::set<std::uint32_t> used;
    for (const auto& poly : p.polygons) used.insert(poly.points.begin(), poly.points.end());
    std::erase_if(p.points, [&](const map::PolygonPoint& pt) { return !used.contains(pt.id); });
}

Op single(std::string label, OpPart part)
{
    Op op;
    op.label = std::move(label);
    op.parts.push_back(std::move(part));
    return op;
}

std::string polygonName(const MapDocument& d, std::size_t i)
{
    const auto& n = d.polygons.polygons[i].name;
    return n.empty() ? "polygon " + std::to_string(i + 1) : "polygon " + n;
}

/// The document's polygon keys, one per polygon (a list edited outside an op gets keys by position).
std::vector<std::uint32_t> polygonKeysOf(const MapDocument& d)
{
    std::vector<std::uint32_t> keys(d.polygonKeys.begin(), d.polygonKeys.begin() + static_cast<std::ptrdiff_t>(std::min(d.polygonKeys.size(), d.polygons.polygons.size())));
    std::uint32_t next = d.nextPolygonKey;
    std::set<std::uint32_t> used(keys.begin(), keys.end());
    while (keys.size() < d.polygons.polygons.size()) {
        while (used.contains(next) || next == 0) ++next;
        used.insert(next);
        keys.push_back(next++);
    }
    return keys;
}

std::vector<std::uint32_t> groupKeysOf(const MapDocument& d)
{
    std::vector<std::uint32_t> keys(d.groupKeys.begin(), d.groupKeys.begin() + static_cast<std::ptrdiff_t>(std::min(d.groupKeys.size(), d.groups.groups.size())));
    std::uint32_t next = d.nextGroupKey;
    std::set<std::uint32_t> used(keys.begin(), keys.end());
    while (keys.size() < d.groups.groups.size()) {
        while (used.contains(next) || next == 0) ++next;
        used.insert(next);
        keys.push_back(next++);
    }
    return keys;
}

std::optional<std::size_t> indexOfKey(const std::vector<std::uint32_t>& keys, std::uint32_t key)
{
    const auto it = std::ranges::find(keys, key);
    return it == keys.end() ? std::nullopt : std::optional(static_cast<std::size_t>(it - keys.begin()));
}

} // namespace

// --- Waypoints ---------------------------------------------------------------------------------------

const std::vector<FlagBit>& waypointFlagBits()
{
    // The waypoint reader (mapRWWaypoints) keeps the flags as stored. Every waypoint search tests
    // bit 0 (scripts' WaypointOn / WaypointOff / WaypointToggle set it); bit 1 is the game's own
    // "registered in the map grid" mark (the waypoint grid register sets it at load and skips a
    // waypoint that has it), so a file must never store it. No other bit is read.
    static const std::vector<FlagBit> bits = {
        {kWaypointEnabled, "Enabled", "Creatures, movers and scripts' waypoint searches use it (scripts turn it on and off)."},
        {kWaypointRegistered, "Registered (run time only)",
         "The game's own mark for a waypoint put in its map grid at load. A map storing it keeps the waypoint out of the grid, "
         "so the nearest-waypoint search never finds it: clear it."},
    };
    return bits;
}

const std::vector<FlagBit>& waypointLinkFlagBits()
{
    // Each link's byte is a set of route classes. At load each waypoint's mask becomes the OR of
    // its own links' bytes and of every link pointing at it; the searches test (mask & wanted)
    // != 0. Roaming monsters and Blink's random destination want 0x80; a monster's roam flags
    // (the script's SetRoamFlag) are the mask its Roam wants, so any bit can be a class.
    static const std::vector<FlagBit> bits = {
        {0x01, "Route class 1", "A creature whose roam flags include class 1 may walk this link."},
        {0x02, "Route class 2", "A creature whose roam flags include class 2 may walk this link."},
        {0x04, "Route class 3", "A creature whose roam flags include class 3 may walk this link."},
        {0x08, "Route class 4", "A creature whose roam flags include class 4 may walk this link."},
        {0x10, "Route class 5", "A creature whose roam flags include class 5 may walk this link."},
        {0x20, "Route class 6", "A creature whose roam flags include class 6 may walk this link."},
        {0x40, "Route class 7", "A creature whose roam flags include class 7 may walk this link."},
        {0x80, "Default (roaming, Blink)", "The default class: roaming monsters and Blink's random destination use it (every stock link has it)."},
    };
    return bits;
}

std::uint8_t waypointRouteMask(const MapDocument& d, std::uint32_t id)
{
    std::uint8_t mask = 0;
    for (const auto& w : d.waypoints.waypoints) {
        for (const auto& l : w.links) {
            if (w.id == id || l.target == id) mask = static_cast<std::uint8_t>(mask | l.flags);
        }
    }
    return mask;
}

std::vector<std::pair<std::uint32_t, std::uint8_t>> incomingLinks(const MapDocument& d, std::uint32_t id)
{
    std::vector<std::pair<std::uint32_t, std::uint8_t>> out;
    for (const auto& w : d.waypoints.waypoints) {
        for (const auto& l : w.links) {
            if (l.target == id) out.emplace_back(w.id, l.flags);
        }
    }
    return out;
}

const map::Waypoint* findWaypoint(const MapDocument& d, std::uint32_t id)
{
    const auto it = std::ranges::find(d.waypoints.waypoints, id, &map::Waypoint::id);
    return it == d.waypoints.waypoints.end() ? nullptr : &*it;
}

std::uint32_t lowestFreeWaypointId(const MapDocument& d, const std::set<std::uint32_t>& alsoTaken)
{
    std::set<std::uint32_t> used(alsoTaken);
    for (const auto& w : d.waypoints.waypoints) used.insert(w.id);
    std::uint32_t id = 1;
    while (used.contains(id)) ++id;
    return id;
}

Op placeWaypointOp(const MapDocument& d, WorldPoint at, std::string name, std::uint32_t* placed)
{
    map::Waypoint w;
    w.id = lowestFreeWaypointId(d);
    w.x = clampWorld(at.x);
    w.y = clampWorld(at.y);
    w.name = std::move(name);
    w.flags = kWaypointEnabled; // (new waypoints are on: the old editor's "always off" fixed)
    if (placed) *placed = w.id;
    const std::uint32_t id = w.id;
    return single("Place " + waypointLabel(id), SetWaypoints{{{id, std::move(w), -1}}});
}

Op placeLinkedWaypointOp(const MapDocument& d, WorldPoint at, std::uint32_t from, bool bothWays, std::uint8_t flags, std::uint32_t* placed)
{
    const map::Waypoint* a = findWaypoint(d, from);
    if (!a) return placeWaypointOp(d, at, {}, placed);
    if (a->links.size() >= 255) throw FormatError("map editor: a waypoint holds at most 255 links");
    map::Waypoint w;
    w.id = lowestFreeWaypointId(d);
    w.x = clampWorld(at.x);
    w.y = clampWorld(at.y);
    w.flags = kWaypointEnabled;
    if (bothWays) w.links.push_back({from, flags});
    map::Waypoint na = *a;
    na.links.push_back({w.id, flags});
    if (placed) *placed = w.id;
    const std::uint32_t id = w.id;
    // (The new waypoint first: the link to it then names a waypoint that exists.)
    return single("Place " + waypointLabel(id) + " linked from " + std::to_string(from), SetWaypoints{{{id, std::move(w), -1}, {from, std::move(na)}}});
}

Op linkWaypointsOp(const MapDocument& d, std::uint32_t from, std::uint32_t to, bool bothWays, std::uint8_t flags)
{
    const map::Waypoint* a = findWaypoint(d, from);
    const map::Waypoint* b = findWaypoint(d, to);
    if (!a || !b || from == to) return {};
    SetWaypoints part;
    const auto linked = [&](const map::Waypoint& w, std::uint32_t target) {
        map::Waypoint c = w;
        bool found = false;
        for (auto& l : c.links) {
            if (l.target == target) {
                l.flags = flags;
                found = true;
            }
        }
        if (!found) {
            if (c.links.size() >= 255) throw FormatError("map editor: a waypoint holds at most 255 links");
            c.links.push_back({target, flags});
        }
        return c;
    };
    map::Waypoint na = linked(*a, to);
    if (!(na == *a)) part.waypoints.push_back({from, std::move(na)});
    if (bothWays) {
        map::Waypoint nb = linked(*b, from);
        if (!(nb == *b)) part.waypoints.push_back({to, std::move(nb)});
    }
    if (part.waypoints.empty()) return {};
    return single((bothWays ? "Link both ways " : "Link ") + waypointLabel(from) + " to " + std::to_string(to), std::move(part));
}

Op unlinkWaypointsOp(const MapDocument& d, std::uint32_t from, std::uint32_t to, bool bothWays)
{
    SetWaypoints part;
    const auto drop = [&](std::uint32_t id, std::uint32_t target) {
        const map::Waypoint* w = findWaypoint(d, id);
        if (!w) return;
        map::Waypoint c = *w;
        std::erase_if(c.links, [&](const map::WaypointLink& l) { return l.target == target; });
        if (c.links.size() != w->links.size()) part.waypoints.push_back({id, std::move(c)});
    };
    drop(from, to);
    if (bothWays) drop(to, from);
    if (part.waypoints.empty()) return {};
    return single("Unlink " + waypointLabel(from) + " from " + std::to_string(to), std::move(part));
}

Op setLinkFlagsOp(const MapDocument& d, std::uint32_t from, std::uint32_t to, std::uint8_t flags)
{
    const map::Waypoint* w = findWaypoint(d, from);
    if (!w) return {};
    map::Waypoint c = *w;
    for (auto& l : c.links) {
        if (l.target == to) l.flags = flags;
    }
    if (c == *w) return {};
    char b[8];
    std::snprintf(b, sizeof b, "0x%02X", flags);
    return single("Set link flags " + std::to_string(from) + " to " + std::to_string(to) + " to " + b, SetWaypoints{{{from, std::move(c)}}});
}

Op setWaypointsEnabledOp(const MapDocument& d, const std::set<std::uint32_t>& ids, bool on)
{
    SetWaypoints part;
    for (const auto& w : d.waypoints.waypoints) {
        if (!ids.contains(w.id)) continue;
        map::Waypoint c = w;
        c.flags = on ? (w.flags | kWaypointEnabled) : (w.flags & ~kWaypointEnabled);
        if (c.flags != w.flags) part.waypoints.push_back({w.id, std::move(c)});
    }
    if (part.waypoints.empty()) return {};
    const std::size_t n = part.waypoints.size();
    return single(std::string(on ? "Enable " : "Disable ") + std::to_string(n) + (n == 1 ? " waypoint" : " waypoints"), std::move(part));
}

Op setWaypointOp(const MapDocument& d, const map::Waypoint& w, std::string label)
{
    const map::Waypoint* now = findWaypoint(d, w.id);
    if (!now || *now == w) return {};
    for (const auto& l : w.links) {
        if (!findWaypoint(d, l.target)) throw FormatError("map editor: a link to waypoint " + std::to_string(l.target) + ", which doesn't exist");
    }
    map::Waypoint c = w;
    c.x = clampWorld(c.x);
    c.y = clampWorld(c.y);
    if (c.name.size() > kMarkerNameMax) throw FormatError("map editor: a waypoint name holds at most 255 bytes");
    return single(label.empty() ? "Edit " + waypointLabel(w.id) : std::move(label), SetWaypoints{{{w.id, std::move(c)}}});
}

Op setWaypointsOp(const MapDocument& d, const std::vector<map::Waypoint>& changed, std::string label)
{
    SetWaypoints part;
    for (const auto& w : changed) {
        const map::Waypoint* now = findWaypoint(d, w.id);
        if (!now || *now == w) continue;
        for (const auto& l : w.links) {
            if (!findWaypoint(d, l.target)) throw FormatError("map editor: a link to waypoint " + std::to_string(l.target) + ", which doesn't exist");
        }
        map::Waypoint c = w;
        c.x = clampWorld(c.x);
        c.y = clampWorld(c.y);
        part.waypoints.push_back({w.id, std::move(c)});
    }
    if (part.waypoints.empty()) return {};
    return single(std::move(label), std::move(part));
}

Op setWaypointFlagOp(const MapDocument& d, const std::set<std::uint32_t>& ids, std::uint32_t bits, bool on)
{
    std::vector<map::Waypoint> changed;
    for (const auto& w : d.waypoints.waypoints) {
        if (!ids.contains(w.id)) continue;
        map::Waypoint c = w;
        c.flags = on ? (w.flags | bits) : (w.flags & ~bits);
        if (c.flags != w.flags) changed.push_back(std::move(c));
    }
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", bits);
    const std::size_t n = changed.size();
    return setWaypointsOp(d, changed, std::string(on ? "Set" : "Clear") + " flag " + b + " on " + std::to_string(n) + (n == 1 ? " waypoint" : " waypoints"));
}

// --- Polygons ----------------------------------------------------------------------------------------

const std::vector<FlagBit>& polygonFlagBits()
{
    // Only bit 0 is read (the polygon load counts it for the quest's secret total; the first
    // visit in quest mode clears it and counts a secret found). Other bits are written back as
    // read and never used.
    static const std::vector<FlagBit> bits = {
        {0x1, "Secret area", "A quest's secret area: counted in the quest's secret total, and found the first time a player enters it."},
    };
    return bits;
}

std::optional<EdgeHit> polygonEdgeAt(const MapDocument& d, float x, float y, float radius, std::optional<std::uint32_t> only)
{
    const auto keys = polygonKeysOf(d);
    float best = radius * radius;
    std::optional<EdgeHit> out;
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) {
        if (only && keys[i] != *only) continue;
        const auto c = polygonCorners(d, i);
        if (c.size() < 2) continue;
        for (std::size_t k = 0; k < c.size(); ++k) {
            const WorldPoint a = c[k], b = c[(k + 1) % c.size()];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len2 = dx * dx + dy * dy;
            const float t = len2 > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
            const WorldPoint p{a.x + dx * t, a.y + dy * t};
            if (const float dd = dist2(p.x, p.y, x, y); dd <= best) {
                best = dd;
                out = EdgeHit{keys[i], k, p};
            }
        }
    }
    return out;
}

map::Polygon defaultPolygon(const MapDocument& d)
{
    map::Polygon p;
    p.name = "Polygon " + std::to_string(d.polygons.polygons.size() + 1);
    p.ambientLight = {static_cast<std::uint8_t>(std::min<std::uint32_t>(d.ambient.r, 255)), static_cast<std::uint8_t>(std::min<std::uint32_t>(d.ambient.g, 255)),
                      static_cast<std::uint8_t>(std::min<std::uint32_t>(d.ambient.b, 255))};
    p.minimapGroup = kNoMinimapGroup;
    p.flags = 0;
    return p;
}

std::vector<WorldPoint> polygonCorners(const MapDocument& d, std::size_t index)
{
    std::vector<WorldPoint> out;
    if (index >= d.polygons.polygons.size()) return out;
    for (const std::uint32_t id : d.polygons.polygons[index].points) {
        if (const auto* p = point(d.polygons, id)) out.push_back({p->x, p->y});
    }
    return out;
}

bool polygonContains(const MapDocument& d, std::size_t index, float x, float y)
{
    const auto c = polygonCorners(d, index);
    if (c.size() < 3) return false;
    bool in = false;
    for (std::size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) {
        if ((c[i].y > y) != (c[j].y > y) && x < (c[j].x - c[i].x) * (y - c[i].y) / (c[j].y - c[i].y) + c[i].x) in = !in;
    }
    return in;
}

std::optional<std::uint32_t> polygonAt(const MapDocument& d, float x, float y)
{
    // The game's lookup walks the list from the first polygon and takes the first that holds the
    // point (World::polygonAt, a fresh lookup): earlier polygons win where they overlap.
    const auto keys = polygonKeysOf(d);
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) {
        if (polygonContains(d, i, x, y)) return keys[i];
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::size_t>> polygonCornerAt(const MapDocument& d, float x, float y, float radius,
                                                                     std::optional<std::uint32_t> only)
{
    const auto keys = polygonKeysOf(d);
    float best = radius * radius;
    std::optional<std::pair<std::uint32_t, std::size_t>> out;
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) {
        if (only && keys[i] != *only) continue;
        const auto c = polygonCorners(d, i);
        for (std::size_t k = 0; k < c.size(); ++k) {
            if (const float dd = dist2(c[k].x, c[k].y, x, y); dd <= best) {
                best = dd;
                out = std::pair{keys[i], k};
            }
        }
    }
    return out;
}

WorldPoint snapToPolygonCorners(const MapDocument& d, WorldPoint p, float radius, std::optional<std::uint32_t> except)
{
    const auto keys = polygonKeysOf(d);
    float best = radius * radius;
    WorldPoint out = p;
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) {
        if (except && keys[i] == *except) continue;
        for (const auto& c : polygonCorners(d, i)) {
            if (const float dd = dist2(c.x, c.y, p.x, p.y); dd <= best) {
                best = dd;
                out = c;
            }
        }
    }
    return out;
}

Op addPolygonOp(const MapDocument& d, const std::vector<WorldPoint>& corners, const map::Polygon& props, std::uint32_t* key)
{
    if (corners.size() < 3) throw FormatError("map editor: a polygon needs at least 3 corners");
    if (corners.size() > 0xFFFF) throw FormatError("map editor: too many corners");
    map::Polygons p = d.polygons;
    auto keys = polygonKeysOf(d);
    map::Polygon poly = props;
    poly.points.clear();
    std::uint32_t id = freePointId(p);
    for (const auto& c : corners) {
        p.points.push_back({id, clampWorld(c.x), clampWorld(c.y)});
        poly.points.push_back(id++);
    }
    p.polygons.push_back(std::move(poly));
    std::uint32_t next = d.nextPolygonKey;
    while (std::ranges::find(keys, next) != keys.end() || next == 0) ++next;
    keys.push_back(next);
    if (key) *key = next;
    return single("Add polygon " + (props.name.empty() ? std::to_string(p.polygons.size()) : props.name), SetPolygons{std::move(p), std::move(keys)});
}

Op setPolygonPropsOp(const MapDocument& d, std::uint32_t key, const map::Polygon& props, std::string label)
{
    const auto keys = polygonKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i) return {};
    map::Polygons p = d.polygons;
    map::Polygon& poly = p.polygons[*i];
    map::Polygon next = props;
    next.points = poly.points;
    if (next == poly) return {};
    poly = std::move(next);
    return single(label.empty() ? "Edit " + polygonName(d, *i) : std::move(label), SetPolygons{std::move(p), keys});
}

Op movePolygonCornerOp(const MapDocument& d, std::uint32_t key, std::size_t corner, WorldPoint to)
{
    const auto keys = polygonKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i || corner >= d.polygons.polygons[*i].points.size()) return {};
    map::Polygons p = d.polygons;
    const std::uint32_t id = p.polygons[*i].points[corner];
    bool moved = false;
    for (auto& pt : p.points) {
        if (pt.id == id && (pt.x != clampWorld(to.x) || pt.y != clampWorld(to.y))) {
            pt.x = clampWorld(to.x);
            pt.y = clampWorld(to.y);
            moved = true;
        }
    }
    if (!moved) return {};
    return single("Move a corner of " + polygonName(d, *i), SetPolygons{std::move(p), keys});
}

Op insertPolygonCornerOp(const MapDocument& d, std::uint32_t key, std::size_t after, WorldPoint at)
{
    const auto keys = polygonKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i) return {};
    map::Polygons p = d.polygons;
    auto& pts = p.polygons[*i].points;
    if (pts.size() >= 0xFFFF) return {};
    const std::uint32_t id = freePointId(p);
    p.points.push_back({id, clampWorld(at.x), clampWorld(at.y)});
    pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(std::min(after + 1, pts.size())), id);
    return single("Add a corner to " + polygonName(d, *i), SetPolygons{std::move(p), keys});
}

Op deletePolygonCornerOp(const MapDocument& d, std::uint32_t key, std::size_t corner)
{
    const auto keys = polygonKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i) return {};
    map::Polygons p = d.polygons;
    auto& pts = p.polygons[*i].points;
    if (corner >= pts.size() || pts.size() <= 3) return {};
    pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(corner));
    dropUnusedPoints(p);
    return single("Remove a corner of " + polygonName(d, *i), SetPolygons{std::move(p), keys});
}

Op reorderPolygonOp(const MapDocument& d, std::uint32_t key, std::size_t to)
{
    auto keys = polygonKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i) return {};
    to = std::min(to, d.polygons.polygons.size() - 1);
    if (to == *i) return {};
    map::Polygons p = d.polygons;
    map::Polygon poly = p.polygons[*i];
    p.polygons.erase(p.polygons.begin() + static_cast<std::ptrdiff_t>(*i));
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(*i));
    p.polygons.insert(p.polygons.begin() + static_cast<std::ptrdiff_t>(to), std::move(poly));
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(to), key);
    return single("Move " + polygonName(d, *i) + " to position " + std::to_string(to + 1), SetPolygons{std::move(p), std::move(keys)});
}

Op deletePolygonsOp(const MapDocument& d, const std::set<std::uint32_t>& gone)
{
    auto keys = polygonKeysOf(d);
    map::Polygons p = d.polygons;
    std::size_t n = 0;
    for (std::size_t i = p.polygons.size(); i-- > 0;) {
        if (gone.contains(keys[i])) {
            p.polygons.erase(p.polygons.begin() + static_cast<std::ptrdiff_t>(i));
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
            ++n;
        }
    }
    if (n == 0) return {};
    dropUnusedPoints(p);
    return single("Delete " + std::to_string(n) + (n == 1 ? " polygon" : " polygons"), SetPolygons{std::move(p), std::move(keys)});
}

Op movePolygonsOp(const MapDocument& d, const std::set<std::uint32_t>& moved, float dx, float dy)
{
    const auto keys = polygonKeysOf(d);
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) {
        if (moved.contains(keys[i])) ids.insert(d.polygons.polygons[i].points.begin(), d.polygons.polygons[i].points.end());
    }
    if (ids.empty() || (dx == 0 && dy == 0)) return {};
    map::Polygons p = d.polygons;
    for (auto& pt : p.points) {
        if (ids.contains(pt.id)) {
            pt.x = clampWorld(pt.x + dx);
            pt.y = clampWorld(pt.y + dy);
        }
    }
    const std::size_t n = moved.size();
    return single("Move " + std::to_string(n) + (n == 1 ? " polygon" : " polygons"), SetPolygons{std::move(p), keys});
}

// --- Groups ------------------------------------------------------------------------------------------

const char* groupKindName(map::Group::Kind kind)
{
    switch (kind) {
    case map::Group::Kind::Objects: return "Objects";
    case map::Group::Kind::Waypoints: return "Waypoints";
    case map::Group::Kind::Walls: return "Walls";
    case map::Group::Kind::Unknown3: return "Groups";
    }
    return "?";
}

std::int32_t freeGroupId(const MapDocument& d)
{
    std::int32_t most = 0;
    for (const auto& g : d.groups.groups) most = std::max(most, g.id);
    return most + 1;
}

const map::Group* findGroupById(const MapDocument& d, std::int32_t id)
{
    const map::Group* found = nullptr;
    for (const auto& g : d.groups.groups) {
        if (g.id == id) found = &g; // (the last read wins, as the game's lookup keeps it)
    }
    return found;
}

bool groupCycle(const MapDocument& d, std::int32_t groupId, std::int32_t memberId)
{
    if (memberId == groupId) return true;
    // Does `memberId` reach `groupId` through kind-3 members?
    std::set<std::int32_t> seen;
    std::vector<std::int32_t> todo{memberId};
    while (!todo.empty()) {
        const std::int32_t id = todo.back();
        todo.pop_back();
        if (!seen.insert(id).second) continue;
        const map::Group* g = findGroupById(d, id);
        if (!g || g->kind != map::Group::Kind::Unknown3) continue;
        for (const auto m : g->members) {
            if (m == groupId) return true;
            todo.push_back(m);
        }
    }
    return false;
}

Op addGroupOp(const MapDocument& d, std::string name, map::Group::Kind kind, std::uint32_t* key)
{
    map::GroupData g = d.groups;
    auto keys = groupKeysOf(d);
    map::Group n;
    n.name = std::move(name);
    n.kind = kind;
    n.id = freeGroupId(d);
    const std::string label = "Add group " + n.name;
    g.groups.push_back(std::move(n));
    std::uint32_t next = d.nextGroupKey;
    while (std::ranges::find(keys, next) != keys.end() || next == 0) ++next;
    keys.push_back(next);
    if (key) *key = next;
    return single(label, SetGroups{std::move(g), std::move(keys)});
}

Op setGroupOp(const MapDocument& d, std::uint32_t key, const map::Group& grp, std::string label)
{
    const auto keys = groupKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i || d.groups.groups[*i] == grp) return {};
    map::GroupData g = d.groups;
    g.groups[*i] = grp;
    if (grp.kind == map::Group::Kind::Walls) {
        g.groups[*i].members.clear();
    } else {
        g.groups[*i].walls.clear();
    }
    return single(label.empty() ? "Edit group " + d.groups.groups[*i].name : std::move(label), SetGroups{std::move(g), keys});
}

Op deleteGroupOp(const MapDocument& d, std::uint32_t key)
{
    auto keys = groupKeysOf(d);
    const auto i = indexOfKey(keys, key);
    if (!i) return {};
    map::GroupData g = d.groups;
    const std::string name = g.groups[*i].name;
    g.groups.erase(g.groups.begin() + static_cast<std::ptrdiff_t>(*i));
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(*i));
    return single("Delete group " + name, SetGroups{std::move(g), std::move(keys)});
}

namespace {

/// The members a selection gives a group of `kind`, in the file's terms.
void membersOf(const MapDocument& d, map::Group::Kind kind, const Selection& s, std::vector<std::int32_t>& ids, std::vector<map::GridPoint>& walls)
{
    switch (kind) {
    case map::Group::Kind::Objects:
        for (const ObjId id : s.objects) {
            if (const auto it = d.objects.find(id); it != d.objects.end()) ids.push_back(static_cast<std::int32_t>(it->second.header.extent));
        }
        break;
    case map::Group::Kind::Waypoints:
        for (const std::uint32_t w : s.waypoints) ids.push_back(static_cast<std::int32_t>(w));
        break;
    case map::Group::Kind::Walls:
        for (const Cell c : s.walls) walls.push_back({c.x, c.y});
        break;
    case map::Group::Kind::Unknown3: break; // (kept as it is)
    }
}

} // namespace

Op addGroupMembersOp(const MapDocument& d, std::uint32_t key, const Selection& picked)
{
    const auto i = indexOfKey(groupKeysOf(d), key);
    if (!i) return {};
    map::Group g = d.groups.groups[*i];
    std::vector<std::int32_t> ids;
    std::vector<map::GridPoint> walls;
    membersOf(d, g.kind, picked, ids, walls);
    for (const auto id : ids) {
        if (std::ranges::find(g.members, id) == g.members.end()) g.members.push_back(id);
    }
    for (const auto& w : walls) {
        if (std::ranges::find(g.walls, w) == g.walls.end()) g.walls.push_back(w);
    }
    return setGroupOp(d, key, g, "Add members to group " + g.name);
}

Op removeGroupMembersOp(const MapDocument& d, std::uint32_t key, const Selection& picked)
{
    const auto i = indexOfKey(groupKeysOf(d), key);
    if (!i) return {};
    map::Group g = d.groups.groups[*i];
    std::vector<std::int32_t> ids;
    std::vector<map::GridPoint> walls;
    membersOf(d, g.kind, picked, ids, walls);
    std::erase_if(g.members, [&](std::int32_t m) { return std::ranges::find(ids, m) != ids.end(); });
    std::erase_if(g.walls, [&](const map::GridPoint& w) { return std::ranges::find(walls, w) != walls.end(); });
    return setGroupOp(d, key, g, "Remove members from group " + g.name);
}

Op addGroupMemberIdsOp(const MapDocument& d, std::uint32_t key, const std::vector<std::int32_t>& ids)
{
    const auto i = indexOfKey(groupKeysOf(d), key);
    if (!i) return {};
    map::Group g = d.groups.groups[*i];
    if (g.kind == map::Group::Kind::Walls) return {};
    for (const auto id : ids) {
        if (g.kind == map::Group::Kind::Unknown3 && groupCycle(d, g.id, id)) continue; // (a group never reaches itself)
        if (std::ranges::find(g.members, id) == g.members.end()) g.members.push_back(id);
    }
    return setGroupOp(d, key, g, "Add members to group " + g.name);
}

Op removeGroupMemberIdsOp(const MapDocument& d, std::uint32_t key, const std::vector<std::int32_t>& ids)
{
    const auto i = indexOfKey(groupKeysOf(d), key);
    if (!i) return {};
    map::Group g = d.groups.groups[*i];
    std::erase_if(g.members, [&](std::int32_t m) { return std::ranges::find(ids, m) != ids.end(); });
    return setGroupOp(d, key, g, "Remove members from group " + g.name);
}

Selection groupSelection(const MapDocument& d, const map::Group& g)
{
    Selection s;
    switch (g.kind) {
    case map::Group::Kind::Objects:
        for (const auto& [id, o] : d.objects) {
            if (o.parent == 0 && std::ranges::find(g.members, static_cast<std::int32_t>(o.header.extent)) != g.members.end()) s.objects.insert(id);
        }
        break;
    case map::Group::Kind::Waypoints:
        for (const auto m : g.members) {
            if (findWaypoint(d, static_cast<std::uint32_t>(m))) s.waypoints.insert(static_cast<std::uint32_t>(m));
        }
        break;
    case map::Group::Kind::Walls:
        for (const auto& w : g.walls) {
            const Cell c{static_cast<std::uint8_t>(w.x), static_cast<std::uint8_t>(w.y)};
            if (w.x >= 0 && w.x < 256 && w.y >= 0 && w.y < 256 && d.walls.contains(c)) s.walls.insert(c);
        }
        break;
    case map::Group::Kind::Unknown3: break;
    }
    return s;
}

std::vector<std::string> groupProblems(const MapDocument& d, std::size_t index)
{
    std::vector<std::string> out;
    if (index >= d.groups.groups.size()) return out;
    const map::Group& g = d.groups.groups[index];
    if (g.name.empty()) out.push_back("It has no name (scripts find groups by name).");
    if (g.name.size() > kGroupNameMax) out.push_back("Its name is longer than the game's 75-byte buffer.");
    if (!g.terminated) out.push_back("Its name was stored without its terminator (the game reads on into its buffer).");
    for (std::size_t j = 0; j < d.groups.groups.size(); ++j) {
        if (j != index && d.groups.groups[j].name == g.name && !g.name.empty()) {
            out.push_back("Another group has the same name (a script finds only one of them).");
            break;
        }
    }
    for (std::size_t j = 0; j < d.groups.groups.size(); ++j) {
        if (j != index && d.groups.groups[j].id == g.id) {
            // (Scripts name groups by id; the game's lookup keeps the last group read with an id.)
            out.push_back("Another group has the same id " + std::to_string(g.id) + " (scripts reach only the last of them).");
            break;
        }
    }
    switch (g.kind) {
    case map::Group::Kind::Objects: {
        std::map<std::int32_t, int> count;
        for (const auto& [id, o] : d.objects) ++count[static_cast<std::int32_t>(o.header.extent)];
        for (const auto m : g.members) {
            const auto it = count.find(m);
            if (it == count.end()) out.push_back("Member extent " + std::to_string(m) + ": no object has it.");
            else if (it->second > 1) out.push_back("Member extent " + std::to_string(m) + ": " + std::to_string(it->second) + " objects share it.");
        }
        break;
    }
    case map::Group::Kind::Waypoints:
        for (const auto m : g.members) {
            if (!findWaypoint(d, static_cast<std::uint32_t>(m))) out.push_back("Member waypoint " + std::to_string(m) + " doesn't exist.");
        }
        break;
    case map::Group::Kind::Walls:
        for (const auto& w : g.walls) {
            const bool on = w.x >= 0 && w.x < 256 && w.y >= 0 && w.y < 256 &&
                            d.walls.contains(Cell{static_cast<std::uint8_t>(w.x), static_cast<std::uint8_t>(w.y)});
            if (!on) out.push_back("Member wall " + std::to_string(w.x) + "," + std::to_string(w.y) + ": no wall there.");
        }
        break;
    case map::Group::Kind::Unknown3:
        // A group of groups: the scripts' for-each walks its members' groups in turn. An empty one
        // crashes the walk (an original bug, OB-158).
        if (g.members.empty()) out.push_back("A group of groups with no members: the game's for-each crashes on it (add a member or delete it).");
        for (const auto m : g.members) {
            if (!findGroupById(d, m)) {
                out.push_back("Member group id " + std::to_string(m) + ": no group has it.");
            } else if (m == g.id || groupCycle(d, g.id, m)) {
                out.push_back("Member group id " + std::to_string(m) + " leads back to this group (the walk never ends).");
            }
        }
        break;
    }
    return out;
}

// --- Map info ----------------------------------------------------------------------------------------

namespace {

// Windows-1252's 0x80..0x9F (0: not a character).
constexpr char16_t kCp1252High[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

} // namespace

std::string toMapText(std::string_view s, bool* lossy)
{
    std::string out;
    bool lost = false;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t c = 0;
        std::size_t n = 1;
        if (b < 0x80) {
            c = b;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            c = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            c = ((b & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
            c = 0x10000; // (beyond the 8-bit text's reach)
            n = 4;
        } else {
            c = 0xFFFD;
        }
        i += n;
        if (c == 0) {
            lost = true; // (a NUL would end the text early)
            continue;
        }
        if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
            out += static_cast<char>(c);
            continue;
        }
        const auto it = std::ranges::find(kCp1252High, static_cast<char16_t>(c));
        if (c <= 0xFFFF && it != std::end(kCp1252High) && *it != 0) {
            out += static_cast<char>(0x80 + (it - std::begin(kCp1252High)));
        } else {
            out += '?';
            lost = true;
        }
    }
    if (lossy) *lossy = lost;
    return out;
}

std::string fromMapText(std::string_view s)
{
    std::string out;
    for (const char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x80) {
            out += ch;
        } else if (b < 0xA0) {
            const char16_t c = kCp1252High[b - 0x80];
            appendUtf8(out, c ? c : 0xFFFD);
        } else {
            appendUtf8(out, b);
        }
    }
    return out;
}

std::string fitMapText(std::string_view utf8, std::size_t maxBytes, bool* lossy, bool* cut)
{
    std::string s = toMapText(utf8, lossy);
    const bool longer = s.size() > maxBytes;
    if (longer) s.resize(maxBytes);
    if (cut) *cut = longer;
    return s;
}

const std::vector<FlagBit>& gameTypeBits()
{
    // MapInfo's game flags as the map list reads them (mapRWMapInfo; the game-type filter tests
    // each bit): a bitfield, a map may offer several.
    static const std::vector<FlagBit> bits = {
        {0x1, "Solo / co-op", "A solo (or co-operative) game's map."},
        {0x2, "Quest", "Quest mode."},
        {0x4, "Arena", "Arena (deathmatch)."},
        {0x8, "Capture the Flag", "Capture the Flag."},
        {0x10, "King of the Realm", "King of the Realm."},
        {0x20, "Elimination", "Elimination."},
        {0x40, "Flag Ball", "Flag Ball."},
        {0x80000000u, "Chat", "Social / chat."},
    };
    return bits;
}

} // namespace nox::mapedit
