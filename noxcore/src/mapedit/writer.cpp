// The map editor's document writer (writer.hpp; the rules are in document.hpp).
#include "noxcore/mapedit/writer.hpp"

#include "doc_types.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/script_doc.hpp"
#include "noxcore/mapedit/sidecar.hpp"
#include "noxcore/mapedit/validate.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <set>

namespace nox::mapedit {

namespace {

std::vector<map::Wall> wallRecords(const MapDocument& d)
{
    std::vector<map::Wall> out;
    out.reserve(d.walls.size());
    std::set<Cell> done;
    for (const Cell c : d.wallOrder.keys) {
        const auto it = d.walls.find(c);
        if (it != d.walls.end() && done.insert(c).second) {
            out.push_back(it->second);
        }
    }
    for (const auto& [c, w] : d.walls) { // (rows y, then x: Cell's order)
        if (!done.contains(c)) {
            out.push_back(w);
        }
    }
    return out;
}

using map::boxOf;

/// The kept value to write: the derived one, or the stored one while the content still derives
/// what it derived when the map was opened.
template <class T>
const T& keptOr(const T& derived, const std::optional<KeptDerived<T>>& kept)
{
    return kept && kept->derivedAtLoad == derived ? kept->stored : derived;
}

map::WallMap wallSection(const MapDocument& d)
{
    map::WallMap s;
    s.header = d.wallHeader;
    s.walls = wallRecords(d);
    if (const auto box = boxOf(s.walls)) {
        s.header.setBox(keptOr(*box, d.keptWallBox));
    }
    return s;
}

std::vector<map::FloorTilePair> floorRecords(const MapDocument& d)
{
    std::vector<map::FloorTilePair> out;
    std::set<std::uint16_t> done;
    const auto emit = [&](int x, int y) {
        const auto& p = d.floor.pair(x, y);
        if (p.empty() || !done.insert(static_cast<std::uint16_t>((x << 8) | y)).second) {
            return;
        }
        out.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), p.odd, p.even});
    };
    // A pair a custom map stored in several records (or as a record with no tile): its records
    // as stored, each in its place, while the pair is what the game read from them; else one.
    std::map<std::uint16_t, std::size_t> next;
    for (const std::uint16_t k : d.floorOrder.keys) {
        const auto stored = d.floorStored.find(k);
        if (stored != d.floorStored.end() && d.floor.pair(k >> 8, k & 0xFF) == stored->second.read) {
            std::size_t& i = next[k];
            if (i < stored->second.records.size()) {
                out.push_back(stored->second.records[i]);
            }
            ++i;
            done.insert(k);
            continue;
        }
        emit(k >> 8, k & 0xFF);
    }
    for (int y = 0; y < FloorGrid::kSize; ++y) {
        for (int x = 0; x < FloorGrid::kSize; ++x) {
            emit(x, y);
        }
    }
    return out;
}

map::FloorMap floorSection(const MapDocument& d)
{
    map::FloorMap s;
    s.header = d.floorHeader;
    s.tiles = floorRecords(d);
    if (const auto box = boxOf(s.tiles)) {
        s.header.setBox(keptOr(*box, d.keptFloorBox));
    }
    return s;
}

map::Polygons polygonSection(const MapDocument& d)
{
    // Points numbered 1..n in list order; each polygon's corners renumbered to match (a corner
    // naming an id two points share takes the later point, as the game's lookup does).
    map::Polygons s = d.polygons;
    // Version 3 has no flags word: a version 3 list given a flag (the panel's "Secret area"...) is
    // written as version 4, which holds it, so the flag isn't dropped. (Unflagged, it stays 3.)
    if (s.version < 4 && std::ranges::any_of(s.polygons, [](const map::Polygon& p) { return p.flags != 0; })) s.version = 4;
    std::map<std::uint32_t, std::uint32_t> index;
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        index[s.points[i].id] = static_cast<std::uint32_t>(i + 1);
        s.points[i].id = static_cast<std::uint32_t>(i + 1);
    }
    for (auto& p : s.polygons) {
        for (auto& id : p.points) {
            if (const auto it = index.find(id); it != index.end()) {
                id = it->second;
            }
        }
    }
    return s;
}

struct ObjectSections {
    map::ObjectTOC toc;
    map::ObjectData data;
};

} // namespace

namespace detail {

std::optional<std::pair<std::uint32_t, std::uint32_t>> derivedWallOffsets(const MapDocument& d)
{
    if (d.walls.empty()) {
        return std::nullopt;
    }
    std::uint32_t ox = 0xFF, oy = 0xFF;
    for (const auto& [c, w] : d.walls) {
        ox = std::min<std::uint32_t>(ox, c.x);
        oy = std::min<std::uint32_t>(oy, c.y);
    }
    return std::pair{ox, oy};
}

std::optional<GridBox> derivedWallBox(const MapDocument& d)
{
    return boxOf(wallRecords(d));
}

std::optional<GridBox> derivedFloorBox(const MapDocument& d)
{
    return boxOf(floorRecords(d));
}

} // namespace detail

std::vector<ObjId> recordOrder(const MapDocument& d)
{
    // Children per parent, in order.
    std::map<ObjId, std::vector<std::pair<std::uint64_t, ObjId>>> kids;
    for (const auto& [id, o] : d.objects) {
        if (o.parent != 0 && !d.objects.contains(o.parent)) {
            throw FormatError("map editor: inventory item whose owner is gone");
        }
        kids[o.parent].emplace_back(o.order, id);
    }
    for (auto& [p, v] : kids) {
        std::ranges::sort(v);
    }
    std::vector<ObjId> order; // records: each object, then its inventory (nested)
    order.reserve(d.objects.size());
    const auto walk = [&](auto&& self, ObjId parent) -> void {
        const auto it = kids.find(parent);
        if (it == kids.end()) {
            return;
        }
        for (const auto& [o, id] : it->second) {
            order.push_back(id);
            self(self, id);
        }
    };
    walk(walk, 0);
    return order;
}

namespace {

ObjectSections objectSections(const MapDocument& d, const things::ThingDb& things)
{
    const std::vector<ObjId> order = recordOrder(d);
    std::map<ObjId, std::size_t> kids; // items per owner
    for (const auto& [id, o] : d.objects) {
        if (o.parent != 0) ++kids[o.parent];
    }

    ObjectSections out;
    // Generator templates name their types too: the template by name and by a TOC id in its
    // embedded record, its items by TOC id alone (as the game's object save writes them, with
    // the save's own table). Each record's type, for the table and for the ids written: an item's
    // from the stored table, or the editor's own id for a type it lacks (templateItemType).
    const auto templateRecordTypes = [&](const map::xfer::MonsterGenerator::Template& t) {
        std::vector<std::string> names{t.type};
        for (const auto& item : t.inventory) names.push_back(templateItemType(d, item.tocId, things));
        return names;
    };
    std::vector<std::string> templateTypes; // in record order
    for (const ObjId id : order) {
        if (const auto* g = std::get_if<map::xfer::MonsterGenerator>(&d.objects.at(id).data)) {
            for (const auto& group : g->groups) {
                for (const auto& t : group) {
                    for (auto& n : templateRecordTypes(t)) {
                        if (!n.empty()) templateTypes.push_back(std::move(n));
                    }
                }
            }
        }
    }
    // The type table: the stored one while it has every type in use, else a fresh one.
    bool keepToc = d.storedToc.has_value();
    if (keepToc) {
        std::set<std::string> stored;
        for (const auto& e : d.storedToc->entries) {
            stored.insert(e.name);
        }
        for (const auto& [id, o] : d.objects) {
            keepToc &= stored.contains(o.type);
        }
        for (const auto& n : templateTypes) {
            keepToc &= stored.contains(n);
        }
    }
    std::map<std::string, std::uint16_t> ids;
    if (keepToc) {
        out.toc = *d.storedToc;
        for (const auto& e : out.toc.entries) {
            ids.try_emplace(e.name, e.id);
        }
    } else {
        out.toc.version = d.storedToc ? d.storedToc->version : 1;
        for (const ObjId id : order) {
            const std::string& name = d.objects.at(id).type;
            const auto [it, added] = ids.try_emplace(name, static_cast<std::uint16_t>(ids.size() + 1));
            if (added) {
                out.toc.entries.push_back({it->second, name});
            }
        }
        for (const auto& name : templateTypes) {
            const auto [it, added] = ids.try_emplace(name, static_cast<std::uint16_t>(ids.size() + 1));
            if (added) {
                out.toc.entries.push_back({it->second, name});
            }
        }
        // Listed in thing.bin order, as the game's own object writer lists them.
        const auto index = [&](const map::ObjectTOC::Entry& e) {
            const auto* def = things.findThing(e.name);
            return def ? def - things.things.data() : -1;
        };
        std::ranges::stable_sort(out.toc.entries, {}, index);
    }
    std::map<std::uint16_t, std::string> tocNames;
    for (const auto& e : out.toc.entries) {
        tocNames.try_emplace(e.id, e.name);
    }

    std::map<std::string, detail::TypeXfer, std::less<>> types;
    out.data.version = d.objectDataVersion;
    out.data.records.reserve(order.size());
    for (const ObjId id : order) {
        const DocObject& o = d.objects.at(id);
        auto typeIt = types.find(o.type);
        if (typeIt == types.end()) {
            typeIt = types.emplace(o.type, detail::typeXfer(things, o.type)).first;
        }
        map::ObjectXfer x;
        x.xferVersion = o.xferVersion;
        x.header = o.header;
        const std::size_t items = kids.contains(id) ? kids.at(id) : 0;
        if (x.header.details) {
            if (items > 0xFF) {
                throw FormatError("map editor: more than 255 items in one inventory");
            }
            x.header.details->inventoryCount = static_cast<std::uint8_t>(items);
        } else if (items > 0) {
            throw FormatError("map editor: an object without header details can't carry items");
        }
        if (const auto* g = std::get_if<map::xfer::MonsterGenerator>(&o.data)) {
            // Its templates' TOC ids, the written table's for their types (unchanged while the
            // stored table is kept and the id already names the type).
            map::xfer::MonsterGenerator copy = *g;
            const auto fix = [&](map::ObjectRecord& rec, const std::string& name) {
                if (name.empty()) return;
                if (keepToc && tocNames.contains(rec.tocId) && tocNames.at(rec.tocId) == name) return;
                if (const auto it = ids.find(name); it != ids.end()) rec.tocId = it->second;
            };
            for (auto& group : copy.groups) {
                for (auto& t : group) {
                    const auto names = templateRecordTypes(t);
                    fix(t.record, names[0]);
                    for (std::size_t i = 0; i < t.inventory.size(); ++i) fix(t.inventory[i], names[i + 1]);
                }
            }
            x.extra = map::xfer::encode(copy, o.xferVersion, typeIt->second.info, x.extraOffset());
        } else {
            x.extra = map::xfer::encode(o.data, o.xferVersion, typeIt->second.info, x.extraOffset());
        }
        map::ObjectRecord rec;
        if (keepToc && o.tocId != 0 && tocNames.contains(o.tocId) && tocNames.at(o.tocId) == o.type) {
            rec.tocId = o.tocId;
        } else {
            rec.tocId = ids.at(o.type);
        }
        rec.padding = o.padding;
        rec.sizeHi = o.sizeHi;
        rec.data = x.serialize();
        rec.size = static_cast<std::uint32_t>(rec.data.size());
        out.data.records.push_back(std::move(rec));
    }
    return out;
}

bool hasContent(const MapDocument& d, std::string_view name)
{
    if (name == map::WallMap::kName) return !d.walls.empty();
    if (name == map::FloorMap::kName) return d.floor.tileCount() > 0;
    if (name == map::SecretWalls::kName) return !d.secretWalls.walls.empty();
    if (name == map::DestructableWalls::kName) return !d.destructableWalls.walls.empty();
    if (name == map::WindowWalls::kName) return !d.windowWalls.walls.empty();
    if (name == map::Waypoints::kName) return !d.waypoints.waypoints.empty();
    if (name == map::DebugData::kName) return !d.debugData.entries.empty();
    if (name == map::GroupData::kName) return !d.groups.groups.empty();
    if (name == map::Polygons::kName) return !d.polygons.points.empty() || !d.polygons.polygons.empty();
    if (name == map::MapIntro::kName) return !d.intro.text.empty();
    if (name == map::ObjectTOC::kName || name == map::ObjectData::kName) return !d.objects.empty();
    return false; // MapInfo, ScriptObject, AmbientData, ScriptData: only when listed
}

} // namespace

map::MapFile toMapFile(const MapDocument& d, const things::ThingDb& things)
{
    map::MapFile f;
    f.magic = d.magic;
    if (const auto off = detail::derivedWallOffsets(d)) {
        const auto [ox, oy] = keptOr(*off, d.keptWallOffsets);
        f.wallOffsetX = ox;
        f.wallOffsetY = oy;
    } else {
        f.wallOffsetX = d.wallOffsetX;
        f.wallOffsetY = d.wallOffsetY;
    }
    // Walls' stored coordinates must be their cells.
    for (const auto& [c, w] : d.walls) {
        if (w.x != c.x || w.y != c.y) {
            throw FormatError("map editor: a wall's coordinates don't match its cell");
        }
    }

    // Sections: as listed, plus any with content that aren't, where the game's table puts them.
    std::vector<std::string> names = d.sections;
    for (const auto name : kSectionOrder) {
        if (std::ranges::find(names, name) == names.end() && hasContent(d, name)) {
            const auto rank = [](std::string_view n) { return std::ranges::find(kSectionOrder, n) - kSectionOrder.begin(); };
            const auto at = std::ranges::find_if(names, [&](const std::string& s) { return rank(s) > rank(name); });
            names.insert(at, std::string(name));
        }
    }
    // ObjectData needs ObjectTOC before it.
    const auto tocAt = std::ranges::find(names, map::ObjectTOC::kName);
    const auto dataAt = std::ranges::find(names, map::ObjectData::kName);
    if (dataAt != names.end() && !d.objects.empty() && (tocAt == names.end() || tocAt > dataAt)) {
        throw FormatError("map editor: ObjectTOC must come before ObjectData");
    }

    std::optional<ObjectSections> objects;
    for (const auto& name : names) {
        std::vector<std::uint8_t> bytes;
        if (name == map::MapInfo::kName) bytes = d.info.serialize();
        else if (name == map::WallMap::kName) bytes = wallSection(d).serialize();
        else if (name == map::FloorMap::kName) bytes = floorSection(d).serialize();
        else if (name == map::SecretWalls::kName) bytes = d.secretWalls.serialize();
        else if (name == map::DestructableWalls::kName) bytes = d.destructableWalls.serialize();
        else if (name == map::Waypoints::kName) bytes = d.waypoints.serialize();
        else if (name == map::DebugData::kName) bytes = d.debugData.serialize();
        else if (name == map::WindowWalls::kName) bytes = d.windowWalls.serialize();
        else if (name == map::GroupData::kName) bytes = d.groups.serialize();
        else if (name == map::ScriptObject::kName) bytes = d.script.serialize();
        else if (name == map::AmbientData::kName) bytes = d.ambient.serialize();
        else if (name == map::Polygons::kName) bytes = polygonSection(d).serialize();
        else if (name == map::MapIntro::kName) bytes = d.intro.serialize();
        else if (name == map::ScriptData::kName) bytes = d.scriptData.serialize();
        else if (name == map::ObjectTOC::kName || name == map::ObjectData::kName) {
            if (!objects) {
                objects = objectSections(d, things);
            }
            bytes = name == map::ObjectTOC::kName ? objects->toc.serialize() : objects->data.serialize();
        } else {
            throw FormatError("map editor: unknown section " + name);
        }
        f.sections.push_back({name, std::move(bytes)});
    }
    return f;
}

std::vector<std::uint8_t> writeMapBytes(const MapDocument& d, const things::ThingDb& things)
{
    auto bytes = map::serializeMapFileWithCrc(toMapFile(d, things), things);
    if (d.loaded && d.magic == map::kMagic && bytes.size() >= 12 && detail::contentHash(bytes) == d.loadedHash) {
        // Unchanged: keep the CRC the file had (Westwood's editor counted it differently;
        // clients compare stored values, so a fresh one would make every copy look different).
        for (int i = 0; i < 4; ++i) {
            bytes[8 + i] = static_cast<std::uint8_t>(d.loadedCrc >> (8 * i));
        }
    }
    return bytes;
}

EncodedFiles encodeFiles(const MapDocument& d, const things::ThingDb& things)
{
    return map::encodeMapBytes(writeMapBytes(d, things));
}

SavedPaths saveDocument(const MapDocument& d, const things::ThingDb& things, const std::filesystem::path& mapsDir,
                        std::string_view name, const Sidecar* sidecar)
{
    if (const auto problem = mapNameProblem(name)) {
        throw FormatError("map editor: " + *problem);
    }
    const auto issues = validate(d, things);
    std::string errors;
    for (const auto& i : issues) {
        if (i.severity == Severity::Error) {
            errors += "\n  " + i.where + ": " + i.message;
        }
    }
    if (!errors.empty()) {
        throw FormatError("map editor: the map can't be saved:" + errors);
    }
    // (Each file written beside, then moved over: a failed save never leaves half a map.)
    constexpr std::string_view kWho = "map editor";
    SavedPaths out;
    out.map = map::writeMapFolder(mapsDir, name, encodeFiles(d, things), kWho);
    out.nxz = std::filesystem::path(out.map).replace_extension(".nxz");
    if (sidecar) {
        out.sidecar = std::filesystem::path(out.map).replace_extension(".nxe");
        // Whether the map needs a NoX Core server, from the script as saved (in the sidecar only).
        Sidecar side = *sidecar;
        markNoxCoreNeeds(side, d);
        const std::string text = side.toJson();
        map::writeFileReplacing(out.sidecar, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), kWho);
    }
    return out;
}

} // namespace nox::mapedit
