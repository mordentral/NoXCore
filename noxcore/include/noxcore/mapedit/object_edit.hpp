// The map editor's object edits beyond placement: changing an object's type, its extent (with every
// link and group member that names it), the "Reorder Extents" command, header details, inventories
// (add, reorder, equip), and a monster generator's templates as editable objects.
//
// Every edit is one op. Links between objects name extents (a transporter's partner, an
// elevator's shaft and back, a mover's carried object; object groups' members), so an extent
// change rewrites them in the same op: references stay right (the old editor's Reorder Extents
// left groups pointing at the wrong objects).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/placement.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

// --- Type ----------------------------------------------------------------------------------------

/// Changes the objects' type: each gets the new type's fresh record from the factory (its type
/// data reset to what the game gives a new one of that type: monster.bin's numbers, a plate's
/// colours...), keeping its place, extent, header details (script name, team, flags, handler,
/// inventory) and facing where both types turn. One op.
Op changeTypeOp(const MapDocument& doc, const ObjectFactory& factory, const std::set<ObjId>& ids, std::string_view type);
/// The object's type data reset to the factory's fresh one (the old editor's "Load Defaults"),
/// keeping the header and facing. One op.
Op resetTypeDataOp(const MapDocument& doc, const ObjectFactory& factory, const std::set<ObjId>& ids);

// --- Extents -------------------------------------------------------------------------------------

/// Where an object's data names another object's extent.
struct ExtentLink {
    ObjId from = 0;
    std::uint32_t extent = 0;
    const char* field = ""; ///< "partner", "shaft", "elevator", "carries"
};
/// Every extent link in the map (world objects and items) that extent edits rewrite.
std::vector<ExtentLink> extentLinks(const MapDocument& doc);

// --- Links, both ways ------------------------------------------------------------------------------

/// What a link names: another object (by its extent) or a waypoint (by its id).
enum class LinkTarget : std::uint8_t { Extent, Waypoint };
/// One link: an object's field (a teleporter's partner, an elevator's shaft, a mover's carried
/// object or waypoints, a light's target), a waypoint's link, or an object or waypoint group's
/// member, naming another object or waypoint. Every link kind is listed once (object_edit.cpp's
/// table), so the panel's "Linked from", the links layer and extent edits see the same ones.
struct Link {
    enum class Source : std::uint8_t { Object, Waypoint, Group };
    Source source = Source::Object;
    std::uint32_t from = 0;   ///< the source object's ObjId, the source waypoint's id, or the group's key
    LinkTarget kind = LinkTarget::Extent;
    std::uint32_t target = 0; ///< the extent, or the waypoint id (never 0)
    const char* field = "";   ///< the link kind: "partner", "shaft", "elevator", "carries", "light target",
                              ///< "starts at", "heading for", "came from", "waypoint link", "member"
    const char* label = "";   ///< the source field's label in the panel ("Leads to", "Shaft"...)
    bool operator==(const Link& o) const
    {
        return source == o.source && from == o.from && kind == o.kind && target == o.target && std::string_view(field) == o.field;
    }
};
/// The links one object's data holds (the non-zero ones), in field order.
std::vector<Link> objectLinks(ObjId id, const DocObject& o);
/// Every link in the map: the objects' (world objects and items, by id), the waypoints', then the
/// object and waypoint groups' members (by group key; a member listed twice counts once).
std::vector<Link> allLinks(const MapDocument& doc);

/// Links by what they name: built once per document change (the History's revision), so the
/// panel's "Linked from" is a lookup, never a walk over the map.
class LinkIndex {
public:
    void build(const MapDocument& doc);
    /// The links naming this extent / waypoint, in allLinks order (empty if none).
    const std::vector<Link>& toExtent(std::uint32_t extent) const;
    const std::vector<Link>& toWaypoint(std::uint32_t id) const;
    /// The links naming this object (its extent: a shared extent's links show on each object with it).
    const std::vector<Link>& toObject(const MapDocument& doc, ObjId id) const;
    const std::vector<Link>& all() const { return links_; }

private:
    std::vector<Link> links_;
    std::map<std::uint32_t, std::vector<Link>> byExtent_, byWaypoint_;
};
/// The objects that use an extent.
std::vector<ObjId> objectsWithExtent(const MapDocument& doc, std::uint32_t extent);
/// Gives the object a new extent and rewrites every link and object-group member that named its
/// old one (only when no other object shares the old extent: else they keep naming that one).
/// Throws FormatError when another object has `extent` already (extents are the map's keys), or
/// for 0.
Op setExtentOp(const MapDocument& doc, ObjId id, std::uint32_t extent);
/// Renumbers every object `first`, `first`+1... in record order (world objects, each followed by
/// its inventory, as the file lists them) and rewrites every link and object-group member to
/// match. Objects sharing an extent get their own numbers; a link or member naming a shared one
/// follows the first object with it (the one the game's lookup finds). One op.
Op reorderExtentsOp(const MapDocument& doc, std::uint32_t first = 1);

// --- Header ----------------------------------------------------------------------------------------

/// The header flags the game's object save writes (masked so on write): the authoring flags.
constexpr std::uint32_t kSavedObjectFlags = 0x11408162u;
/// The xStatus bits the save writes (0x5E).
constexpr std::uint32_t kSavedXStatus = 0x5Eu;
/// Header flag bits.
namespace ObjectFlag {
inline constexpr std::uint32_t Equipped = 0x100u;
inline constexpr std::uint32_t Enabled = 0x01000000u;
} // namespace ObjectFlag

/// Gives the object header details (the "Extra Bytes": needed for a script name, team, flags,
/// handler or inventory), with the header version's optional fields; or removes them (refused
/// while it carries items). One op.
Op setDetailsOp(const MapDocument& doc, ObjId id, bool on, const ObjectFactory* factory = nullptr);
/// The object with details added (as setDetailsOp does), for edits that need them: its flags
/// `freshFlags` (ObjectFactory::freshFlags: what the type has, so nothing changes in the game).
DocObject withDetails(DocObject o, std::uint32_t freshFlags = ObjectFlag::Enabled);

// --- Inventory ---------------------------------------------------------------------------------------

/// A new item of `type` in `owner`'s inventory (at the end, or before `at`): the factory's fresh
/// record, the owner given details if it had none. One op; `added` gets its id.
Op addItemOp(const MapDocument& doc, const ObjectFactory& factory, ObjId owner, std::string_view type, ObjId* added = nullptr,
             std::optional<std::size_t> at = std::nullopt);
/// A copy of an item (and what it carries) right after it (the fork's Clone).
Op cloneItemOp(const MapDocument& doc, ObjId item, ObjId* added = nullptr);
/// Moves the item to position `to` in its owner's inventory.
Op moveItemOp(const MapDocument& doc, ObjId item, std::size_t to);
/// Sets or clears the item's EQUIPPED flag (worn by a creature; the item given details if needed).
Op setEquippedOp(const MapDocument& doc, ObjId item, bool on, const ObjectFactory* factory = nullptr);

// --- Generator templates ------------------------------------------------------------------------------

/// A monster generator template's monster as a document object (type, header, typed data), or
/// nothing when its record doesn't decode.
std::optional<DocObject> templateObject(const map::xfer::MonsterGenerator::Template& t, const things::ThingDb& things);
/// The template for an object (its record re-encoded; its inventory records kept from `keep`).
map::xfer::MonsterGenerator::Template makeTemplate(const DocObject& o, const things::ThingDb& things,
                                                   const map::xfer::MonsterGenerator::Template* keep = nullptr);
/// A template of `type` made fresh by the factory (a slot whose type changed starts over: the
/// old monster's data never carries across).
map::xfer::MonsterGenerator::Template freshTemplate(const ObjectFactory& factory, std::string_view type);

// --- Generator templates' inventories ------------------------------------------------------------------
//
// A template's items are object records after its own (as the game's object save writes a monster
// and what it carries; the generator's reader has the template's xfer read them, by ObjectTOC id).
// They have no type name of their own, only that id: the map's stored ObjectTOC names it, and an
// item whose type that table lacks (or a map with no stored table) carries the editor's own id for
// its type, kEditorTocBase + its thing.bin index, which no table the game writes reaches. The
// writer gives every one the written table's id for its type.

/// Where a generator template is: the generator, its variant (group) and its slot.
struct TemplateSlot {
    ObjId generator = 0;
    std::size_t group = 0;
    std::size_t slot = 0;
};
/// The editor's own ObjectTOC ids for template items' types: this plus the thing.bin index.
constexpr std::uint16_t kEditorTocBase = 0x8000;
/// The type a template item's record names by `tocId` (the stored table's, or the editor's own
/// id's); empty when neither names one.
std::string templateItemType(const MapDocument& doc, std::uint16_t tocId, const things::ThingDb& things);
/// The id a template item of `type` carries in the document: the stored table's for it (the first
/// listing it), else the editor's own (0 for a type thing.bin doesn't have).
std::uint16_t templateItemTocId(const MapDocument& doc, std::string_view type, const things::ThingDb& things);
/// The template at `slot` (null when there's none).
const map::xfer::MonsterGenerator::Template* templateAt(const MapDocument& doc, const TemplateSlot& slot);
/// The template's monster and its items as a small document of their own: the monster is ObjId 1
/// (in the world), its items its inventory, nested as stored, ids 2... in stream order. The
/// ordinary object edits work on it (addItemOp, cloneItemOp, moveItemOp, setEquippedOp,
/// setFieldOp, changeTypeOp, RemoveObjects...); templateEditOp makes one op on the generator from
/// such an edit. Nothing when a record doesn't decode (its type unknown: then it's kept as stored).
/// Its storedToc is the map's (the edits' new items take the map's ids for their types).
std::optional<MapDocument> templateDocument(const MapDocument& doc, const TemplateSlot& slot, const things::ThingDb& things);
/// The template a template document makes: the monster's record, then its items' in stream order
/// (each owner's count from its items), each item with templateItemTocId's id for its type (the
/// one it had while that still names the type). The monster's ObjectTOC id kept from `keep`.
map::xfer::MonsterGenerator::Template templateFromDocument(const MapDocument& doc, const MapDocument& one, const things::ThingDb& things,
                                                           const map::xfer::MonsterGenerator::Template& keep);
/// One op on the generator: the template at `slot` with `inner` (an edit of its templateDocument)
/// applied. Empty when `inner` is or changes nothing; throws FormatError as applyOp does.
Op templateEditOp(const MapDocument& doc, const TemplateSlot& slot, const Op& inner, const things::ThingDb& things);

// --- Edits through copies ---------------------------------------------------------------------------

/// One SetObjects op for the objects changed (only those that differ); empty if none does.
Op editObjectsOp(const MapDocument& doc, const std::vector<std::pair<ObjId, DocObject>>& changed, std::string label);

} // namespace nox::mapedit
