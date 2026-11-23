// What HD Spell FX's water (hd_water.hpp, shaders/fx_water.frag.hlsl) is drawn from, worked out
// once on the CPU from the floor's own tiles; no SDL here (the tests take it as it is).
//
// An animated floor (Water: 3 x 3 tiles a frame, 6 frames) laid out by the map editors' variant
// rule (variant = (u + a) mod cols + cols ((b - v) mod rows), u = (gx + gy) / 2, v = (gx - gy) / 2:
// paint.hpp) makes one picture that repeats every 46 x cols world units across and down. The base
// is that picture for each frame, at a texel a world unit (the tiles' own pixels), with a soft
// low-pass of it beside it, and what the shader keeps its detail within: the tiles' darkest and
// brightest colours, the detail's colour direction and strength, and how alike one frame is to
// the next.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl::water {

inline constexpr int kTile = 46; ///< a floor tile's side, pixels and world units
/// Texels of the picture repeated round each block (the shader's widest reach past a texel), so
/// its reads never wrap one by one.
inline constexpr int kApron = 2;

struct Base {
    int period = 0; ///< world units the picture repeats over, across and down (46 x cols)
    int frames = 0;
    int block = 0;  ///< period + 2 kApron: a frame's picture (or low-pass) with its apron
    int width = 0;  ///< 2 x block: each frame's picture, then its low-pass beside it
    int height = 0; ///< frames x block, frame 0 at the top
    std::vector<std::uint8_t> rgba;
    std::array<float, 3> lo{};    ///< the darkest of each channel over every frame, 0-1
    std::array<float, 3> hi{};    ///< the brightest
    /// The detail's colour (the main direction of the picture less its low-pass), scaled so a
    /// step along it changes the luminance by one.
    std::array<float, 3> grain{};
    float detail = 0; ///< the detail's luminance standard deviation, 0-1
    float rho = 0;    ///< the detail's correlation from one frame to the next (the loop's mean)
};

/// The base from `images`: frames x cols x rows tiles, kTile x kTile RGBA (straight alpha), as
/// FLOR lists them (frame by frame, each frame's variants in order). nullopt unless cols == rows
/// (only then does the picture repeat on a square) and every image is there and kTile a side.
std::optional<Base> buildBase(std::span<const std::uint8_t* const> images, int cols, int rows, int frames);

/// A tile of the floor the anchor is learnt from: its top-left in world units (the game's
/// (23 gx - 11, 23 gy + 12)) and its variant.
struct PlacedTile {
    int x = 0;
    int y = 0;
    int variant = 0;
};
/// The variant rule's anchor (a, b) most of `tiles` follow ((0, 0) for none).
std::pair<int, int> learnAnchor(std::span<const PlacedTile> tiles, int cols, int rows);
/// What the base is sampled at for world point p: p plus this (mod the period), so the picture
/// shows each tile's own variant under anchor (a, b).
std::pair<int, int> anchorOffset(int a, int b, int period);

} // namespace noxsdl::water
