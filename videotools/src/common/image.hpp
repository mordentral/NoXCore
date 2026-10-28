// RGBA images for the image tools: straight (not premultiplied) alpha, 8 bits per
// channel, rows top to bottom.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace videotools {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4

    RgbaImage() = default;
    RgbaImage(int w, int h) : width(w), height(h), rgba(std::size_t(w) * std::size_t(h) * 4, 0) {}

    bool empty() const { return width <= 0 || height <= 0; }
    std::uint8_t* at(int x, int y) { return &rgba[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4]; }
    const std::uint8_t* at(int x, int y) const
    {
        return &rgba[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4];
    }
};

/// Decodes a PNG (any bit depth or colour type) to RGBA. Throws std::runtime_error.
RgbaImage decodePng(std::span<const std::uint8_t> png);
RgbaImage readPng(const std::filesystem::path& path);
/// Writes RGBA as a compressed PNG, creating the folder. A 0 x 0 image is refused.
void writePng(const std::filesystem::path& path, const RgbaImage& image);
/// Writes only the colour (a 3-channel PNG), for tools that ignore alpha.
void writePngRgb(const std::filesystem::path& path, const RgbaImage& image);
/// The PNG bytes of an image, RGBA (4) or RGB (3) channels.
std::vector<std::uint8_t> encodePngBytes(const RgbaImage& image, int channels);

/// Copies `src` onto `dst` with its top-left at (x, y), replacing pixels (no blending); parts
/// outside `dst` are clipped.
void paste(RgbaImage& dst, const RgbaImage& src, int x, int y);
/// The w x h region at (x, y); outside `src` reads as transparent black.
RgbaImage crop(const RgbaImage& src, int x, int y, int w, int h);

} // namespace videotools
