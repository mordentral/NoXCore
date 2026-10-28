// Deflate-compressed PNG encoding (stb_image_write).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace videotools {

/// `pixels` holds width * height * channels bytes, rows top to bottom; channels 1 (grey) or 4 (RGBA).
std::vector<std::uint8_t> encodePng(int width, int height, int channels, std::span<const std::uint8_t> pixels);

} // namespace videotools
