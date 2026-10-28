#include "common/image.hpp"

#include "png_write.hpp"

#include "noxcore/io/file.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace videotools {

RgbaImage decodePng(std::span<const std::uint8_t> png)
{
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &channels, 4);
    if (!pixels) {
        throw std::runtime_error(std::string("PNG decode failed: ") + stbi_failure_reason());
    }
    RgbaImage img(w, h);
    std::memcpy(img.rgba.data(), pixels, img.rgba.size());
    stbi_image_free(pixels);
    return img;
}

RgbaImage readPng(const std::filesystem::path& path)
{
    try {
        return decodePng(nox::readFileBytes(path));
    } catch (const std::exception& ex) {
        throw std::runtime_error(path.string() + ": " + ex.what());
    }
}

void writePng(const std::filesystem::path& path, const RgbaImage& image)
{
    if (image.empty()) {
        throw std::invalid_argument("writePng: empty image for " + path.string());
    }
    std::filesystem::create_directories(path.parent_path());
    nox::writeFileBytes(path, encodePng(image.width, image.height, 4, image.rgba));
}

std::vector<std::uint8_t> encodePngBytes(const RgbaImage& image, int channels)
{
    if (image.empty()) {
        throw std::invalid_argument("encodePngBytes: empty image");
    }
    if (channels == 4) {
        return encodePng(image.width, image.height, 4, image.rgba);
    }
    std::vector<std::uint8_t> rgb(std::size_t(image.width) * std::size_t(image.height) * 3);
    for (std::size_t i = 0, o = 0; i < image.rgba.size(); i += 4, o += 3) {
        rgb[o] = image.rgba[i];
        rgb[o + 1] = image.rgba[i + 1];
        rgb[o + 2] = image.rgba[i + 2];
    }
    return encodePng(image.width, image.height, 3, rgb);
}

void writePngRgb(const std::filesystem::path& path, const RgbaImage& image)
{
    std::filesystem::create_directories(path.parent_path());
    nox::writeFileBytes(path, encodePngBytes(image, 3));
}

void paste(RgbaImage& dst, const RgbaImage& src, int x, int y)
{
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(dst.width, x + src.width);
    const int y1 = std::min(dst.height, y + src.height);
    for (int yy = y0; yy < y1; ++yy) {
        std::memcpy(dst.at(x0, yy), src.at(x0 - x, yy - y), std::size_t(std::max(0, x1 - x0)) * 4);
    }
}

RgbaImage crop(const RgbaImage& src, int x, int y, int w, int h)
{
    RgbaImage out(w, h);
    paste(out, src, -x, -y);
    return out;
}

} // namespace videotools
