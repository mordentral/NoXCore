// The map editor's ops: apply with inverse, byte form, and the undo history.
#include "noxcore/mapedit/ops.hpp"

#include "doc_types.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace nox::mapedit {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

bool atCell(const map::GridPoint& p, Cell c)
{
    return p.x == c.x && p.y == c.y;
}

/// Inserts `v` at position `at` (clamped; -1 appends).
template <class T>
void insertAt(std::vector<T>& list, std::int32_t at, T v)
{
    if (at < 0 || static_cast<std::size_t>(at) >= list.size()) {
        list.push_back(std::move(v));
    } else {
        list.insert(list.begin() + at, std::move(v));
    }
}

/// Removes the first entry `match` accepts and returns where it was (-1 if none).
template <class T, class F>
std::int32_t removeFirst(std::vector<T>& list, F match)
{
    const auto it = std::ranges::find_if(list, match);
    if (it == list.end()) {
        return -1;
    }
    const auto at = static_cast<std::int32_t>(it - list.begin());
    list.erase(it);
    return at;
}

// --- Parts --------------------------------------------------------------------------------------

SetWalls apply(MapDocument& d, const SetWalls& p)
{
    SetWalls inv;
    inv.cells.reserve(p.cells.size());
    for (const WallChange& c : p.cells) {
        if (c.cell.x == 0xFF) {
            throw FormatError("map editor op: wall x 255 ends the wall list");
        }
        WallChange undo;
        undo.cell = c.cell;
        undo.state = wallState(d, c.cell);
        // Out with the old (its list positions noted)...
        if (undo.state) {
            d.walls.erase(c.cell);
            undo.secretAt = removeFirst(d.secretWalls.walls, [&](const map::SecretWall& w) { return atCell(w.pos, c.cell); });
            undo.breakableAt = removeFirst(d.destructableWalls.walls, [&](const map::GridPoint& g) { return atCell(g, c.cell); });
            undo.windowAt = removeFirst(d.windowWalls.walls, [&](const map::GridPoint& g) { return atCell(g, c.cell); });
        }
        // ...in with the new.
        if (c.state) {
            map::Wall w = c.state->wall;
            w.x = c.cell.x;
            w.y = c.cell.y;
            d.setWall(w);
            const map::GridPoint at{c.cell.x, c.cell.y};
            if (c.state->secret) {
                map::SecretWall s = *c.state->secret;
                s.pos = at;
                insertAt(d.secretWalls.walls, c.secretAt, s);
                d.ensureSection(map::SecretWalls::kName);
            }
            if (c.state->destructible) {
                insertAt(d.destructableWalls.walls, c.breakableAt, at);
                d.ensureSection(map::DestructableWalls::kName);
            }
            if (c.state->window) {
                insertAt(d.windowWalls.walls, c.windowAt, at);
                d.ensureSection(map::WindowWalls::kName);
            }
        }
        inv.cells.push_back(std::move(undo));
    }
    std::ranges::reverse(inv.cells);
    return inv;
}

SetTiles apply(MapDocument& d, const SetTiles& p)
{
    SetTiles inv;
    inv.tiles.reserve(p.tiles.size());
    for (const TileChange& c : p.tiles) {
        const map::FloorTile* old = d.floor.tile(c.gx, c.gy);
        TileChange undo{c.gx, c.gy, old ? std::optional(*old) : std::nullopt};
        if (!d.floor.setTile(c.gx, c.gy, c.tile)) {
            throw FormatError("map editor op: no floor tile can be at " + std::to_string(c.gx) + "," + std::to_string(c.gy));
        }
        if (c.tile) {
            d.ensureSection(map::FloorMap::kName);
        }
        inv.tiles.push_back(std::move(undo));
    }
    std::ranges::reverse(inv.tiles);
    return inv;
}

/// `id` and its inventory, owner first, in record order.
void subtree(const MapDocument& d, ObjId id, std::vector<ObjectEntry>& out)
{
    out.push_back({id, d.objects.at(id)});
    for (const ObjId child : d.childrenOf(id)) {
        subtree(d, child, out);
    }
}

RemoveObjects apply(MapDocument& d, const AddObjects& p)
{
    std::set<ObjId> added;
    for (const ObjectEntry& e : p.objects) {
        if (e.id == 0 || d.objects.contains(e.id)) {
            throw FormatError("map editor op: object id " + std::to_string(e.id) + " is in use");
        }
        if (e.object.parent != 0 && !d.objects.contains(e.object.parent)) {
            throw FormatError("map editor op: object " + std::to_string(e.id) + "'s owner doesn't exist");
        }
        d.objects.emplace(e.id, e.object);
        d.nextObjId = std::max(d.nextObjId, e.id + 1);
        added.insert(e.id);
    }
    if (!p.objects.empty()) {
        d.ensureSection(map::ObjectTOC::kName);
        d.ensureSection(map::ObjectData::kName);
    }
    // Undone by removing the roots added (their items go with them).
    RemoveObjects inv;
    for (auto it = p.objects.rbegin(); it != p.objects.rend(); ++it) {
        if (!added.contains(it->object.parent)) {
            inv.ids.push_back(it->id);
        }
    }
    return inv;
}

AddObjects apply(MapDocument& d, const RemoveObjects& p)
{
    AddObjects inv;
    for (const ObjId id : p.ids) {
        if (!d.objects.contains(id)) {
            throw FormatError("map editor op: object " + std::to_string(id) + " doesn't exist");
        }
        std::vector<ObjectEntry> tree;
        subtree(d, id, tree);
        d.removeObject(id);
        inv.objects.insert(inv.objects.end(), std::make_move_iterator(tree.begin()), std::make_move_iterator(tree.end()));
    }
    // Put back in removal order: owners come before their items within each tree, and a tree
    // removed later can't belong to one removed earlier (it would have gone with its owner).
    return inv;
}

SetObjects apply(MapDocument& d, const SetObjects& p)
{
    SetObjects inv;
    for (const ObjectEntry& e : p.objects) {
        const auto it = d.objects.find(e.id);
        if (it == d.objects.end()) {
            throw FormatError("map editor op: object " + std::to_string(e.id) + " doesn't exist");
        }
        if (e.object.parent != 0 && (e.object.parent == e.id || !d.objects.contains(e.object.parent))) {
            throw FormatError("map editor op: object " + std::to_string(e.id) + "'s owner doesn't exist");
        }
        inv.objects.push_back({e.id, it->second});
        it->second = e.object;
    }
    std::ranges::reverse(inv.objects);
    return inv;
}

MoveObjects apply(MapDocument& d, const MoveObjects& p)
{
    MoveObjects inv;
    for (const ObjectMove& m : p.moves) {
        const auto it = d.objects.find(m.id);
        if (it == d.objects.end()) {
            throw FormatError("map editor op: object " + std::to_string(m.id) + " doesn't exist");
        }
        inv.moves.push_back({m.id, it->second.header.x, it->second.header.y});
        it->second.header.x = m.x;
        it->second.header.y = m.y;
    }
    std::ranges::reverse(inv.moves);
    return inv;
}

SetWaypoints apply(MapDocument& d, const SetWaypoints& p)
{
    SetWaypoints inv;
    auto& list = d.waypoints.waypoints;
    for (const WaypointChange& c : p.waypoints) {
        WaypointChange undo;
        undo.id = c.id;
        const auto it = std::ranges::find(list, c.id, &map::Waypoint::id);
        std::int32_t was = -1;
        if (it != list.end()) {
            undo.waypoint = *it;
            was = static_cast<std::int32_t>(it - list.begin());
            list.erase(it);
        }
        undo.at = was;
        if (c.waypoint) {
            map::Waypoint w = *c.waypoint;
            w.id = c.id;
            insertAt(list, c.at >= 0 ? c.at : was, std::move(w));
            d.ensureSection(map::Waypoints::kName);
        }
        inv.waypoints.push_back(std::move(undo));
    }
    std::ranges::reverse(inv.waypoints);
    return inv;
}

template <class Part, class Field>
Part swapValue(Field& field, const Field& value)
{
    Part inv{field};
    field = value;
    return inv;
}

/// The keys a keyed list gets: `given` when the op names them (one per entry, distinct), else the
/// keys the list positions had, new ones for positions past them.
std::vector<std::uint32_t> keysFor(const std::vector<std::uint32_t>& given, const std::vector<std::uint32_t>& had, std::size_t n,
                                   std::uint32_t next)
{
    if (!given.empty()) {
        if (given.size() != n) {
            throw FormatError("map editor op: " + std::to_string(given.size()) + " keys for " + std::to_string(n) + " entries");
        }
        if (std::set<std::uint32_t>(given.begin(), given.end()).size() != n || std::ranges::find(given, 0u) != given.end()) {
            throw FormatError("map editor op: keys repeated or zero");
        }
        return given;
    }
    std::vector<std::uint32_t> out(had.begin(), had.begin() + static_cast<std::ptrdiff_t>(std::min(had.size(), n)));
    std::set<std::uint32_t> used(out.begin(), out.end());
    while (out.size() < n) {
        while (used.contains(next) || next == 0) ++next;
        used.insert(next);
        out.push_back(next++);
    }
    return out;
}

/// Swaps a keyed list (polygons, groups) and its keys; the inverse names the old keys.
template <class Part, class Section>
Part swapKeyed(Section& field, std::vector<std::uint32_t>& keys, std::uint32_t& next, const Section& value,
               const std::vector<std::uint32_t>& given, std::size_t (*count)(const Section&))
{
    std::vector<std::uint32_t> newKeys = keysFor(given, keys, count(value), next);
    Part inv{field, keysFor({}, keys, count(field), next)};
    field = value;
    keys = std::move(newKeys);
    for (const std::uint32_t k : keys) next = std::max(next, k + 1);
    for (const std::uint32_t k : inv.keys) next = std::max(next, k + 1);
    return inv;
}

std::size_t polygonCount(const map::Polygons& p) { return p.polygons.size(); }
std::size_t groupCount(const map::GroupData& g) { return g.groups.size(); }

OpPart applyPart(MapDocument& d, const OpPart& part)
{
    return std::visit(Overloaded{
                          [&](const SetWalls& p) -> OpPart { return apply(d, p); },
                          [&](const SetTiles& p) -> OpPart { return apply(d, p); },
                          [&](const AddObjects& p) -> OpPart { return apply(d, p); },
                          [&](const RemoveObjects& p) -> OpPart { return apply(d, p); },
                          [&](const SetObjects& p) -> OpPart { return apply(d, p); },
                          [&](const MoveObjects& p) -> OpPart { return apply(d, p); },
                          [&](const SetWaypoints& p) -> OpPart { return apply(d, p); },
                          [&](const SetMapInfo& p) -> OpPart {
                              d.ensureSection(map::MapInfo::kName);
                              return swapValue<SetMapInfo>(d.info, p.info);
                          },
                          [&](const SetAmbient& p) -> OpPart {
                              d.ensureSection(map::AmbientData::kName);
                              return swapValue<SetAmbient>(d.ambient, p.ambient);
                          },
                          [&](const SetIntro& p) -> OpPart { return swapValue<SetIntro>(d.intro, p.intro); },
                          [&](const SetDebug& p) -> OpPart { return swapValue<SetDebug>(d.debugData, p.debug); },
                          [&](const SetPolygons& p) -> OpPart {
                              return swapKeyed<SetPolygons>(d.polygons, d.polygonKeys, d.nextPolygonKey, p.polygons, p.keys, polygonCount);
                          },
                          [&](const SetGroups& p) -> OpPart {
                              return swapKeyed<SetGroups>(d.groups, d.groupKeys, d.nextGroupKey, p.groups, p.keys, groupCount);
                          },
                          [&](const SetScript& p) -> OpPart {
                              d.ensureSection(map::ScriptObject::kName);
                              return swapValue<SetScript>(d.script, p.script);
                          },
                          [&](const SetSections& p) -> OpPart { return swapValue<SetSections>(d.sections, p.sections); },
                          [&](const SetScriptSource& p) -> OpPart { return swapValue<SetScriptSource>(d.scriptSource, p.source); },
                      },
                      part);
}

// --- Byte form ----------------------------------------------------------------------------------

constexpr std::uint32_t kOpMagic = 0x504F584E; // "NXOP"
/// 2: SetPolygons and SetGroups carry their keys (1 is still read: no keys). 3: a waypoint change's
/// flags (fresh), SetPolygons' and SetGroups' `before` (2 and 1 still read: none).
constexpr std::uint8_t kOpFormat = 3;

void str(ByteWriter& w, std::string_view s)
{
    if (s.size() > 0xFFFF) {
        throw FormatError("map editor op: string too long");
    }
    w.u16(static_cast<std::uint16_t>(s.size()));
    w.rawString(s);
}
std::string str(ByteReader& r)
{
    return r.rawString(r.u16());
}
void blob(ByteWriter& w, std::span<const std::uint8_t> b)
{
    w.u32(static_cast<std::uint32_t>(b.size()));
    w.bytes(b);
}
std::vector<std::uint8_t> blob(ByteReader& r)
{
    const auto b = r.bytes(r.u32());
    return {b.begin(), b.end()};
}
void count(ByteWriter& w, std::size_t n)
{
    w.u32(static_cast<std::uint32_t>(n));
}
std::size_t count(ByteReader& r)
{
    const std::uint32_t n = r.u32();
    if (n > r.remaining()) { // every element takes at least a byte
        r.fail("count past the end");
    }
    return n;
}

void put(ByteWriter& w, const map::Wall& v)
{
    w.u8(v.x);
    w.u8(v.y);
    w.u8(v.direction);
    w.u8(v.material);
    w.u8(v.variant);
    w.u8(v.minimapGroup);
    w.u8(v.unknown);
}
map::Wall getWall(ByteReader& r)
{
    map::Wall v;
    v.x = r.u8();
    v.y = r.u8();
    v.direction = r.u8();
    v.material = r.u8();
    v.variant = r.u8();
    v.minimapGroup = r.u8();
    v.unknown = r.u8();
    return v;
}
void put(ByteWriter& w, const map::SecretWall& v)
{
    w.i32(v.pos.x);
    w.i32(v.pos.y);
    w.u32(v.openWait);
    w.u8(v.flags);
    w.u8(v.state);
    w.u8(v.openDelay);
    w.u32(v.lastOpen);
    w.u32(v.unknown);
}
map::SecretWall getSecret(ByteReader& r)
{
    map::SecretWall v;
    v.pos.x = r.i32();
    v.pos.y = r.i32();
    v.openWait = r.u32();
    v.flags = r.u8();
    v.state = r.u8();
    v.openDelay = r.u8();
    v.lastOpen = r.u32();
    v.unknown = r.u32();
    return v;
}
void put(ByteWriter& w, const map::FloorTile& v)
{
    w.u8(v.image);
    w.i16(v.variant);
    w.u8(v.unknown1);
    w.u8(v.unknown2);
    count(w, v.edges.size());
    for (const auto& e : v.edges) {
        w.u8(e.floor);
        w.i16(e.variant);
        w.u8(e.edge);
        w.u8(e.direction);
    }
}
map::FloorTile getTile(ByteReader& r)
{
    map::FloorTile v;
    v.image = r.u8();
    v.variant = r.i16();
    v.unknown1 = r.u8();
    v.unknown2 = r.u8();
    v.edges.resize(count(r));
    for (auto& e : v.edges) {
        e.floor = r.u8();
        e.variant = r.i16();
        e.edge = r.u8();
        e.direction = r.u8();
    }
    return v;
}
void put(ByteWriter& w, const map::Waypoint& v)
{
    w.u32(v.id);
    w.f32(v.x);
    w.f32(v.y);
    str(w, v.name);
    w.u32(v.flags);
    count(w, v.links.size());
    for (const auto& l : v.links) {
        w.u32(l.target);
        w.u8(l.flags);
    }
}
map::Waypoint getWaypoint(ByteReader& r)
{
    map::Waypoint v;
    v.id = r.u32();
    v.x = r.f32();
    v.y = r.f32();
    v.name = str(r);
    v.flags = r.u32();
    v.links.resize(count(r));
    for (auto& l : v.links) {
        l.target = r.u32();
        l.flags = r.u8();
    }
    return v;
}
void put(ByteWriter& w, const DocObject& o, const things::ThingDb& things)
{
    const detail::TypeXfer t = detail::typeXfer(things, o.type);
    map::ObjectXfer x;
    x.xferVersion = o.xferVersion;
    x.header = o.header;
    x.extra = map::xfer::encode(o.data, o.xferVersion, t.info, x.extraOffset());
    str(w, o.type);
    blob(w, x.serialize());
    w.u32(o.parent);
    w.u64(o.order);
    blob(w, o.padding);
    w.u32(o.sizeHi);
    w.u16(o.tocId);
}
DocObject getObject(ByteReader& r, const things::ThingDb& things)
{
    DocObject o;
    o.type = str(r);
    const auto record = blob(r);
    const map::ObjectXfer x = map::ObjectXfer::parse(record);
    const detail::TypeXfer t = detail::typeXfer(things, o.type);
    o.xferVersion = x.xferVersion;
    o.header = x.header;
    o.data = map::xfer::decode(t.xfer, x.xferVersion, x.extra, t.info, x.extraOffset());
    o.parent = r.u32();
    o.order = r.u64();
    o.padding = blob(r);
    o.sizeHi = r.u32();
    o.tocId = r.u16();
    return o;
}

template <class Section>
void putSection(ByteWriter& w, const Section& s)
{
    blob(w, s.serialize());
}
/// (Polygons: a version 3 list has no flags word, so one with a flag set goes as version 4, as the
/// writer saves it; else a session's peers lost the flag.)
void putSection(ByteWriter& w, const map::Polygons& s)
{
    if (s.version < 4 && std::ranges::any_of(s.polygons, [](const map::Polygon& p) { return p.flags != 0; })) {
        map::Polygons v4 = s;
        v4.version = 4;
        blob(w, v4.serialize());
        return;
    }
    blob(w, s.serialize());
}
template <class Section>
Section getSection(ByteReader& r)
{
    const auto b = blob(r);
    return Section::parse(b);
}

void putKeys(ByteWriter& w, const std::vector<std::uint32_t>& keys)
{
    count(w, keys.size());
    for (const std::uint32_t k : keys) w.u32(k);
}
std::vector<std::uint32_t> getKeys(ByteReader& r)
{
    std::vector<std::uint32_t> keys(count(r));
    for (auto& k : keys) k = r.u32();
    return keys;
}

/// A keyed list's part (SetPolygons, SetGroups): the list, its keys, and (format 3) the list
/// before with its keys.
template <class Part, class Section>
void putKeyed(ByteWriter& w, const Part& p, const Section& list)
{
    putSection(w, list);
    putKeys(w, p.keys);
    w.u8(p.before ? 1 : 0);
    if (p.before) {
        putSection(w, *p.before);
        putKeys(w, p.beforeKeys);
    }
}
template <class Part>
Part getKeyed(ByteReader& r, std::uint8_t format)
{
    using Section = typename decltype(Part::before)::value_type;
    Part p{getSection<Section>(r), {}};
    if (format >= 2) {
        p.keys = getKeys(r);
    }
    if (format >= 3 && r.u8() != 0) {
        p.before = getSection<Section>(r);
        p.beforeKeys = getKeys(r);
    }
    return p;
}

/// Objects by id (AddObjects, SetObjects).
void putEntries(ByteWriter& w, const std::vector<ObjectEntry>& objects, const things::ThingDb& things)
{
    count(w, objects.size());
    for (const auto& e : objects) {
        w.u32(e.id);
        put(w, e.object, things);
    }
}
std::vector<ObjectEntry> getEntries(ByteReader& r, const things::ThingDb& things)
{
    std::vector<ObjectEntry> objects(count(r));
    for (auto& e : objects) {
        e.id = r.u32();
        e.object = getObject(r, things);
    }
    return objects;
}

void put(ByteWriter& w, const OpPart& part, const things::ThingDb& things)
{
    w.u8(static_cast<std::uint8_t>(part.index()));
    std::visit(Overloaded{
                   [&](const SetWalls& p) {
                       count(w, p.cells.size());
                       for (const auto& c : p.cells) {
                           w.u8(c.cell.x);
                           w.u8(c.cell.y);
                           w.i32(c.secretAt);
                           w.i32(c.breakableAt);
                           w.i32(c.windowAt);
                           w.u8(c.state ? 1 : 0);
                           if (c.state) {
                               put(w, c.state->wall);
                               w.u8(static_cast<std::uint8_t>((c.state->secret ? 1 : 0) | (c.state->destructible ? 2 : 0) |
                                                              (c.state->window ? 4 : 0)));
                               if (c.state->secret) {
                                   put(w, *c.state->secret);
                               }
                           }
                       }
                   },
                   [&](const SetTiles& p) {
                       count(w, p.tiles.size());
                       for (const auto& t : p.tiles) {
                           w.i16(t.gx);
                           w.i16(t.gy);
                           w.u8(t.tile ? 1 : 0);
                           if (t.tile) {
                               put(w, *t.tile);
                           }
                       }
                   },
                   [&](const AddObjects& p) { putEntries(w, p.objects, things); },
                   [&](const RemoveObjects& p) {
                       count(w, p.ids.size());
                       for (const ObjId id : p.ids) {
                           w.u32(id);
                       }
                   },
                   [&](const SetObjects& p) { putEntries(w, p.objects, things); },
                   [&](const MoveObjects& p) {
                       count(w, p.moves.size());
                       for (const auto& m : p.moves) {
                           w.u32(m.id);
                           w.f32(m.x);
                           w.f32(m.y);
                       }
                   },
                   [&](const SetWaypoints& p) {
                       count(w, p.waypoints.size());
                       for (const auto& c : p.waypoints) {
                           w.u32(c.id);
                           w.i32(c.at);
                           w.u8(static_cast<std::uint8_t>((c.waypoint ? 1 : 0) | (c.fresh ? 2 : 0)));
                           if (c.waypoint) {
                               put(w, *c.waypoint);
                           }
                       }
                   },
                   [&](const SetMapInfo& p) { putSection(w, p.info); },
                   [&](const SetAmbient& p) { putSection(w, p.ambient); },
                   [&](const SetIntro& p) { putSection(w, p.intro); },
                   [&](const SetDebug& p) { putSection(w, p.debug); },
                   [&](const SetPolygons& p) { putKeyed(w, p, p.polygons); },
                   [&](const SetGroups& p) { putKeyed(w, p, p.groups); },
                   [&](const SetScript& p) { putSection(w, p.script); },
                   [&](const SetSections& p) {
                       count(w, p.sections.size());
                       for (const auto& s : p.sections) {
                           str(w, s);
                       }
                   },
                   [&](const SetScriptSource& p) {
                       w.u8(p.source ? 1 : 0);
                       if (p.source) {
                           blob(w, std::span(reinterpret_cast<const std::uint8_t*>(p.source->data()), p.source->size()));
                       }
                   },
               },
               part);
}

OpPart getPart(ByteReader& r, const things::ThingDb& things, std::uint8_t format)
{
    switch (r.u8()) {
    case 0: {
        SetWalls p;
        p.cells.resize(count(r));
        for (auto& c : p.cells) {
            c.cell.x = r.u8();
            c.cell.y = r.u8();
            c.secretAt = r.i32();
            c.breakableAt = r.i32();
            c.windowAt = r.i32();
            if (r.u8() != 0) {
                WallState s;
                s.wall = getWall(r);
                const std::uint8_t bits = r.u8();
                s.destructible = (bits & 2) != 0;
                s.window = (bits & 4) != 0;
                if ((bits & 1) != 0) {
                    s.secret = getSecret(r);
                }
                c.state = std::move(s);
            }
        }
        return p;
    }
    case 1: {
        SetTiles p;
        p.tiles.resize(count(r));
        for (auto& t : p.tiles) {
            t.gx = r.i16();
            t.gy = r.i16();
            if (r.u8() != 0) {
                t.tile = getTile(r);
            }
        }
        return p;
    }
    case 2: return AddObjects{getEntries(r, things)};
    case 3: {
        RemoveObjects p;
        p.ids.resize(count(r));
        for (auto& id : p.ids) {
            id = r.u32();
        }
        return p;
    }
    case 4: return SetObjects{getEntries(r, things)};
    case 5: {
        MoveObjects p;
        p.moves.resize(count(r));
        for (auto& m : p.moves) {
            m.id = r.u32();
            m.x = r.f32();
            m.y = r.f32();
        }
        return p;
    }
    case 6: {
        SetWaypoints p;
        p.waypoints.resize(count(r));
        for (auto& c : p.waypoints) {
            c.id = r.u32();
            c.at = r.i32();
            const std::uint8_t bits = r.u8();
            if (format < 3 ? bits != 0 : (bits & 1) != 0) {
                c.waypoint = getWaypoint(r);
            }
            c.fresh = format >= 3 && (bits & 2) != 0;
        }
        return p;
    }
    case 7: return SetMapInfo{getSection<map::MapInfo>(r)};
    case 8: return SetAmbient{getSection<map::AmbientData>(r)};
    case 9: return SetIntro{getSection<map::MapIntro>(r)};
    case 10: return SetDebug{getSection<map::DebugData>(r)};
    case 11: return getKeyed<SetPolygons>(r, format);
    case 12: return getKeyed<SetGroups>(r, format);
    case 13: return SetScript{getSection<map::ScriptObject>(r)};
    case 14: {
        SetSections p;
        p.sections.resize(count(r));
        for (auto& s : p.sections) {
            s = str(r);
        }
        return p;
    }
    case 15: {
        SetScriptSource p;
        if (r.u8() != 0) {
            const auto b = blob(r);
            p.source = std::string(b.begin(), b.end());
        }
        return p;
    }
    default: r.fail("unknown op part");
    }
}

static_assert(std::variant_size_v<OpPart> == 16, "a new op part needs its byte form (put, getPart)");

} // namespace

std::optional<WallState> wallState(const MapDocument& d, Cell cell)
{
    const auto it = d.walls.find(cell);
    if (it == d.walls.end()) {
        return std::nullopt;
    }
    WallState s;
    s.wall = it->second;
    if (const auto sw = std::ranges::find_if(d.secretWalls.walls, [&](const map::SecretWall& w) { return atCell(w.pos, cell); });
        sw != d.secretWalls.walls.end()) {
        s.secret = *sw;
    }
    s.destructible = std::ranges::any_of(d.destructableWalls.walls, [&](const map::GridPoint& g) { return atCell(g, cell); });
    s.window = std::ranges::any_of(d.windowWalls.walls, [&](const map::GridPoint& g) { return atCell(g, cell); });
    return s;
}

bool Op::empty() const
{
    return std::ranges::all_of(parts, [](const OpPart& p) {
        return std::visit(Overloaded{
                              [](const SetWalls& v) { return v.cells.empty(); },
                              [](const SetTiles& v) { return v.tiles.empty(); },
                              [](const AddObjects& v) { return v.objects.empty(); },
                              [](const RemoveObjects& v) { return v.ids.empty(); },
                              [](const SetObjects& v) { return v.objects.empty(); },
                              [](const MoveObjects& v) { return v.moves.empty(); },
                              [](const SetWaypoints& v) { return v.waypoints.empty(); },
                              [](const auto&) { return false; },
                          },
                          p);
    });
}

Op applyOp(MapDocument& doc, const Op& op)
{
    const std::vector<std::string> sectionsBefore = doc.sections;
    Op inv;
    inv.label = op.label;
    inv.parts.reserve(op.parts.size() + 1);
    try {
        for (const OpPart& part : op.parts) {
            inv.parts.push_back(applyPart(doc, part));
        }
    } catch (...) {
        // Roll back what was applied, last first.
        for (auto it = inv.parts.rbegin(); it != inv.parts.rend(); ++it) {
            applyPart(doc, *it);
        }
        doc.sections = sectionsBefore;
        throw;
    }
    std::ranges::reverse(inv.parts);
    const bool setsSections = std::ranges::any_of(op.parts, [](const OpPart& p) { return std::holds_alternative<SetSections>(p); });
    if (!setsSections && doc.sections != sectionsBefore) {
        // A section the op's content needed: undo takes it out of the list again.
        inv.parts.push_back(SetSections{sectionsBefore});
    }
    return inv;
}

std::vector<std::uint8_t> serializeOp(const Op& op, const things::ThingDb& things)
{
    ByteWriter w;
    w.u32(kOpMagic);
    w.u8(kOpFormat);
    str(w, op.label);
    count(w, op.parts.size());
    for (const OpPart& part : op.parts) {
        put(w, part, things);
    }
    return std::move(w).take();
}

Op parseOp(std::span<const std::uint8_t> bytes, const things::ThingDb& things)
{
    ByteReader r(bytes, "map editor op");
    if (r.u32() != kOpMagic) {
        r.fail("not an op");
    }
    const std::uint8_t format = r.u8();
    if (format < 1 || format > kOpFormat) {
        r.fail("op format " + std::to_string(format));
    }
    Op op;
    op.label = str(r);
    op.parts.resize(count(r));
    for (auto& part : op.parts) {
        part = getPart(r, things, format);
    }
    r.expectEnd();
    return op;
}

std::size_t approxBytes(const Op& op)
{
    std::size_t n = sizeof(Op) + op.label.size();
    for (const OpPart& p : op.parts) {
        n += sizeof(OpPart);
        std::visit(Overloaded{
                       [&](const SetWalls& v) { n += v.cells.size() * sizeof(WallChange); },
                       [&](const SetTiles& v) {
                           for (const auto& t : v.tiles) {
                               n += sizeof(TileChange) + (t.tile ? t.tile->edges.size() * sizeof(map::FloorEdge) : 0);
                           }
                       },
                       [&](const AddObjects& v) { n += v.objects.size() * (sizeof(ObjectEntry) + 256); },
                       [&](const RemoveObjects& v) { n += v.ids.size() * sizeof(ObjId); },
                       [&](const SetObjects& v) { n += v.objects.size() * (sizeof(ObjectEntry) + 256); },
                       [&](const MoveObjects& v) { n += v.moves.size() * sizeof(ObjectMove); },
                       [&](const SetWaypoints& v) { n += v.waypoints.size() * (sizeof(WaypointChange) + 32); },
                       [&](const SetScript& v) { n += v.script.code.size(); },
                       [&](const SetScriptSource& v) { n += v.source ? v.source->size() : 0; },
                       [&](const SetPolygons& v) { n += v.polygons.points.size() * 12 + v.polygons.polygons.size() * 96; },
                       [&](const auto& v) { n += sizeof(v); },
                   },
                   p);
    }
    return n;
}

// --- History ------------------------------------------------------------------------------------

void History::perform(MapDocument& doc, const Op& op)
{
    if (op.empty()) {
        return;
    }
    recordApplied(op, applyOp(doc, op));
}

void History::recordApplied(const Op& op, Op inverse)
{
    if (op.empty()) {
        return;
    }
    if (position_ < entries_.size()) {
        // The undone tail is gone; if the save was in it, no position equals it any more.
        if (saved_ > static_cast<std::ptrdiff_t>(dropped_ + position_)) {
            saved_ = -1;
        }
        for (std::size_t i = position_; i < entries_.size(); ++i) {
            bytes_ -= entries_[i].bytes;
        }
        entries_.resize(position_);
    }
    Entry e{op, std::move(inverse), 0};
    e.bytes = approxBytes(e.op) + approxBytes(e.inverse);
    bytes_ += e.bytes;
    entries_.push_back(std::move(e));
    ++position_;
    ++revision_;
    trim();
}

std::string History::undo(MapDocument& doc)
{
    if (!canUndo()) {
        return {};
    }
    Entry& e = entries_[position_ - 1];
    applyOp(doc, e.inverse);
    --position_;
    ++revision_;
    return e.op.label;
}

std::string History::redo(MapDocument& doc)
{
    if (!canRedo()) {
        return {};
    }
    Entry& e = entries_[position_];
    applyOp(doc, e.op);
    ++position_;
    ++revision_;
    return e.op.label;
}

bool History::stepBack()
{
    if (!canUndo()) {
        return false;
    }
    --position_;
    ++revision_;
    return true;
}

bool History::stepForward()
{
    if (!canRedo()) {
        return false;
    }
    ++position_;
    ++revision_;
    return true;
}

void History::replaceDone(std::size_t back, Op op, Op inverse)
{
    if (back == 0 || back > position_) {
        return;
    }
    Entry& e = entries_[position_ - back];
    bytes_ -= e.bytes;
    e.op = std::move(op);
    e.inverse = std::move(inverse);
    e.bytes = approxBytes(e.op) + approxBytes(e.inverse);
    bytes_ += e.bytes;
}

void History::jumpTo(MapDocument& doc, std::size_t position)
{
    position = std::min(position, entries_.size());
    while (position_ > position) {
        undo(doc);
    }
    while (position_ < position) {
        redo(doc);
    }
}

void History::clear()
{
    entries_.clear();
    position_ = 0;
    dropped_ = 0;
    saved_ = 0;
    bytes_ = 0;
    ++revision_;
}

void History::trim()
{
    std::size_t drop = 0;
    while (bytes_ > capBytes_ && drop + 1 < position_) {
        bytes_ -= entries_[drop].bytes;
        ++drop;
    }
    if (drop > 0) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
        position_ -= drop;
        dropped_ += drop;
        if (saved_ >= 0 && saved_ < static_cast<std::ptrdiff_t>(dropped_)) {
            saved_ = -1; // (the saved state can't be reached by undoing any more)
        }
    }
}

} // namespace nox::mapedit
