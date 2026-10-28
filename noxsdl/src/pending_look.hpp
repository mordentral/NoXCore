// A map editor session's pending looks on the CPU: the same numbers as
// shaders/pending_common.hlsli, for the sprites' copies when there's no pending shader
// (SpriteCache::pendingCopy) and for the floor's composites, which never go through a shader.
// No SDL here (the tests take it).
#pragma once

#include "noxcore/mapedit/pending.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace noxsdl::pending {

using Look = nox::mapedit::PendingLook;

/// The alpha a look's sprite is drawn with, out of 255 (times its own): pending walls and things
/// translucent (≈ 0.6), ghosts more so.
constexpr std::uint8_t kAlpha[3] = {255, 158, 140};

inline std::uint8_t alphaFor(Look look, std::uint8_t alpha)
{
    return static_cast<std::uint8_t>(alpha * kAlpha[static_cast<int>(look) % 3] / 255);
}

/// pendingGrey: the pixel's brightness (0.299 R + 0.587 G + 0.114 B) in the look's grey.
inline void grey(std::uint8_t* rgb, Look look)
{
    const float l = (0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]) / 255.0f;
    const bool ghost = look == Look::Ghost;
    const float tint[3] = {ghost ? 0.74f : 0.80f, ghost ? 0.76f : 0.87f, ghost ? 0.86f : 1.00f};
    const float bright = ghost ? 0.78f : 1.12f;
    for (int c = 0; c < 3; ++c) {
        rgb[c] = static_cast<std::uint8_t>(std::clamp(l * tint[c] * bright, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

/// pendingHatch at a pixel: a ghost's every other 4-pixel diagonal band at 35%.
inline float hatch(int x, int y, Look look)
{
    return look == Look::Ghost && ((x + y) & 7) < 4 ? 0.35f : 1.0f;
}

/// RGBA pixels (`width` a row) in the look: greyed, a ghost's alpha hatched (`hatchScale` image
/// pixels a hatch pixel: an upscaled image's bands as wide as the original's).
inline void apply(std::vector<std::uint8_t>& rgba, int width, Look look, int hatchScale = 1)
{
    if (look == Look::Normal || width <= 0) return;
    hatchScale = std::max(hatchScale, 1);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        grey(&rgba[i], look);
        const int p = static_cast<int>(i / 4);
        const float h = hatch(p % width / hatchScale, p / width / hatchScale, look);
        if (h < 1.0f) rgba[i + 3] = static_cast<std::uint8_t>(rgba[i + 3] * h + 0.5f);
    }
}

/// A floor tile in the look: drawn opaque into the floor (nothing under it but black), so a
/// pending tile is only greyed, and a ghost (a tile the working copy removed) is the grey darkened
/// with its hatch bands darker still rather than see-through.
inline void applyFloor(std::vector<std::uint8_t>& rgba, int width, Look look, int hatchScale = 1)
{
    if (look == Look::Normal || width <= 0) return;
    hatchScale = std::max(hatchScale, 1);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        grey(&rgba[i], look);
        if (look == Look::Ghost) {
            const int p = static_cast<int>(i / 4);
            const float k = hatch(p % width / hatchScale, p / width / hatchScale, look) < 1.0f ? 0.45f : 0.8f;
            for (int c = 0; c < 3; ++c) rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(rgba[i + static_cast<std::size_t>(c)] * k);
        }
    }
}

} // namespace noxsdl::pending
