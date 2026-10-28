// Little-endian fields at an offset and a 16-bit value's bytes: the small helpers the message,
// save and file code shares. No bounds checks: the caller keeps the offset inside the data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nox::util {

/// The little-endian u16 at `off`.
constexpr std::uint16_t u16At(std::span<const std::uint8_t> d, std::size_t off)
{
    return static_cast<std::uint16_t>(d[off] | (d[off + 1] << 8));
}

/// The little-endian u32 at `off`.
constexpr std::uint32_t u32At(std::span<const std::uint8_t> d, std::size_t off)
{
    return static_cast<std::uint32_t>(d[off]) | static_cast<std::uint32_t>(d[off + 1]) << 8 |
           static_cast<std::uint32_t>(d[off + 2]) << 16 | static_cast<std::uint32_t>(d[off + 3]) << 24;
}

/// The little-endian u16 at `p`.
constexpr std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

/// The little-endian u32 at `p`.
constexpr std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

/// The big-endian u32 at `p`.
constexpr std::uint32_t be32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

/// Low and high byte of a 16-bit value, for little-endian message fields.
constexpr std::uint8_t lo(std::uint32_t v) { return static_cast<std::uint8_t>(v); }
constexpr std::uint8_t hi(std::uint32_t v) { return static_cast<std::uint8_t>(v >> 8); }

/// A u32 appended, little-endian.
inline void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

} // namespace nox::util
