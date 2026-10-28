// Nox's light map: ambient plus point lights on a grid, sampled by everything drawn.
//
// The game (client, rebuilt every frame):
//   grid       points 23 units apart at world (23i - 11, 23j - 11), covering the view; RGB in
//              16.16 fixed point, 0-255, filled with the map's ambient colour (AmbientData,
//              setAmbientColor)
//   lights     every drawable near the view with a light (cliLight16): LIGHT
//              class and enabled, or a monster's built-in glow. An omni light of
//              intensity I (clamped to 63; FLICKER adds up to I/4 at random) lights the points
//              within radius(I) (lightRadiusFromIntensity) by
//                  v = (min(I, 31) + 4) / (1 + 0.96789 * d^2 / floor(I)^2)
//              adding colour * (v - 4) / 32 where v > 4, or subtracting it for an
//              anti-light, clamped to 0-255
//   sampling   bilinear between the four points around a position; sprites and walls
//              are shaded by the sample at their base, the floor per pixel
//
//   cones      a directional light (LightOmni not 0xFFFF) lights only the grid points inside its
//              kite, by the same formula without the radius test, and never darkens
//              (light_cone.hpp has the shape, the game's scanline filler, and the animation)
//
// Here the grid covers the camera view at any size, and the floor is shaded by drawing the grid
// as a texture, one texel per point with linear filtering, multiplied over it; that is the same
// bilinear interpolation, and stays smooth at any resolution.
//
// Per-pixel lighting (the GPU renderer's default; --grid-light keeps the grid): the same lights,
// colours, intensities and flicker, but the formula is worked out for every screen pixel of the
// floor (the fx_light shader, into a target starting at the ambient colour, multiplied over
// the floor) and exactly at each sprite's base, instead of at the grid points 23 units apart and
// blended between them: a light is round and as bright as the formula says at its centre, not a
// blurred diamond, and small lights (sparks, particles) light what's round them.
#pragma once

#include "camera.hpp"
#include "light_cone.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl {

using Rgb = std::array<std::uint8_t, 3>;

struct LightSource {
    float x = 0;
    float y = 0;
    float intensity = 0; ///< as the game's light intensity (positive; `anti` for negative)
    Rgb color{255, 255, 255};
    bool anti = false;
    bool flicker = false;
    std::uint32_t seed = 0; ///< varies the flicker between lights
    /// A directional light's cone (its direction and half width, 16-bit angles); nullopt: all round.
    std::optional<lightcone::Cone> cone;
};

/// Light radius for an intensity (lightRadiusFromIntensity).
float lightRadius(float intensity);

class LightMap {
public:
    ~LightMap();

    /// Rebuilds the grid over the camera view. `frame` drives flicker.
    void build(const Camera& camera, Rgb ambient, std::span<const LightSource> lights, std::uint32_t frame);
    /// Light at a world position (bilinear); full ambient outside the grid. Per-pixel: the
    /// formula worked out exactly there.
    Rgb sample(float x, float y) const;
    /// Multiplies the grid over what's already drawn, in `camera`'s space.
    void drawOverlay(SDL_Renderer* renderer, const Camera& camera);

    /// Per-pixel lighting through the fx_light render state, drawn on `white` (a white pixel);
    /// nullptr for the game's grid.
    void setPerPixel(SDL_GPURenderState* state, SDL_Texture* white)
    {
        perPixel_ = state;
        white_ = white;
    }
    bool perPixel() const { return perPixel_ && white_; }
    /// Per-pixel: multiplies the light over the whole screen (called with the floor drawn and
    /// nothing else); `camera` is the screen's.
    void drawPerPixel(SDL_Renderer* renderer, const Camera& camera);

private:
    struct Accum {
        float r = 0;
        float g = 0;
        float b = 0;
    };
    /// A light as build() worked it out this frame (flicker applied).
    struct Resolved {
        float x = 0, y = 0;
        float radius = 0;    ///< lightRadius(intensity)
        float whole = 0;     ///< floor(intensity)
        float numerator = 0; ///< min(intensity, 31) + 4
        Rgb color{};
        bool anti = false;
        std::optional<lightcone::Kite> kite; ///< a cone's kite (world units); nullopt: all round
    };
    const Accum* at(int i, int j) const;
    Rgb sampleExact(float x, float y) const;
    /// One light's light at a point d2 (squared world units) away, added into p and clamped.
    static void add(Accum& p, const Resolved& l, float d2);

    int originI_ = 0; ///< lattice index of grid column 0 (world x = 23 * i - 11)
    int originJ_ = 0;
    int width_ = 0;
    int height_ = 0;
    Rgb ambient_{255, 255, 255};
    std::vector<Accum> grid_;
    SDL_Texture* texture_ = nullptr;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    std::vector<Resolved> resolved_;
    std::vector<std::pair<int, int>> conePoints_; ///< a cone's grid points (rasterize), reused
    SDL_GPURenderState* perPixel_ = nullptr;
    SDL_Texture* white_ = nullptr;
    SDL_Texture* target_ = nullptr; ///< per-pixel light, screen-sized
    int targetWidth_ = 0;
    int targetHeight_ = 0;
};

} // namespace noxsdl
