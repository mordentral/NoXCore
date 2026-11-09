// The row pitch of GpuAtlas's staged uploads (gpu_atlas.hpp): each row of a region padded to a
// multiple of it, so the copy pass takes the rows where they are. Direct3D 12 wants 256 bytes and
// the other backends take that as well; an atlas can be made with another (1: rows packed tight).
// No SDL in it, so the tests drive it.
#pragma once

#include <cstdint>

namespace noxsdl::rowpitch {

/// The pitch an atlas pads its rows to unless it's made with another.
inline constexpr std::uint32_t kDefault = 256;

/// A row of `bytes` padded to a multiple of `pitch` (0 taken as 1: no padding).
constexpr std::uint32_t padded(std::uint32_t bytes, std::uint32_t pitch)
{
    const std::uint32_t to = pitch == 0 ? 1 : pitch;
    return (bytes + to - 1) / to * to;
}

} // namespace noxsdl::rowpitch
