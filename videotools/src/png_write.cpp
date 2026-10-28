#include "png_write.hpp"

#include <stdexcept>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace videotools {

std::vector<std::uint8_t> encodePng(int width, int height, int channels, std::span<const std::uint8_t> pixels)
{
    if (width <= 0 || height <= 0 || pixels.size() != std::size_t(width) * std::size_t(height) * std::size_t(channels)) {
        throw std::invalid_argument("encodePng: pixel buffer doesn't match the size");
    }
    std::vector<std::uint8_t> out;
    const auto append = [](void* context, void* data, int size) {
        auto* v = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        v->insert(v->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(append, &out, width, height, channels, pixels.data(), width * channels)) {
        throw std::runtime_error("encodePng: stb_image_write failed");
    }
    return out;
}

} // namespace videotools
