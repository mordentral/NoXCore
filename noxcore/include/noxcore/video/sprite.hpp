// Decoding video.bag images (16-bit video.bag formats; port of OpenNox noximage/pcx, MIT).
//
// Types:
//   0        floor tile: a 46x46 diamond of RGB555 pixels (rows 1, 3, .. 45, 45, .. 1 wide,
//            centred; table built by initSight)
//   1        tile edge: u8 firstRow, u8 lastRow, then for each of those diamond rows runs
//            { u8 op; u8 count } covering the row: 1 leave, 2 count RGB555 pixels follow,
//            3 show the blended floor tile (drawer drawTexEdgesProbably)
//   2, 7     full RGBA5551 image with the sprite header
//   3-6, 8   run-length sprite: header, then per row { u8 op; u8 count; data } runs
// Sprite header: i32 width, height, offsetX, offsetY; u8 unknown.
// Run ops (low nibble): 1 transparent, 2/7 RGB555 (RGB565 in type 8), 4 material-shaded bytes
// (high nibble = material slot), 5 RGBA4444, 6 no data.
// RGBA5551 is RGB555 with bit 15 meaning transparent.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nox::video {

struct DecodedImage {
    std::uint8_t type = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t offsetX = 0; // draw offset of sprites (0 for tiles)
    std::int32_t offsetY = 0;
    std::uint8_t unknown = 0; // header byte 16
    std::vector<std::uint8_t> rgba; // width * height * 4, straight alpha
    /// Empty when unused. Otherwise per pixel: 0 = plain colour, n = material slot n-1; such
    /// pixels hold a grey intensity in `rgba` that the game shades with the object's colour.
    std::vector<std::uint8_t> material;
    /// Type 1 only, per pixel: 0 leave what's there, 1 the edge's own colour (in `rgba`),
    /// 2 the floor texture being blended in (FloorEdge::floor).
    std::vector<std::uint8_t> edgeMask;
    /// Bytes of the image data the decoder didn't use (sprites end with a short terminator).
    std::size_t unusedBytes = 0;
};

/// Decodes one image's bytes. Throws FormatError for unknown types or malformed data.
DecodedImage decodeImage(std::span<const std::uint8_t> data, std::uint8_t type);

} // namespace nox::video
