// HD Spell FX's water: with HD Spell FX on, MapView draws the Water floors (Water,
// WaterNoTeleport) through shaders/fx_water.frag.hlsl, at the floor target's resolution: a water
// surface in Nox's own murky green over a body made from the floor's own tiles (water_base.hpp),
// with waves, the game's lights' sheen, lapping at the shore, and ripples and splashes where
// things are in it (water_ripples.hpp). Visual only; off (the default), the floor is drawn as
// ever, from whatever the image source gives.
//
// This holds the shader's render state and its base texture (made once from the tiles), and sets
// its uniforms for a floor target's world rectangle.
#pragma once

#include "lighting.hpp"
#include "water_base.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <span>

namespace noxsdl {

class HdWater {
public:
    /// The base on `renderer` through `shader` (GpuShaders::water); nullptr if anything fails.
    static std::unique_ptr<HdWater> create(SDL_Renderer* renderer, SDL_GPUShader* shader, const water::Base& base);
    ~HdWater();
    HdWater(const HdWater&) = delete;
    HdWater& operator=(const HdWater&) = delete;

    /// The body's frames per second: the frames run one into the next this fast (the six-frame
    /// loop in 4 s).
    static constexpr double kFramesPerSecond = 1.5;
    /// The most lights and ripples the shader takes (its kMaxLights, kMaxRipples).
    static constexpr int kMaxLights = 8;
    static constexpr int kMaxRipples = 24;

    SDL_GPURenderState* state() const { return state_; }
    /// The uniforms for a floor target whose top-left is world (left, top) at `texelsPerUnit`, at
    /// `seconds` (the map view's clock), with the map's anchor offset (water::anchorOffset), the
    /// lights round the view (LightMap::pointLights) and the ripples in flight
    /// (WaterRipples::pack: four floats each).
    void setView(float left, float top, float texelsPerUnit, double seconds, std::pair<int, int> offset,
                 std::span<const LightMap::PointLight> lights, std::span<const float> ripples);

private:
    HdWater() = default;
    SDL_Renderer* renderer_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_Texture* texture_ = nullptr; ///< the base (its GPU texture bound as the shader's second)
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;
    int period_ = 0;
    int frames_ = 0;
    float rho_ = 0;
    float detail_ = 0;
    std::array<float, 3> grain_{};
    std::array<float, 3> lo_{};
    std::array<float, 3> hi_{};
};

} // namespace noxsdl
