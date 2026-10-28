// Writes video.bag / video.idx; the rules the loader sets are in video_bag_writer.hpp.
#include "noxcore/video/video_bag_writer.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/video/image_pack.hpp"
#include "noxcore/video/nxz.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace nox::video {

namespace {

constexpr std::uint32_t kMagicPlain = 0xFAEDBCEA;
constexpr std::uint32_t kMagicCompressed = 0xFAEDBCEB;
constexpr std::size_t kMaxSegments = 65535; // the loader's 16-bit segment number

struct OutImage {
    const BagImage* info = nullptr;
    std::uint32_t size16 = 0;
    std::uint32_t size8 = 0;
};

class Writer {
public:
    Writer(const std::filesystem::path& bag, bool compressed)
        : out_(bag, std::ios::binary | std::ios::trunc), compressed_(compressed)
    {
        if (!out_) {
            throw std::system_error(errno, std::generic_category(), "cannot create " + bag.string());
        }
    }

    /// One index record and its stored bytes. `singleImage` writes count -1.
    void segment(std::uint32_t unknown, std::uint32_t size, std::span<const std::uint8_t> stored,
                 const std::vector<OutImage>& images, bool singleImage)
    {
        idx_.u32(unknown);
        idx_.u32(size);
        if (compressed_) {
            idx_.u32(static_cast<std::uint32_t>(stored.size()));
        }
        idx_.i32(singleImage ? -1 : static_cast<std::int32_t>(images.size()));
        for (const auto& img : images) {
            const std::string& name = img.info->storedName;
            if (name.size() > 255) {
                throw FormatError("video.idx: image name " + img.info->name + " too long");
            }
            idx_.u8(static_cast<std::uint8_t>(name.size()));
            idx_.rawString(name);
            idx_.u8(img.info->type);
            idx_.u32(img.size16);
            idx_.u32(img.size8);
        }
        out_.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        ++segments_;
        images_ += images.size();
    }

    /// A rebuilt segment from its decompressed bytes: NXZ when that's smaller (compressed bags).
    void rebuilt(std::uint32_t unknown, const std::vector<std::uint8_t>& data, const std::vector<OutImage>& images,
                 bool singleImage)
    {
        if (compressed_ && !data.empty()) {
            auto packed = nxzCompress(data);
            if (packed.size() < data.size()) {
                segment(unknown, static_cast<std::uint32_t>(data.size()), packed, images, singleImage);
                return;
            }
        }
        segment(unknown, static_cast<std::uint32_t>(data.size()), data, images, singleImage);
    }

    std::size_t segments() const { return segments_; }
    std::size_t images() const { return images_; }
    const std::vector<std::uint8_t>& records() const { return idx_.data(); }

    void close()
    {
        out_.close();
        if (!out_) {
            throw std::system_error(std::make_error_code(std::errc::io_error), "writing video.bag failed");
        }
    }

private:
    std::ofstream out_;
    bool compressed_;
    ByteWriter idx_;
    std::size_t segments_ = 0;
    std::size_t images_ = 0;
};

std::filesystem::path partPath(const std::filesystem::path& p)
{
    std::filesystem::path t = p;
    t += ".part";
    return t;
}

} // namespace

BagWriteResult writeVideoBag(VideoBag& source, const std::map<std::uint32_t, std::vector<std::uint8_t>>& replacements,
                             const std::filesystem::path& bag, const std::filesystem::path& idx,
                             const std::function<void(std::size_t, std::size_t)>& progress)
{
    const auto& images = source.images();
    const auto& segments = source.segments();
    for (const auto& [index, bytes] : replacements) {
        if (index >= images.size()) {
            throw FormatError("video.bag has no image #" + std::to_string(index));
        }
        if (bytes.size() > std::numeric_limits<std::int32_t>::max()) {
            throw FormatError("image #" + std::to_string(index) + " too big");
        }
    }
    const std::uint32_t slot = source.segmentBufferSize();
    const bool eightBit = source.eightBit();
    BagWriteResult result;

    const auto bagPart = partPath(bag);
    const auto idxPart = partPath(idx);
    try {
        Writer w(bagPart, source.compressed());
        auto next = replacements.begin();
        for (std::size_t s = 0; s < segments.size(); ++s) {
            if (progress && s % 64 == 0) {
                progress(s, segments.size());
            }
            const BagSegment& seg = segments[s];
            const std::uint32_t first = seg.firstImage;
            const std::uint32_t end = first + seg.imageCount;
            while (next != replacements.end() && next->first < first) {
                ++next;
            }
            const bool touched = next != replacements.end() && next->first < end;
            if (!touched) {
                std::vector<OutImage> out;
                for (std::uint32_t i = first; i < end; ++i) {
                    out.push_back({&images[i], images[i].size16, images[i].size8});
                }
                w.segment(seg.unknown, seg.size, source.readStoredSegment(s), out, seg.singleImage);
                ++result.segmentsCopied;
                continue;
            }
            ++result.segmentsRebuilt;
            const auto original = source.readSegment(s);
            // Each image's bytes, replaced or as they were.
            std::vector<std::vector<std::uint8_t>> datas;
            std::vector<OutImage> out;
            for (std::uint32_t i = first; i < end; ++i) {
                const BagImage& info = images[i];
                OutImage o{&info, info.size16, info.size8};
                if (const auto it = replacements.find(i); it != replacements.end()) {
                    datas.push_back(it->second);
                    (eightBit ? o.size8 : o.size16) = static_cast<std::uint32_t>(it->second.size());
                    ++result.imagesReplaced;
                } else {
                    if (std::size_t{info.offset} + info.size > original.size()) {
                        throw FormatError("video.bag: image " + info.name + " runs past its segment");
                    }
                    datas.emplace_back(original.begin() + info.offset, original.begin() + info.offset + info.size);
                }
                out.push_back(o);
            }
            if (seg.singleImage) {
                w.rebuilt(seg.unknown, datas.front(), out, true);
                continue;
            }
            // Images in order, as many to a segment as fit the loader's slot; one bigger than a
            // slot goes alone into a single-image segment.
            std::vector<std::uint8_t> group;
            std::vector<OutImage> groupImages;
            bool firstGroup = true;
            const auto flush = [&] {
                if (groupImages.empty()) {
                    return;
                }
                w.rebuilt(firstGroup ? seg.unknown : 0, group, groupImages, false);
                firstGroup = false;
                group.clear();
                groupImages.clear();
            };
            for (std::size_t k = 0; k < datas.size(); ++k) {
                if (datas[k].size() > slot) {
                    flush();
                    w.rebuilt(firstGroup ? seg.unknown : 0, datas[k], {out[k]}, true);
                    firstGroup = false;
                    continue;
                }
                if (group.size() + datas[k].size() > slot) {
                    flush();
                }
                group.insert(group.end(), datas[k].begin(), datas[k].end());
                groupImages.push_back(out[k]);
            }
            flush();
        }
        if (w.segments() > kMaxSegments) {
            throw FormatError("video.bag would have " + std::to_string(w.segments()) +
                              " segments; the game takes at most 65535");
        }
        result.segmentsWritten = w.segments();
        w.close();

        ByteWriter header;
        header.u32(source.compressed() ? kMagicCompressed : kMagicPlain);
        header.u32(0); // the index's size, below
        header.u32(static_cast<std::uint32_t>(w.segments()));
        header.u32(slot);
        header.u32(source.headerUnknown());
        header.u32(std::max<std::uint32_t>(source.headerImageCount(), static_cast<std::uint32_t>(w.images())));
        header.bytes(w.records());
        if (header.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw FormatError("video.idx too big");
        }
        header.patchU32(4, static_cast<std::uint32_t>(header.size()));
        result.idxCrc = packCrc(header.data());
        writeFileBytes(idxPart, header.data());
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(bagPart, ec);
        std::filesystem::remove(idxPart, ec);
        throw;
    }
    std::filesystem::rename(bagPart, bag);
    std::filesystem::rename(idxPart, idx);
    if (progress) {
        progress(segments.size(), segments.size());
    }
    return result;
}

} // namespace nox::video
