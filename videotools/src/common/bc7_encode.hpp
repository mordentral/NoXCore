// BC7 encoding for the image pack's kCodecBc7 (noxcore/video/image_pack.hpp): an image's colour
// mip chain as BC7 blocks, by bc7enc_rdo (bc7enc.cpp, with optional RDO: a lambda > 0 makes the
// blocks compress better under zstd at some cost in quality; 0.25 measured as indistinguishable).
#pragma once

#include <cstdint>
#include <vector>

namespace videotools {

/// The mip chain noxsdl draws an HD image with (SpriteCache::makeMips): level 0, then each level
/// half the one before (rounding up, alpha-weighted 2x2 averages), at most kBc7MaxMips more, none
/// with a side under kBc7MinMipSide. RGBA, level 0 first, with each level's width and height.
struct MipLevel {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
};
inline constexpr int kBc7MaxMips = 4;
inline constexpr int kBc7MinMipSide = 8;
std::vector<MipLevel> mipChain(const std::vector<std::uint8_t>& rgba, int width, int height);

/// Every level of mipChain as BC7 blocks, each level padded to multiples of 4 with clear pixels.
/// Thread-safe (call it from several threads at once to encode a pack).
std::vector<std::vector<std::uint8_t>> encodeBc7Chain(const std::vector<std::uint8_t>& rgba, int width, int height,
                                                      float rdoLambda);

} // namespace videotools
