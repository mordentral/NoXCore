// The collaborative session's op transforms (session_merge.hpp).
#include "noxcore/mapedit/session_merge.hpp"

#include "noxcore/mapedit/writer.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <variant>

namespace nox::mapedit {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

template <class Section>
bool sameSection(const Section& a, const Section& b)
{
    return a.serialize() == b.serialize();
}

template <class K, class V>
void remapIn(V& v, const std::map<K, K>& m)
{
    if (const auto it = m.find(static_cast<K>(v)); it != m.end()) v = static_cast<V>(it->second);
}

/// A group's members and id followed through the moves.
void remapGroup(map::Group& g, const IdMoves& m, const std::map<std::uint32_t, std::uint32_t>& extents)
{
    remapIn(g.id, m.groupIds);
    for (auto& member : g.members) {
        switch (g.kind) {
        case map::Group::Kind::Objects: {
            auto e = static_cast<std::uint32_t>(member);
            remapIn(e, extents);
            member = static_cast<std::int32_t>(e);
            break;
        }
        case map::Group::Kind::Waypoints: {
            auto w = static_cast<std::uint32_t>(member);
            remapIn(w, m.waypoints);
            member = static_cast<std::int32_t>(w);
            break;
        }
        case map::Group::Kind::Unknown3: remapIn(member, m.groupIds); break;
        case map::Group::Kind::Walls: break;
        }
    }
}

// --- Keyed lists ---------------------------------------------------------------------------------

/// A polygon with its corners' places (its points' ids are the list's own business).
struct PolyEntry {
    map::Polygon poly;
    std::vector<std::pair<float, float>> corners;
    bool operator==(const PolyEntry& o) const
    {
        map::Polygon a = poly, b = o.poly;
        a.points.clear();
        b.points.clear();
        return a == b && corners == o.corners;
    }
};

template <class E>
struct Keyed {
    std::vector<std::uint32_t> keys;
    std::vector<E> entries;
    std::optional<std::size_t> find(std::uint32_t k) const
    {
        const auto it = std::ranges::find(keys, k);
        if (it == keys.end()) return std::nullopt;
        return static_cast<std::size_t>(it - keys.begin());
    }
    bool valid() const
    {
        if (keys.size() != entries.size()) return false;
        std::set<std::uint32_t> s(keys.begin(), keys.end());
        return s.size() == keys.size() && !s.contains(0u);
    }
};

Keyed<PolyEntry> polyEntries(const map::Polygons& p, const std::vector<std::uint32_t>& keys)
{
    Keyed<PolyEntry> out;
    out.keys = keys;
    for (const auto& poly : p.polygons) {
        PolyEntry e{poly, {}};
        for (const std::uint32_t id : poly.points) {
            const auto it = std::ranges::find(p.points, id, &map::PolygonPoint::id);
            e.corners.emplace_back(it == p.points.end() ? -1.0f : it->x, it == p.points.end() ? -1.0f : it->y);
        }
        out.entries.push_back(std::move(e));
    }
    return out;
}

Keyed<map::Group> groupEntries(const map::GroupData& g, const std::vector<std::uint32_t>& keys)
{
    return Keyed<map::Group>{keys, g.groups};
}

struct MergeResult {
    std::vector<std::uint32_t> keys;
    std::vector<bool> fromOp; ///< the entry is the op's (else the list's as it is now)
    std::vector<std::size_t> at; ///< its index in the op's list (fromOp) or in the current one
    std::map<std::uint32_t, std::uint32_t> renamed; ///< the op's new keys moved off taken ones
    std::vector<std::uint32_t> skipped; ///< (Theirs) keys left as they are now
};

/// The per-entry merge of `after` (the op's list) made against `before` onto `now`.
template <class E>
MergeResult mergeKeyed(const Keyed<E>& before, const Keyed<E>& after, const Keyed<E>& now, MergeRule rule, std::uint32_t nextKey)
{
    MergeResult r;
    std::set<std::uint32_t> removed, added, changed;
    for (std::size_t i = 0; i < before.keys.size(); ++i) {
        const auto j = after.find(before.keys[i]);
        if (!j) {
            removed.insert(before.keys[i]);
        } else if (!(after.entries[*j] == before.entries[i])) {
            changed.insert(before.keys[i]);
        }
    }
    for (const std::uint32_t k : after.keys) {
        if (!before.find(k)) added.insert(k);
    }
    // Conflicts: an entry the op changes or removes that someone changed (or removed) since.
    std::set<std::uint32_t> take; // keys whose entry is the op's
    std::set<std::uint32_t> drop; // keys removed
    for (const std::uint32_t k : removed) {
        const auto c = now.find(k);
        if (!c) continue; // (already gone)
        if (rule == MergeRule::Theirs && !(now.entries[*c] == before.entries[*before.find(k)])) {
            r.skipped.push_back(k);
            continue;
        }
        drop.insert(k);
    }
    for (const std::uint32_t k : changed) {
        const auto c = now.find(k);
        if (rule == MergeRule::Theirs && (!c || !(now.entries[*c] == before.entries[*before.find(k)]))) {
            r.skipped.push_back(k);
            continue;
        }
        take.insert(k);
        if (!c) added.insert(k); // (removed meanwhile: the op's change brings it back)
    }
    // New keys clear of the ones in use.
    std::uint32_t next = nextKey;
    for (const auto* list : {&before.keys, &after.keys, &now.keys}) {
        for (const std::uint32_t k : *list) next = std::max(next, k + 1);
    }
    std::map<std::uint32_t, std::uint32_t> keyOf; // (the op's key -> the result's)
    for (const std::uint32_t k : after.keys) {
        if (added.contains(k) && now.find(k) && !before.find(k)) {
            keyOf[k] = next;
            r.renamed[k] = next++;
        } else {
            keyOf[k] = k;
        }
        if (added.contains(k)) take.insert(k);
    }
    // The order: the list as it is now without what goes; the op's order for the entries it
    // reordered; the op's new entries before the entry that follows them in its list.
    std::vector<std::uint32_t> order;
    for (const std::uint32_t k : now.keys) {
        if (!drop.contains(k)) order.push_back(k);
    }
    {
        std::vector<std::uint32_t> inA, inB, inOrder;
        std::set<std::uint32_t> both;
        for (const std::uint32_t k : order) {
            if (after.find(k) && before.find(k)) both.insert(k);
        }
        for (const std::uint32_t k : after.keys) if (both.contains(k)) inA.push_back(k);
        for (const std::uint32_t k : before.keys) if (both.contains(k)) inB.push_back(k);
        for (const std::uint32_t k : order) if (both.contains(k)) inOrder.push_back(k);
        if (inA != inB && inA != inOrder) {
            // (The op reordered: those entries take its order in the slots they hold now.)
            std::size_t n = 0;
            for (auto& k : order) {
                if (both.contains(k)) k = inA[n++];
            }
        }
    }
    for (std::size_t i = 0; i < after.keys.size(); ++i) {
        const std::uint32_t k = after.keys[i];
        if (!added.contains(k)) continue;
        if (std::ranges::find(order, k) != order.end() && !r.renamed.contains(k)) continue;
        std::optional<std::size_t> before_;
        for (std::size_t j = i + 1; j < after.keys.size() && !before_; ++j) {
            const std::uint32_t follower = keyOf.at(after.keys[j]);
            if (const auto it = std::ranges::find(order, follower); it != order.end()) {
                before_ = static_cast<std::size_t>(it - order.begin());
            }
        }
        const std::uint32_t key = keyOf.at(k);
        if (before_) {
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(*before_), key);
        } else {
            order.push_back(key);
        }
    }
    std::map<std::uint32_t, std::uint32_t> opKeyOf; // (the result's key -> the op's)
    for (const auto& [a, b] : keyOf) opKeyOf[b] = a;
    for (const std::uint32_t k : order) {
        const auto ok = opKeyOf.find(k);
        const bool ours = ok != opKeyOf.end() && take.contains(ok->second) && (r.renamed.contains(ok->second) || !now.find(k) || changed.contains(k) || added.contains(k));
        r.keys.push_back(k);
        r.fromOp.push_back(ours);
        r.at.push_back(ours ? *after.find(ok->second) : *now.find(k));
    }
    return r;
}

/// The merged polygons: the current list's points kept (its sharing too), the op's entries'
/// corners on points of their own where the current list has none at the same id and place;
/// points left unused that the current list used dropped.
map::Polygons buildPolygons(const map::Polygons& now, const map::Polygons& opList, const MergeResult& r)
{
    map::Polygons out;
    out.version = now.version;
    out.points = now.points;
    std::uint32_t nextPoint = 1;
    for (const auto& p : now.points) nextPoint = std::max(nextPoint, p.id + 1);
    std::map<std::uint32_t, std::uint32_t> pointOf; // (the op's point id -> ours)
    for (std::size_t i = 0; i < r.keys.size(); ++i) {
        if (!r.fromOp[i]) {
            out.polygons.push_back(now.polygons[r.at[i]]);
            continue;
        }
        map::Polygon poly = opList.polygons[r.at[i]];
        for (auto& id : poly.points) {
            if (const auto it = pointOf.find(id); it != pointOf.end()) {
                id = it->second;
                continue;
            }
            const auto src = std::ranges::find(opList.points, id, &map::PolygonPoint::id);
            const float x = src == opList.points.end() ? 0.0f : src->x;
            const float y = src == opList.points.end() ? 0.0f : src->y;
            const auto same = std::ranges::find_if(now.points, [&](const map::PolygonPoint& p) { return p.id == id && p.x == x && p.y == y; });
            if (same != now.points.end()) {
                pointOf[id] = id;
                continue;
            }
            out.points.push_back({nextPoint, x, y});
            pointOf[id] = nextPoint;
            id = nextPoint++;
        }
        out.polygons.push_back(std::move(poly));
    }
    // Points nobody uses now that some polygon used before go (as the editor's own ops drop them).
    std::set<std::uint32_t> usedNow, usedBefore;
    for (const auto& p : out.polygons) usedNow.insert(p.points.begin(), p.points.end());
    for (const auto& p : now.polygons) usedBefore.insert(p.points.begin(), p.points.end());
    std::erase_if(out.points, [&](const map::PolygonPoint& p) { return !usedNow.contains(p.id) && usedBefore.contains(p.id); });
    return out;
}

} // namespace

// --- Moves ---------------------------------------------------------------------------------------

void IdMoves::merge(const IdMoves& later)
{
    for (const auto& [a, b] : later.objects) objects[a] = b;
    for (const auto& [a, b] : later.extents) extents[a] = b;
    for (const auto& [a, b] : later.waypoints) waypoints[a] = b;
    for (const auto& [a, b] : later.polygonKeys) polygonKeys[a] = b;
    for (const auto& [a, b] : later.groupKeys) groupKeys[a] = b;
    for (const auto& [a, b] : later.groupIds) groupIds[a] = b;
}

void remapIds(Op& op, const IdMoves& m)
{
    if (m.empty()) {
        return;
    }
    // What the op itself adds keeps its ids.
    std::set<ObjId> ownObjects;
    std::set<std::uint32_t> ownWaypoints;
    for (const auto& part : op.parts) {
        if (const auto* a = std::get_if<AddObjects>(&part)) {
            for (const auto& e : a->objects) ownObjects.insert(e.id);
        } else if (const auto* s = std::get_if<SetWaypoints>(&part)) {
            for (const auto& c : s->waypoints) {
                if (c.fresh) ownWaypoints.insert(c.id);
            }
        }
    }
    std::map<ObjId, ObjId> objects;
    for (const auto& [a, b] : m.objects) {
        if (!ownObjects.contains(a)) objects[a] = b;
    }
    std::map<std::uint32_t, std::uint32_t> waypoints;
    for (const auto& [a, b] : m.waypoints) {
        if (!ownWaypoints.contains(a)) waypoints[a] = b;
    }
    IdMoves mm = m;
    mm.waypoints = waypoints;
    std::map<std::uint32_t, std::uint32_t> extents;
    for (const auto& [id, e] : m.extents) extents[e.first] = e.second;
    const auto obj = [&](ObjId& id) { remapIn(id, objects); };
    for (auto& part : op.parts) {
        std::visit(Overloaded{
                       [&](AddObjects& p) {
                           for (auto& e : p.objects) {
                               if (e.object.parent != 0 && !ownObjects.contains(e.object.parent)) obj(e.object.parent);
                           }
                       },
                       [&](RemoveObjects& p) {
                           for (auto& id : p.ids) obj(id);
                       },
                       [&](SetObjects& p) {
                           for (auto& e : p.objects) {
                               obj(e.id);
                               obj(e.object.parent);
                               if (const auto it = m.extents.find(e.id); it != m.extents.end() && e.object.header.extent == it->second.first) {
                                   e.object.header.extent = it->second.second;
                               }
                           }
                       },
                       [&](MoveObjects& p) {
                           for (auto& mv : p.moves) obj(mv.id);
                       },
                       [&](SetWaypoints& p) {
                           for (auto& c : p.waypoints) {
                               remapIn(c.id, waypoints);
                               if (c.waypoint) {
                                   c.waypoint->id = c.id;
                                   for (auto& l : c.waypoint->links) remapIn(l.target, waypoints);
                               }
                           }
                       },
                       [&](SetPolygons& p) {
                           for (auto& k : p.keys) remapIn(k, m.polygonKeys);
                           for (auto& k : p.beforeKeys) remapIn(k, m.polygonKeys);
                       },
                       [&](SetGroups& p) {
                           for (auto& k : p.keys) remapIn(k, m.groupKeys);
                           for (auto& k : p.beforeKeys) remapIn(k, m.groupKeys);
                           for (auto& g : p.groups.groups) remapGroup(g, mm, extents);
                           if (p.before) {
                               for (auto& g : p.before->groups) remapGroup(g, mm, extents);
                           }
                       },
                       [](auto&) {},
                   },
                   part);
    }
}

IdMoves movesBetween(const Op& sent, const Op& committed)
{
    IdMoves m;
    if (sent.parts.size() != committed.parts.size()) {
        return m;
    }
    for (std::size_t i = 0; i < sent.parts.size(); ++i) {
        const auto* a = std::get_if<AddObjects>(&sent.parts[i]);
        const auto* b = std::get_if<AddObjects>(&committed.parts[i]);
        if (a && b && a->objects.size() == b->objects.size()) {
            for (std::size_t j = 0; j < a->objects.size(); ++j) {
                const auto& x = a->objects[j];
                const auto& y = b->objects[j];
                if (x.id != y.id) m.objects[x.id] = y.id;
                if (x.object.parent == 0 && x.object.header.extent != y.object.header.extent) {
                    m.extents[y.id] = {x.object.header.extent, y.object.header.extent};
                }
            }
        }
        const auto* sa = std::get_if<SetWaypoints>(&sent.parts[i]);
        const auto* sb = std::get_if<SetWaypoints>(&committed.parts[i]);
        if (sa && sb && sa->waypoints.size() == sb->waypoints.size()) {
            for (std::size_t j = 0; j < sa->waypoints.size(); ++j) {
                if (sa->waypoints[j].fresh && sa->waypoints[j].id != sb->waypoints[j].id) {
                    m.waypoints[sa->waypoints[j].id] = sb->waypoints[j].id;
                }
            }
        }
    }
    return m;
}

// --- Notes ---------------------------------------------------------------------------------------

void annotateForHost(Op& op, const Op& inverse)
{
    const std::size_t n = op.parts.size();
    if (inverse.parts.size() < n) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const OpPart& inv = inverse.parts[n - 1 - i];
        if (auto* s = std::get_if<SetWaypoints>(&op.parts[i])) {
            const auto* u = std::get_if<SetWaypoints>(&inv);
            if (!u || u->waypoints.size() != s->waypoints.size()) continue;
            const std::size_t m = s->waypoints.size();
            for (std::size_t j = 0; j < m; ++j) {
                s->waypoints[j].fresh = s->waypoints[j].waypoint.has_value() && !u->waypoints[m - 1 - j].waypoint.has_value();
            }
        } else if (auto* p = std::get_if<SetPolygons>(&op.parts[i])) {
            if (const auto* u = std::get_if<SetPolygons>(&inv)) {
                p->before = u->polygons;
                p->beforeKeys = u->keys;
            }
        } else if (auto* g = std::get_if<SetGroups>(&op.parts[i])) {
            if (const auto* u = std::get_if<SetGroups>(&inv)) {
                g->before = u->groups;
                g->beforeKeys = u->keys;
            }
        }
    }
}

Op withoutNotes(Op op)
{
    for (auto& part : op.parts) {
        if (auto* s = std::get_if<SetWaypoints>(&part)) {
            for (auto& c : s->waypoints) c.fresh = false;
        } else if (auto* p = std::get_if<SetPolygons>(&part)) {
            p->before.reset();
            p->beforeKeys.clear();
        } else if (auto* g = std::get_if<SetGroups>(&part)) {
            g->before.reset();
            g->beforeKeys.clear();
        }
    }
    return op;
}

// --- Per-entry merges ----------------------------------------------------------------------------

namespace {

std::size_t mergeParts(map::Polygons& nowPolys, std::vector<std::uint32_t>& nowPolyKeys, std::uint32_t nextPolyKey, map::GroupData& nowGroups,
                       std::vector<std::uint32_t>& nowGroupKeys, std::uint32_t nextGroupKey, Op& op, MergeRule rule, IdMoves* moves,
                       std::vector<UndoSkip>* skips)
{
    std::size_t skipped = 0;
    for (auto& part : op.parts) {
        if (auto* p = std::get_if<SetPolygons>(&part)) {
            if (!p->before) {
                nowPolys = p->polygons;
                nowPolyKeys = p->keys.empty() ? nowPolyKeys : p->keys;
                continue;
            }
            const auto before = polyEntries(*p->before, p->beforeKeys);
            const auto after = polyEntries(p->polygons, p->keys);
            const auto now = polyEntries(nowPolys, nowPolyKeys);
            const bool unchanged = sameSection(*p->before, nowPolys) && p->beforeKeys == nowPolyKeys;
            if (!unchanged && before.valid() && after.valid() && now.valid()) {
                const MergeResult r = mergeKeyed(before, after, now, rule, nextPolyKey);
                p->polygons = buildPolygons(nowPolys, p->polygons, r);
                p->keys = r.keys;
                skipped += r.skipped.size();
                if (skips) {
                    for (const auto k : r.skipped) skips->push_back({ElementKey::polygon(k), "changed since"});
                }
                if (moves) {
                    for (const auto& [a, b] : r.renamed) moves->polygonKeys[a] = b;
                }
            }
            p->before.reset();
            p->beforeKeys.clear();
            nowPolys = p->polygons;
            nowPolyKeys = p->keys;
        } else if (auto* g = std::get_if<SetGroups>(&part)) {
            if (!g->before) {
                nowGroups = g->groups;
                nowGroupKeys = g->keys.empty() ? nowGroupKeys : g->keys;
                continue;
            }
            const auto before = groupEntries(*g->before, g->beforeKeys);
            const auto after = groupEntries(g->groups, g->keys);
            const auto now = groupEntries(nowGroups, nowGroupKeys);
            const bool unchanged = sameSection(*g->before, nowGroups) && g->beforeKeys == nowGroupKeys;
            if (!unchanged && before.valid() && after.valid() && now.valid()) {
                const MergeResult r = mergeKeyed(before, after, now, rule, nextGroupKey);
                map::GroupData out;
                out.version = nowGroups.version;
                // New groups' ids clear of the ids others' groups took meanwhile.
                std::set<std::int32_t> idsNow;
                std::int32_t most = 0;
                for (const auto& x : nowGroups.groups) {
                    idsNow.insert(x.id);
                    most = std::max(most, x.id);
                }
                for (const auto& x : g->groups.groups) most = std::max(most, x.id);
                std::set<std::int32_t> idsBefore;
                for (const auto& x : g->before->groups) idsBefore.insert(x.id);
                for (std::size_t i = 0; i < r.keys.size(); ++i) {
                    if (!r.fromOp[i]) {
                        out.groups.push_back(nowGroups.groups[r.at[i]]);
                        continue;
                    }
                    map::Group grp = g->groups.groups[r.at[i]];
                    const bool isNew = !before.find(g->keys[r.at[i]]);
                    if (isNew && idsNow.contains(grp.id) && !idsBefore.contains(grp.id)) {
                        const std::int32_t was = grp.id;
                        grp.id = ++most;
                        if (moves) moves->groupIds[was] = grp.id;
                    }
                    out.groups.push_back(std::move(grp));
                }
                g->groups = std::move(out);
                g->keys = r.keys;
                skipped += r.skipped.size();
                if (skips) {
                    for (const auto k : r.skipped) skips->push_back({ElementKey::group(k), "changed since"});
                }
                if (moves) {
                    for (const auto& [a, b] : r.renamed) moves->groupKeys[a] = b;
                }
            }
            g->before.reset();
            g->beforeKeys.clear();
            nowGroups = g->groups;
            nowGroupKeys = g->keys;
        }
    }
    return skipped;
}

} // namespace

std::size_t mergeConcurrent(const MapDocument& doc, Op& op, MergeRule rule, IdMoves* moves)
{
    map::Polygons polys = doc.polygons;
    std::vector<std::uint32_t> polyKeys = doc.polygonKeys;
    map::GroupData groups = doc.groups;
    std::vector<std::uint32_t> groupKeys = doc.groupKeys;
    return mergeParts(polys, polyKeys, doc.nextPolygonKey, groups, groupKeys, doc.nextGroupKey, op, rule, moves, nullptr);
}

// --- Selective undo ------------------------------------------------------------------------------

Op selectiveOp(const MapDocument& doc, const Op& apply, const Op& expect, std::vector<UndoSkip>* skipped)
{
    const auto skip = [&](ElementKey key, std::string why) {
        if (skipped) skipped->push_back({std::move(key), std::move(why)});
    };
    // What `expect` left: the values the elements should still have.
    std::map<Cell, std::optional<WallState>> walls;
    std::map<std::pair<int, int>, std::optional<map::FloorTile>> tiles;
    std::map<ObjId, std::optional<DocObject>> objects;
    std::map<std::uint32_t, std::optional<map::Waypoint>> waypoints;
    std::optional<map::MapInfo> info;
    std::optional<map::AmbientData> ambient;
    std::optional<map::MapIntro> intro;
    std::optional<map::DebugData> debug;
    std::optional<map::ScriptObject> script;
    std::optional<std::optional<std::string>> source;
    std::optional<std::pair<map::Polygons, std::vector<std::uint32_t>>> polygons;
    std::optional<std::pair<map::GroupData, std::vector<std::uint32_t>>> groups;
    for (const auto& part : expect.parts) {
        std::visit(Overloaded{
                       [&](const SetWalls& p) {
                           for (const auto& c : p.cells) {
                               std::optional<WallState> s = c.state;
                               if (s) {
                                   // (as applying left it: the wall and its secret entry at the cell)
                                   s->wall.x = c.cell.x;
                                   s->wall.y = c.cell.y;
                                   if (s->secret) s->secret->pos = {c.cell.x, c.cell.y};
                               }
                               walls[c.cell] = std::move(s);
                           }
                       },
                       [&](const SetTiles& p) {
                           for (const auto& t : p.tiles) tiles[{t.gx, t.gy}] = t.tile;
                       },
                       [&](const AddObjects& p) {
                           for (const auto& e : p.objects) objects[e.id] = e.object;
                       },
                       [&](const RemoveObjects& p) {
                           for (const auto id : p.ids) objects[id] = std::nullopt;
                       },
                       [&](const SetObjects& p) {
                           for (const auto& e : p.objects) objects[e.id] = e.object;
                       },
                       [&](const MoveObjects& p) {
                           for (const auto& m : p.moves) {
                               auto& o = objects[m.id];
                               if (!o) {
                                   const auto it = doc.objects.find(m.id);
                                   if (it == doc.objects.end()) continue;
                                   o = it->second; // (only its place is expected: the rest as it is)
                               }
                               o->header.x = m.x;
                               o->header.y = m.y;
                           }
                       },
                       [&](const SetWaypoints& p) {
                           for (const auto& c : p.waypoints) {
                               std::optional<map::Waypoint> w = c.waypoint;
                               if (w) w->id = c.id;
                               waypoints[c.id] = std::move(w);
                           }
                       },
                       [&](const SetMapInfo& p) { info = p.info; },
                       [&](const SetAmbient& p) { ambient = p.ambient; },
                       [&](const SetIntro& p) { intro = p.intro; },
                       [&](const SetDebug& p) { debug = p.debug; },
                       [&](const SetScript& p) { script = p.script; },
                       [&](const SetScriptSource& p) { source = p.source; },
                       [&](const SetPolygons& p) { polygons = std::pair{p.polygons, p.keys.empty() ? doc.polygonKeys : p.keys}; },
                       [&](const SetGroups& p) { groups = std::pair{p.groups, p.keys.empty() ? doc.groupKeys : p.keys}; },
                       [](const SetSections&) {},
                   },
                   part);
    }
    // Others' later work that a removal would take with it.
    const auto removalBlocked = [&](ObjId id) -> std::optional<std::string> {
        std::vector<ObjId> todo = doc.childrenOf(id);
        while (!todo.empty()) {
            const ObjId c = todo.back();
            todo.pop_back();
            const auto e = objects.find(c);
            if (e == objects.end() || !e->second || !(*e->second == doc.objects.at(c))) {
                return std::string("has items put in it since");
            }
            const auto more = doc.childrenOf(c);
            todo.insert(todo.end(), more.begin(), more.end());
        }
        const auto it = doc.objects.find(id);
        if (it != doc.objects.end() && it->second.parent == 0 && it->second.header.extent != 0) {
            const auto extent = static_cast<std::int32_t>(it->second.header.extent);
            for (std::size_t i = 0; i < doc.groups.groups.size(); ++i) {
                const auto& g = doc.groups.groups[i];
                if (g.kind != map::Group::Kind::Objects || std::ranges::find(g.members, extent) == g.members.end()) continue;
                bool expected = false;
                if (groups && i < doc.groupKeys.size()) {
                    const auto k = std::ranges::find(groups->second, doc.groupKeys[i]);
                    expected = k != groups->second.end() && groups->first.groups[static_cast<std::size_t>(k - groups->second.begin())] == g;
                }
                if (!expected) return "is in group " + g.name + " since";
            }
        }
        return std::nullopt;
    };
    Op out;
    out.label = apply.label;
    for (const auto& part : apply.parts) {
        std::visit(Overloaded{
                       [&](const SetWalls& p) {
                           SetWalls kept;
                           for (const auto& c : p.cells) {
                               const auto e = walls.find(c.cell);
                               if (e != walls.end() && wallState(doc, c.cell) != e->second) {
                                   skip(ElementKey::wall(c.cell), "changed since");
                                   continue;
                               }
                               kept.cells.push_back(c);
                           }
                           if (!kept.cells.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const SetTiles& p) {
                           SetTiles kept;
                           for (const auto& t : p.tiles) {
                               const auto e = tiles.find({t.gx, t.gy});
                               const map::FloorTile* now = doc.floor.tile(t.gx, t.gy);
                               const std::optional<map::FloorTile> cur = now ? std::optional(*now) : std::nullopt;
                               if (e != tiles.end() && cur != e->second) {
                                   skip(ElementKey::tile({t.gx, t.gy}), "changed since");
                                   continue;
                               }
                               kept.tiles.push_back(t);
                           }
                           if (!kept.tiles.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const RemoveObjects& p) {
                           RemoveObjects kept;
                           for (const ObjId id : p.ids) {
                               const auto now = doc.objects.find(id);
                               if (now == doc.objects.end()) {
                                   skip(ElementKey::object(id), "deleted since");
                                   continue;
                               }
                               const auto e = objects.find(id);
                               if (e != objects.end() && e->second && !(*e->second == now->second)) {
                                   skip(ElementKey::object(id), "changed since");
                                   continue;
                               }
                               if (const auto why = removalBlocked(id)) {
                                   skip(ElementKey::object(id), *why);
                                   continue;
                               }
                               kept.ids.push_back(id);
                           }
                           if (!kept.ids.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const AddObjects& p) {
                           AddObjects kept;
                           std::set<ObjId> adding;
                           std::set<std::uint32_t> extents;
                           for (const auto& [id, o] : doc.objects) {
                               if (o.parent == 0 && o.header.extent != 0) extents.insert(o.header.extent);
                           }
                           for (const auto& e : p.objects) {
                               const ElementKey what = ElementKey::object(e.id);
                               if (doc.objects.contains(e.id)) {
                                   if (e.object.parent == 0) skip(what, "made again since");
                                   continue;
                               }
                               if (e.object.parent != 0 && !doc.objects.contains(e.object.parent) && !adding.contains(e.object.parent)) {
                                   if (!objects.contains(e.object.parent)) skip(what, "its owner was deleted since");
                                   continue; // (an item of a root left out goes with it)
                               }
                               if (e.object.parent == 0 && e.object.header.extent != 0 && extents.contains(e.object.header.extent)) {
                                   skip(what, "its extent is another object's now");
                                   continue;
                               }
                               adding.insert(e.id);
                               kept.objects.push_back(e);
                           }
                           if (!kept.objects.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const SetObjects& p) {
                           SetObjects kept;
                           for (const auto& e : p.objects) {
                               const auto now = doc.objects.find(e.id);
                               if (now == doc.objects.end()) {
                                   skip(ElementKey::object(e.id), "deleted since");
                                   continue;
                               }
                               const auto x = objects.find(e.id);
                               if (x != objects.end() && x->second && !(*x->second == now->second)) {
                                   skip(ElementKey::object(e.id), "changed since");
                                   continue;
                               }
                               if (e.object.parent != 0 && !doc.objects.contains(e.object.parent)) {
                                   skip(ElementKey::object(e.id), "its owner was deleted since");
                                   continue;
                               }
                               kept.objects.push_back(e);
                           }
                           if (!kept.objects.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const MoveObjects& p) {
                           MoveObjects kept;
                           for (const auto& m : p.moves) {
                               const auto now = doc.objects.find(m.id);
                               if (now == doc.objects.end()) {
                                   skip(ElementKey::object(m.id), "deleted since");
                                   continue;
                               }
                               const auto x = objects.find(m.id);
                               if (x != objects.end() && x->second &&
                                   (x->second->header.x != now->second.header.x || x->second->header.y != now->second.header.y)) {
                                   skip(ElementKey::object(m.id), "moved since");
                                   continue;
                               }
                               kept.moves.push_back(m);
                           }
                           if (!kept.moves.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const SetWaypoints& p) {
                           SetWaypoints kept;
                           std::set<std::uint32_t> inPart;
                           for (const auto& c : p.waypoints) inPart.insert(c.id);
                           for (const auto& c : p.waypoints) {
                               const ElementKey what = ElementKey::waypoint(c.id);
                               const auto now = std::ranges::find(doc.waypoints.waypoints, c.id, &map::Waypoint::id);
                               const std::optional<map::Waypoint> cur = now == doc.waypoints.waypoints.end() ? std::nullopt : std::optional(*now);
                               const auto e = waypoints.find(c.id);
                               if (e != waypoints.end() && cur != e->second) {
                                   skip(what, cur ? "changed since" : "deleted since");
                                   continue;
                               }
                               if (!c.waypoint && cur) {
                                   // (Removing it: nobody else's waypoint may link to it, nor a group name it.)
                                   const auto linked = std::ranges::find_if(doc.waypoints.waypoints, [&](const map::Waypoint& w) {
                                       return !inPart.contains(w.id) && std::ranges::any_of(w.links, [&](const auto& l) { return l.target == c.id; });
                                   });
                                   if (linked != doc.waypoints.waypoints.end()) {
                                       skip(what, "waypoint " + std::to_string(linked->id) + " links to it since");
                                       continue;
                                   }
                               }
                               if (c.waypoint) {
                                   // (Its links must name waypoints still there.)
                                   const bool dangling = std::ranges::any_of(c.waypoint->links, [&](const auto& l) {
                                       return !inPart.contains(l.target) &&
                                              std::ranges::find(doc.waypoints.waypoints, l.target, &map::Waypoint::id) == doc.waypoints.waypoints.end();
                                   });
                                   if (dangling) {
                                       skip(what, "a waypoint it links to was deleted since");
                                       continue;
                                   }
                               }
                               kept.waypoints.push_back(c);
                           }
                           if (!kept.waypoints.empty()) out.parts.push_back(std::move(kept));
                       },
                       [&](const SetMapInfo& p) {
                           if (info && !sameSection(*info, doc.info)) return skip(ElementKey::mapInfo(), "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetAmbient& p) {
                           if (ambient && !sameSection(*ambient, doc.ambient)) return skip(ElementKey::mapInfo(), "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetIntro& p) {
                           if (intro && !sameSection(*intro, doc.intro)) return skip(ElementKey::mapInfo(), "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetDebug& p) {
                           if (debug && !sameSection(*debug, doc.debugData)) return skip(ElementKey::mapInfo(), "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetScript& p) {
                           if (script && !sameSection(*script, doc.script)) return skip(ElementKey{ElementKey::Kind::ScriptFunction, 0, 0, "*"}, "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetScriptSource& p) {
                           if (source && *source != doc.scriptSource) return skip(ElementKey{ElementKey::Kind::ScriptFunction, 0, 0, "*"}, "changed since");
                           out.parts.push_back(p);
                       },
                       [&](const SetPolygons& p) {
                           SetPolygons q = p;
                           if (polygons) {
                               q.before = polygons->first;
                               q.beforeKeys = polygons->second;
                           }
                           out.parts.push_back(std::move(q));
                       },
                       [&](const SetGroups& p) {
                           SetGroups q = p;
                           if (groups) {
                               q.before = groups->first;
                               q.beforeKeys = groups->second;
                           }
                           out.parts.push_back(std::move(q));
                       },
                       // (The section list stays: another editor's content may use a section the
                       // op added. An empty section is written and read like any.)
                       [](const SetSections&) {},
                   },
                   part);
    }
    // The keyed lists per entry, leaving what others changed since.
    {
        map::Polygons polys = doc.polygons;
        std::vector<std::uint32_t> polyKeys = doc.polygonKeys;
        map::GroupData grps = doc.groups;
        std::vector<std::uint32_t> grpKeys = doc.groupKeys;
        mergeParts(polys, polyKeys, doc.nextPolygonKey, grps, grpKeys, doc.nextGroupKey, out, MergeRule::Theirs, nullptr, skipped);
    }
    // Keyed sets that change nothing any more (everything left alone) go.
    std::erase_if(out.parts, [&](const OpPart& part) {
        if (const auto* p = std::get_if<SetPolygons>(&part)) return sameSection(p->polygons, doc.polygons) && p->keys == doc.polygonKeys;
        if (const auto* g = std::get_if<SetGroups>(&part)) return sameSection(g->groups, doc.groups) && g->keys == doc.groupKeys;
        return false;
    });
    return out;
}

// --- The compact snapshot ------------------------------------------------------------------------

std::optional<Op> diffOp(const MapDocument& from, const MapDocument& to)
{
    Op op;
    op.label = "Snapshot";
    // Walls, with the secret, breakable and window lists in `to`'s order.
    {
        std::set<Cell> cells;
        for (const auto& [c, w] : from.walls) cells.insert(c);
        for (const auto& [c, w] : to.walls) cells.insert(c);
        std::vector<Cell> differ;
        for (const Cell c : cells) {
            if (wallState(from, c) != wallState(to, c)) differ.push_back(c);
        }
        const bool listsSame = from.secretWalls.walls == to.secretWalls.walls && from.destructableWalls.walls == to.destructableWalls.walls &&
                               from.windowWalls.walls == to.windowWalls.walls;
        SetWalls part;
        if (listsSame) {
            for (const Cell c : differ) {
                WallChange ch{c, wallState(to, c)};
                const auto pos = [&](const auto& list, auto at) -> std::int32_t {
                    const auto it = std::ranges::find_if(list, at);
                    return it == list.end() ? -1 : static_cast<std::int32_t>(it - list.begin());
                };
                const auto here = [&](const map::GridPoint& g) { return g.x == c.x && g.y == c.y; };
                ch.secretAt = pos(to.secretWalls.walls, [&](const map::SecretWall& s) { return here(s.pos); });
                ch.breakableAt = pos(to.destructableWalls.walls, here);
                ch.windowAt = pos(to.windowWalls.walls, here);
                part.cells.push_back(std::move(ch));
            }
        } else {
            // Every cell on a list cleared (the lists are then empty), then set again in `to`'s
            // order: each entry at the count of its list's entries before it already back.
            std::set<Cell> clear(differ.begin(), differ.end());
            const auto cellOf = [](const map::GridPoint& g) { return Cell{static_cast<std::uint8_t>(g.x), static_cast<std::uint8_t>(g.y)}; };
            for (const auto& s : from.secretWalls.walls) clear.insert(cellOf(s.pos));
            for (const auto& g : from.destructableWalls.walls) clear.insert(cellOf(g));
            for (const auto& g : from.windowWalls.walls) clear.insert(cellOf(g));
            std::set<Cell> set(differ.begin(), differ.end());
            for (const auto& s : to.secretWalls.walls) set.insert(cellOf(s.pos));
            for (const auto& g : to.destructableWalls.walls) set.insert(cellOf(g));
            for (const auto& g : to.windowWalls.walls) set.insert(cellOf(g));
            for (const Cell c : clear) {
                if (from.walls.contains(c)) part.cells.push_back(WallChange{c, std::nullopt});
            }
            std::vector<Cell> secretOrder, breakOrder, windowOrder;
            for (const auto& s : to.secretWalls.walls) secretOrder.push_back(cellOf(s.pos));
            for (const auto& g : to.destructableWalls.walls) breakOrder.push_back(cellOf(g));
            for (const auto& g : to.windowWalls.walls) windowOrder.push_back(cellOf(g));
            std::set<Cell> back;
            const auto at = [&](const std::vector<Cell>& order, Cell c) -> std::int32_t {
                std::int32_t n = 0;
                for (const Cell o : order) {
                    if (o == c) return n;
                    if (back.contains(o)) ++n;
                }
                return -1;
            };
            for (const Cell c : set) {
                const auto st = wallState(to, c);
                if (!st) continue;
                WallChange ch{c, st};
                ch.secretAt = at(secretOrder, c);
                ch.breakableAt = at(breakOrder, c);
                ch.windowAt = at(windowOrder, c);
                part.cells.push_back(std::move(ch));
                back.insert(c);
            }
        }
        if (!part.cells.empty()) op.parts.push_back(std::move(part));
    }
    // Floor tiles.
    {
        SetTiles part;
        for (int y = 0; y < FloorGrid::kSize; ++y) {
            for (int x = 0; x < FloorGrid::kSize; ++x) {
                const FloorPair& a = from.floor.pair(x, y);
                const FloorPair& b = to.floor.pair(x, y);
                if (a.odd != b.odd) part.tiles.push_back({static_cast<std::int16_t>(2 * x + 1), static_cast<std::int16_t>(2 * y - 1), b.odd});
                if (a.even != b.even) part.tiles.push_back({static_cast<std::int16_t>(2 * x), static_cast<std::int16_t>(2 * y), b.even});
            }
        }
        if (!part.tiles.empty()) op.parts.push_back(std::move(part));
    }
    // Objects: the new ones (owners first), the changed ones, then the gone ones' roots.
    {
        const auto depth = [&](ObjId id) {
            int d = 0;
            for (ObjId at = id; d < 64; ++d) {
                const auto it = to.objects.find(at);
                if (it == to.objects.end() || it->second.parent == 0) break;
                at = it->second.parent;
            }
            return d;
        };
        std::vector<std::pair<int, ObjId>> fresh;
        SetObjects set;
        for (const auto& [id, o] : to.objects) {
            const auto it = from.objects.find(id);
            if (it == from.objects.end()) {
                fresh.emplace_back(depth(id), id);
            } else if (!(it->second == o)) {
                set.objects.push_back({id, o});
            }
        }
        std::ranges::sort(fresh);
        AddObjects add;
        for (const auto& [d, id] : fresh) add.objects.push_back({id, to.objects.at(id)});
        RemoveObjects remove;
        for (const auto& [id, o] : from.objects) {
            if (to.objects.contains(id)) continue;
            if (o.parent == 0 || to.objects.contains(o.parent)) {
                // (A root of what goes: its owner stays, or it had none.)
                remove.ids.push_back(id);
            } else if (!from.objects.contains(o.parent)) {
                return std::nullopt;
            }
        }
        if (!add.objects.empty()) op.parts.push_back(std::move(add));
        if (!set.objects.empty()) op.parts.push_back(std::move(set));
        if (!remove.ids.empty()) op.parts.push_back(std::move(remove));
    }
    // Waypoints, in `to`'s order.
    if (from.waypoints.waypoints != to.waypoints.waypoints) {
        SetWaypoints part;
        for (const auto& w : from.waypoints.waypoints) {
            if (std::ranges::find(to.waypoints.waypoints, w.id, &map::Waypoint::id) == to.waypoints.waypoints.end()) {
                part.waypoints.push_back({w.id, std::nullopt});
            }
        }
        for (std::size_t i = 0; i < to.waypoints.waypoints.size(); ++i) {
            const auto& w = to.waypoints.waypoints[i];
            part.waypoints.push_back({w.id, w, static_cast<std::int32_t>(i)});
        }
        op.parts.push_back(std::move(part));
    }
    if (!sameSection(from.info, to.info)) op.parts.push_back(SetMapInfo{to.info});
    if (!sameSection(from.ambient, to.ambient)) op.parts.push_back(SetAmbient{to.ambient});
    if (!sameSection(from.intro, to.intro)) op.parts.push_back(SetIntro{to.intro});
    if (!sameSection(from.debugData, to.debugData)) op.parts.push_back(SetDebug{to.debugData});
    if (!sameSection(from.polygons, to.polygons) || from.polygonKeys != to.polygonKeys) {
        if (to.polygonKeys.size() != to.polygons.polygons.size()) return std::nullopt;
        op.parts.push_back(SetPolygons{to.polygons, to.polygonKeys});
    }
    if (!sameSection(from.groups, to.groups) || from.groupKeys != to.groupKeys) {
        if (to.groupKeys.size() != to.groups.groups.size()) return std::nullopt;
        op.parts.push_back(SetGroups{to.groups, to.groupKeys});
    }
    if (!sameSection(from.script, to.script)) op.parts.push_back(SetScript{to.script});
    if (from.scriptSource != to.scriptSource) op.parts.push_back(SetScriptSource{to.scriptSource});
    op.parts.push_back(SetSections{to.sections}); // (last: the parts before may have listed sections)
    return op;
}

bool sameSessionState(const MapDocument& a, const MapDocument& b, const things::ThingDb& things)
{
    if (a.objects != b.objects || a.nextObjId != b.nextObjId || a.polygonKeys != b.polygonKeys || a.groupKeys != b.groupKeys ||
        a.nextPolygonKey != b.nextPolygonKey || a.nextGroupKey != b.nextGroupKey || a.scriptSource != b.scriptSource || a.sections != b.sections) {
        return false;
    }
    return writeMapBytes(a, things) == writeMapBytes(b, things);
}

} // namespace nox::mapedit
