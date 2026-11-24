// HD Spell FX's water (hd_water.hpp).
#include "hd_water.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl {

namespace {

// The look's settings against the tiles' own measures (water_base.hpp), set by eye against the
// original in game: the detail a little stronger than the smooth sample leaves it (the
// original's contrast back), the grain as strong as the original's detail, and the flecks'
// outlines nudged under half a texel. The waves' strength scales their slopes (shaders/
// fx_water.frag.hlsl waveSlope): a gentle relief.
constexpr float kDetailGain = 1.25f;
constexpr float kGrainOfDetail = 1.65f;
constexpr float kWarpUnits = 0.35f;
constexpr float kWaves = 0.4f;
/// The clock the waves run on, looped (a float keeps a second's fractions exact that far).
constexpr double kLoopSeconds = 3600;

} // namespace

std::unique_ptr<HdWater> HdWater::create(SDL_Renderer* renderer, SDL_GPUShader* shader, const water::Base& base)
{
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
    if (!device || !shader || base.rgba.empty()) {
        return nullptr;
    }
    std::unique_ptr<HdWater> out(new HdWater);
    out->device_ = device;
    out->period_ = base.period;
    out->frames_ = base.frames;
    out->rho_ = base.rho;
    out->detail_ = base.detail;
    out->grain_ = base.grain;
    out->lo_ = base.lo;
    out->hi_ = base.hi;
    out->texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, base.width, base.height);
    if (!out->texture_ || !SDL_UpdateTexture(out->texture_, nullptr, base.rgba.data(), base.width * 4)) {
        SDL_Log("hd water: the base texture: %s", SDL_GetError());
        return nullptr;
    }
    auto* gpu = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(SDL_GetTextureProperties(out->texture_), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    SDL_GPUSamplerCreateInfo sampler{};
    // Linear: the shader's reads each blend four texels (its Catmull-Rom in nine reads, its
    // low-pass in one), always within a frame's block and its apron.
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    out->sampler_ = gpu ? SDL_CreateGPUSampler(device, &sampler) : nullptr;
    if (!out->sampler_) {
        SDL_Log("hd water: the base's sampler: %s", SDL_GetError());
        return nullptr;
    }
    SDL_GPUTextureSamplerBinding binding{gpu, out->sampler_};
    SDL_GPURenderStateCreateInfo state{};
    state.fragment_shader = shader;
    state.num_sampler_bindings = 1;
    state.sampler_bindings = &binding;
    out->state_ = SDL_CreateGPURenderState(renderer, &state);
    if (!out->state_) {
        SDL_Log("hd water: render state: %s", SDL_GetError());
        return nullptr;
    }
    return out;
}

HdWater::~HdWater()
{
    if (state_) {
        SDL_DestroyGPURenderState(state_);
    }
    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
    }
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
}

void HdWater::setView(float left, float top, float texelsPerUnit, double seconds, std::pair<int, int> offset,
                      std::span<const LightMap::PointLight> lights, std::span<const float> ripples)
{
    // The frame and the waves' clock, looped here in double so the floats the shader gets stay
    // exact however long the map has run.
    seconds = std::max(seconds, 0.0);
    const double phase = std::fmod(seconds * kFramesPerSecond, static_cast<double>(frames_));
    const int lightCount = static_cast<int>(std::min<std::size_t>(lights.size(), kMaxLights));
    const int rippleCount = static_cast<int>(std::min<std::size_t>(ripples.size() / 4, kMaxRipples));
    // (As the shader's cbuffer: 7 float4s, then the lights' 2 each, then the ripples'.)
    struct Uniforms {
        float head[28];
        float lights[kMaxLights * 8];
        float ripples[kMaxRipples * 4];
    } u{};
    const float head[28] = {
        left, top, texelsPerUnit, static_cast<float>(phase),
        static_cast<float>(offset.first), static_cast<float>(offset.second), static_cast<float>(period_), static_cast<float>(frames_),
        rho_, kDetailGain, kGrainOfDetail * detail_, kWarpUnits,
        grain_[0], grain_[1], grain_[2], 0,
        lo_[0], lo_[1], lo_[2], 0,
        hi_[0], hi_[1], hi_[2], 0,
        static_cast<float>(std::fmod(seconds, kLoopSeconds)), static_cast<float>(rippleCount), static_cast<float>(lightCount), kWaves,
    };
    std::copy(std::begin(head), std::end(head), u.head);
    for (int i = 0; i < lightCount; ++i) {
        const LightMap::PointLight& l = lights[static_cast<std::size_t>(i)];
        float* o = &u.lights[i * 8];
        o[0] = l.x;
        o[1] = l.y;
        o[2] = l.radius;
        o[4] = l.color[0] / 255.0f;
        o[5] = l.color[1] / 255.0f;
        o[6] = l.color[2] / 255.0f;
    }
    std::copy(ripples.begin(), ripples.begin() + rippleCount * 4, u.ripples);
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, &u, sizeof(u));
}

} // namespace noxsdl
