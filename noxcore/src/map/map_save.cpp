// Map writer that reproduces GAME.EXE's header CRC (see map_save.hpp). Unlike serializeMapFile,
// which copies section bytes as they are, this decodes every section and object and writes them
// back through a CRC-tracking ByteWriter with the game's exact write-call boundaries.
#include "noxcore/map/map_save.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/nxz.hpp"

#include "section_io.hpp"
#include "map_publication.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace nox::map {

namespace {

// The CRC sits in bytes 8..11, in its own cipher block after the magic.
constexpr std::size_t kBlock = 8;
constexpr std::size_t kCrcOffset = 8;

// One pass over the map: re-encodes everything into w_ while the writer accumulates the CRC.
class Walker {
public:
    Walker(const MapFile& map, const things::ThingDb& things)
        : map_(map)
        , things_(things)
    {
    }

    /// Writes the whole file into `w_` (CRC tracking on) and returns it with the CRC patched in.
    std::vector<std::uint8_t> run()
    {
        w_.trackCrc();
        w_.u32(map_.magic);
        if (map_.magic == kMagic) {
            w_.align(kBlock);
            w_.zeros(kBlock); // CRC block, written below the CRC and patched at the end
        }
        w_.u32(map_.wallOffsetX);
        w_.u32(map_.wallOffsetY);

        for (const auto& sect : map_.sections) {
            // The name length and the name (with its NUL) are two separate calls, as the game writes them.
            w_.u8(static_cast<std::uint8_t>(sect.name.size() + 1));
            w_.rawString(sect.name + '\0');
            w_.align(kBlock);
            // The u64 size isn't counted by the CRC; reserve it and patch it once the section is written.
            const std::size_t sizeAt = w_.size();
            w_.uncountedU64(0);
            const std::size_t start = w_.size();
            writeSection(sect);
            const std::uint64_t size = w_.size() - start;
            w_.patchU32(sizeAt, static_cast<std::uint32_t>(size));
            w_.patchU32(sizeAt + 4, static_cast<std::uint32_t>(size >> 32));
        }
        w_.u8(0); // end marker
        w_.align(kBlock);

        crc_ = w_.crc();
        auto out = std::move(w_).take();
        if (map_.magic == kMagic) {
            for (int i = 0; i < 4; ++i) {
                out[kCrcOffset + i] = static_cast<std::uint8_t>(crc_ >> (8 * i));
            }
        }
        return out;
    }

    std::uint32_t crc() const { return crc_; }

private:
    // Writes the section through its typed codec if its name matches S.
    template <class S>
    bool tryTyped(const RawSection& sect)
    {
        if (sect.name != S::kName) {
            return false;
        }
        S::parse(sect.data).write(w_);
        return true;
    }

    void writeSection(const RawSection& sect)
    {
        // Objects need thing.bin (for each type's xfer), so they get their own walk.
        if (sect.name == ObjectData::kName) {
            writeObjectData(sect);
            return;
        }
        const bool typed = tryTyped<MapInfo>(sect) || tryTyped<WallMap>(sect) || tryTyped<FloorMap>(sect) ||
                           tryTyped<SecretWalls>(sect) || tryTyped<DestructableWalls>(sect) ||
                           tryTyped<Waypoints>(sect) || tryTyped<DebugData>(sect) || tryTyped<WindowWalls>(sect) ||
                           tryTyped<GroupData>(sect) || tryTyped<ScriptObject>(sect) ||
                           tryTyped<AmbientData>(sect) || tryTyped<Polygons>(sect) || tryTyped<MapIntro>(sect) ||
                           tryTyped<ScriptData>(sect) || tryTyped<ObjectTOC>(sect);
        if (!typed) {
            throw FormatError("map CRC: unknown section " + sect.name);
        }
    }

    // Maps each ObjectTOC id to its type's XFER attribute (DefaultXfer when absent) and class bits.
    void loadTypes()
    {
        for (const auto& e : decodeSection<ObjectTOC>(map_).entries) {
            const things::ThingDef* t = things_.findThing(e.name);
            if (!t) {
                throw FormatError("map CRC: object type " + e.name + " is not in thing.bin");
            }
            types_[e.id] = xfer::typeXferOf(*t);
        }
    }

    /// The xfer a record is read with: a generator template's by its type name (`templateType`),
    /// any other record's by its ObjectTOC id.
    const xfer::TypeXfer& recordType(const ObjectRecord& rec, std::string_view templateType)
    {
        if (!templateType.empty()) {
            const std::string name(templateType);
            auto it = templateTypes_.find(name);
            if (it == templateTypes_.end()) {
                const things::ThingDef* t = things_.findThing(name);
                if (!t) {
                    throw FormatError("map CRC: generator template type " + name + " is not in thing.bin");
                }
                it = templateTypes_.emplace(name, xfer::typeXferOf(*t)).first;
            }
            return it->second;
        }
        const auto it = types_.find(rec.tocId);
        if (it == types_.end()) {
            throw FormatError("map CRC: object record with unknown type id " + std::to_string(rec.tocId));
        }
        return it->second;
    }

    /// An object record's data, field by field (xfer version, header, typed xfer).
    void writeRecordData(const ObjectRecord& rec, std::string_view templateType = {})
    {
        const xfer::TypeXfer& type = recordType(rec, templateType);
        const ObjectXfer obj = ObjectXfer::parse(rec.data);
        const std::size_t extraOffset = obj.extraOffset();
        const auto data = xfer::decode(type.xfer, obj.xferVersion, obj.extra, type.info, extraOffset);
        w_.u16(obj.xferVersion);
        writeObjectHeader(w_, obj.header);
        // Generator templates are records inside a record: the callback re-encodes them the same way.
        xfer::write(w_, data, obj.xferVersion, type.info, extraOffset,
                    [this](ByteWriter&, const ObjectRecord& nested, std::string_view nestedType) { writeRecordData(nested, nestedType); });
    }

    void writeObjectData(const RawSection& sect)
    {
        loadTypes();
        const ObjectData od = ObjectData::parse(sect.data);
        w_.u16(od.version);
        for (const auto& rec : od.records) {
            w_.u16(rec.tocId);
            detail::writeRecordPadding(w_, rec); // (ObjectData::write's rule)
            const std::size_t sizeAt = w_.size();
            w_.uncountedU32(0);
            w_.uncountedU32(rec.sizeHi);
            const std::size_t start = w_.size();
            writeRecordData(rec);
            w_.patchU32(sizeAt, static_cast<std::uint32_t>(w_.size() - start));
        }
        w_.u16(0);
    }

    const MapFile& map_;
    const things::ThingDb& things_;
    std::map<std::uint16_t, xfer::TypeXfer> types_;      // by ObjectTOC id
    std::map<std::string, xfer::TypeXfer> templateTypes_; // generator templates', by type name
    ByteWriter w_;
    std::uint32_t crc_ = 0;
};

} // namespace

std::uint32_t computeMapCrc(const MapFile& map, const things::ThingDb& things)
{
    Walker walker(map, things);
    walker.run();
    return walker.crc();
}

std::vector<std::uint8_t> serializeMapFileWithCrc(const MapFile& map, const things::ThingDb& things)
{
    return Walker(map, things).run();
}

void saveMapFileWithCrc(const std::filesystem::path& path, const MapFile& map, const things::ThingDb& things)
{
    auto data = serializeMapFileWithCrc(map, things);
    crypto::cipherForKey(crypto::FileKey::Map).encrypt(data);
    writeFileBytes(path, data);
}

EncodedMapFiles encodeMapBytes(std::vector<std::uint8_t> plain)
{
    EncodedMapFiles out;
    out.map = std::move(plain);
    crypto::cipherForKey(crypto::FileKey::Map).encrypt(out.map); // (as saveMapFileWithCrc stores it)
    const auto size = static_cast<std::uint32_t>(out.map.size());
    out.nxz = {static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size >> 16),
               static_cast<std::uint8_t>(size >> 24)};
    const auto packed = video::nxzCompress(out.map);
    out.nxz.insert(out.nxz.end(), packed.begin(), packed.end());
    return out;
}

void writeFileReplacing(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string_view who)
{
    const auto tmp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            throw FormatError(std::string(who) + ": can't write " + pathUtf8(tmp));
        }
    }
    std::filesystem::rename(tmp, path);
}

std::filesystem::path detail::publishMap(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& filesToSave,
                                       std::string_view who, std::optional<std::string_view> sidecar,
                                       std::filesystem::path& retainedCleanup, ReplaceFile replace, RemoveTree cleanup)
{
    namespace fs = std::filesystem;
    retainedCleanup.clear();
    const fs::path dir = mapsDir / std::string(name);
    fs::create_directories(dir);
    const auto target = [&](const char* ext) { return dir / (std::string(name) + ext); };
    struct File {
        fs::path path;
        std::span<const std::uint8_t> data;
        bool hadOriginal = false;
    };
    std::vector<File> files{{target(".map"), filesToSave.map}, {target(".nxz"), filesToSave.nxz}};
    if (sidecar) {
        files.push_back({target(".nxe"), {reinterpret_cast<const std::uint8_t*>(sidecar->data()), sidecar->size()}});
    }

    // Exclusive across processes. A retained directory also protects backups after
    // an interrupted save or unsuccessful rollback.
    const fs::path stage = dir / ".nox-save";
    if (!fs::create_directory(stage)) {
        throw FormatError(std::string(who) + ": unfinished or concurrent save in " + pathUtf8(stage));
    }
    std::size_t published = 0;
    try {
        fs::create_directory(stage / "previous");
        for (auto& f : files) {
            const auto type = fs::symlink_status(f.path).type();
            if (type != fs::file_type::not_found) {
                if (type != fs::file_type::regular) {
                    throw FormatError(std::string(who) + ": not a regular file: " + pathUtf8(f.path));
                }
                const auto backup = stage / "previous" / f.path.filename();
                std::error_code ec;
                fs::create_hard_link(f.path, backup, ec);
                if (ec) fs::copy_file(f.path, backup);
                f.hadOriginal = true;
            }
            writeFileBytes(stage / f.path.filename(), f.data);
        }
        for (const auto& f : files) {
            replace(stage / f.path.filename(), f.path);
            ++published;
        }
    } catch (...) {
        std::error_code rollbackError;
        while (published > 0) {
            const auto& f = files[--published];
            std::error_code ec;
            if (f.hadOriginal) fs::rename(stage / "previous" / f.path.filename(), f.path, ec);
            else fs::remove(f.path, ec);
            if (ec) rollbackError = ec;
        }
        if (rollbackError) throw fs::filesystem_error("cannot restore map; backups retained", stage, rollbackError);
        fs::remove_all(stage);
        throw;
    }
    // Publication is complete. A cleanup error must not turn it into a failed save.
    std::error_code ec;
    cleanup(stage, ec);
    if (ec) {
        retainedCleanup = stage;
        // Move any leftovers away from the lock name so a later save can proceed.
        // We still own the lock; only this writer can choose a new cleanup name.
        for (unsigned i = 1; i != 0; ++i) {
            const fs::path spare = dir / (".nox-save-cleanup-" + std::to_string(i));
            const auto st = fs::symlink_status(spare, ec);
            if (st.type() == fs::file_type::not_found) {
                fs::rename(stage, spare, ec);
                if (!ec) retainedCleanup = spare;
                break;
            }
            if (ec) break;
        }
    }
    return files.front().path;
}

std::filesystem::path writeMapFolder(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& files,
                                     std::string_view who, std::optional<std::string_view> sidecar, std::filesystem::path* retainedCleanup)
{
    std::filesystem::path retained;
    auto map = detail::publishMap(mapsDir, name, files, who, sidecar, retained, std::filesystem::rename, std::filesystem::remove_all);
    if (!retained.empty()) std::clog << who << ": saved map; cleanup required in " << pathUtf8(retained) << '\n';
    if (retainedCleanup) *retainedCleanup = std::move(retained);
    return map;
}

} // namespace nox::map
