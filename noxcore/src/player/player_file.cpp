// The .plr container (layout in player_file.hpp): decrypt, split into id-tagged raw sections, and
// write them back. Typed section decoding is in sections.cpp.
#include "noxcore/player/player_file.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>

namespace nox::player {

namespace {

constexpr std::size_t kBlock = crypto::Blowfish::kBlockSize;

bool allZero(std::span<const std::uint8_t> b)
{
    return std::all_of(b.begin(), b.end(), [](std::uint8_t v) { return v == 0; });
}

} // namespace

const RawSection* PlayerFile::find(SectionId id) const
{
    for (const auto& s : sections) {
        if (s.id == static_cast<std::uint32_t>(id)) {
            return &s;
        }
    }
    return nullptr;
}

PlayerFile parsePlayerFile(std::span<const std::uint8_t> plain)
{
    ByteReader r(plain, ".plr");
    PlayerFile f;
    // A section has no terminator: the list ends where only zero bytes remain.
    while (!r.atEnd()) {
        const auto rest = plain.subspan(r.offset());
        if (allZero(rest)) {
            // End: zero padding to the cipher block, then possibly whole zero blocks.
            const std::size_t pad = (kBlock - r.offset() % kBlock) % kBlock;
            // Accept up to 255 extra zero blocks (the count is kept in a u8 so the file round-trips).
            if (rest.size() < pad || (rest.size() - pad) % kBlock != 0 || (rest.size() - pad) / kBlock > 0xFF) {
                r.fail("unexpected trailing bytes");
            }
            f.extraZeroBlocks = static_cast<std::uint8_t>((rest.size() - pad) / kBlock);
            break;
        }
        RawSection s;
        s.id = r.u32();
        if (s.id == 0) {
            r.fail("section id 0 before non-zero data");
        }
        const std::size_t padStart = r.offset();
        r.align(kBlock);
        if (!allZero(plain.subspan(padStart, r.offset() - padStart))) {
            r.fail("non-zero padding after section id");
        }
        const std::uint64_t size = r.u64();
        if (size > r.remaining()) {
            r.fail("section larger than the file");
        }
        auto data = r.bytes(static_cast<std::size_t>(size));
        s.data.assign(data.begin(), data.end());
        f.sections.push_back(std::move(s));
    }
    return f;
}

std::vector<std::uint8_t> serializePlayerFile(const PlayerFile& file)
{
    ByteWriter w;
    for (const auto& s : file.sections) {
        if (s.id == 0) {
            throw FormatError(".plr: section id 0");
        }
        w.u32(s.id);
        w.align(kBlock);
        w.u64(s.data.size());
        w.bytes(s.data);
    }
    w.align(kBlock);
    w.zeros(kBlock * file.extraZeroBlocks);
    return std::move(w).take();
}

PlayerFile loadPlayerFile(const std::filesystem::path& path)
{
    auto data = readFileBytes(path);
    if (data.size() % kBlock != 0) {
        throw FormatError(path.string() + ": size is not a multiple of the cipher block");
    }
    crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(data);
    return parsePlayerFile(data);
}

void savePlayerFile(const std::filesystem::path& path, const PlayerFile& file)
{
    auto data = serializePlayerFile(file);
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(data);
    writeFileBytes(path, data);
}

} // namespace nox::player
