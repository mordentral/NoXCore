#pragma once

// Tile edges drawn larger than the original's 46 x 46: an edge image's mask (a value a pixel:
// 0 keeps the tile, 1 takes the edge's own colour, 2 the floor it blends in) as smooth blend
// weights at an upscaled size, and an image's colours spread past its own pixels so a soft border
// has colour to blend.

#include <cstdint>
#include <span>
#include <vector>

namespace nox::video {

/// An edge mask's blend weights at `scale` times its size.
struct EdgeWeights {
    int size = 0;             ///< side * scale
    std::vector<float> own;   ///< size * size: how much of the edge's own colour, 0..1
    std::vector<float> floor; ///< size * size: how much of the floor it blends in; own + floor <= 1
};

/// The weights of an edge mask (side x side, one byte a pixel) at `scale` times its size. The
/// original's masks are hard, and many dither one value into another; scaled up as they are they
/// show as blocks and stipple beside upscaled floors. Each value's coverage is blurred a little and
/// sampled smoothly (Catmull-Rom): where the mask dithers (most neighbours differ) the coverage is
/// kept as a density, so the dither becomes the gradient it stood for; elsewhere a contrast curve
/// keeps the border crisp, so thin parts (a rug's fringe, a metal trim) stay solid. `inside` (side x
/// side, nonzero inside the tile; empty for all of it) keeps what lies outside the tile's diamond
/// out of the blur, so the weights don't fall away at the tile's rim.
EdgeWeights edgeWeights(std::span<const std::uint8_t> mask, int side, int scale, std::span<const std::uint8_t> inside = {});

/// Every pixel of an RGBA image (width x height) not marked `known` (nonzero) given the colour of
/// the nearest that is, alpha kept: a blend reaching past an edge's own pixels then mixes in its
/// colour rather than the transparent black around it. Unchanged if none is known.
void spreadColour(std::span<std::uint8_t> rgba, std::span<const std::uint8_t> known, int width, int height);

} // namespace nox::video
