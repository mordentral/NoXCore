// Changes to one part of a map, the rest kept as stored (map_patch.hpp).
#include "noxcore/map/map_patch.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/thing_db.hpp"

#include <map>
#include <variant>

namespace nox::map {

namespace {

constexpr std::string_view kExitXfer = "ExitXfer";

/// The ObjectTOC ids of the map's exit types (their xfer ExitXfer), with each one's xfer and name.
struct ExitTypes {
    std::map<std::uint16_t, std::pair<xfer::TypeXfer, std::string>> byId;
};

ExitTypes exitTypes(const MapFile& map, const things::ThingDb& things)
{
    ExitTypes out;
    for (const auto& e : decodeSection<ObjectTOC>(map).entries) {
        const things::ThingDef* t = things.findThing(e.name);
        if (!t) {
            throw FormatError("map exits: object type " + e.name + " is not in thing.bin");
        }
        xfer::TypeXfer x = xfer::typeXferOf(*t);
        if (x.xfer == kExitXfer) {
            out.byId.emplace(e.id, std::make_pair(std::move(x), e.name));
        }
    }
    return out;
}

/// An exit record decoded: its xfer, its data (an Exit), and where the type's fields start.
struct ExitRecord {
    ObjectXfer obj;
    xfer::Data data;
    std::size_t head = 0; ///< the record's bytes before `extra` (xfer version and header)
};

std::optional<ExitRecord> decodeExit(const ObjectRecord& rec, const xfer::TypeXfer& type)
{
    ExitRecord out;
    out.obj = ObjectXfer::parse(rec.data);
    if (out.obj.xferVersion < 0x3C) {
        return std::nullopt; // (older than the typed layouts)
    }
    out.data = xfer::decode(type.xfer, out.obj.xferVersion, out.obj.extra, type.info, out.obj.extraOffset());
    if (!std::holds_alternative<xfer::Exit>(out.data)) {
        return std::nullopt;
    }
    out.head = rec.data.size() - out.obj.extra.size();
    return out;
}

/// An exit's destination: its stored name up to the first NUL (the game's editors store the name
/// with its NUL, and the game reads it as a C string).
std::string_view exitName(const xfer::Exit& x)
{
    const std::string_view s = x.mapName;
    return s.substr(0, s.find('\0'));
}

} // namespace

void replaceScriptObject(MapFile& map, std::span<const std::uint8_t> code, MissingScriptObject missing)
{
    RawSection* raw = map.find(ScriptObject::kName);
    if (!raw) {
        if (missing == MissingScriptObject::Skip) {
            return;
        }
        if (missing == MissingScriptObject::Append) {
            ScriptObject s;
            s.code.assign(code.begin(), code.end());
            map.sections.push_back({std::string(ScriptObject::kName), s.serialize()});
            return;
        }
        throw FormatError("map has no " + std::string(ScriptObject::kName) + " section");
    }
    ScriptObject s = ScriptObject::parse(raw->data);
    s.code.assign(code.begin(), code.end());
    raw->data = s.serialize();
}

std::vector<MapExit> listExits(const MapFile& map, const things::ThingDb& things)
{
    const ExitTypes types = exitTypes(map, things);
    const ObjectData od = decodeSection<ObjectData>(map);
    std::vector<MapExit> out;
    for (std::size_t i = 0; i < od.records.size(); ++i) {
        const ObjectRecord& rec = od.records[i];
        const auto t = types.byId.find(rec.tocId);
        if (t == types.byId.end()) {
            continue;
        }
        const auto e = decodeExit(rec, t->second.first);
        if (!e) {
            continue;
        }
        const auto& x = std::get<xfer::Exit>(e->data);
        out.push_back({i, t->second.second, e->obj.header.extent, e->obj.header.x, e->obj.header.y, std::string(exitName(x)), x.x, x.y});
    }
    return out;
}

std::size_t renameExits(MapFile& map, const things::ThingDb& things,
                        const std::function<std::optional<std::string>(std::string_view)>& rename)
{
    const ExitTypes types = exitTypes(map, things);
    RawSection* raw = map.find(ObjectData::kName);
    if (!raw) {
        throw FormatError("map has no " + std::string(ObjectData::kName) + " section");
    }
    ObjectData od = ObjectData::parse(raw->data);
    if (od.serialize() != raw->data) {
        throw FormatError("map exits: ObjectData doesn't write back as stored");
    }
    std::size_t changed = 0;
    for (ObjectRecord& rec : od.records) {
        const auto t = types.byId.find(rec.tocId);
        if (t == types.byId.end()) {
            continue;
        }
        auto e = decodeExit(rec, t->second.first);
        if (!e) {
            continue;
        }
        auto& x = std::get<xfer::Exit>(e->data);
        const std::string name(exitName(x));
        const auto to = rename(name);
        if (!to || *to == name) {
            continue;
        }
        // (The game reads the stored bytes, the NUL and anything after it too, into its 80-byte
        // buffer: the name with what follows it must fit there.)
        const std::size_t tail = x.mapName.size() - name.size();
        if (to->size() > kMaxExitMapName || to->size() + tail > kMaxExitMapName + 1 || to->find('\0') != std::string::npos) {
            throw FormatError("map exits: the name " + *to + " is longer than " + std::to_string(kMaxExitMapName) +
                              " bytes with what the exit stores after its name, or holds a NUL");
        }
        const xfer::TypeInfo& info = t->second.first.info;
        const std::size_t extraOffset = e->obj.extraOffset();
        if (xfer::encode(e->data, e->obj.xferVersion, info, extraOffset) != e->obj.extra) {
            throw FormatError("map exits: the exit to " + name + " doesn't write back as stored");
        }
        x.mapName = *to + x.mapName.substr(name.size()); // (the stored NUL, and anything after it, kept)
        const auto extra = xfer::encode(e->data, e->obj.xferVersion, info, extraOffset);
        rec.data.resize(e->head);
        rec.data.insert(rec.data.end(), extra.begin(), extra.end());
        rec.size = static_cast<std::uint32_t>(rec.data.size());
        ++changed;
    }
    if (changed) {
        raw->data = od.serialize();
    }
    return changed;
}

std::vector<std::uint8_t> serializeMapKeepingSections(const MapFile& map, const things::ThingDb& things)
{
    MapFile out = map;
    out.crc = computeMapCrc(map, things);
    return serializeMapFile(out);
}

void saveMapKeepingSections(const std::filesystem::path& path, const MapFile& map, const things::ThingDb& things)
{
    auto data = serializeMapKeepingSections(map, things);
    crypto::cipherForKey(crypto::FileKey::Map).encrypt(data);
    writeFileReplacing(path, data, "map save");
}

} // namespace nox::map
