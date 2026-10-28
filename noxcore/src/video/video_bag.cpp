// After OpenNox libs bag (MIT, Copyright (c) 2022 NoX World), checked against GAME.EXE; see
// THIRD_PARTY_NOTICES.txt.
#include "noxcore/video/video_bag.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/video/nxz.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace nox::video {

namespace {

constexpr std::uint32_t kMagicPlain = 0xFAEDBCEA;
constexpr std::uint32_t kMagicCompressed = 0xFAEDBCEB;

using util::normalizeName;

} // namespace

// Reads the whole of video.idx up front (layout in video_bag.hpp); the bag is read per segment.
VideoBag::VideoBag(const std::filesystem::path& bag, const std::filesystem::path& idx, bool eightBit)
    : bag_(resolveNoCase(bag), std::ios::binary)
{
    if (!bag_) {
        throw std::system_error(errno, std::generic_category(), "cannot open " + bag.string());
    }
    bag_.seekg(0, std::ios::end);
    bagSize_ = static_cast<std::uint64_t>(std::max<std::streamoff>(0, bag_.tellg()));
    bag_.seekg(0);
    const auto index = readFileBytes(idx);
    ByteReader r(index, "video.idx");
    const std::uint32_t magic = r.u32();
    if (magic != kMagicPlain && magic != kMagicCompressed) {
        r.fail("not a video index");
    }
    compressed_ = magic == kMagicCompressed;
    if (r.u32() != index.size()) {
        r.fail("index size doesn't match its header");
    }
    const std::uint32_t segmentCount = r.u32();
    segmentBufferSize_ = r.u32();
    headerUnknown_ = r.u32();
    const std::uint32_t imageCount = r.u32();
    eightBit_ = eightBit;

    // Segments lie back to back in the bag, so each one's offset is the sum of the sizes before it.
    std::uint64_t bagOffset = 0;
    // (Room for what the index can hold: its header's counts are claims, the image count one the
    // game uses only to size an array, and either may say billions. 12 and 10 bytes are the
    // least a segment's and an image's records take.)
    segments_.reserve(std::min<std::size_t>(segmentCount, r.remaining() / 12));
    images_.reserve(std::min<std::size_t>(imageCount, r.remaining() / 10));
    // Per segment: u32 unknown, u32 size, u32 compressed size (compressed bags only), i32 image count (-1: a single image).
    for (std::uint32_t i = 0; i < segmentCount; ++i) {
        BagSegment s;
        s.unknown = r.u32();
        s.size = r.u32();
        s.compressedSize = compressed_ ? r.u32() : s.size;
        const std::int32_t count = r.i32();
        if (count < -1) {
            r.fail("negative image count");
        }
        s.bagOffset = bagOffset;
        s.firstImage = static_cast<std::uint32_t>(images_.size());
        s.singleImage = count == -1;
        s.imageCount = s.singleImage ? 1 : static_cast<std::uint32_t>(count);
        bagOffset += s.compressedSize;

        std::uint32_t offset = 0;
        // Per image: u8-length name, u8 type, u32 size of the 16-bit data, u32 size of the 8-bit data;
        // the images lie back to back within the decompressed segment.
        for (std::uint32_t j = 0; j < s.imageCount; ++j) {
            BagImage img;
            img.storedName = r.rawString(r.u8());
            img.name = img.storedName;
            // Names are stored with their padding; cut at the first NUL.
            if (const auto nul = img.name.find('\0'); nul != std::string::npos) {
                img.name.resize(nul);
            }
            img.type = r.u8();
            img.size16 = r.u32();
            img.size8 = r.u32();
            img.size = eightBit ? img.size8 : img.size16;
            img.segment = i;
            img.offset = offset;
            offset += img.size;
            images_.push_back(std::move(img));
        }
        segments_.push_back(s);
    }
    r.expectEnd();
    // The game only sizes its image array from this; the shipped index claims 25 more images
    // than it holds.
    headerImageCount_ = imageCount;
}

std::vector<std::uint8_t> VideoBag::readStoredSegment(std::size_t segment)
{
    const BagSegment& s = segments_.at(segment);
    // (Checked before the buffer is made: video.idx's u32 sizes may say gigabytes.)
    if (s.bagOffset > bagSize_ || s.compressedSize > bagSize_ - s.bagOffset) {
        throw FormatError("video.bag: segment runs past the end of the file");
    }
    std::vector<std::uint8_t> raw(s.compressedSize);
    bag_.clear();
    bag_.seekg(static_cast<std::streamoff>(s.bagOffset));
    bag_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!bag_) {
        throw FormatError("video.bag: segment runs past the end of the file");
    }
    return raw;
}

std::vector<std::uint8_t> VideoBag::readSegment(std::size_t segment)
{
    const BagSegment& s = segments_.at(segment);
    auto raw = readStoredSegment(segment);
    // Equal sizes mean the segment is stored uncompressed.
    if (s.compressedSize == s.size) {
        return raw;
    }
    return nxzDecompress(raw, s.size);
}

std::vector<std::uint8_t> VideoBag::readImage(std::size_t image)
{
    const BagImage& img = images_.at(image);
    const auto segment = readSegment(img.segment);
    if (std::size_t{img.offset} + img.size > segment.size()) {
        throw FormatError("video.bag: image " + img.name + " runs past its segment");
    }
    return {segment.begin() + img.offset, segment.begin() + img.offset + img.size};
}

long VideoBag::findImage(std::string_view name) const
{
    // Each normalised name's first image, made once from the names as read.
    std::call_once(names_->made, [&] {
        names_->byName.reserve(images_.size());
        for (std::size_t i = 0; i < images_.size(); ++i) {
            names_->byName.try_emplace(normalizeName(images_[i].name), static_cast<std::uint32_t>(i));
        }
    });
    const auto it = names_->byName.find(util::RawImageName{name});
    return it == names_->byName.end() ? -1 : static_cast<long>(it->second);
}

} // namespace nox::video
