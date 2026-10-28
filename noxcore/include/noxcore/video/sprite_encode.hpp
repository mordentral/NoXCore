// Encoding images into video.bag's formats (the reverse of video/sprite.hpp's decodeImage), for
// tools that replace an image in the bag (video/video_bag_writer.hpp).
//
// decodeImage(encodeImage(img), img.type) gives img back with its colours rounded to what the
// type stores (RGB555, RGB565, RGBA4444, 1-bit alpha), and an image that came from decodeImage
// comes back exactly. Per type:
//   0     the 46 x 46 diamond's pixels; alpha and what lies outside the diamond are dropped
//   1     the diamond's rows covered by the edge mask (img.edgeMask; without one, opaque pixels
//         are the edge's own colour and the rest left alone); first and last row are the
//         outermost rows the mask touches
//   2, 7  every pixel, RGBA5551 (alpha under 128 sets the transparent bit; the colour is kept)
//   3-6   runs: transparent (alpha 0; RGBA4444 if a colour RGBA4444 holds is kept under it), material-shaded (img.material: the grey level, by its
//         slot), opaque RGB555, and RGBA4444 for partial alpha (and for opaque colours that are
//         exact in RGBA4444 but not in RGB555, so a decoded image comes back exactly)
//   8     runs, RGB565, alpha 0 or 255 (under 128 is transparent)
// Runs are at most 253 pixels and a sprite's data ends with an empty run (0, 0), as in every
// shipped sprite. The game's run drawer also knows a run kind with no data that doesn't move
// along the row (op 6) and op 7 (drawn as op 2); the shipped data has neither, and the encoder
// writes neither.
//
// Against the shipped video.bag (1.2b), re-encoding every decoded image gives the same picture
// for all of them, and the very same bytes for every floor tile and type 3 and 4 sprite, all but
// 4 of 851 tile edges and about half of the type 5 sprites (whose encoder stored some clear
// pixels as RGBA4444 zeros).
#pragma once

#include "noxcore/video/sprite.hpp"

#include <cstdint>
#include <vector>

namespace nox::video {

/// Whether encodeImage writes images of `type`.
bool canEncodeImage(std::uint8_t type);

/// Encodes `image` into video.bag's bytes for image.type, with its width, height, offsets and
/// header byte (`unknown`). Throws FormatError if the type can't be encoded, a tile isn't 46 x 46,
/// a sprite is over 1024 pixels a side, or the planes don't match the size.
std::vector<std::uint8_t> encodeImage(const DecodedImage& image);

/// The nearest 5-, 6- and 4-bit levels of an 8-bit channel (the inverse of expand5 & co. on
/// their outputs).
std::uint8_t nearest5(std::uint8_t v);
std::uint8_t nearest6(std::uint8_t v);
std::uint8_t nearest4(std::uint8_t v);

} // namespace nox::video
