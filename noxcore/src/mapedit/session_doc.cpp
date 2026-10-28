// The session's document on the wire (session_doc.hpp).
#include "noxcore/mapedit/session_doc.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/mapedit/session_merge.hpp"
#include "noxcore/mapedit/writer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <variant>

namespace nox::mapedit {

namespace {
constexpr std::uint32_t kMagic = 0x4458534E; // "NSXD"
constexpr std::uint16_t kFormat = 1;
} // namespace

std::vector<std::uint8_t> encodeSessionDocument(const MapDocument& doc, const things::ThingDb& things)
{
    ByteWriter w;
    w.u32(kMagic);
    w.u16(kFormat);
    const std::vector<std::uint8_t> map = writeMapBytes(doc, things);
    w.u32(static_cast<std::uint32_t>(map.size()));
    w.bytes(map);
    const std::vector<ObjId> order = recordOrder(doc);
    w.u32(static_cast<std::uint32_t>(order.size()));
    for (const ObjId id : order) {
        w.u32(id);
        w.u64(doc.objects.at(id).order);
    }
    w.u32(doc.nextObjId);
    w.u32(static_cast<std::uint32_t>(doc.polygonKeys.size()));
    for (const auto k : doc.polygonKeys) w.u32(k);
    w.u32(doc.nextPolygonKey);
    w.u32(static_cast<std::uint32_t>(doc.groupKeys.size()));
    for (const auto k : doc.groupKeys) w.u32(k);
    w.u32(doc.nextGroupKey);
    w.u8(doc.scriptSource ? 1 : 0);
    if (doc.scriptSource) {
        w.u32(static_cast<std::uint32_t>(doc.scriptSource->size()));
        w.rawString(*doc.scriptSource);
    }
    w.u8(doc.origin.stock ? 1 : 0);
    w.u16(static_cast<std::uint16_t>(doc.origin.name.size()));
    w.rawString(doc.origin.name);
    return std::move(w).take();
}

MapDocument decodeSessionDocument(std::span<const std::uint8_t> bytes, const things::ThingDb& things)
{
    ByteReader r(bytes, "session document");
    if (r.u32() != kMagic || r.u16() != kFormat) {
        throw FormatError("session document: not one (or a newer format)");
    }
    const std::uint32_t mapSize = r.u32();
    const auto mapBytes = r.bytes(mapSize);
    MapDocument fresh = MapDocument::fromMapFile(map::parseMapFile(mapBytes), things);
    const std::vector<ObjId> freshOrder = recordOrder(fresh);
    const std::uint32_t n = r.u32();
    if (n != freshOrder.size()) {
        throw FormatError("session document: the object ids don't match its records");
    }
    std::map<ObjId, DocObject> objects;
    std::map<ObjId, ObjId> idOf; // fresh id -> the host's
    std::vector<std::pair<ObjId, std::uint64_t>> ids(n);
    for (auto& [id, order] : ids) {
        id = r.u32();
        order = r.u64();
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        idOf[freshOrder[i]] = ids[i].first;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        DocObject o = std::move(fresh.objects.at(freshOrder[i]));
        o.order = ids[i].second;
        if (o.parent != 0) {
            o.parent = idOf.at(o.parent);
        }
        if (!objects.emplace(ids[i].first, std::move(o)).second) {
            throw FormatError("session document: an object id given twice");
        }
    }
    fresh.objects = std::move(objects);
    fresh.nextObjId = r.u32();
    fresh.polygonKeys.resize(r.count(r.u32(), 4));
    for (auto& k : fresh.polygonKeys) k = r.u32();
    fresh.nextPolygonKey = r.u32();
    fresh.groupKeys.resize(r.count(r.u32(), 4));
    for (auto& k : fresh.groupKeys) k = r.u32();
    fresh.nextGroupKey = r.u32();
    if (r.u8()) {
        const std::uint32_t len = r.u32();
        fresh.scriptSource = r.rawString(len);
    } else {
        fresh.scriptSource.reset();
    }
    fresh.origin.stock = r.u8() != 0;
    const std::uint16_t nameLen = r.u16();
    fresh.origin.name = r.rawString(nameLen);
    if (!r.atEnd()) {
        throw FormatError("session document: bytes left over");
    }
    return fresh;
}

MapDocument canonicalSessionDocument(const MapDocument& doc, const things::ThingDb& things)
{
    return decodeSessionDocument(encodeSessionDocument(doc, things), things);
}

namespace {

/// The op's new waypoints (marked fresh) moved off ids `doc` uses (the lowest free, as the editor
/// picks them), the op's own references to them following (links, entries, groups of waypoints).
bool freshenWaypoints(const MapDocument& doc, Op& op, IdMoves* out)
{
    std::set<std::uint32_t> used;
    for (const auto& w : doc.waypoints.waypoints) used.insert(w.id);
    std::set<std::uint32_t> inOp; // (every id the op names: a moved one must not land on any)
    for (const auto& part : op.parts) {
        if (const auto* s = std::get_if<SetWaypoints>(&part)) {
            for (const auto& c : s->waypoints) {
                inOp.insert(c.id);
                if (c.waypoint) {
                    for (const auto& l : c.waypoint->links) inOp.insert(l.target);
                }
            }
        }
    }
    std::map<std::uint32_t, std::uint32_t> moved;
    std::set<std::uint32_t> claimed; // (fresh ids kept or given in this op)
    for (auto& part : op.parts) {
        auto* s = std::get_if<SetWaypoints>(&part);
        if (!s) continue;
        for (auto& c : s->waypoints) {
            if (!c.fresh || !c.waypoint || moved.contains(c.id)) continue;
            if (!used.contains(c.id) && !claimed.contains(c.id)) {
                claimed.insert(c.id);
                continue;
            }
            std::uint32_t id = 1;
            while (used.contains(id) || claimed.contains(id) || inOp.contains(id)) ++id;
            moved[c.id] = id;
            claimed.insert(id);
        }
    }
    if (moved.empty()) {
        return false;
    }
    const auto remap = [&](std::uint32_t& id) {
        if (const auto it = moved.find(id); it != moved.end()) id = it->second;
    };
    for (auto& part : op.parts) {
        if (auto* s = std::get_if<SetWaypoints>(&part)) {
            for (auto& c : s->waypoints) {
                remap(c.id);
                if (c.waypoint) {
                    c.waypoint->id = c.id;
                    for (auto& l : c.waypoint->links) remap(l.target);
                }
            }
        } else if (auto* g = std::get_if<SetGroups>(&part)) {
            for (auto& grp : g->groups.groups) {
                if (grp.kind != map::Group::Kind::Waypoints) continue;
                for (auto& m : grp.members) {
                    auto id = static_cast<std::uint32_t>(m);
                    remap(id);
                    m = static_cast<std::int32_t>(id);
                }
            }
        }
    }
    if (out) {
        for (const auto& [a, b] : moved) out->waypoints[a] = b;
    }
    return true;
}

} // namespace

bool freshenNewIds(const MapDocument& doc, Op& op, IdMoves* out)
{
    bool changed = freshenWaypoints(doc, op, out);
    std::map<ObjId, ObjId> moved;
    ObjId next = doc.nextObjId;
    std::set<ObjId> taken;
    for (const auto& [id, o] : doc.objects) taken.insert(id);
    std::set<std::uint32_t> extents;
    for (const auto& [id, o] : doc.objects) {
        if (o.parent == 0 && o.header.extent != 0) extents.insert(o.header.extent);
    }
    std::uint32_t nextExtent = extents.empty() ? 1 : *extents.rbegin() + 1;
    std::map<std::uint32_t, std::uint32_t> extentMoved; // (old -> new, for groups naming them)
    for (auto& part : op.parts) {
        if (auto* add = std::get_if<AddObjects>(&part)) {
            for (auto& e : add->objects) {
                if (taken.contains(e.id) || moved.contains(e.id)) {
                    while (taken.contains(next)) ++next;
                    moved[e.id] = next;
                    if (out) out->objects[e.id] = next;
                    e.id = next;
                    taken.insert(next++);
                    changed = true;
                } else {
                    taken.insert(e.id);
                }
                if (const auto it = moved.find(e.object.parent); it != moved.end()) {
                    e.object.parent = it->second;
                }
                if (e.object.parent == 0 && e.object.header.extent != 0) {
                    if (extents.contains(e.object.header.extent)) {
                        const std::uint32_t was = e.object.header.extent;
                        e.object.header.extent = nextExtent++;
                        extentMoved[was] = e.object.header.extent;
                        if (out) out->extents[e.id] = {was, e.object.header.extent};
                        changed = true;
                    }
                    extents.insert(e.object.header.extent);
                    nextExtent = std::max(nextExtent, e.object.header.extent + 1);
                }
            }
        } else if (auto* groups = std::get_if<SetGroups>(&part); groups && !extentMoved.empty()) {
            // (A group naming a new object by its extent: the extent it has now.)
            for (auto& g : groups->groups.groups) {
                if (g.kind != map::Group::Kind::Objects) continue;
                for (auto& m : g.members) {
                    if (const auto it = extentMoved.find(static_cast<std::uint32_t>(m)); it != extentMoved.end()) {
                        m = static_cast<std::int32_t>(it->second);
                    }
                }
            }
        } else if (!moved.empty()) {
            const auto remap = [&](ObjId& id) {
                if (const auto it = moved.find(id); it != moved.end()) id = it->second;
            };
            if (auto* rm = std::get_if<RemoveObjects>(&part)) {
                for (auto& id : rm->ids) remap(id);
            } else if (auto* set = std::get_if<SetObjects>(&part)) {
                for (auto& e : set->objects) {
                    remap(e.id);
                    remap(e.object.parent);
                }
            } else if (auto* mv = std::get_if<MoveObjects>(&part)) {
                for (auto& m : mv->moves) remap(m.id);
            }
        }
    }
    return changed;
}

} // namespace nox::mapedit
