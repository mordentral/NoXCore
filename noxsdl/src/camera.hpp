// World-to-screen mapping. Everything is laid out in world units (the original game's pixels),
// and the camera scales by `zoom`, so the view works at any window size and HD art only changes
// the textures, never positions.
#pragma once

#include <SDL3/SDL.h>

#include <cmath>

namespace noxsdl {

struct Camera {
    float x = 2944; ///< world point at the centre of the view
    float y = 2944;
    float zoom = 1; ///< screen pixels per world unit
    int viewWidth = 1024;
    int viewHeight = 768;

    float left() const { return x - viewWidth / (2 * zoom); }
    float top() const { return y - viewHeight / (2 * zoom); }
    float right() const { return x + viewWidth / (2 * zoom); }
    float bottom() const { return y + viewHeight / (2 * zoom); }

    /// Sampling for original-resolution art at this zoom: pixel-art scaling (crisp, even pixels at
    /// fractional factors) when magnifying, linear when shrinking so detail averages out instead
    /// of shimmering.
    SDL_ScaleMode artScaleMode() const { return zoom >= 1 ? SDL_SCALEMODE_PIXELART : SDL_SCALEMODE_LINEAR; }

    /// Screen rectangle for a world rectangle, with edges snapped to whole pixels.
    SDL_FRect toScreen(float wx, float wy, float ww, float wh) const
    {
        const float x0 = std::floor((wx - left()) * zoom);
        const float y0 = std::floor((wy - top()) * zoom);
        const float x1 = std::floor((wx + ww - left()) * zoom);
        const float y1 = std::floor((wy + wh - top()) * zoom);
        return {x0, y0, x1 - x0, y1 - y0};
    }
};

} // namespace noxsdl
