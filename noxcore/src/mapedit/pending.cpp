// A session's pending edits and how a view draws them (pending.hpp).
#include "noxcore/mapedit/pending.hpp"

#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <tuple>

namespace nox::mapedit {

bool clientLoadedType(const things::ThingDef& t)
{
    namespace oc = things::ObjectClass;
    return (things::thingClasses(t) & (oc::ClientPersist | oc::Immobile)) != 0;
}

namespace {

/// Whether `id`'s top-level owner in `doc` is a client-loaded thing (false when `doc` hasn't it).
bool clientLoadedAt(const MapDocument& doc, ObjId id, const things::ThingDb& things)
{
    for (int depth = 0; depth < 64; ++depth) {
        const auto it = doc.objects.find(id);
        if (it == doc.objects.end()) {
            return false;
        }
        if (it->second.parent == 0) {
            const things::ThingDef* t = things.findThing(it->second.type);
            return t && clientLoadedType(*t);
        }
        id = it->second.parent;
    }
    return false;
}

} // namespace

bool heldForPublish(const MapDocument& published, const MapDocument& working, ObjId id, const things::ThingDb& things)
{
    return clientLoadedAt(published, id, things) || clientLoadedAt(working, id, things);
}

std::map<ObjId, DocObject> playedObjects(const MapDocument& published, const MapDocument& working, const things::ThingDb& things)
{
    // Each object once: the working copy's when it goes live, the published copy's when it's held.
    std::map<ObjId, DocObject> out;
    for (const auto& [id, o] : working.objects) {
        if (!heldForPublish(published, working, id, things)) out.emplace(id, o);
    }
    for (const auto& [id, o] : published.objects) {
        if (heldForPublish(published, working, id, things)) out.emplace(id, o);
    }
    return out;
}

const char* pendingViewName(PendingView view)
{
    switch (view) {
    case PendingView::Shaded: return "Shaded";
    case PendingView::AsFinal: return "As final";
    case PendingView::Hidden: return "Hidden";
    }
    return "Shaded";
}

std::optional<PendingView> parsePendingView(std::string_view text)
{
    std::string t;
    for (const char c : text) {
        if (c != ' ' && c != '_' && c != '-') t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (t == "shaded" || t == "0") return PendingView::Shaded;
    if (t == "final" || t == "asfinal" || t == "1") return PendingView::AsFinal;
    if (t == "hidden" || t == "published" || t == "2") return PendingView::Hidden;
    return std::nullopt;
}

namespace {

/// What of a wall cell is drawn or tells in the game: the wall's material, facing and variant and
/// its window, secret and breakable entries (not its seen byte or minimap group, which the game
/// works out itself).
bool sameWall(const std::optional<WallState>& a, const std::optional<WallState>& b)
{
    if (!a || !b) return a.has_value() == b.has_value();
    return a->wall.material == b->wall.material && a->wall.direction == b->wall.direction && a->wall.variant == b->wall.variant &&
           a->window == b->window && a->destructible == b->destructible && a->secret == b->secret;
}

/// A top-level client-loaded thing's type and place, for matching.
using ObjectKey = std::tuple<std::string, float, float>;

std::map<ObjectKey, int> clientLoadedThings(const MapDocument& doc, const things::ThingDb& things)
{
    std::map<ObjectKey, int> out;
    std::map<std::string, bool> loaded; // (by type, looked up once)
    for (const auto& [id, o] : doc.objects) {
        if (o.parent != 0) continue;
        auto [it, fresh] = loaded.try_emplace(o.type, false);
        if (fresh) {
            const things::ThingDef* t = things.findThing(o.type);
            it->second = t && clientLoadedType(*t);
        }
        if (it->second) ++out[{o.type, o.header.x, o.header.y}];
    }
    return out;
}

} // namespace

PendingEdits diffPending(const MapDocument& working, const MapDocument& reference, const things::ThingDb& things)
{
    PendingEdits out;
    // Walls: every cell either copy has (both maps are ordered by row, then column).
    {
        auto a = working.walls.begin();
        auto b = reference.walls.begin();
        while (a != working.walls.end() || b != reference.walls.end()) {
            Cell c;
            if (b == reference.walls.end() || (a != working.walls.end() && a->first < b->first)) {
                c = (a++)->first;
            } else if (a == working.walls.end() || b->first < a->first) {
                c = (b++)->first;
            } else {
                c = a->first;
                ++a;
                ++b;
            }
            std::optional<WallState> w = wallState(working, c);
            std::optional<WallState> r = wallState(reference, c);
            if (!sameWall(w, r)) out.walls.push_back({c, std::move(w), std::move(r)});
        }
    }
    // Floor tiles, edges with them.
    for (int y = 0; y < FloorGrid::kSize; ++y) {
        for (int x = 0; x < FloorGrid::kSize; ++x) {
            const FloorPair& a = working.floor.pair(x, y);
            const FloorPair& b = reference.floor.pair(x, y);
            if (a.odd != b.odd) {
                out.tiles.push_back({{static_cast<std::int16_t>(2 * x + 1), static_cast<std::int16_t>(2 * y - 1)}, a.odd, b.odd});
            }
            if (a.even != b.even) {
                out.tiles.push_back({{static_cast<std::int16_t>(2 * x), static_cast<std::int16_t>(2 * y)}, a.even, b.even});
            }
        }
    }
    // Client-loaded things, as many of each type and place as either copy has more of.
    const auto w = clientLoadedThings(working, things);
    const auto r = clientLoadedThings(reference, things);
    for (const auto& [key, n] : w) {
        const auto it = r.find(key);
        for (int i = it == r.end() ? 0 : it->second; i < n; ++i) {
            out.objects.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), true});
        }
    }
    for (const auto& [key, n] : r) {
        const auto it = w.find(key);
        for (int i = it == w.end() ? 0 : it->second; i < n; ++i) {
            out.objects.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), false});
        }
    }
    return out;
}

PendingPlan planPending(const PendingEdits& edits, PendingBase base, PendingView view)
{
    PendingPlan plan;
    const bool baseWorking = base == PendingBase::Working;
    // Per item: which copy is drawn (working / reference / none) and how. An item the base
    // already draws as it is leaves the plan; anything else hides the base's and draws the plan's.
    struct Pick {
        bool working = false;
        bool reference = false;
        PendingLook look = PendingLook::Normal;
    };
    const auto pick = [&](bool hasWorking, bool hasReference) {
        Pick p;
        switch (view) {
        case PendingView::Shaded:
            if (hasWorking) {
                p.working = true;
                p.look = PendingLook::Pending;
            } else if (hasReference) {
                p.reference = true;
                p.look = PendingLook::Ghost;
            }
            break;
        case PendingView::AsFinal:
            p.working = hasWorking;
            break;
        case PendingView::Hidden:
            p.reference = hasReference;
            break;
        }
        return p;
    };
    // (The base has something there: its copy has the item.)
    for (const auto& pw : edits.walls) {
        const Pick p = pick(pw.working.has_value(), pw.reference.has_value());
        const WallState* drawn = p.working ? &*pw.working : p.reference ? &*pw.reference : nullptr;
        const bool inBase = drawn && (p.working == baseWorking);
        const bool baseHas = baseWorking ? pw.working.has_value() : pw.reference.has_value();
        if (drawn && inBase && p.look == PendingLook::Normal) continue; // (the base's own, as it is)
        if (!inBase && baseHas) plan.hideWalls.push_back(pw.cell);
        if (drawn) {
            PendingPlan::Wall w{drawn->wall, drawn->window, p.look, inBase};
            w.wall.x = pw.cell.x;
            w.wall.y = pw.cell.y;
            plan.walls.push_back(w);
        }
    }
    for (const auto& pt : edits.tiles) {
        const Pick p = pick(pt.working.has_value(), pt.reference.has_value());
        const map::FloorTile* drawn = p.working ? &*pt.working : p.reference ? &*pt.reference : nullptr;
        if (drawn && p.working == baseWorking && p.look == PendingLook::Normal) continue;
        if (baseWorking ? pt.working.has_value() : pt.reference.has_value()) plan.hideTiles.push_back(pt.at);
        if (drawn) plan.tiles.push_back({pt.at, *drawn, p.look});
    }
    for (const auto& po : edits.objects) {
        const Pick p = pick(po.inWorking, !po.inWorking);
        const bool drawn = p.working || p.reference;
        const bool inBase = po.inWorking == baseWorking;
        if (drawn && inBase && p.look == PendingLook::Normal) continue;
        if (inBase) plan.hideObjects.push_back({po.type, po.x, po.y, PendingLook::Normal});
        if (drawn) plan.objects.push_back({po.type, po.x, po.y, p.look});
    }
    return plan;
}

} // namespace nox::mapedit
