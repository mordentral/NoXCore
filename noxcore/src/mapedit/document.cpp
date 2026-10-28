// MapDocument: decoding a map into the editor's document, and the small edit helpers.
#include "noxcore/mapedit/document.hpp"

#include "doc_types.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace nox::mapedit {

const std::array<std::string_view, 16> kSectionOrder = {
    map::MapInfo::kName,      map::WallMap::kName,     map::FloorMap::kName,          map::SecretWalls::kName,
    map::DestructableWalls::kName, map::Waypoints::kName, map::DebugData::kName,      map::WindowWalls::kName,
    map::GroupData::kName,    map::ScriptObject::kName, map::AmbientData::kName,      map::Polygons::kName,
    map::MapIntro::kName,     map::ScriptData::kName,  map::ObjectTOC::kName,         map::ObjectData::kName};

// ---------------------------------------------------------------------------------------------
// FloorGrid

FloorGrid::FloorGrid()
    : pairs_(static_cast<std::size_t>(kSize) * kSize)
{
}

namespace {

/// The pair holding full-grid tile (gx, gy), and whether it's the odd one; false if none can.
bool pairOf(int gx, int gy, int& x, int& y, bool& odd)
{
    if (((gx + gy) & 1) != 0 || gx < 0 || gx > 255 || gy < -1 || gy > 254) {
        return false;
    }
    const map::FloorPairSlot slot = map::floorPairOf(gx, gy);
    x = slot.x;
    y = slot.y;
    odd = slot.odd;
    return x < FloorGrid::kSize && y < FloorGrid::kSize;
}

} // namespace

const map::FloorTile* FloorGrid::tile(int gx, int gy) const
{
    int x = 0, y = 0;
    bool odd = false;
    if (!pairOf(gx, gy, x, y, odd)) {
        return nullptr;
    }
    const auto& p = pair(x, y);
    const auto& t = odd ? p.odd : p.even;
    return t ? &*t : nullptr;
}

bool FloorGrid::setTile(int gx, int gy, std::optional<map::FloorTile> tile)
{
    int x = 0, y = 0;
    bool odd = false;
    if (!pairOf(gx, gy, x, y, odd)) {
        return false;
    }
    auto& p = pair(x, y);
    (odd ? p.odd : p.even) = std::move(tile);
    return true;
}

std::size_t FloorGrid::tileCount() const
{
    std::size_t n = 0;
    for (const auto& p : pairs_) {
        n += (p.odd ? 1 : 0) + (p.even ? 1 : 0);
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// Types

namespace detail {

TypeXfer typeXfer(const things::ThingDb& things, std::string_view name)
{
    const things::ThingDef* t = things.findThing(name);
    if (!t) {
        throw FormatError("map editor: object type " + std::string(name) + " is not in thing.bin");
    }
    return map::xfer::typeXferOf(*t);
}

std::uint64_t contentHash(std::span<const std::uint8_t> plain)
{
    // FNV-1a over the decrypted file with the CRC field (bytes 8..11 of a FADEFACE file) as zeros.
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        const std::uint8_t b = (i >= 8 && i < 12) ? 0 : plain[i];
        h = (h ^ b) * 1099511628211ull;
    }
    return h ^ plain.size();
}

} // namespace detail

namespace {

bool knownSection(std::string_view name)
{
    return std::ranges::find(kSectionOrder, name) != kSectionOrder.end();
}

/// Canonical floor pair order: rows y, then x.
bool floorKeyLess(std::uint16_t a, std::uint16_t b)
{
    return std::tuple(a & 0xFF, a >> 8) < std::tuple(b & 0xFF, b >> 8);
}

void loadObjects(MapDocument& d, const map::ObjectTOC& toc, const map::ObjectData& od, const things::ThingDb& things)
{
    std::map<std::uint16_t, std::string> names;
    for (const auto& e : toc.entries) {
        names.try_emplace(e.id, e.name);
    }
    std::vector<map::ObjectXfer> xs;
    xs.reserve(od.records.size());
    for (const auto& rec : od.records) {
        xs.push_back(map::ObjectXfer::parse(rec.data));
    }
    // Parents from the inventory nesting.
    std::vector<ObjId> parent(xs.size(), 0);
    const auto trees = map::buildObjectTree(xs);
    const auto walk = [&](auto&& self, const map::ObjectTree& t) -> void {
        for (const auto& item : t.inventory) {
            parent[item.record] = static_cast<ObjId>(t.record + 1);
            self(self, item);
        }
    };
    for (const auto& t : trees) {
        walk(walk, t);
    }
    std::map<std::string, detail::TypeXfer, std::less<>> types;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const auto& rec = od.records[i];
        const auto nameIt = names.find(rec.tocId);
        if (nameIt == names.end()) {
            throw FormatError("map editor: object record with type id " + std::to_string(rec.tocId) + " not in ObjectTOC");
        }
        auto typeIt = types.find(nameIt->second);
        if (typeIt == types.end()) {
            typeIt = types.emplace(nameIt->second, detail::typeXfer(things, nameIt->second)).first;
        }
        DocObject o;
        o.type = nameIt->second;
        o.tocId = rec.tocId;
        o.xferVersion = xs[i].xferVersion;
        o.header = xs[i].header;
        o.data = map::xfer::decode(typeIt->second.xfer, o.xferVersion, xs[i].extra, typeIt->second.info, xs[i].extraOffset());
        o.parent = parent[i];
        o.order = i;
        o.padding = rec.padding;
        o.sizeHi = rec.sizeHi;
        d.objects.emplace(static_cast<ObjId>(i + 1), std::move(o));
    }
    d.nextObjId = static_cast<ObjId>(xs.size() + 1);
}

} // namespace

MapDocument MapDocument::fromMapFile(const map::MapFile& file, const things::ThingDb& things)
{
    MapDocument d;
    d.magic = file.magic;
    d.wallOffsetX = file.wallOffsetX;
    d.wallOffsetY = file.wallOffsetY;
    d.loadedCrc = file.crc;
    d.loadedHash = detail::contentHash(map::serializeMapFile(file));
    d.loaded = true;

    std::set<std::string> seen;
    for (const auto& s : file.sections) {
        if (!knownSection(s.name)) {
            throw FormatError("map editor: unknown section " + s.name + " (the game can't load this map)");
        }
        if (!seen.insert(s.name).second) {
            throw FormatError("map editor: section " + s.name + " appears twice");
        }
        d.sections.push_back(s.name);
    }
    const auto raw = [&](std::string_view name) { return file.find(name); };

    if (const auto* s = raw(map::MapInfo::kName)) d.info = map::MapInfo::parse(s->data);
    if (const auto* s = raw(map::WallMap::kName)) {
        auto walls = map::WallMap::parse(s->data);
        d.wallHeader = walls.header;
        for (const auto& w : walls.walls) {
            const Cell c{w.x, w.y};
            if (!d.walls.emplace(c, w).second) {
                throw FormatError("map editor: two walls at one cell");
            }
            d.wallOrder.keys.push_back(c);
        }
        if (std::ranges::is_sorted(d.wallOrder.keys)) {
            d.wallOrder.keys.clear();
        }
        if (const auto box = detail::derivedWallBox(d)) {
            if (const GridBox stored = walls.header.box(); stored != *box) {
                d.keptWallBox = KeptDerived<GridBox>{stored, *box};
            }
        }
    }
    if (const auto off = detail::derivedWallOffsets(d)) {
        if (const std::pair stored{file.wallOffsetX, file.wallOffsetY}; stored != *off) {
            d.keptWallOffsets = KeptDerived<std::pair<std::uint32_t, std::uint32_t>>{stored, *off};
        }
    }
    if (const auto* s = raw(map::FloorMap::kName)) {
        auto floor = map::FloorMap::parse(s->data);
        d.floorHeader = floor.header;
        // As the game's FloorMap reader takes the records: in turn, each tile replacing the
        // pair's earlier one (its edges too) and a record without that tile leaving it, so a pair
        // stored twice (as old community editors write them) gets each tile from the last record
        // that has it, and a record with no tile changes nothing.
        std::map<std::uint16_t, int> records; // records per pair key
        for (const auto& p : floor.tiles) {
            const auto key = static_cast<std::uint16_t>((p.x << 8) | p.y);
            d.floorOrder.keys.push_back(key);
            ++records[key];
            if (!p.odd && !p.even) {
                records[key] = std::max(records[key], 2); // (kept as stored even when alone)
            }
        }
        for (auto& p : floor.tiles) {
            const auto key = static_cast<std::uint16_t>((p.x << 8) | p.y);
            if (records[key] > 1) {
                d.floorStored[key].records.push_back(p);
            }
            auto& cell = d.floor.pair(p.x, p.y);
            if (p.odd) cell.odd = std::move(p.odd);
            if (p.even) cell.even = std::move(p.even);
        }
        for (auto& [key, stored] : d.floorStored) {
            stored.read = d.floor.pair(key >> 8, key & 0xFF);
        }
        if (d.floorStored.empty() && std::ranges::is_sorted(d.floorOrder.keys, floorKeyLess)) {
            d.floorOrder.keys.clear();
        }
        if (const auto box = detail::derivedFloorBox(d)) {
            if (const GridBox stored = floor.header.box(); stored != *box) {
                d.keptFloorBox = KeptDerived<GridBox>{stored, *box};
            }
        }
    }
    if (const auto* s = raw(map::SecretWalls::kName)) d.secretWalls = map::SecretWalls::parse(s->data);
    if (const auto* s = raw(map::DestructableWalls::kName)) d.destructableWalls = map::DestructableWalls::parse(s->data);
    if (const auto* s = raw(map::Waypoints::kName)) d.waypoints = map::Waypoints::parse(s->data);
    if (const auto* s = raw(map::DebugData::kName)) d.debugData = map::DebugData::parse(s->data);
    if (const auto* s = raw(map::WindowWalls::kName)) d.windowWalls = map::WindowWalls::parse(s->data);
    if (const auto* s = raw(map::GroupData::kName)) d.groups = map::GroupData::parse(s->data);
    if (const auto* s = raw(map::ScriptObject::kName)) d.script = map::ScriptObject::parse(s->data);
    if (const auto* s = raw(map::AmbientData::kName)) d.ambient = map::AmbientData::parse(s->data);
    if (const auto* s = raw(map::Polygons::kName)) d.polygons = map::Polygons::parse(s->data);
    if (const auto* s = raw(map::MapIntro::kName)) d.intro = map::MapIntro::parse(s->data);
    if (const auto* s = raw(map::ScriptData::kName)) d.scriptData = map::ScriptData::parse(s->data);
    for (std::size_t i = 0; i < d.polygons.polygons.size(); ++i) d.polygonKeys.push_back(d.nextPolygonKey++);
    for (std::size_t i = 0; i < d.groups.groups.size(); ++i) d.groupKeys.push_back(d.nextGroupKey++);

    const auto* tocRaw = raw(map::ObjectTOC::kName);
    const auto* dataRaw = raw(map::ObjectData::kName);
    if (tocRaw) {
        d.storedToc = map::ObjectTOC::parse(tocRaw->data);
    }
    if (dataRaw) {
        const auto od = map::ObjectData::parse(dataRaw->data);
        d.objectDataVersion = od.version;
        if (!od.records.empty() && !d.storedToc) {
            throw FormatError("map editor: ObjectData without ObjectTOC");
        }
        loadObjects(d, d.storedToc ? *d.storedToc : map::ObjectTOC{}, od, things);
    }
    return d;
}

MapDocument MapDocument::load(const std::filesystem::path& path, const things::ThingDb& things)
{
    MapDocument d = fromMapFile(map::loadMapFile(path), things);
    d.origin.name = path.stem().string();
    d.origin.stock = isStockMapName(d.origin.name);
    return d;
}

MapDocument MapDocument::blank()
{
    MapDocument d;
    for (const auto name : kSectionOrder) {
        d.sections.emplace_back(name);
    }
    d.info.version = 3;
    // No script yet: an empty ScriptObject, as the stock maps without scripts store it.
    return d;
}

// ---------------------------------------------------------------------------------------------
// Edit helpers

std::optional<std::size_t> MapDocument::polygonIndex(std::uint32_t key) const
{
    const auto it = std::ranges::find(polygonKeys, key);
    if (it == polygonKeys.end() || static_cast<std::size_t>(it - polygonKeys.begin()) >= polygons.polygons.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - polygonKeys.begin());
}

std::optional<std::size_t> MapDocument::groupIndex(std::uint32_t key) const
{
    const auto it = std::ranges::find(groupKeys, key);
    if (it == groupKeys.end() || static_cast<std::size_t>(it - groupKeys.begin()) >= groups.groups.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - groupKeys.begin());
}

void MapDocument::setWall(const map::Wall& wall)
{
    walls[Cell{wall.x, wall.y}] = wall;
    ensureSection(map::WallMap::kName);
}

bool MapDocument::removeWall(Cell cell)
{
    if (walls.erase(cell) == 0) {
        return false;
    }
    const auto at = [&](const map::GridPoint& p) { return p.x == cell.x && p.y == cell.y; };
    std::erase_if(secretWalls.walls, [&](const map::SecretWall& w) { return at(w.pos); });
    std::erase_if(destructableWalls.walls, at);
    std::erase_if(windowWalls.walls, at);
    return true;
}

std::vector<ObjId> MapDocument::childrenOf(ObjId parent) const
{
    std::vector<std::pair<std::uint64_t, ObjId>> v;
    for (const auto& [id, o] : objects) {
        if (o.parent == parent) {
            v.emplace_back(o.order, id);
        }
    }
    std::ranges::sort(v);
    std::vector<ObjId> out;
    out.reserve(v.size());
    for (const auto& [order, id] : v) {
        out.push_back(id);
    }
    return out;
}

std::uint32_t MapDocument::nextFreeExtent() const
{
    std::uint32_t most = 0;
    for (const auto& [id, o] : objects) {
        most = std::max(most, o.header.extent);
    }
    return most + 1;
}

ObjId MapDocument::addObject(DocObject obj)
{
    if (obj.parent != 0 && !objects.contains(obj.parent)) {
        throw FormatError("map editor: inventory owner doesn't exist");
    }
    if (obj.header.extent == 0) {
        obj.header.extent = nextFreeExtent();
    }
    // The header's optional fields follow its version (pickup handler from 0x3F, decay from 0x40).
    obj.header.detailsFlag = obj.header.details ? (obj.header.detailsFlag ? obj.header.detailsFlag : 0xFF) : 0;
    if (auto& det = obj.header.details) {
        if (obj.header.version > 0x3E && !det->onPickup) det->onPickup = map::ScriptHandler{};
        if (obj.header.version <= 0x3E) det->onPickup.reset();
        if (obj.header.version > 0x3F && !det->decayFrames) det->decayFrames = 0;
        if (obj.header.version <= 0x3F) det->decayFrames.reset();
    }
    std::uint64_t last = 0;
    bool any = false;
    for (const auto& [id, o] : objects) {
        if (o.parent == obj.parent) {
            last = std::max(last, o.order);
            any = true;
        }
    }
    obj.order = any ? last + 1 : 0;
    obj.tocId = 0;
    const ObjId id = nextObjId++;
    objects.emplace(id, std::move(obj));
    ensureSection(map::ObjectTOC::kName);
    ensureSection(map::ObjectData::kName);
    return id;
}

bool MapDocument::removeObject(ObjId id)
{
    if (!objects.contains(id)) {
        return false;
    }
    for (const ObjId child : childrenOf(id)) {
        removeObject(child);
    }
    objects.erase(id);
    return true;
}

void MapDocument::ensureSection(std::string_view name)
{
    if (std::ranges::find(sections, name) != sections.end()) {
        return;
    }
    const auto rank = [](std::string_view n) { return std::ranges::find(kSectionOrder, n) - kSectionOrder.begin(); };
    const auto at = std::ranges::find_if(sections, [&](const std::string& s) { return rank(s) > rank(name); });
    sections.insert(at, std::string(name));
}

// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kStockMaps[] = {
    "Bankshot", "BluDeath", "Bunker",   "CapFlag",  "Con01A",   "Con02a",   "Con03A",   "Con03B",   "Con04a",   "Con04b",
    "Con04c",   "Con05A",   "Con05B",   "Con05C",   "Con06a",   "Con06b",   "Con07A",   "Con07B",   "Con07C",   "Con07D",
    "Con07E",   "Con07F",   "Con07G",   "Con07H",   "Con08a",   "Con08b",   "Con08c",   "Con08d",   "Con08e",   "Con09a",
    "Con09b",   "Con09c",   "Con09d",   "Con10a",   "Con10b",   "Con10c",   "Con10d",   "Con11a",   "EndGame",  "Estate",
    "FlagBall", "FlagWar",  "FortNox",  "fortress", "FreezOut", "G_CastlD", "G_Castle", "G_CryptD", "G_Crypts", "G_ForesD",
    "G_Forest", "G_Lava",   "G_LOTD",   "G_LOTDD",  "G_Mines",  "G_Swamp",  "G_TemplD", "G_Temple", "Inferno",  "Kingdoms",
    "Library",  "LostTomb", "ManaMine", "MiniMine", "MnaVault", "Oasis",    "So_Beach", "So_Brin",  "So_Druid", "So_Dun",
    "So_FOV",   "So_Galav", "So_Grok",  "So_Ix",    "So_LOD",   "So_Mines", "So_Mount", "So_Open",  "So_Swamp", "So_Waste",
    "So_Woods", "SpyFort",  "treehaus", "TriLevel", "War01A",   "War02A",   "War02b",   "War03a",   "War03b",   "War03c",
    "War03d",   "War04a",   "War04b",   "War04c",   "War05A",   "War05B",   "War05C",   "War06a",   "War06b",   "War07A",
    "War07B",   "War07C",   "War07D",   "War07E",   "War07F",   "War07G",   "War07H",   "War08a",   "War08b",   "War08c",
    "War08d",   "War08e",   "War09a",   "War09b",   "War09c",   "War09d",   "War10a",   "War10b",   "War10c",   "War10d",
    "War11a",   "Wiz01A",   "Wiz02A",   "Wiz02B",   "Wiz02C",   "Wiz03a",   "Wiz03b",   "Wiz03c",   "Wiz04a",   "Wiz04b",
    "Wiz04c",   "Wiz05A",   "Wiz05B",   "Wiz05C",   "Wiz06a",   "Wiz06b",   "Wiz06c",   "Wiz07A",   "Wiz07B",   "Wiz07C",
    "Wiz07D",   "Wiz07E",   "Wiz07F",   "Wiz08a",   "Wiz08b",   "Wiz08c",   "Wiz08d",   "Wiz08e",   "Wiz09a",   "Wiz09b",
    "Wiz09c",   "Wiz09d",   "Wiz10a",   "Wiz10b",   "Wiz10c",   "Wiz10d",   "Wiz11A"};


} // namespace

bool isStockMapName(std::string_view name)
{
    return std::ranges::any_of(kStockMaps, [&](std::string_view s) { return util::equalsNoCase(s, name); });
}

} // namespace nox::mapedit
