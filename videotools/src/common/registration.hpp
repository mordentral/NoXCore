// Frame registration: putting a direction's differently cropped frames on one shared canvas by
// their Nox draw offsets, so that a change of crop never looks like motion.
//
// Coordinate spaces, per track:
//   Nox offset space  where the game places a frame: its top-left at (offsetX, offsetY)
//   canvas            original pixels; canvas = nox - origin. Every frame of the track fits, with
//                     `padding` transparent pixels around the union of the frames and the size
//                     rounded up to a multiple of `align` (the extra on the right and bottom)
//   HD                canvas * scale (integer scales), an upscaled image's working and output space
//   final crop        a frame's own rectangle in HD: canvasRect(i) * scale. For an original frame
//                     that is exactly its Nox crop, so its Nox offsets carry over unchanged
//                     (noxsdl draws sprites at their logical size whatever the texture size).
#pragma once

#include "common/image.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace videotools {

struct IRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
    IRect scaled(int s) const { return {x * s, y * s, w * s, h * s}; }
    bool operator==(const IRect&) const = default;
};

/// The smallest rectangle holding both (an empty one doesn't count).
IRect unite(const IRect& a, const IRect& b);

/// A frame as the game places it.
struct FrameGeometry {
    int offsetX = 0;
    int offsetY = 0;
    int width = 0;
    int height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
};

struct RegistrationOptions {
    int padding = 8; // transparent border around the frames, in original pixels (room for AI context)
    int align = 4;   // canvas size multiple
};

struct Registration {
    int originX = 0; // the Nox offset at canvas pixel (0, 0)
    int originY = 0;
    int width = 0;   // canvas size in original pixels
    int height = 0;
    RegistrationOptions options;
    std::vector<FrameGeometry> frames;

    /// Where frame i's cropped image sits on the canvas (empty for an empty frame).
    IRect canvasRect(std::size_t i) const;
    /// The union of every frame's canvasRect.
    IRect contentRect() const;
};

/// The shared canvas for one track's frames. Throws std::invalid_argument if all are empty.
Registration registerFrames(std::span<const FrameGeometry> frames, const RegistrationOptions& options = {});

/// Frame i's cropped image on a transparent canvas (a transparent canvas for an empty frame).
RgbaImage placeOnCanvas(const Registration& reg, std::size_t i, const RgbaImage& cropped);

/// Cuts `rect` (canvas coordinates) out of a canvas image at `scale` times the original size.
RgbaImage cropFromCanvas(const RgbaImage& canvasImage, const IRect& rect, int scale);

nlohmann::json toJson(const Registration& reg);

} // namespace videotools
