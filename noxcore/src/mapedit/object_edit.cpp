// The map editor's object edits (object_edit.hpp).
#include "noxcore/mapedit/object_edit.hpp"

#include "doc_types.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>

namespace nox::mapedit {

namespace {

namespace xf = map::xfer;

Op single(std::string label, OpPart part)
{
    Op op;
    op.label = std::move(label);
    op.parts.push_back(std::move(part));
    return op;
}

/// One link kind: what it names, its names, and whether extent edits rewrite it.
struct LinkKind {
    LinkTarget kind;
    const char* field;
    const char* label; ///< (the panel's label for the source field: properties.cpp)
    bool followsExtents; ///< Set Extent / Reorder Extents rewrite it
};
constexpr LinkKind kPartner{LinkTarget::Extent, "partner", "Leads to", true};
constexpr LinkKind kShaft{LinkTarget::Extent, "shaft", "Shaft", true};
constexpr LinkKind kElevator{LinkTarget::Extent, "elevator", "Elevator", true};
constexpr LinkKind kCarries{LinkTarget::Extent, "carries", "Carries", true};
constexpr LinkKind kLightTarget{LinkTarget::Extent, "light target", "Target", true};
constexpr LinkKind kStartsAt{LinkTarget::Waypoint, "starts at", "Starts at", false};
constexpr LinkKind kHeadingFor{LinkTarget::Waypoint, "heading for", "Heading for", false};
constexpr LinkKind kCameFrom{LinkTarget::Waypoint, "came from", "Came from", false};
constexpr LinkKind kWaypointLink{LinkTarget::Waypoint, "waypoint link", "Link", false};
// (A group's members: objects by extent, waypoints by id. renumberOp rewrites object groups itself.)
constexpr LinkKind kObjectMember{LinkTarget::Extent, "member", "member", false};
constexpr LinkKind kWaypointMember{LinkTarget::Waypoint, "member", "member", false};

/// Calls fn(field, kind) for every link field in an object's data (a DocObject or a const one):
/// the one list of link kinds (Link, extent edits, the links layer).
template <class Obj, class Fn>
void forEachLinkField(Obj& o, Fn&& fn)
{
    if (auto* t = std::get_if<xf::Transporter>(&o.data)) fn(t->targetExtent, kPartner);
    if (auto* e = std::get_if<xf::Elevator>(&o.data)) fn(e->linkedExtent, kShaft);
    if (auto* s = std::get_if<xf::ElevatorShaft>(&o.data)) fn(s->linkedExtent, kElevator);
    if (auto* m = std::get_if<xf::Mover>(&o.data)) {
        fn(m->waypoint, kStartsAt);
        fn(m->movedExtent, kCarries);
        fn(m->waypointStart, kHeadingFor);
        fn(m->waypointEnd, kCameFrom);
    }
    if (auto* l = std::get_if<xf::InvisibleLight>(&o.data)) fn(l->target, kLightTarget);
}

const std::vector<Link>& noLinks()
{
    static const std::vector<Link> none;
    return none;
}

/// Record order: world objects by order key, each followed by its inventory (as the writer lists them).
std::vector<ObjId> recordOrder(const MapDocument& d)
{
    std::vector<ObjId> out;
    const auto walk = [&](auto&& self, ObjId parent) -> void {
        for (const ObjId id : d.childrenOf(parent)) {
            out.push_back(id);
            self(self, id);
        }
    };
    walk(walk, 0);
    return out;
}

/// An op that renames extents (old -> new per object) and rewrites every link and object-group
/// member: `rename` maps each object to its new extent; `follow` maps an old extent to the new
/// one links naming it take.
Op renumberOp(const MapDocument& d, const std::map<ObjId, std::uint32_t>& rename, const std::map<std::uint32_t, std::uint32_t>& follow,
              std::string label)
{
    SetObjects objects;
    for (const auto& [id, o] : d.objects) {
        DocObject c = o;
        bool changed = false;
        if (const auto it = rename.find(id); it != rename.end() && it->second != o.header.extent) {
            c.header.extent = it->second;
            changed = true;
        }
        forEachLinkField(c, [&](std::uint32_t& field, const LinkKind& k) {
            if (field == 0 || k.kind != LinkTarget::Extent || !k.followsExtents) return;
            if (const auto f = follow.find(field); f != follow.end() && f->second != field) {
                field = f->second;
                changed = true;
            }
        });
        if (changed) objects.objects.push_back({id, std::move(c)});
    }
    Op op;
    op.label = std::move(label);
    if (!objects.objects.empty()) op.parts.push_back(std::move(objects));
    map::GroupData groups = d.groups;
    bool groupsChanged = false;
    for (auto& g : groups.groups) {
        if (g.kind != map::Group::Kind::Objects) continue; // (waypoints, walls, kind 3: not extents)
        for (auto& m : g.members) {
            if (const auto f = follow.find(static_cast<std::uint32_t>(m)); f != follow.end() && static_cast<std::uint32_t>(m) != f->second) {
                m = static_cast<std::int32_t>(f->second);
                groupsChanged = true;
            }
        }
    }
    if (groupsChanged) {
        std::vector<std::uint32_t> keys(d.groupKeys.begin(), d.groupKeys.begin() + static_cast<std::ptrdiff_t>(std::min(d.groupKeys.size(), groups.groups.size())));
        if (keys.size() != groups.groups.size()) keys.clear(); // (the op's rule: by position)
        op.parts.push_back(SetGroups{std::move(groups), std::move(keys)});
    }
    return op;
}

/// The facing carried from one object to another of a new type (where both turn).
void carryFacing(const DocObject& from, DocObject& to)
{
    if (const auto deg = facingDegrees(from)) setFacingDegrees(to, *deg);
}

} // namespace

// --- Type ----------------------------------------------------------------------------------------

Op changeTypeOp(const MapDocument& d, const ObjectFactory& factory, const std::set<ObjId>& ids, std::string_view type)
{
    std::vector<std::pair<ObjId, DocObject>> changed;
    for (const ObjId id : ids) {
        const auto it = d.objects.find(id);
        if (it == d.objects.end() || it->second.type == type) continue;
        const DocObject& old = it->second;
        DocObject n = factory.make(type, old.header.x, old.header.y);
        n.header.extent = old.header.extent;
        n.header.scriptId = old.header.scriptId;
        n.parent = old.parent;
        n.order = old.order;
        n.padding = old.padding;
        n.sizeHi = old.sizeHi;
        // The header's details (script name, team, flags, handler, what it carries) stay; a type
        // whose fresh record has none takes them anyway if the old one had them (they hold the
        // inventory and the script name).
        if (old.header.details) {
            n = withDetails(std::move(n), factory.freshFlags(type));
            auto& det = *n.header.details;
            const auto& was = *old.header.details;
            det.scriptName = was.scriptName;
            det.team = was.team;
            det.flags = was.flags;
            det.statusFlags = was.statusFlags;
            det.ownedScriptIds = was.ownedScriptIds;
            if (det.onPickup && was.onPickup) det.onPickup = was.onPickup;
            if (det.decayFrames && was.decayFrames) det.decayFrames = was.decayFrames;
        }
        carryFacing(old, n);
        changed.emplace_back(id, std::move(n));
    }
    if (changed.empty()) return {};
    const std::size_t n = changed.size();
    return editObjectsOp(d, changed, "Change " + std::to_string(n) + (n == 1 ? " object" : " objects") + " to " + std::string(type));
}

Op resetTypeDataOp(const MapDocument& d, const ObjectFactory& factory, const std::set<ObjId>& ids)
{
    std::vector<std::pair<ObjId, DocObject>> changed;
    for (const ObjId id : ids) {
        const auto it = d.objects.find(id);
        if (it == d.objects.end()) continue;
        DocObject c = it->second;
        const DocObject fresh = factory.make(c.type, c.header.x, c.header.y);
        c.xferVersion = fresh.xferVersion;
        c.data = fresh.data;
        carryFacing(it->second, c);
        changed.emplace_back(id, std::move(c));
    }
    return editObjectsOp(d, changed, "Reset type data");
}

// --- Extents -------------------------------------------------------------------------------------

std::vector<ExtentLink> extentLinks(const MapDocument& d)
{
    std::vector<ExtentLink> out;
    for (const auto& [id, o] : d.objects) {
        forEachLinkField(o, [&](const std::uint32_t& field, const LinkKind& k) {
            if (field != 0 && k.kind == LinkTarget::Extent && k.followsExtents) out.push_back({id, field, k.field});
        });
    }
    return out;
}

// --- Links, both ways ------------------------------------------------------------------------------

std::vector<Link> objectLinks(ObjId id, const DocObject& o)
{
    std::vector<Link> out;
    forEachLinkField(o, [&](const std::uint32_t& field, const LinkKind& k) {
        if (field != 0) out.push_back({Link::Source::Object, id, k.kind, field, k.field, k.label});
    });
    return out;
}

std::vector<Link> allLinks(const MapDocument& d)
{
    std::vector<Link> out;
    for (const auto& [id, o] : d.objects) {
        forEachLinkField(o, [&](const std::uint32_t& field, const LinkKind& k) {
            if (field != 0) out.push_back({Link::Source::Object, id, k.kind, field, k.field, k.label});
        });
    }
    for (const auto& w : d.waypoints.waypoints) {
        for (const auto& l : w.links) {
            if (l.target != 0) out.push_back({Link::Source::Waypoint, w.id, kWaypointLink.kind, l.target, kWaypointLink.field, kWaypointLink.label});
        }
    }
    const auto& groups = d.groups.groups;
    for (std::size_t i = 0; i < groups.size() && i < d.groupKeys.size(); ++i) {
        const auto& g = groups[i];
        const LinkKind* k = g.kind == map::Group::Kind::Objects ? &kObjectMember : g.kind == map::Group::Kind::Waypoints ? &kWaypointMember : nullptr;
        if (!k) continue; // (walls aren't extents or waypoints; a group of groups names groups)
        std::set<std::int32_t> seen; // (a member listed twice is one link)
        for (const std::int32_t m : g.members) {
            if (m > 0 && seen.insert(m).second) out.push_back({Link::Source::Group, d.groupKeys[i], k->kind, static_cast<std::uint32_t>(m), k->field, k->label});
        }
    }
    return out;
}

void LinkIndex::build(const MapDocument& d)
{
    links_ = allLinks(d);
    byExtent_.clear();
    byWaypoint_.clear();
    for (const auto& l : links_) (l.kind == LinkTarget::Extent ? byExtent_ : byWaypoint_)[l.target].push_back(l);
}

const std::vector<Link>& LinkIndex::toExtent(std::uint32_t extent) const
{
    const auto it = byExtent_.find(extent);
    return it == byExtent_.end() ? noLinks() : it->second;
}

const std::vector<Link>& LinkIndex::toWaypoint(std::uint32_t id) const
{
    const auto it = byWaypoint_.find(id);
    return it == byWaypoint_.end() ? noLinks() : it->second;
}

const std::vector<Link>& LinkIndex::toObject(const MapDocument& d, ObjId id) const
{
    const auto it = d.objects.find(id);
    if (it == d.objects.end() || it->second.header.extent == 0) return noLinks();
    return toExtent(it->second.header.extent);
}

std::vector<ObjId> objectsWithExtent(const MapDocument& d, std::uint32_t extent)
{
    std::vector<ObjId> out;
    for (const auto& [id, o] : d.objects) {
        if (o.header.extent == extent) out.push_back(id);
    }
    return out;
}

Op setExtentOp(const MapDocument& d, ObjId id, std::uint32_t extent)
{
    const auto it = d.objects.find(id);
    if (it == d.objects.end()) return {};
    const std::uint32_t old = it->second.header.extent;
    if (old == extent) return {};
    if (extent == 0) throw FormatError("map editor: extent 0 means \"none\"; pick another");
    if (!objectsWithExtent(d, extent).empty()) throw FormatError("map editor: extent " + std::to_string(extent) + " is in use");
    std::map<ObjId, std::uint32_t> rename{{id, extent}};
    std::map<std::uint32_t, std::uint32_t> follow;
    if (objectsWithExtent(d, old).size() == 1) follow[old] = extent; // (shared: links keep naming the other)
    return renumberOp(d, rename, follow, "Set extent of " + it->second.type + " to " + std::to_string(extent));
}

Op reorderExtentsOp(const MapDocument& d, std::uint32_t first)
{
    if (first == 0) first = 1;
    std::map<ObjId, std::uint32_t> rename;
    std::map<std::uint32_t, std::uint32_t> follow;
    std::uint32_t next = first;
    for (const ObjId id : recordOrder(d)) {
        const std::uint32_t old = d.objects.at(id).header.extent;
        rename[id] = next;
        follow.try_emplace(old, next); // (a shared extent: links follow the first with it)
        ++next;
    }
    return renumberOp(d, rename, follow, "Reorder extents from " + std::to_string(first));
}

// --- Header ----------------------------------------------------------------------------------------

DocObject withDetails(DocObject o, std::uint32_t freshFlags)
{
    auto& h = o.header;
    if (!h.details) {
        h.details.emplace();
        h.details->flags = freshFlags; // (what the type has: the reader takes these over the type's)
    }
    h.detailsFlag = h.detailsFlag ? h.detailsFlag : 0xFF;
    auto& det = *h.details;
    if (h.version > 0x3E && !det.onPickup) det.onPickup = map::ScriptHandler{};
    if (h.version > 0x3F && !det.decayFrames) det.decayFrames = 0;
    return o;
}

Op setDetailsOp(const MapDocument& d, ObjId id, bool on, const ObjectFactory* factory)
{
    const auto it = d.objects.find(id);
    if (it == d.objects.end() || it->second.header.details.has_value() == on) return {};
    DocObject c = it->second;
    if (on) {
        // (The type's flags first: a call's arguments may be built in any order, and the object
        // is moved into the first.)
        const std::uint32_t flags = factory ? factory->freshFlags(c.type) : ObjectFlag::Enabled;
        c = withDetails(std::move(c), flags);
    } else {
        if (!d.childrenOf(id).empty()) throw FormatError("map editor: it carries items, which need the header's details");
        c.header.details.reset();
        c.header.detailsFlag = 0;
    }
    std::string label = std::string(on ? "Add" : "Remove") + " header details of " + c.type; // (before c is moved from)
    return single(std::move(label), SetObjects{{{id, std::move(c)}}});
}

// --- Inventory ---------------------------------------------------------------------------------------

Op addItemOp(const MapDocument& d, const ObjectFactory& factory, ObjId owner, std::string_view type, ObjId* added, std::optional<std::size_t> at)
{
    const auto it = d.objects.find(owner);
    if (it == d.objects.end()) return {};
    Op op;
    op.label = "Add " + std::string(type) + " to " + it->second.type + "'s inventory";
    if (!it->second.header.details) {
        op.parts.push_back(SetObjects{{{owner, withDetails(it->second, factory.freshFlags(it->second.type))}}});
    }
    const auto items = d.childrenOf(owner);
    if (items.size() >= 255) throw FormatError("map editor: an inventory holds at most 255 items");
    DocObject item = factory.make(type, it->second.header.x, it->second.header.y);
    item.parent = owner;
    item.tocId = 0;
    item.header.extent = d.nextFreeExtent();
    // Its order: at the end, or between its neighbours (the others renumbered when there's no room).
    std::uint64_t order = 0;
    if (!items.empty()) order = d.objects.at(items.back()).order + 1;
    const ObjId id = d.nextObjId;
    if (at && *at < items.size()) {
        SetObjects renumber;
        std::uint64_t k = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i == *at) order = k++;
            DocObject c = d.objects.at(items[i]);
            if (c.order != k) {
                c.order = k;
                renumber.objects.push_back({items[i], std::move(c)});
            }
            ++k;
        }
        if (!renumber.objects.empty()) op.parts.push_back(std::move(renumber));
    }
    item.order = order;
    op.parts.push_back(AddObjects{{{id, std::move(item)}}});
    if (added) *added = id;
    return op;
}

Op cloneItemOp(const MapDocument& d, ObjId item, ObjId* added)
{
    const auto it = d.objects.find(item);
    if (it == d.objects.end() || it->second.parent == 0) return {};
    const ObjId owner = it->second.parent;
    const auto items = d.childrenOf(owner);
    if (items.size() >= 255) throw FormatError("map editor: an inventory holds at most 255 items");
    Op op;
    op.label = "Clone " + it->second.type;
    // Room after it: the later items move up one.
    SetObjects renumber;
    std::uint64_t k = 0;
    std::uint64_t cloneOrder = 0;
    for (const ObjId i : items) {
        DocObject c = d.objects.at(i);
        if (c.order != k) {
            c.order = k;
            renumber.objects.push_back({i, c});
        }
        ++k;
        if (i == item) cloneOrder = k++;
    }
    if (!renumber.objects.empty()) op.parts.push_back(std::move(renumber));
    // The copy and what it carries, new ids and extents.
    AddObjects add;
    ObjId nextId = d.nextObjId;
    std::uint32_t nextExtent = d.nextFreeExtent();
    const auto copy = [&](auto&& self, ObjId src, ObjId parent, std::uint64_t order) -> void {
        DocObject c = d.objects.at(src);
        const ObjId id = nextId++;
        c.parent = parent;
        c.order = order;
        c.tocId = 0;
        c.header.extent = nextExtent++;
        c.header.scriptId = 0;
        add.objects.push_back({id, std::move(c)});
        std::uint64_t n = 0;
        for (const ObjId child : d.childrenOf(src)) self(self, child, id, n++);
    };
    copy(copy, item, owner, cloneOrder);
    if (added) *added = add.objects.front().id;
    op.parts.push_back(std::move(add));
    return op;
}

Op moveItemOp(const MapDocument& d, ObjId item, std::size_t to)
{
    const auto it = d.objects.find(item);
    if (it == d.objects.end() || it->second.parent == 0) return {};
    auto items = d.childrenOf(it->second.parent);
    const auto at = std::ranges::find(items, item);
    const auto from = static_cast<std::size_t>(at - items.begin());
    to = std::min(to, items.size() - 1);
    if (from == to) return {};
    items.erase(at);
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), item);
    SetObjects part;
    for (std::size_t i = 0; i < items.size(); ++i) {
        DocObject c = d.objects.at(items[i]);
        if (c.order != i) {
            c.order = i;
            part.objects.push_back({items[i], std::move(c)});
        }
    }
    return single("Move " + it->second.type + " to slot " + std::to_string(to + 1), std::move(part));
}

Op setEquippedOp(const MapDocument& d, ObjId item, bool on, const ObjectFactory* factory)
{
    const auto it = d.objects.find(item);
    if (it == d.objects.end()) return {};
    DocObject c = withDetails(it->second, factory ? factory->freshFlags(it->second.type) : ObjectFlag::Enabled);
    auto& f = c.header.details->flags;
    f = on ? (f | ObjectFlag::Equipped) : (f & ~ObjectFlag::Equipped);
    if (c == it->second) return {};
    std::string label = std::string(on ? "Equip " : "Unequip ") + c.type; // (before c is moved from)
    return single(std::move(label), SetObjects{{{item, std::move(c)}}});
}

// --- Generator templates ------------------------------------------------------------------------------

std::optional<DocObject> templateObject(const xf::MonsterGenerator::Template& t, const things::ThingDb& things)
{
    try {
        const detail::TypeXfer tx = detail::typeXfer(things, t.type);
        const map::ObjectXfer x = map::ObjectXfer::parse(t.record.data);
        DocObject o;
        o.type = t.type;
        o.xferVersion = x.xferVersion;
        o.header = x.header;
        o.data = xf::decode(tx.xfer, x.xferVersion, x.extra, tx.info, x.extraOffset());
        o.padding = t.record.padding;
        o.sizeHi = t.record.sizeHi;
        o.tocId = t.record.tocId;
        return o;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

xf::MonsterGenerator::Template makeTemplate(const DocObject& o, const things::ThingDb& things, const xf::MonsterGenerator::Template* keep)
{
    const detail::TypeXfer tx = detail::typeXfer(things, o.type);
    map::ObjectXfer x;
    x.xferVersion = o.xferVersion;
    x.header = o.header;
    if (x.header.details) {
        x.header.details->inventoryCount = keep && keep->type == o.type ? static_cast<std::uint8_t>(keep->inventory.size()) : 0;
    }
    x.extra = xf::encode(o.data, o.xferVersion, tx.info, x.extraOffset());
    xf::MonsterGenerator::Template t;
    t.type = o.type;
    t.record.tocId = keep && keep->type == o.type ? keep->record.tocId : 0; // (the writer gives the table's id)
    t.record.padding = o.padding;
    t.record.sizeHi = o.sizeHi;
    t.record.data = x.serialize();
    t.record.size = static_cast<std::uint32_t>(t.record.data.size());
    if (keep && keep->type == o.type) t.inventory = keep->inventory;
    return t;
}

xf::MonsterGenerator::Template freshTemplate(const ObjectFactory& factory, std::string_view type)
{
    DocObject o = factory.make(type, 0, 0);
    return makeTemplate(o, factory.things());
}

// --- Generator templates' inventories ------------------------------------------------------------------

std::string templateItemType(const MapDocument& d, std::uint16_t tocId, const things::ThingDb& things)
{
    if (tocId >= kEditorTocBase) {
        const std::size_t i = tocId - kEditorTocBase;
        return i < things.things.size() ? things.things[i].name : std::string();
    }
    if (d.storedToc) {
        for (const auto& e : d.storedToc->entries) {
            if (e.id == tocId) return e.name; // (the first with the id: the game's table lookup)
        }
    }
    return {};
}

std::uint16_t templateItemTocId(const MapDocument& d, std::string_view type, const things::ThingDb& things)
{
    if (d.storedToc) {
        for (const auto& e : d.storedToc->entries) {
            if (e.name == type && e.id != 0 && e.id < kEditorTocBase && templateItemType(d, e.id, things) == type) return e.id;
        }
    }
    const auto* def = things.findThing(type);
    if (!def) return 0;
    const auto i = static_cast<std::size_t>(def - things.things.data());
    return i + kEditorTocBase <= 0xFFFF ? static_cast<std::uint16_t>(kEditorTocBase + i) : 0;
}

const xf::MonsterGenerator::Template* templateAt(const MapDocument& d, const TemplateSlot& slot)
{
    const auto it = d.objects.find(slot.generator);
    if (it == d.objects.end()) return nullptr;
    const auto* g = std::get_if<xf::MonsterGenerator>(&it->second.data);
    if (!g || slot.group >= g->groups.size() || slot.slot >= g->groups[slot.group].size()) return nullptr;
    return &g->groups[slot.group][slot.slot];
}

std::optional<MapDocument> templateDocument(const MapDocument& d, const TemplateSlot& slot, const things::ThingDb& things)
{
    const auto* t = templateAt(d, slot);
    if (!t) return std::nullopt;
    auto monster = templateObject(*t, things);
    if (!monster) return std::nullopt;
    MapDocument one;
    one.storedToc = d.storedToc;
    one.objectDataVersion = d.objectDataVersion;
    monster->parent = 0;
    monster->order = 0;
    one.objects.emplace(1, std::move(*monster));
    try {
        // The items' nesting from the inventory counts: the template's own first, as the stream has it.
        std::vector<map::ObjectXfer> xs;
        xs.reserve(t->inventory.size() + 1);
        xs.push_back(map::ObjectXfer::parse(t->record.data));
        for (const auto& rec : t->inventory) xs.push_back(map::ObjectXfer::parse(rec.data));
        const auto trees = map::buildObjectTree(xs);
        if (trees.size() != 1) return std::nullopt; // (items left over: kept as stored)
        std::vector<ObjId> parent(xs.size(), 0);
        const auto walk = [&](auto&& self, const map::ObjectTree& n) -> void {
            for (const auto& item : n.inventory) {
                parent[item.record] = static_cast<ObjId>(n.record + 1);
                self(self, item);
            }
        };
        walk(walk, trees.front());
        for (std::size_t i = 1; i < xs.size(); ++i) {
            const auto& rec = t->inventory[i - 1];
            const std::string type = templateItemType(d, rec.tocId, things);
            if (type.empty()) return std::nullopt;
            const detail::TypeXfer tx = detail::typeXfer(things, type);
            DocObject o;
            o.type = type;
            o.tocId = rec.tocId;
            o.xferVersion = xs[i].xferVersion;
            o.header = xs[i].header;
            o.data = xf::decode(tx.xfer, o.xferVersion, xs[i].extra, tx.info, xs[i].extraOffset());
            o.parent = parent[i];
            o.order = i;
            o.padding = rec.padding;
            o.sizeHi = rec.sizeHi;
            one.objects.emplace(static_cast<ObjId>(i + 1), std::move(o));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    one.nextObjId = static_cast<ObjId>(one.objects.size() + 1);
    return one;
}

xf::MonsterGenerator::Template templateFromDocument(const MapDocument& d, const MapDocument& one, const things::ThingDb& things,
                                                    const xf::MonsterGenerator::Template& keep)
{
    const auto monsters = one.childrenOf(0);
    if (monsters.size() != 1) throw FormatError("map editor: a generator template is one monster");
    // Each record re-encoded with its items' count (the writer's rule for the map's objects).
    const auto encode = [&](ObjId id) {
        const DocObject& o = one.objects.at(id);
        const detail::TypeXfer tx = detail::typeXfer(things, o.type);
        map::ObjectXfer x;
        x.xferVersion = o.xferVersion;
        x.header = o.header;
        const std::size_t items = one.childrenOf(id).size();
        if (x.header.details) {
            if (items > 0xFF) throw FormatError("map editor: more than 255 items in one inventory");
            x.header.details->inventoryCount = static_cast<std::uint8_t>(items);
        } else if (items > 0) {
            throw FormatError("map editor: an object without header details can't carry items");
        }
        x.extra = xf::encode(o.data, o.xferVersion, tx.info, x.extraOffset());
        map::ObjectRecord rec;
        rec.padding = o.padding;
        rec.sizeHi = o.sizeHi;
        rec.data = x.serialize();
        rec.size = static_cast<std::uint32_t>(rec.data.size());
        return rec;
    };
    xf::MonsterGenerator::Template t;
    const DocObject& m = one.objects.at(monsters.front());
    t.type = m.type;
    t.record = encode(monsters.front());
    t.record.tocId = keep.type == m.type ? keep.record.tocId : 0; // (the writer gives the table's id)
    const auto walk = [&](auto&& self, ObjId owner) -> void {
        for (const ObjId id : one.childrenOf(owner)) {
            const DocObject& o = one.objects.at(id);
            map::ObjectRecord rec = encode(id);
            rec.tocId = o.tocId != 0 && templateItemType(d, o.tocId, things) == o.type ? o.tocId : templateItemTocId(d, o.type, things);
            if (rec.tocId == 0) throw FormatError("map editor: no type " + o.type);
            t.inventory.push_back(std::move(rec));
            self(self, id);
        }
    };
    walk(walk, monsters.front());
    return t;
}

Op templateEditOp(const MapDocument& d, const TemplateSlot& slot, const Op& inner, const things::ThingDb& things)
{
    if (inner.empty()) return {};
    const auto* keep = templateAt(d, slot);
    auto one = templateDocument(d, slot, things);
    if (!keep || !one) throw FormatError("map editor: that generator template's records can't be read");
    applyOp(*one, inner);
    xf::MonsterGenerator::Template t = templateFromDocument(d, *one, things, *keep);
    if (t == *keep) return {};
    DocObject c = d.objects.at(slot.generator);
    std::get<xf::MonsterGenerator>(c.data).groups[slot.group][slot.slot] = std::move(t);
    return single(inner.label + " (generator monster)", SetObjects{{{slot.generator, std::move(c)}}});
}

// --- Edits through copies ---------------------------------------------------------------------------

Op editObjectsOp(const MapDocument& d, const std::vector<std::pair<ObjId, DocObject>>& changed, std::string label)
{
    SetObjects part;
    for (const auto& [id, o] : changed) {
        const auto it = d.objects.find(id);
        if (it == d.objects.end() || it->second == o) continue;
        part.objects.push_back({id, o});
    }
    if (part.objects.empty()) return {};
    return single(std::move(label), std::move(part));
}

} // namespace nox::mapedit
