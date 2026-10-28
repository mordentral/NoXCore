// Writing video.bag / video.idx (layout in video/video_bag.hpp) with some images replaced, in a
// form GAME.EXE's loader takes:
//   - The index's header keeps its magic (compressed or not), segment buffer size, unused field
//     and claimed image count (the loader sizes its image array from it, so it never drops below
//     the images written).
//   - The loader decompresses a segment of several images into one of its cache slots, which are
//     segmentBufferSize bytes each; a segment stored with count -1 holds a single image of any
//     size in a buffer of its own. So a rebuilt segment that would outgrow a slot is split into
//     several, and an image bigger than a slot goes alone into a count -1 segment. Images keep
//     their order, so their indexes (which thing.bin and the maps use) don't change.
//   - The loader keeps a segment's number in 16 bits: at most 65535 segments.
//   - A segment the loader finds with equal sizes is read as stored, so a rebuilt one is NXZ
//     compressed (in a compressed bag) only when that makes it smaller.
// Segments with no replaced image are copied byte for byte, index record included, so a bag
// written with no replacements is the original exactly.
#pragma once

#include "noxcore/video/video_bag.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>

namespace nox::video {

struct BagWriteResult {
    std::size_t segmentsCopied = 0;  // unchanged, copied as stored
    std::size_t segmentsRebuilt = 0; // holding a replaced image (each may become several)
    std::size_t segmentsWritten = 0; // in the new index
    std::size_t imagesReplaced = 0;
    std::uint32_t idxCrc = 0; // CRC-32 (packCrc) of the new video.idx, for stamping image packs
};

/// Writes `source`'s images to `bag` and `idx`, each image in `replacements` (by index) replaced
/// by those bytes, already in the image's own type (video/sprite_encode.hpp). The sizes written
/// for a replaced image are the bag's own (16- or 8-bit, as `source` was opened); the other's are
/// kept. The files are written beside the targets and renamed over them at the end. `progress`,
/// if given, hears (segments done, segment count). Throws FormatError / std::system_error.
BagWriteResult writeVideoBag(VideoBag& source, const std::map<std::uint32_t, std::vector<std::uint8_t>>& replacements,
                             const std::filesystem::path& bag, const std::filesystem::path& idx,
                             const std::function<void(std::size_t, std::size_t)>& progress = {});

} // namespace nox::video
