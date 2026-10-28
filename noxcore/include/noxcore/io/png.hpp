// A minimal PNG encoder: 8-bit RGBA, stored (uncompressed) deflate blocks. Enough for tool output
// and screenshots; not meant for shipping art.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nox {

/// Encodes `rgba` (width * height * 4 bytes, rows top to bottom) as a PNG file.
std::vector<std::uint8_t> encodePng(int width, int height, std::span<const std::uint8_t> rgba);

} // namespace nox
