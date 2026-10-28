// The .map container (layout in map_file.hpp): Blowfish-decrypt the file, then split it into
// named raw sections. Section contents are decoded separately (sections.hpp, objects.hpp).
#include "noxcore/map/map_file.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>

namespace nox::map {

namespace {

// Every pad in the container aligns to the 8-byte cipher block.
constexpr std::size_t kBlock = crypto::Blowfish::kBlockSize;

bool allZero(std::span<const std::uint8_t> b)
{
    return std::all_of(b.begin(), b.end(), [](std::uint8_t v) { return v == 0; });
}

// Skips to the next 8-byte boundary, insisting the skipped bytes are zero.
void expectZeroPad(ByteReader& r)
{
    const std::size_t rem = r.offset() % kBlock;
    if (rem != 0 && !allZero(r.bytes(kBlock - rem))) {
        r.fail("non-zero alignment padding");
    }
}

} // namespace

const RawSection* MapFile::find(std::string_view name) const
{
    for (const auto& s : sections) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

MapFile parseMapFile(std::span<const std::uint8_t> plain)
{
    ByteReader r(plain, "map header");
    MapFile map;
    map.magic = r.u32();
    if (map.magic == kMagic) {
        expectZeroPad(r);
        map.crc = r.u32();
        expectZeroPad(r);
    } else if (map.magic != kMagicOld) {
        r.fail("unknown map magic");
    }
    map.wallOffsetX = r.u32();
    map.wallOffsetY = r.u32();

    // Sections until a zero name length.
    for (;;) {
        RawSection sect;
        const std::uint8_t nameLen = r.u8();
        if (nameLen == 0) {
            expectZeroPad(r); // end marker, then zero padding to the cipher block
            if (!r.atEnd()) {
                r.fail("data after the end marker");
            }
            break;
        }
        const std::string name = r.rawString(nameLen);
        // The stored length counts the NUL, which must be the name's last byte and its only NUL.
        if (name.back() != '\0' || name.find('\0') != nameLen - 1u) {
            r.fail("malformed section name");
        }
        sect.name = name.substr(0, nameLen - 1u);
        expectZeroPad(r);
        const std::uint64_t size = r.u64();
        if (size > r.remaining()) {
            r.fail("section '" + sect.name + "' is larger than the file");
        }
        auto data = r.bytes(static_cast<std::size_t>(size));
        sect.data.assign(data.begin(), data.end());
        map.sections.push_back(std::move(sect));
    }
    return map;
}

std::vector<std::uint8_t> serializeMapFile(const MapFile& map)
{
    ByteWriter w;
    w.u32(map.magic);
    if (map.magic == kMagic) {
        w.align(kBlock);
        w.u32(map.crc);
        w.align(kBlock);
    }
    w.u32(map.wallOffsetX);
    w.u32(map.wallOffsetY);

    for (const auto& sect : map.sections) {
        if (sect.name.size() + 1 > 0xFF) {
            throw FormatError("map section name too long: " + sect.name);
        }
        w.u8(static_cast<std::uint8_t>(sect.name.size() + 1));
        w.rawString(sect.name);
        w.u8(0);
        w.align(kBlock);
        w.u64(sect.data.size());
        w.bytes(sect.data);
    }
    w.u8(0); // end marker
    w.align(kBlock);
    return std::move(w).take();
}

MapFile loadMapFile(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    // Blowfish works on whole 8-byte blocks, so a valid file is always a multiple of 8.
    if (data.size() % kBlock != 0) {
        throw FormatError(path.string() + ": size is not a multiple of the cipher block");
    }
    crypto::cipherForKey(crypto::FileKey::Map).decrypt(data);
    return parseMapFile(data);
}

} // namespace nox::map
