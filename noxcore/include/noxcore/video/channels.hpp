// Colour channels of 5, 6 and 4 bits widened to 8 (the top bits repeated into the bottom), for
// the image decoders (sprite.cpp) and the movies' pictures (vqa.cpp).
#pragma once

#include <cstdint>

namespace nox::video {

constexpr std::uint8_t expand5(unsigned v)
{
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

constexpr std::uint8_t expand6(unsigned v)
{
    return static_cast<std::uint8_t>((v << 2) | (v >> 4));
}

constexpr std::uint8_t expand4(unsigned v)
{
    return static_cast<std::uint8_t>(v * 17);
}

} // namespace nox::video
