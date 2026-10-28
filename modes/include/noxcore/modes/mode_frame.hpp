// The frame every NoX Core mode message has (MSG_NOXCPP_MODE_F4's, MSG_NOXCPP_MODE_F5's, the mode
// UI's 0xF8): {u8 op, u8 sub, u16 the rest's length (little-endian), the rest}.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nox::modes {

/// `sub` and `rest` framed as an `op` message.
inline std::vector<std::uint8_t> framed(std::uint8_t op, std::uint8_t sub, std::span<const std::uint8_t> rest)
{
    std::vector<std::uint8_t> m{op, sub, static_cast<std::uint8_t>(rest.size()), static_cast<std::uint8_t>(rest.size() >> 8)};
    m.insert(m.end(), rest.begin(), rest.end());
    return m;
}

/// An `op` message's sub and rest (any bytes past the rest's length left out); none: not one, or
/// cut short.
inline std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> unframed(std::uint8_t op, std::span<const std::uint8_t> m)
{
    if (m.size() < 4 || m[0] != op) {
        return std::nullopt;
    }
    const std::size_t length = m[2] | static_cast<std::size_t>(m[3]) << 8;
    if (m.size() < 4 + length) {
        return std::nullopt;
    }
    return std::pair{m[1], m.subspan(4, length)};
}

} // namespace nox::modes
