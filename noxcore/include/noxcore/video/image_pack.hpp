// The image pack (.nxp): video.bag's images kept for fast random access and for higher
// resolutions, beside video.bag, which stays what the stock client (and for now noxsdl) reads.
// The server never reads images, so a pack changes nothing on the wire.
//
// What it fixes in video.bag: images there are packed into NXZ segments, so reading one
// decompresses its whole segment; colour is 16-bit; and one image pixel is one world unit, so a
// bigger image would draw bigger instead of sharper. Here every image is compressed on its own
// (zstd, which decompresses at GB/s), stored as 8-bit RGBA with its material and edge planes,
// and carries its size in original pixels (logical) apart from the stored resolution, so an
// image upscaled 2x or 4x drops in over the original with the same metadata.
//
// Slots are video.idx's image indexes (thing.bin and the maps name images by that index), so a
// pack built from video.bag has a slot for every image, and a tool replacing images (the
// upscaler) writes a new pack copying the untouched slots' bytes as they are
// (ImagePackWriter::putStored).
//
// File, little endian:
//   header (64 bytes)
//     char[8] magic "NOXPACK" 0x1A
//     u32 version (3; readers take 1 and 2 too: version 2 added kCodecZstdGradient, 3 kCodecBc7;
//                  the writer stamps 3 only on a pack with kCodecBc7 entries, else 2)
//     u32 imageCount       slots, one per video.idx image
//     u64 indexOffset      imageCount entries of 64 bytes
//     u64 namesOffset      the images' names back to back (entries point into them)
//     u32 namesSize
//     u32 sourceImages     video.idx's image count when built (0 if unknown)
//     u32 sourceIdxCrc     CRC-32 of the video.idx it was built from (0 if unknown)
//     u32 flags            0
//     u8[16] reserved
//   image data: each image's stored bytes, anywhere after the header
//   names, then the index (the writer streams the data and ends with these)
//
//   entry (64 bytes)
//     u64 offset           stored bytes' file offset
//     u32 storedSize       as stored (compressed)
//     u32 rawSize          once decompressed
//     u32 crc              CRC-32 of the raw bytes
//     u32 nameOffset       into the names
//     u16 nameLength
//     u8  type             video.bag's image type (video/sprite.hpp)
//     u8  headerByte       byte 16 of the sprite header (meaning unknown; kept)
//     i32 width, height    in original pixels (logical)
//     i32 offsetX, offsetY draw offset, in original pixels
//     u32 pixelWidth, pixelHeight   stored resolution (width x scale ...)
//     u8  planes           kPlaneRgba | kPlaneMaterial | kPlaneEdge
//     u8  codec            kCodecStored, kCodecZstd, kCodecZstdGradient or kCodecBc7
//     u16 flags            kEntryPresent
//     u8  levels           kCodecBc7: the colour mip levels stored (level 0 included); else 0
//     u8[7] reserved
//
//   raw bytes (pixelWidth x pixelHeight = n): planar, R[n] G[n] B[n] A[n] (straight alpha), then
//   material[n] if kPlaneMaterial (0 plain colour, k = material slot k - 1: those pixels hold a
//   grey intensity the object's colour shades), then edge[n] if kPlaneEdge (0 leave what's
//   there, 1 the edge's own colour, 2 the floor being blended in). Planar compresses better than
//   interleaved RGBA for this art.
//
//   kCodecZstdGradient: before zstd, each of the R, G, B and A planes holds, per pixel, its value
//   minus the gradient prediction left + up - upleft (mod 256, 0 outside the image); the material
//   and edge planes are as they are. Undoing it is a running sum along each row, then adding the
//   row above. About a quarter smaller than plain zstd on the HD art; the writer keeps whichever
//   of the two is smaller. The CRC is of the raw bytes, as for the other codecs.
//
//   kCodecBc7: the colour as BC7 blocks, ready for the GPU (a quarter of RGBA's memory, nothing to
//   decode), with its mip chain, so a renderer uploads each level as a texture of its own. Raw
//   bytes, zstd-compressed as one frame: level 0's blocks, then level 1's ... (`levels` of them),
//   then material[n] if kPlaneMaterial and edge[n] if kPlaneEdge, at the full stored size, lossless
//   (no mips: they're labels). Level 0 is pixelWidth x pixelHeight; each next level is
//   ((w + 1) / 2) x ((h + 1) / 2), each pixel the alpha-weighted average of its 2x2 block (as
//   noxsdl's mips). A level's blocks cover it padded to multiples of 4 (the padding clear), in
//   rows of 4x4 blocks, 16 bytes each: bc7Levels() gives every level's sizes and offset.
//   Readers decode level 0 to RGBA on the CPU (bcdec) unless asked for the blocks only.
#pragma once

#include "noxcore/util/text.hpp"
#include "noxcore/video/sprite.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::video {

inline constexpr std::uint32_t kPackVersion = 3;
inline constexpr std::uint8_t kPlaneRgba = 1;
inline constexpr std::uint8_t kPlaneMaterial = 2;
inline constexpr std::uint8_t kPlaneEdge = 4;
inline constexpr std::uint8_t kCodecStored = 0;
inline constexpr std::uint8_t kCodecZstd = 1;
inline constexpr std::uint8_t kCodecZstdGradient = 2;
inline constexpr std::uint8_t kCodecBc7 = 3;
inline constexpr std::uint16_t kEntryPresent = 1;

/// One BC7 mip level: its size in pixels, padded to whole 4x4 blocks, and where its blocks start
/// in the entry's raw bytes.
struct Bc7Level {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t paddedWidth = 0;
    std::uint32_t paddedHeight = 0;
    std::size_t offset = 0;
    std::size_t size = 0; ///< (paddedWidth / 4) * (paddedHeight / 4) * 16
};

/// The levels of a w x h image's BC7 mip chain (see the file comment), level 0 first.
std::vector<Bc7Level> bc7Levels(std::uint32_t width, std::uint32_t height, std::uint32_t levels);

/// BC7 blocks of a level to RGBA (paddedWidth x paddedHeight blocks, cropped to width x height).
std::vector<std::uint8_t> decodeBc7(std::span<const std::uint8_t> blocks, const Bc7Level& level);

/// One image as the pack keeps it.
struct PackImage {
    std::string name;
    std::uint8_t type = 0;
    std::uint8_t headerByte = 0;
    std::int32_t width = 0;   ///< original pixels (where and how big it draws)
    std::int32_t height = 0;
    std::int32_t offsetX = 0; ///< draw offset, original pixels
    std::int32_t offsetY = 0;
    std::int32_t pixelWidth = 0;  ///< stored resolution
    std::int32_t pixelHeight = 0;
    std::vector<std::uint8_t> rgba;     ///< pixelWidth * pixelHeight * 4, straight alpha
    std::vector<std::uint8_t> material; ///< empty, or one byte a pixel (see the file comment)
    std::vector<std::uint8_t> edgeMask; ///< empty, or one byte a pixel
    /// BC7 colour, level 0 first, each level's blocks as bc7Levels lays them out. Writing: set
    /// these (encoded by the caller) to store the image as kCodecBc7; `rgba` is then not stored.
    /// Reading a kCodecBc7 entry: filled, and `rgba` is level 0 decoded unless the read asked for
    /// the blocks only. Empty for the other codecs.
    std::vector<std::vector<std::uint8_t>> bc7;

    /// A video.bag image at its own resolution.
    static PackImage fromDecoded(const DecodedImage& image, std::string name);
};

/// An index entry (see the file comment).
struct PackEntry {
    std::uint64_t offset = 0;
    std::uint32_t storedSize = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t crc = 0;
    std::uint32_t nameOffset = 0;
    std::uint16_t nameLength = 0;
    std::uint8_t type = 0;
    std::uint8_t headerByte = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t offsetX = 0;
    std::int32_t offsetY = 0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    std::uint8_t planes = 0;
    std::uint8_t codec = 0;
    std::uint16_t flags = 0;
    std::uint8_t levels = 0; ///< kCodecBc7: mip levels stored

    bool present() const { return (flags & kEntryPresent) != 0; }
};

/// An image compressed and ready to write: its entry (offset and name fields aside) and bytes.
/// Encoding is the slow part and needs no writer, so tools can encode on many threads and write
/// the results in order.
struct EncodedImage {
    PackEntry entry;
    std::string name;
    std::vector<std::uint8_t> stored;
};

/// Compresses an image (zstd at `level`, 1-22; 0 stores it uncompressed). With `gradient`, it is
/// also compressed through the gradient filter and the smaller of the two is kept (twice the
/// encoding time). An image with `bc7` levels is stored as kCodecBc7 (zstd at `level`, at least 1).
/// Throws FormatError if its planes or levels don't match its size.
EncodedImage encodePackImage(const PackImage& image, int level = 19, bool gradient = true);

/// Reads a pack. Opening reads the header, index and names; each image is one read and one
/// decompression. Not thread-safe (one file handle): give each thread its own.
class ImagePackReader {
public:
    /// Throws FormatError / std::system_error.
    explicit ImagePackReader(const std::filesystem::path& path);

    std::uint32_t size() const { return static_cast<std::uint32_t>(entries_.size()); }
    std::uint32_t sourceImages() const { return sourceImages_; }
    std::uint32_t sourceIdxCrc() const { return sourceIdxCrc_; }
    const PackEntry& entry(std::size_t index) const { return entries_.at(index); }
    std::string_view name(std::size_t index) const;

    /// The image in slot `index`, or nullopt if the slot is empty. With `verify`, checks the
    /// CRC. A kCodecBc7 image comes with its `bc7` levels, and with level 0 decoded into `rgba`
    /// unless `blocksOnly` (a renderer that uploads the blocks as they are). Throws FormatError on
    /// corrupt data.
    std::optional<PackImage> read(std::size_t index, bool verify = false, bool blocksOnly = false);
    /// The slot's bytes as stored (for copying into another pack unchanged).
    std::vector<std::uint8_t> readStored(std::size_t index);

    /// Slot by name, case-insensitive and ignoring the extension, as VideoBag::findImage; or -1;
    /// the first filled slot of a name. (By an index of the names made at the first call.)
    long find(std::string_view name) const;

private:
    std::ifstream file_;
    std::uint64_t fileSize_ = 0; ///< (what the index's offsets and sizes are checked against)
    std::vector<PackEntry> entries_;
    std::string names_;
    struct NameIndex {
        std::once_flag made;
        std::unordered_map<std::string, std::uint32_t, util::ImageNameHash, util::ImageNameEqual> byName;
    };
    std::unique_ptr<NameIndex> index_ = std::make_unique<NameIndex>(); ///< (find's)
    std::uint32_t sourceImages_ = 0;
    std::uint32_t sourceIdxCrc_ = 0;
};

/// Writes a pack: images in any order into their slots, then finish() writes the names and the
/// index. Slots never written are left empty. The file is complete only after finish().
class ImagePackWriter {
public:
    /// Throws std::system_error if the file can't be created.
    ImagePackWriter(const std::filesystem::path& path, std::uint32_t imageCount, std::uint32_t sourceImages = 0,
                    std::uint32_t sourceIdxCrc = 0);

    /// Compresses and writes an image into slot `index`.
    void put(std::uint32_t index, const PackImage& image, int level = 19);
    /// Writes an already encoded image.
    void put(std::uint32_t index, const EncodedImage& image);
    /// Copies a slot as stored: `entry` from another pack's index, `stored` its bytes.
    void putStored(std::uint32_t index, const PackEntry& entry, std::string_view name,
                   std::span<const std::uint8_t> stored);

    /// Writes the names and the index and closes the file. Throws std::system_error.
    void finish();

private:
    std::ofstream file_;
    std::filesystem::path path_;
    std::vector<PackEntry> entries_;
    std::string names_;
    std::uint64_t offset_ = 0;
    std::uint32_t sourceImages_ = 0;
    std::uint32_t sourceIdxCrc_ = 0;
    bool finished_ = false;
};

/// CRC-32 (the standard reflected one) of `data`.
std::uint32_t packCrc(std::span<const std::uint8_t> data);

} // namespace nox::video
