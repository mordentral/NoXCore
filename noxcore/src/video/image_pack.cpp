// The image pack (.nxp) reader and writer; the file layout and codecs are described in image_pack.hpp.
#include "noxcore/video/image_pack.hpp"

#include "noxcore/crypto/crc.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/util/text.hpp"

#include <zstd.h>

// bcdec: kCodecBc7's CPU decode (the implementation is compiled here, the one user).
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include <bcdec.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>

namespace nox::video {

namespace {

constexpr std::array<char, 8> kMagic{'N', 'O', 'X', 'P', 'A', 'C', 'K', '\x1A'};
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kEntrySize = 64;
/// The largest side and raw size a pack's image may have (the reader refuses bigger entries).
constexpr std::uint32_t kMaxPackSide = 16384;
constexpr std::uint32_t kMaxPackRaw = 256u << 20;

// As VideoBag::findImage: case-insensitive, without the extension.
using util::normalizeName;

std::size_t pixelsOf(std::uint32_t w, std::uint32_t h)
{
    return static_cast<std::size_t>(w) * h;
}

/// Raw size for an entry's planes at its stored resolution (kCodecBc7: its levels' blocks instead
/// of RGBA).
std::size_t rawSizeOf(std::uint32_t w, std::uint32_t h, std::uint8_t planes, std::uint8_t codec = 0,
                      std::uint8_t levels = 0)
{
    const std::size_t n = pixelsOf(w, h);
    std::size_t colour = planes & kPlaneRgba ? n * 4 : 0;
    if (codec == kCodecBc7) {
        colour = 0;
        for (const auto& l : bc7Levels(w, h, levels)) {
            colour += l.size;
        }
    }
    return colour + n * ((planes & kPlaneMaterial ? 1 : 0) + (planes & kPlaneEdge ? 1 : 0));
}

// One 64-byte index entry, in the field order image_pack.hpp lists.
void writeEntry(ByteWriter& w, const PackEntry& e)
{
    w.u64(e.offset);
    w.u32(e.storedSize);
    w.u32(e.rawSize);
    w.u32(e.crc);
    w.u32(e.nameOffset);
    w.u16(e.nameLength);
    w.u8(e.type);
    w.u8(e.headerByte);
    w.i32(e.width);
    w.i32(e.height);
    w.i32(e.offsetX);
    w.i32(e.offsetY);
    w.u32(e.pixelWidth);
    w.u32(e.pixelHeight);
    w.u8(e.planes);
    w.u8(e.codec);
    w.u16(e.flags);
    w.u8(e.levels);
    w.zeros(7);
}

PackEntry readEntry(ByteReader& r)
{
    PackEntry e;
    e.offset = r.u64();
    e.storedSize = r.u32();
    e.rawSize = r.u32();
    e.crc = r.u32();
    e.nameOffset = r.u32();
    e.nameLength = r.u16();
    e.type = r.u8();
    e.headerByte = r.u8();
    e.width = r.i32();
    e.height = r.i32();
    e.offsetX = r.i32();
    e.offsetY = r.i32();
    e.pixelWidth = r.u32();
    e.pixelHeight = r.u32();
    e.planes = r.u8();
    e.codec = r.u8();
    e.flags = r.u16();
    e.levels = r.u8(); // 0 in packs before version 3 (reserved, written as zero)
    r.skip(7);
    return e;
}

/// The gradient filter on one w x h plane, in place: each byte becomes itself minus
/// left + up - upleft (mod 256, 0 outside the plane). Backwards, so the neighbours are still raw.
void gradientFilter(std::uint8_t* p, std::size_t w, std::size_t h)
{
    for (std::size_t y = h; y-- > 0;) {
        std::uint8_t* row = p + y * w;
        const std::uint8_t* up = y > 0 ? row - w : nullptr;
        for (std::size_t x = w; x-- > 0;) {
            const unsigned left = x > 0 ? row[x - 1] : 0;
            const unsigned above = up ? up[x] : 0;
            const unsigned upLeft = up && x > 0 ? up[x - 1] : 0;
            row[x] = static_cast<std::uint8_t>(row[x] - left - above + upLeft);
        }
    }
}

/// Undoes gradientFilter: a running sum along each row gives each byte minus the one above,
/// then the row above (already undone) is added.
void gradientUnfilter(std::uint8_t* p, std::size_t w, std::size_t h)
{
    for (std::size_t y = 0; y < h; ++y) {
        std::uint8_t* row = p + y * w;
        std::uint8_t sum = 0;
        for (std::size_t x = 0; x < w; ++x) {
            sum = static_cast<std::uint8_t>(sum + row[x]);
            row[x] = sum;
        }
        if (y > 0) {
            const std::uint8_t* up = row - w;
            for (std::size_t x = 0; x < w; ++x) {
                row[x] = static_cast<std::uint8_t>(row[x] + up[x]);
            }
        }
    }
}

/// gradientUnfilter on the R, G, B and A planes and interleaving them into `rgba`, in one pass:
/// the four running sums are independent, so they go through the CPU side by side.
void gradientUnfilterInterleave(const std::uint8_t* planes, std::size_t w, std::size_t h, std::uint8_t* rgba)
{
    const std::size_t n = w * h;
    const std::uint8_t* pr = planes;
    const std::uint8_t* pg = planes + n;
    const std::uint8_t* pb = planes + 2 * n;
    const std::uint8_t* pa = planes + 3 * n;
    for (std::size_t y = 0; y < h; ++y) {
        std::uint8_t* out = rgba + y * w * 4;
        const std::size_t at = y * w;
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
        for (std::size_t x = 0; x < w; ++x) {
            r = static_cast<std::uint8_t>(r + pr[at + x]);
            g = static_cast<std::uint8_t>(g + pg[at + x]);
            b = static_cast<std::uint8_t>(b + pb[at + x]);
            a = static_cast<std::uint8_t>(a + pa[at + x]);
            out[x * 4 + 0] = r;
            out[x * 4 + 1] = g;
            out[x * 4 + 2] = b;
            out[x * 4 + 3] = a;
        }
        if (y > 0) {
            const std::uint8_t* up = out - w * 4;
            for (std::size_t i = 0; i < w * 4; ++i) {
                out[i] = static_cast<std::uint8_t>(out[i] + up[i]);
            }
        }
    }
}

std::vector<std::uint8_t> zstdCompress(std::span<const std::uint8_t> raw, int level)
{
    std::vector<std::uint8_t> out(ZSTD_compressBound(raw.size()));
    const std::size_t size = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), level);
    if (ZSTD_isError(size)) {
        throw FormatError(std::string("image pack: zstd: ") + ZSTD_getErrorName(size));
    }
    out.resize(size);
    return out;
}

// Reads exactly out.size() bytes at `offset`; throws FormatError on a short read.
void readAt(std::ifstream& file, std::uint64_t offset, std::span<std::uint8_t> out)
{
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file || static_cast<std::size_t>(file.gcount()) != out.size()) {
        throw FormatError("image pack: read past the end of the file");
    }
}

} // namespace

std::uint32_t packCrc(std::span<const std::uint8_t> data)
{
    return crypto::updateCrc(crypto::kCrcInit, data);
}

std::vector<Bc7Level> bc7Levels(std::uint32_t width, std::uint32_t height, std::uint32_t levels)
{
    std::vector<Bc7Level> out;
    out.reserve(levels);
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < levels; ++i) {
        Bc7Level l;
        l.width = width;
        l.height = height;
        l.paddedWidth = (width + 3) / 4 * 4;
        l.paddedHeight = (height + 3) / 4 * 4;
        l.offset = offset;
        l.size = static_cast<std::size_t>(l.paddedWidth / 4) * (l.paddedHeight / 4) * 16;
        offset += l.size;
        out.push_back(l);
        width = (width + 1) / 2; // as noxsdl's halve()
        height = (height + 1) / 2;
    }
    return out;
}

std::vector<std::uint8_t> decodeBc7(std::span<const std::uint8_t> blocks, const Bc7Level& level)
{
    if (blocks.size() != level.size) {
        throw FormatError("image pack: BC7 blocks don't match their level's size");
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(level.width) * level.height * 4);
    std::array<std::uint8_t, 4 * 4 * 4> block{};
    const std::uint32_t bx = level.paddedWidth / 4;
    for (std::uint32_t by = 0; by < level.paddedHeight / 4; ++by) {
        for (std::uint32_t x4 = 0; x4 < bx; ++x4) {
            bcdec_bc7(blocks.data() + (static_cast<std::size_t>(by) * bx + x4) * 16, block.data(), 4 * 4);
            // The block's pixels inside the level (the padding is dropped).
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < level.height; ++y) {
                const std::uint32_t w = std::min<std::uint32_t>(4, level.width - x4 * 4);
                std::memcpy(out.data() + ((static_cast<std::size_t>(by) * 4 + y) * level.width + x4 * 4) * 4,
                            block.data() + y * 16, static_cast<std::size_t>(w) * 4);
            }
        }
    }
    return out;
}

// Copies a decoded video.bag image; the stored resolution equals the logical size.
PackImage PackImage::fromDecoded(const DecodedImage& image, std::string name)
{
    PackImage p;
    p.name = std::move(name);
    p.type = image.type;
    p.headerByte = image.unknown;
    p.width = image.width;
    p.height = image.height;
    p.offsetX = image.offsetX;
    p.offsetY = image.offsetY;
    p.pixelWidth = image.width;
    p.pixelHeight = image.height;
    p.rgba = image.rgba;
    p.material = image.material;
    p.edgeMask = image.edgeMask;
    return p;
}

EncodedImage encodePackImage(const PackImage& image, int level, bool gradient)
{
    // Check the sizes first: RGBA must be exactly pixelWidth x pixelHeight x 4, the optional planes one byte a pixel.
    if (image.pixelWidth < 0 || image.pixelHeight < 0 || image.width < 0 || image.height < 0) {
        throw FormatError("image pack: negative size for " + image.name);
    }
    const auto pw = static_cast<std::uint32_t>(image.pixelWidth);
    const auto ph = static_cast<std::uint32_t>(image.pixelHeight);
    const std::size_t n = pixelsOf(pw, ph);
    const bool bc7 = !image.bc7.empty();
    if ((!bc7 && image.rgba.size() != n * 4) || (!image.material.empty() && image.material.size() != n) ||
        (!image.edgeMask.empty() && image.edgeMask.size() != n)) {
        throw FormatError("image pack: planes don't match the size of " + image.name);
    }
    if (image.name.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw FormatError("image pack: name too long");
    }

    // Copy the descriptive fields into the index entry; the plane bits record which optional planes exist.
    EncodedImage out;
    out.name = image.name;
    PackEntry& e = out.entry;
    e.type = image.type;
    e.headerByte = image.headerByte;
    e.width = image.width;
    e.height = image.height;
    e.offsetX = image.offsetX;
    e.offsetY = image.offsetY;
    e.pixelWidth = pw;
    e.pixelHeight = ph;
    e.planes = static_cast<std::uint8_t>(kPlaneRgba | (image.material.empty() ? 0 : kPlaneMaterial) |
                                         (image.edgeMask.empty() ? 0 : kPlaneEdge));
    e.flags = kEntryPresent;

    // BC7 colour: the levels' blocks back to back, then the material and edge planes, one zstd frame.
    if (bc7) {
        if (image.bc7.size() > 255) {
            throw FormatError("image pack: too many BC7 levels for " + image.name);
        }
        const auto levels = bc7Levels(pw, ph, static_cast<std::uint32_t>(image.bc7.size()));
        std::vector<std::uint8_t> raw;
        raw.reserve(rawSizeOf(pw, ph, e.planes, kCodecBc7, static_cast<std::uint8_t>(levels.size())));
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (image.bc7[i].size() != levels[i].size) {
                throw FormatError("image pack: BC7 level " + std::to_string(i) + " doesn't match the size of " + image.name);
            }
            raw.insert(raw.end(), image.bc7[i].begin(), image.bc7[i].end());
        }
        raw.insert(raw.end(), image.material.begin(), image.material.end());
        raw.insert(raw.end(), image.edgeMask.begin(), image.edgeMask.end());
        if (raw.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw FormatError("image pack: image too large: " + image.name);
        }
        e.codec = kCodecBc7;
        e.levels = static_cast<std::uint8_t>(levels.size());
        e.rawSize = static_cast<std::uint32_t>(raw.size());
        e.crc = packCrc(raw);
        out.stored = zstdCompress(raw, std::max(level, 1));
        e.storedSize = static_cast<std::uint32_t>(out.stored.size());
        return out;
    }

    // Planar: R, G, B, A, then the material and edge planes.
    std::vector<std::uint8_t> raw(rawSizeOf(pw, ph, e.planes));
    for (std::size_t c = 0; c < 4; ++c) {
        std::uint8_t* plane = raw.data() + c * n;
        for (std::size_t i = 0; i < n; ++i) {
            plane[i] = image.rgba[i * 4 + c];
        }
    }
    std::size_t at = n * 4;
    if (!image.material.empty()) {
        std::memcpy(raw.data() + at, image.material.data(), n);
        at += n;
    }
    if (!image.edgeMask.empty()) {
        std::memcpy(raw.data() + at, image.edgeMask.data(), n);
    }
    if (raw.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError("image pack: image too large: " + image.name);
    }
    // The CRC is always of the raw, unfiltered planes, whatever codec ends up storing them.
    e.rawSize = static_cast<std::uint32_t>(raw.size());
    e.crc = packCrc(raw);

    // Level 0 (or an empty image) stores the planes uncompressed.
    if (level <= 0 || raw.empty()) {
        e.codec = kCodecStored;
        out.stored = std::move(raw);
    } else {
        out.stored = zstdCompress(raw, level);
        e.codec = kCodecZstd;
        // Try the gradient-filtered planes too and keep whichever compresses smaller.
        if (gradient) {
            std::vector<std::uint8_t> filtered = raw;
            // Only the R, G, B and A planes are filtered; material and edge stay as they are.
            for (std::size_t c = 0; c < 4; ++c) {
                gradientFilter(filtered.data() + c * n, pw, ph);
            }
            std::vector<std::uint8_t> g = zstdCompress(filtered, level);
            if (g.size() < out.stored.size()) {
                out.stored = std::move(g);
                e.codec = kCodecZstdGradient;
            }
        }
        if (out.stored.size() >= raw.size()) {
            e.codec = kCodecStored; // incompressible: keep it plain
            out.stored = std::move(raw);
        }
    }
    e.storedSize = static_cast<std::uint32_t>(out.stored.size());
    return out;
}

// ------------------------------------------------------------------------------------ reader

// Reads the 64-byte header, then the names and the 64-byte index entries; image data is read on demand.
ImagePackReader::ImagePackReader(const std::filesystem::path& path)
    : file_(path, std::ios::binary)
{
    if (!file_) {
        throw std::system_error(errno, std::generic_category(), "cannot open " + path.string());
    }
    file_.seekg(0, std::ios::end);
    fileSize_ = static_cast<std::uint64_t>(std::max<std::streamoff>(0, file_.tellg()));
    std::array<std::uint8_t, kHeaderSize> header{};
    readAt(file_, 0, header);
    ByteReader h(header, "image pack header");
    const auto magic = h.bytes(kMagic.size());
    if (std::memcmp(magic.data(), kMagic.data(), kMagic.size()) != 0) {
        throw FormatError(path.string() + " is not an image pack");
    }
    // Version 1 packs have no gradient codec but are otherwise the same.
    const std::uint32_t version = h.u32();
    if (version < 1 || version > kPackVersion) {
        throw FormatError("image pack version " + std::to_string(version) + " (this reads " +
                          std::to_string(kPackVersion) + ")");
    }
    const std::uint32_t count = h.u32();
    const std::uint64_t indexOffset = h.u64();
    const std::uint64_t namesOffset = h.u64();
    const std::uint32_t namesSize = h.u32();
    sourceImages_ = h.u32();
    sourceIdxCrc_ = h.u32();

    // (The names and the index checked against the file before their buffers are made: the
    // header's counts are a u32 and the offsets a u64 each.)
    const auto within = [&](std::uint64_t offset, std::uint64_t size) { return offset <= fileSize_ && size <= fileSize_ - offset; };
    if (!within(namesOffset, namesSize) || !within(indexOffset, std::uint64_t{count} * kEntrySize)) {
        throw FormatError("image pack: index or names past the end of the file");
    }
    names_.resize(namesSize);
    readAt(file_, namesOffset, {reinterpret_cast<std::uint8_t*>(names_.data()), names_.size()});
    std::vector<std::uint8_t> index(static_cast<std::size_t>(count) * kEntrySize);
    readAt(file_, indexOffset, index);
    ByteReader r(index, "image pack index");
    entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        entries_.push_back(readEntry(r));
        const PackEntry& e = entries_.back();
        // Present entries must name within the names block and match their plane sizes. (Their
        // sides at most kMaxPackSide and their raw bytes kMaxPackRaw: past those the sizes' product
        // wraps, or one image would want gigabytes; no image the game draws comes near.)
        if (e.present() && (static_cast<std::size_t>(e.nameOffset) + e.nameLength > names_.size() ||
                            (e.codec == kCodecBc7 && e.levels == 0) || e.pixelWidth > kMaxPackSide ||
                            e.pixelHeight > kMaxPackSide || e.rawSize > kMaxPackRaw ||
                            e.rawSize != rawSizeOf(e.pixelWidth, e.pixelHeight, e.planes, e.codec, e.levels))) {
            throw FormatError("image pack: bad index entry " + std::to_string(i));
        }
    }
}

std::string_view ImagePackReader::name(std::size_t index) const
{
    const PackEntry& e = entries_.at(index);
    return std::string_view(names_).substr(e.nameOffset, e.nameLength);
}

std::vector<std::uint8_t> ImagePackReader::readStored(std::size_t index)
{
    const PackEntry& e = entries_.at(index);
    if (e.present() && (e.offset > fileSize_ || e.storedSize > fileSize_ - e.offset)) {
        throw FormatError("image pack: slot " + std::to_string(index) + " runs past the end of the file");
    }
    std::vector<std::uint8_t> stored(e.present() ? e.storedSize : 0);
    if (!stored.empty()) {
        readAt(file_, e.offset, stored);
    }
    return stored;
}

std::optional<PackImage> ImagePackReader::read(std::size_t index, bool verify, bool blocksOnly)
{
    const PackEntry& e = entries_.at(index);
    if (!e.present()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> stored = readStored(index);
    // Decompress (or take as it is) into the raw planar bytes, which must come out at exactly rawSize.
    std::vector<std::uint8_t> raw;
    if (e.codec == kCodecStored) {
        raw = std::move(stored);
    } else if (e.codec == kCodecZstd || e.codec == kCodecZstdGradient || e.codec == kCodecBc7) {
        // (The frame says what it holds (the writer's ZSTD_compress always does): checked before
        // the buffer is made, as an entry's sizes can say gigabytes.)
        if (ZSTD_getFrameContentSize(stored.data(), stored.size()) != e.rawSize) {
            throw FormatError("image pack: slot " + std::to_string(index) + " doesn't decompress");
        }
        raw.resize(e.rawSize);
        const std::size_t size = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
        if (ZSTD_isError(size) || size != raw.size()) {
            throw FormatError("image pack: slot " + std::to_string(index) + " doesn't decompress");
        }
    } else {
        throw FormatError("image pack: unknown codec " + std::to_string(e.codec));
    }
    if (raw.size() != e.rawSize) {
        throw FormatError("image pack: slot " + std::to_string(index) + " has the wrong size");
    }
    const std::size_t n = pixelsOf(e.pixelWidth, e.pixelHeight);
    // Unverified gradient images are unfiltered straight into the interleaved RGBA (the fast path);
    // the CRC is of the raw planes, so verifying unfilters the planes first.
    const bool fused = e.codec == kCodecZstdGradient && !verify;
    if (e.codec == kCodecZstdGradient && verify) {
        for (std::size_t c = 0; c < 4; ++c) {
            gradientUnfilter(raw.data() + c * n, e.pixelWidth, e.pixelHeight);
        }
    }
    if (verify && packCrc(raw) != e.crc) {
        throw FormatError("image pack: slot " + std::to_string(index) + " fails its CRC");
    }

    // Build the image: interleave R, G, B, A back into RGBA, then copy the optional planes that follow.
    PackImage p;
    p.name = std::string(name(index));
    p.type = e.type;
    p.headerByte = e.headerByte;
    p.width = e.width;
    p.height = e.height;
    p.offsetX = e.offsetX;
    p.offsetY = e.offsetY;
    p.pixelWidth = static_cast<std::int32_t>(e.pixelWidth);
    p.pixelHeight = static_cast<std::int32_t>(e.pixelHeight);
    // The optional planes after the colours, from `at`: the material plane, then the edge mask.
    const auto extraPlanes = [&](std::size_t at) {
        if (e.planes & kPlaneMaterial) {
            p.material.assign(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        }
        if (e.planes & kPlaneEdge) {
            p.edgeMask.assign(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        }
    };
    if (e.codec == kCodecBc7) {
        // The levels' blocks as stored, then the lossless planes; level 0 decoded unless not wanted.
        const auto levels = bc7Levels(e.pixelWidth, e.pixelHeight, e.levels);
        p.bc7.reserve(levels.size());
        std::size_t at = 0;
        for (const auto& l : levels) {
            p.bc7.emplace_back(raw.begin() + static_cast<std::ptrdiff_t>(l.offset),
                               raw.begin() + static_cast<std::ptrdiff_t>(l.offset + l.size));
            at = l.offset + l.size;
        }
        if (!blocksOnly) {
            p.rgba = decodeBc7(p.bc7.front(), levels.front());
        }
        extraPlanes(at);
        return p;
    }
    p.rgba.resize(n * 4);
    if (fused) {
        gradientUnfilterInterleave(raw.data(), e.pixelWidth, e.pixelHeight, p.rgba.data());
    } else {
        for (std::size_t c = 0; c < 4; ++c) {
            const std::uint8_t* plane = raw.data() + c * n;
            for (std::size_t i = 0; i < n; ++i) {
                p.rgba[i * 4 + c] = plane[i];
            }
        }
    }
    extraPlanes(n * 4);
    return p;
}

// Each normalised name's first filled slot, indexed once from the names as read.
long ImagePackReader::find(std::string_view name) const
{
    std::call_once(index_->made, [&] {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].present()) {
                index_->byName.try_emplace(normalizeName(this->name(i)), static_cast<std::uint32_t>(i));
            }
        }
    });
    const auto it = index_->byName.find(util::RawImageName{name});
    return it == index_->byName.end() ? -1 : static_cast<long>(it->second);
}

// ------------------------------------------------------------------------------------ writer

ImagePackWriter::ImagePackWriter(const std::filesystem::path& path, std::uint32_t imageCount,
                                 std::uint32_t sourceImages, std::uint32_t sourceIdxCrc)
    : file_(path, std::ios::binary | std::ios::trunc)
    , path_(path)
    , entries_(imageCount)
    , sourceImages_(sourceImages)
    , sourceIdxCrc_(sourceIdxCrc)
{
    if (!file_) {
        throw std::system_error(errno, std::generic_category(), "cannot create " + path.string());
    }
    // The header is rewritten by finish(); until then the file isn't a valid pack.
    const std::array<char, kHeaderSize> blank{};
    file_.write(blank.data(), blank.size());
    offset_ = kHeaderSize;
}

void ImagePackWriter::put(std::uint32_t index, const PackImage& image, int level)
{
    put(index, encodePackImage(image, level));
}

void ImagePackWriter::put(std::uint32_t index, const EncodedImage& image)
{
    putStored(index, image.entry, image.name, image.stored);
}

// Appends the stored bytes at the end of the data and records the entry; names go to names_ until finish().
void ImagePackWriter::putStored(std::uint32_t index, const PackEntry& entry, std::string_view name,
                                std::span<const std::uint8_t> stored)
{
    if (finished_) {
        throw FormatError("image pack: writing after finish()");
    }
    if (index >= entries_.size()) {
        throw FormatError("image pack: slot " + std::to_string(index) + " past the end");
    }
    if (stored.size() != entry.storedSize || name.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw FormatError("image pack: slot " + std::to_string(index) + " doesn't match its entry");
    }
    PackEntry e = entry;
    e.offset = offset_;
    e.nameOffset = static_cast<std::uint32_t>(names_.size());
    e.nameLength = static_cast<std::uint16_t>(name.size());
    e.flags |= kEntryPresent;
    names_.append(name);
    file_.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    offset_ += stored.size();
    entries_[index] = e;
}

// Writes names then the index after the image data, then rewrites the header at offset 0 to point at them.
void ImagePackWriter::finish()
{
    if (finished_) {
        return;
    }
    const std::uint64_t namesOffset = offset_;
    file_.write(names_.data(), static_cast<std::streamsize>(names_.size()));
    offset_ += names_.size();
    const std::uint64_t indexOffset = offset_;
    ByteWriter index;
    for (const auto& e : entries_) {
        writeEntry(index, e);
    }
    file_.write(reinterpret_cast<const char*>(index.data().data()), static_cast<std::streamsize>(index.size()));

    ByteWriter h;
    h.bytes({reinterpret_cast<const std::uint8_t*>(kMagic.data()), kMagic.size()});
    // The oldest version that reads it: 3 only with BC7 entries, so a pack without them still
    // opens in readers from before kCodecBc7 (version 2 clients).
    const bool anyBc7 = std::ranges::any_of(entries_, [](const PackEntry& e) { return e.present() && e.codec == kCodecBc7; });
    h.u32(anyBc7 ? kPackVersion : 2);
    h.u32(static_cast<std::uint32_t>(entries_.size()));
    h.u64(indexOffset);
    h.u64(namesOffset);
    h.u32(static_cast<std::uint32_t>(names_.size()));
    h.u32(sourceImages_);
    h.u32(sourceIdxCrc_);
    h.u32(0); // flags
    h.zeros(kHeaderSize - h.size());
    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(h.data().data()), static_cast<std::streamsize>(h.size()));
    file_.close();
    if (!file_) {
        throw std::system_error(errno, std::generic_category(), "writing " + path_.string());
    }
    finished_ = true;
}

} // namespace nox::video
