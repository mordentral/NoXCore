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

#include <algorithm>
#include <array>
#include <cerrno>
#include <map>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

void writeFileReplacing(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string_view /*who*/)
{
    replaceFileBytes(path, bytes, ".tmp"); // (its error names the file)
}

namespace {

namespace fs = std::filesystem;

// An editor save's staging folder, in the map's folder: the lock, and one numbered folder per
// save with its new files (<file>.new), the old ones (<file>.old) and the commit mark, put there
// once every file is written whole, just before the first is moved in. (Never named .map: the
// scans for a folder's maps don't find them.)
constexpr std::string_view kStageFolder = ".nox-save";
constexpr std::string_view kLockFile = "lock";
constexpr std::string_view kCommitMark = "commit";

fs::path newFile(const fs::path& attempt, const std::string& name)
{
    return attempt / (name + ".new");
}

fs::path oldFile(const fs::path& attempt, const std::string& name)
{
    return attempt / (name + ".old");
}

// One save of a map at a time, from any process: the staging folder's lock file, held open for
// the whole save (on Windows shared with no one, elsewhere an flock). The system lets go when the
// process ends, killed or not, so a lock file left behind never blocks a save: only a save under
// way does.
class SaveLock {
public:
    explicit SaveLock(const fs::path& stage) : stage_(stage), file_(stage / kLockFile)
    {
        // (Tried again when the folder went: a save ending beside this one removes it.)
        for (int tries = 0; tries < 4 && fd_ < 0; ++tries) {
            std::error_code ec;
            fs::create_directories(stage, ec);
#ifdef _WIN32
            const errno_t err = _wsopen_s(&fd_, file_.c_str(), _O_CREAT | _O_RDWR | _O_BINARY | _O_NOINHERIT, _SH_DENYRW,
                                          _S_IREAD | _S_IWRITE);
            if (err != 0) {
                fd_ = -1;
                if (err != ENOENT) {
                    return;
                }
            }
#else
            const int fd = ::open(file_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
            if (fd < 0) {
                if (errno != ENOENT) {
                    return;
                }
                continue;
            }
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                ::close(fd);
                return;
            }
            // (A save that ended meanwhile removed the file this one locked: lock the new one.)
            struct stat held{};
            struct stat named{};
            if (::fstat(fd, &held) == 0 && ::stat(file_.c_str(), &named) == 0 && held.st_dev == named.st_dev &&
                held.st_ino == named.st_ino) {
                fd_ = fd;
            } else {
                ::close(fd);
            }
#endif
        }
    }
    ~SaveLock() { release(); }
    SaveLock(const SaveLock&) = delete;
    SaveLock& operator=(const SaveLock&) = delete;

    bool held() const { return fd_ >= 0; }
    const fs::path& file() const { return file_; }

    /// Lets go and removes the lock file (elsewhere than Windows removed while still held, so the
    /// next save locks a new file, never this one), then the staging folder when that leaves it
    /// empty.
    void release()
    {
        if (fd_ < 0) {
            return;
        }
        std::error_code ec;
#ifdef _WIN32
        _close(fd_);
        fs::remove(file_, ec); // (fails, harmlessly, when the next save holds it already)
#else
        fs::remove(file_, ec);
        ::close(fd_);
#endif
        fd_ = -1;
        fs::remove(stage_, ec); // (only when empty: what a save left stays for the next)
    }

private:
    fs::path stage_;
    fs::path file_;
    int fd_ = -1;
};

// The files a save of `base` stages, in the order they're moved in.
std::array<std::string, 3> stagedNames(const std::string& base)
{
    return {base + ".map", base + ".nxz", base + ".nxe"};
}

// Finishes a save that was killed while moving its files in (it has the commit mark): moves in
// what it still has staged, so the folder holds that save's whole set. Throws when a file can't
// be moved (held open at the moment): this save fails, and the next one tries again.
void finishEarlierSave(const fs::path& attempt, const fs::path& dir, const std::string& base, const std::string& who)
{
    for (const auto& n : stagedNames(base)) {
        const fs::path staged = newFile(attempt, n);
        if (fs::symlink_status(staged).type() != fs::file_type::regular) {
            continue;
        }
        try {
            fs::rename(staged, dir / n);
        } catch (const fs::filesystem_error& e) {
            throw FormatError(who + ": can't finish an earlier save of " + base + ": " + e.what());
        }
    }
}

} // namespace

std::filesystem::path writeMapFolder(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& files,
                                     std::string_view who)
{
    const std::filesystem::path dir = mapsDir / std::string(name);
    std::filesystem::create_directories(dir);
    const std::filesystem::path mapPath = dir / (std::string(name) + ".map");
    writeFileReplacing(mapPath, files.map, who);
    writeFileReplacing(dir / (std::string(name) + ".nxz"), files.nxz, who);
    return mapPath;
}

PublishedMapFolder publishMapFolder(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& files,
                                    std::optional<std::string_view> sidecar, std::string_view who)
{
    const std::string base(name);
    const std::string by(who);
    const fs::path dir = mapsDir / base;
    fs::create_directories(dir);
    const fs::path stage = dir / kStageFolder;
    SaveLock lock(stage);
    if (!lock.held()) {
        throw FormatError(by + ": another save of " + base + " is under way (" + pathUtf8(lock.file()) + " is held)");
    }

    // What earlier saves left: one killed while moving its files in is finished, the rest dropped.
    std::vector<std::pair<unsigned long, fs::path>> earlier;
    for (const auto& e : fs::directory_iterator(stage)) {
        const std::string n = pathUtf8(e.path().filename());
        if (n == kLockFile) {
            continue;
        }
        if (!n.empty() && n.size() < 10 && std::ranges::all_of(n, [](char c) { return c >= '0' && c <= '9'; }) && e.is_directory()) {
            earlier.emplace_back(std::stoul(n), e.path());
        } else {
            std::error_code ec;
            fs::remove_all(e.path(), ec);
        }
    }
    std::ranges::sort(earlier);
    unsigned long number = 1;
    for (const auto& [n, attempt] : earlier) {
        if (fs::exists(attempt / kCommitMark)) {
            finishEarlierSave(attempt, dir, base, by);
        }
        // (What can't be removed is harmless: nothing staged is left in it, and this save takes
        // the next number.)
        std::error_code ec;
        fs::remove_all(attempt, ec);
        number = n + 1;
    }

    const auto names = stagedNames(base);
    struct File {
        std::string name;
        std::span<const std::uint8_t> bytes;
        bool hadOld = false;
    };
    std::vector<File> list{{names[0], files.map}, {names[1], files.nxz}};
    if (sidecar) {
        list.push_back({names[2], {reinterpret_cast<const std::uint8_t*>(sidecar->data()), sidecar->size()}});
    }
    for (auto& f : list) {
        const auto type = fs::symlink_status(dir / f.name).type();
        if (type != fs::file_type::not_found && type != fs::file_type::regular) {
            throw FormatError(by + ": " + pathUtf8(dir / f.name) + " isn't a file");
        }
        f.hadOld = type == fs::file_type::regular;
    }

    // Staged: each new file whole, and the old one kept (a second name for it, or a copy where
    // the drive has no hard links).
    const fs::path attempt = stage / std::to_string(number);
    fs::create_directory(attempt);
    try {
        for (const auto& f : list) {
            if (f.hadOld) {
                const fs::path keep = oldFile(attempt, f.name);
                std::error_code ec;
                fs::create_hard_link(dir / f.name, keep, ec);
                if (ec) {
                    fs::copy_file(dir / f.name, keep);
                }
            }
            writeFileBytes(newFile(attempt, f.name), f.bytes);
        }
        writeFileBytes(attempt / kCommitMark, {});
    } catch (...) {
        std::error_code ec;
        fs::remove_all(attempt, ec);
        throw;
    }

    std::size_t moved = 0;
    try {
        for (; moved < list.size(); ++moved) {
            fs::rename(newFile(attempt, list[moved].name), dir / list[moved].name);
        }
    } catch (...) {
        // Back as it was: each moved file returned to the staging folder, then the old one put
        // back. Stopped part way (or killed), the commit mark stays, and the next save first
        // finishes this one (its whole new set) instead.
        bool back = true;
        while (back && moved > 0) {
            const File& f = list[--moved];
            std::error_code ec;
            fs::rename(dir / f.name, newFile(attempt, f.name), ec);
            if (ec) {
                back = false;
            } else if (f.hadOld) {
                fs::rename(oldFile(attempt, f.name), dir / f.name, ec);
                if (ec) {
                    fs::rename(newFile(attempt, f.name), dir / f.name, ec); // (the new one again, rather than none)
                    back = false;
                }
            }
        }
        if (!back) {
            throw FormatError(by + ": the save of " + base + " failed and the old files couldn't all be put back: the next save finishes it (" +
                              pathUtf8(attempt) + ")");
        }
        std::error_code ec;
        fs::remove(attempt / kCommitMark, ec);
        if (!ec) {
            fs::remove_all(attempt, ec);
        }
        throw;
    }

    PublishedMapFolder out;
    out.map = dir / list.front().name;
    std::error_code ec;
    fs::remove_all(attempt, ec);
    if (ec) {
        out.leftovers = attempt;
    }
    return out; // (the lock let go, and the staging folder removed when empty)
}

} // namespace nox::map
