// Element locks for collaborative editing.
#include "noxcore/mapedit/locks.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"

#include <algorithm>
#include <variant>

namespace nox::mapedit {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

/// An object's lock is its world owner's: an inventory item is locked with what carries it.
ObjId lockRoot(const MapDocument& d, ObjId id)
{
    for (int depth = 0; depth < 64; ++depth) {
        const auto it = d.objects.find(id);
        if (it == d.objects.end() || it->second.parent == 0) {
            return id;
        }
        id = it->second.parent;
    }
    return id;
}

/// The keys a keyed list (polygons, groups) has after an op naming `given` (ops.cpp's rule: the
/// keys given, else the positions' keys and new ones past them; new ones are never held).
std::vector<std::uint32_t> keysAfter(const std::vector<std::uint32_t>& given, const std::vector<std::uint32_t>& had, std::size_t n)
{
    if (!given.empty()) return given;
    std::vector<std::uint32_t> out(had.begin(), had.begin() + static_cast<std::ptrdiff_t>(std::min(had.size(), n)));
    out.resize(n, 0); // (0: a new entry, which no one holds)
    return out;
}

/// The elements of a keyed list an op changes: every existing key whose entry (as `same` compares
/// it) is gone or different afterwards.
template <class T, class Same>
void changedKeys(const std::vector<T>& now, const std::vector<std::uint32_t>& nowKeys, const std::vector<T>& next,
                 const std::vector<std::uint32_t>& nextKeys, ElementKey::Kind kind, Same same, std::set<ElementKey>& out)
{
    for (std::size_t i = 0; i < now.size() && i < nowKeys.size(); ++i) {
        const auto it = std::ranges::find(nextKeys, nowKeys[i]);
        const std::size_t j = static_cast<std::size_t>(it - nextKeys.begin());
        if (it == nextKeys.end() || j >= next.size() || !same(now[i], next[j])) {
            out.insert({kind, static_cast<std::int32_t>(nowKeys[i]), 0, {}});
        }
    }
}

/// A polygon with its corners' coordinates (a point moved changes every polygon using it).
std::vector<std::pair<float, float>> corners(const map::Polygons& all, const map::Polygon& p)
{
    std::vector<std::pair<float, float>> out;
    for (const std::uint32_t id : p.points) {
        const auto it = std::ranges::find(all.points, id, &map::PolygonPoint::id);
        out.emplace_back(it == all.points.end() ? -1.0f : it->x, it == all.points.end() ? -1.0f : it->y);
    }
    return out;
}

constexpr std::uint32_t kLockMagic = 0x4B4C584E; // "NXLK"

} // namespace

std::string ElementKey::text() const
{
    switch (kind) {
    case Kind::Object: return "object " + std::to_string(a);
    case Kind::Wall: return "wall " + std::to_string(a) + "," + std::to_string(b);
    case Kind::Tile: return "tile " + std::to_string(a) + "," + std::to_string(b);
    case Kind::Waypoint: return "waypoint " + std::to_string(a);
    case Kind::Polygon: return "polygon #" + std::to_string(a);
    case Kind::Group: return "group #" + std::to_string(a);
    case Kind::ScriptFunction: return name == "*" ? std::string("the script") : "script function " + name;
    case Kind::MapInfo: return "the map info";
    }
    return "element";
}

std::set<ElementKey> elementsOf(const MapDocument& d, const Selection& s)
{
    std::set<ElementKey> out;
    for (const ObjId id : s.objects) {
        out.insert(ElementKey::object(lockRoot(d, id)));
    }
    for (const Cell c : s.walls) out.insert(ElementKey::wall(c));
    for (const TileCell t : s.tiles) out.insert(ElementKey::tile(t));
    for (const std::uint32_t w : s.waypoints) out.insert(ElementKey::waypoint(w));
    for (const std::uint32_t p : s.polygons) out.insert(ElementKey::polygon(p));
    return out;
}

std::set<ElementKey> elementsTouched(const MapDocument& d, const Op& op)
{
    std::set<ElementKey> out;
    const auto object = [&](ObjId id) {
        if (d.objects.contains(id)) {
            out.insert(ElementKey::object(lockRoot(d, id)));
        }
    };
    for (const OpPart& part : op.parts) {
        std::visit(Overloaded{
                       [&](const SetWalls& p) {
                           for (const auto& c : p.cells) out.insert(ElementKey::wall(c.cell));
                       },
                       [&](const SetTiles& p) {
                           for (const auto& t : p.tiles) out.insert(ElementKey::tile({t.gx, t.gy}));
                       },
                       [&](const AddObjects& p) {
                           // A new object touches nothing held, but an item put into a held
                           // object's inventory touches that object.
                           for (const auto& e : p.objects) {
                               if (e.object.parent != 0) object(e.object.parent);
                           }
                       },
                       [&](const RemoveObjects& p) {
                           for (const ObjId id : p.ids) object(id);
                       },
                       [&](const SetObjects& p) {
                           for (const auto& e : p.objects) {
                               object(e.id);
                               if (e.object.parent != 0) object(e.object.parent);
                           }
                       },
                       [&](const MoveObjects& p) {
                           for (const auto& m : p.moves) object(m.id);
                       },
                       [&](const SetWaypoints& p) {
                           for (const auto& c : p.waypoints) {
                               if (std::ranges::find(d.waypoints.waypoints, c.id, &map::Waypoint::id) != d.waypoints.waypoints.end()) {
                                   out.insert(ElementKey::waypoint(c.id));
                               }
                           }
                       },
                       [&](const SetPolygons& p) {
                           const auto same = [&](const map::Polygon& a, const map::Polygon& b) {
                               if (!(a.name == b.name && a.ambientLight == b.ambientLight && a.minimapGroup == b.minimapGroup &&
                                     a.playerEnter == b.playerEnter && a.monsterEnter == b.monsterEnter && a.flags == b.flags)) {
                                   return false;
                               }
                               return corners(d.polygons, a) == corners(p.polygons, b);
                           };
                           changedKeys(d.polygons.polygons, d.polygonKeys, p.polygons.polygons,
                                       keysAfter(p.keys, d.polygonKeys, p.polygons.polygons.size()), ElementKey::Kind::Polygon, same, out);
                       },
                       [&](const SetGroups& p) {
                           changedKeys(d.groups.groups, d.groupKeys, p.groups.groups, keysAfter(p.keys, d.groupKeys, p.groups.groups.size()),
                                       ElementKey::Kind::Group, std::equal_to<map::Group>(), out);
                       },
                       [&](const SetScript&) { out.insert({ElementKey::Kind::ScriptFunction, 0, 0, "*"}); },
                       [&](const SetScriptSource&) { out.insert({ElementKey::Kind::ScriptFunction, 0, 0, "*"}); },
                       [&](const SetMapInfo&) { out.insert({ElementKey::Kind::MapInfo, 0, 0, {}}); },
                       [&](const SetAmbient&) { out.insert({ElementKey::Kind::MapInfo, 0, 0, {}}); },
                       [&](const SetIntro&) { out.insert({ElementKey::Kind::MapInfo, 0, 0, {}}); },
                       [&](const SetDebug&) { out.insert({ElementKey::Kind::MapInfo, 0, 0, {}}); },
                       [&](const SetSections&) {},
                   },
                   part);
    }
    return out;
}

Selection withoutLocked(const MapDocument& d, const Selection& s, const LockTable& locks, PeerId self, std::size_t* skipped)
{
    Selection out;
    std::size_t n = 0;
    const auto free = [&](const ElementKey& k) {
        const auto h = locks.holder(k);
        const bool ok = !h || *h == self;
        n += ok ? 0 : 1;
        return ok;
    };
    for (const ObjId id : s.objects) {
        if (free(ElementKey::object(lockRoot(d, id)))) out.objects.insert(id);
    }
    for (const Cell c : s.walls) {
        if (free(ElementKey::wall(c))) out.walls.insert(c);
    }
    for (const TileCell t : s.tiles) {
        if (free(ElementKey::tile(t))) out.tiles.insert(t);
    }
    for (const std::uint32_t w : s.waypoints) {
        if (free(ElementKey::waypoint(w))) out.waypoints.insert(w);
    }
    for (const std::uint32_t p : s.polygons) {
        if (free(ElementKey::polygon(p))) out.polygons.insert(p);
    }
    if (skipped) {
        *skipped = n;
    }
    return out;
}

std::set<ElementKey> LockTable::request(PeerId peer, const std::set<ElementKey>& keys, std::uint64_t now)
{
    std::set<ElementKey> refused;
    for (const ElementKey& k : keys) {
        const auto it = locks_.find(k);
        if (it != locks_.end() && it->second.peer != peer) {
            refused.insert(k);
            continue;
        }
        locks_[k] = {peer, now};
    }
    return refused;
}

std::set<ElementKey> LockTable::setHeld(PeerId peer, const std::set<ElementKey>& keys, std::uint64_t now)
{
    std::erase_if(locks_, [&](const auto& kv) { return kv.second.peer == peer && !keys.contains(kv.first); });
    return request(peer, keys, now);
}

void LockTable::release(PeerId peer, const std::set<ElementKey>& keys)
{
    for (const ElementKey& k : keys) {
        if (const auto it = locks_.find(k); it != locks_.end() && it->second.peer == peer) {
            locks_.erase(it);
        }
    }
}

void LockTable::releaseAll(PeerId peer)
{
    std::erase_if(locks_, [&](const auto& kv) { return kv.second.peer == peer; });
}

void LockTable::touch(PeerId peer, std::uint64_t now)
{
    for (auto& [k, h] : locks_) {
        if (h.peer == peer) {
            h.lastUse = now;
        }
    }
}

std::set<PeerId> LockTable::expire(std::uint64_t now, std::uint64_t timeout)
{
    std::set<PeerId> lost;
    std::erase_if(locks_, [&](const auto& kv) {
        if (now >= timeout && kv.second.lastUse < now - timeout) {
            lost.insert(kv.second.peer);
            return true;
        }
        return false;
    });
    return lost;
}

std::optional<PeerId> LockTable::holder(const ElementKey& key) const
{
    const auto it = locks_.find(key);
    return it == locks_.end() ? std::nullopt : std::optional(it->second.peer);
}

std::optional<std::pair<ElementKey, PeerId>> LockTable::conflict(PeerId peer, const std::set<ElementKey>& keys) const
{
    for (const ElementKey& k : keys) {
        if (const auto it = locks_.find(k); it != locks_.end() && it->second.peer != peer) {
            return std::pair{k, it->second.peer};
        }
    }
    return std::nullopt;
}

std::set<ElementKey> LockTable::heldBy(PeerId peer) const
{
    std::set<ElementKey> out;
    for (const auto& [k, h] : locks_) {
        if (h.peer == peer) {
            out.insert(k);
        }
    }
    return out;
}

std::vector<std::uint8_t> LockTable::serialize() const
{
    ByteWriter w;
    w.u32(kLockMagic);
    w.u32(static_cast<std::uint32_t>(locks_.size()));
    for (const auto& [k, h] : locks_) {
        w.u8(static_cast<std::uint8_t>(k.kind));
        w.i32(k.a);
        w.i32(k.b);
        w.u16(static_cast<std::uint16_t>(k.name.size()));
        w.rawString(k.name);
        w.u32(h.peer);
        w.u64(h.lastUse);
    }
    return std::move(w).take();
}

LockTable LockTable::parse(std::span<const std::uint8_t> bytes)
{
    ByteReader r(bytes, "lock table");
    if (r.u32() != kLockMagic) {
        r.fail("not a lock table");
    }
    LockTable t;
    for (std::uint32_t n = r.u32(); n > 0; --n) {
        ElementKey k;
        const std::uint8_t kind = r.u8();
        if (kind > static_cast<std::uint8_t>(ElementKey::Kind::MapInfo)) {
            r.fail("unknown element kind");
        }
        k.kind = static_cast<ElementKey::Kind>(kind);
        k.a = r.i32();
        k.b = r.i32();
        k.name = r.rawString(r.u16());
        Holder h;
        h.peer = r.u32();
        h.lastUse = r.u64();
        t.locks_[k] = h;
    }
    r.expectEnd();
    return t;
}

} // namespace nox::mapedit
