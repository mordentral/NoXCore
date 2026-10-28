// video.bag / video.idx: the game's sprite archive.
//
// video.idx (loader):
//   u32 magic (0xFAEDBCEA plain, 0xFAEDBCEB with compressed segments); u32 index file size;
//   u32 segmentCount; u32 segmentBufferSize; u32 unknown (unused); u32 imageCount
//   segmentCount x { u32 unknown (unused); u32 size; [u32 compressedSize]; i32 count (-1 = one image);
//                    count x { u8 nameLen; name; u8 type; u32 size16; u32 size8 } }
// video.bag holds the segments back to back, each NXZ-compressed when compressedSize != size.
// Images lie back to back inside their segment; video.bag uses the 16-bit sizes, video8.bag the
// 8-bit ones. The game caches segments in slots of segmentBufferSize bytes; only segments with
// several images have to fit one (single-image segments can be much larger).
#pragma once

#include "noxcore/util/text.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::video {

struct BagSegment {
    std::uint32_t unknown = 0;        // not read by the game
    std::uint32_t size = 0;           // decompressed
    std::uint32_t compressedSize = 0; // as stored in the bag
    std::uint64_t bagOffset = 0;
    std::uint32_t firstImage = 0;
    std::uint32_t imageCount = 0;
    bool singleImage = false; // stored count -1
};

struct BagImage {
    std::string name;
    std::uint8_t type = 0; // see video/sprite.hpp
    std::uint32_t segment = 0;
    std::uint32_t offset = 0; // within the decompressed segment
    std::uint32_t size = 0;
    // As stored in the index, for writing it back (video/video_bag_writer.hpp).
    std::string storedName;   // the name field with its padding
    std::uint32_t size16 = 0; // video.bag's size of the image
    std::uint32_t size8 = 0;  // video8.bag's
};

class VideoBag {
public:
    /// Opens a .bag and its .idx. `eightBit` selects video8.bag's image sizes.
    VideoBag(const std::filesystem::path& bag, const std::filesystem::path& idx, bool eightBit = false);

    bool compressed() const { return compressed_; }
    std::uint32_t segmentBufferSize() const { return segmentBufferSize_; }
    /// What the header claims; the game only uses it to size an array.
    std::uint32_t headerImageCount() const { return headerImageCount_; }
    /// The header's unused field (kept when writing).
    std::uint32_t headerUnknown() const { return headerUnknown_; }
    /// Whether the image sizes are video8.bag's.
    bool eightBit() const { return eightBit_; }
    const std::vector<BagSegment>& segments() const { return segments_; }
    const std::vector<BagImage>& images() const { return images_; }

    /// Decompressed segment bytes. Throws FormatError / std::system_error.
    std::vector<std::uint8_t> readSegment(std::size_t segment);

    /// One image's bytes (reads and decompresses its whole segment).
    std::vector<std::uint8_t> readImage(std::size_t image);

    /// A segment's bytes as stored in the bag (compressed or not), for copying it unchanged.
    std::vector<std::uint8_t> readStoredSegment(std::size_t segment);

    /// Image index by name, case-insensitive and ignoring the extension (as OpenNox does), or -1;
    /// the first of a name. (By an index of the names made at the first call.)
    long findImage(std::string_view name) const;

private:
    struct NameIndex {
        std::once_flag made;
        std::unordered_map<std::string, std::uint32_t, util::ImageNameHash, util::ImageNameEqual> byName;
    };
    std::unique_ptr<NameIndex> names_ = std::make_unique<NameIndex>(); ///< (findImage's)
    std::ifstream bag_;
    std::uint64_t bagSize_ = 0; ///< (readStoredSegment checks a segment against it before its buffer)
    bool compressed_ = false;
    std::uint32_t segmentBufferSize_ = 0;
    std::uint32_t headerImageCount_ = 0;
    std::uint32_t headerUnknown_ = 0;
    bool eightBit_ = false;
    std::vector<BagSegment> segments_;
    std::vector<BagImage> images_;
};

} // namespace nox::video
