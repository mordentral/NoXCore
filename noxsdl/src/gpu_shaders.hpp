// Our shaders for SDL's GPU renderer (the default; --no-gpu for SDL's usual one). The GPU
// renderer runs the same drawing as the default one, on SDL's GPU API; our shaders then do parts
// of it at a higher quality, keeping the game's behaviour:
//
//   material   recolours material pixels (bodies, clothes, armour, items) as they're drawn, so a
//              sprite needs one texture however many colour combinations it's drawn in
//              (shaders/material.frag.hlsl; SpriteCache::tinted builds a texture per combination
//              on the CPU otherwise). The same pixels as the CPU path: the map view is
//              pixel-identical; the menus' linear-filtered previews differ by at most 1 in 256 in a
//              few hundred pixels (the hardware's own rounding when it filters).
//   fx_disc,   the effects' dots and lines drawn smooth-edged at the screen's resolution instead
//   fx_rect    of scaled-up pixel shapes (effects_draw.hpp), same sizes, colours and positions
//              (unrounded, so they move smoothly); --stock-effects keeps the pixel shapes.
//   fx_glow    the effects' glow discs worked out per pixel from the game's falloff instead of
//              stamped from a texture (also off with --stock-effects).
//   fx_glowline  a line of those glow discs (lightning, beams) summed in one quad per segment
//              instead of thousands of overlapping discs.
//   fx_puff    soft, noisy puffs of smoke and gas: toxic clouds (HD Spell FX) and smoke blasts
//              (Smooth FX) as particles instead of the game's hard-edged sprites
//              (effects_puffs.cpp; --stock-effects).
//   fx_barrier the Force Field as a glassy honeycomb sphere instead of the game's
//              SphericalShieldAnim (effects_enchant.cpp; HD Spell FX, --stock-effects).
//   fx_ripple  the Reflective Shield as a disc of rippling water at the screen's resolution
//              instead of the game's ReflectiveShield* animations (effects_enchant.cpp; HD Spell
//              FX, --stock-effects).
//   bloom_blur the blur of the effects' bloom (bloom.hpp; --no-bloom, --bloom N).
//   bloom_bright  the bright parts of light sources' sprites, for the bloom.
//   fx_light   the game's light formula per screen pixel (lighting.hpp; --grid-light keeps the
//              game's 23-unit grid).
//   pending    a map editor session's pending edits greyed, and what they remove as hatched
//              ghosts (SpriteCache::renderPending). Optional:
//              without it SpriteCache::pendingCopy makes the same look on the CPU.
//   weather    nox.weather's rain, snow, storm and wind (weather.hpp). Optional: without it (and
//              on SDL's usual renderer) no weather is drawn.
//
// The shaders are DXIL for Direct3D 12 (what the GPU renderer picks on Windows) and SPIR-V for
// Vulkan (Linux), each built when the build finds its compiler (dxc, glslang). Otherwise, or on a
// device that takes neither, the GPU renderer does everything the way the default renderer does.
// A custom shader's inputs must be named as SDL's vertex shader names its outputs (TEXCOORD0
// colour, TEXCOORD1 texture coordinate: locations 0 and 1 in SPIR-V, in declaration order), and
// it must take the one texture SDL binds, or the driver refuses the pipeline and SDL skips the
// draw without a word; SDL_RENDER_GPU_DEBUG=1 shows such errors.
#pragma once

#include "gpu_shader_code.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace noxsdl {

class GpuShaders {
public:
    /// Our shaders on `renderer`, or nullptr if it isn't the GPU renderer or can't take them.
    static std::unique_ptr<GpuShaders> create(SDL_Renderer* renderer);
    ~GpuShaders();
    GpuShaders(const GpuShaders&) = delete;
    GpuShaders& operator=(const GpuShaders&) = delete;

    SDL_GPURenderState* material() const { return material_; }
    /// The material shader with the image and its material bytes in two textures
    /// (shaders/material_split.frag.hlsl): the image pack's BC7 images (GpuAtlas makes its render
    /// states). Null if it couldn't be made.
    SDL_GPUShader* materialSplit() const { return materialSplit_; }
    SDL_GPURenderState* disc() const { return disc_; }
    SDL_GPURenderState* rect() const { return rect_; }
    SDL_GPURenderState* glow() const { return glow_; }
    /// A line of glow discs summed in one quad (shaders/fx_glowline.frag.hlsl).
    SDL_GPURenderState* glowLine() const { return glowLine_; }
    SDL_GPURenderState* puff() const { return puff_; }
    /// The Force Field's barrier (shaders/fx_barrier.frag.hlsl).
    SDL_GPURenderState* barrier() const { return barrier_; }
    /// The Reflective Shield's water disc (shaders/fx_ripple.frag.hlsl).
    SDL_GPURenderState* ripple() const { return ripple_; }
    SDL_GPURenderState* blur() const { return blur_; }
    SDL_GPURenderState* bright() const { return bright_; }
    SDL_GPURenderState* light() const { return light_; }
    /// The game's gamma ramp over the finished frame (shaders/gamma.frag.hlsl).
    SDL_GPURenderState* gamma() const { return gamma_; }
    /// A pending edit's look (shaders/pending.frag.hlsl); null if it couldn't be made (the CPU's
    /// copies then: SpriteCache::pendingCopy).
    SDL_GPURenderState* pending() const { return pending_; }
    /// nox.weather's rain, snow, storm and wind (shaders/weather.frag.hlsl); null if it couldn't
    /// be made (no weather is drawn then).
    SDL_GPURenderState* weather() const { return weather_; }

private:
    GpuShaders() = default;
    bool add(SDL_Renderer* renderer, const char* name, ShaderId id, std::uint32_t uniformBuffers,
             SDL_GPURenderState*& state);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShaderFormat format_ = SDL_GPU_SHADERFORMAT_INVALID; ///< DXIL or SPIR-V, as the device takes
    std::vector<SDL_GPUShader*> shaders_;
    SDL_GPURenderState* material_ = nullptr;
    SDL_GPUShader* materialSplit_ = nullptr;
    SDL_GPURenderState* disc_ = nullptr;
    SDL_GPURenderState* rect_ = nullptr;
    SDL_GPURenderState* glow_ = nullptr;
    SDL_GPURenderState* glowLine_ = nullptr;
    SDL_GPURenderState* puff_ = nullptr;
    SDL_GPURenderState* barrier_ = nullptr;
    SDL_GPURenderState* ripple_ = nullptr;
    SDL_GPURenderState* blur_ = nullptr;
    SDL_GPURenderState* bright_ = nullptr;
    SDL_GPURenderState* light_ = nullptr;
    SDL_GPURenderState* gamma_ = nullptr;
    SDL_GPURenderState* pending_ = nullptr;
    SDL_GPURenderState* weather_ = nullptr;
};

} // namespace noxsdl
