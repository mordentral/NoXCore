// The 3D view's world (view3d.hpp) drawn with a depth buffer, on SDL_Renderer's own GPU device.
//
// SDL_Renderer has no depth buffer, so View3D's triangles go straight to the renderer's
// SDL_GPUDevice instead: into a texture the size of the view, which the renderer then draws
// like any other (the HUD over it). Sprites are the renderer's own textures (their
// SDL_GPUTexture), sampled as they are; material sprites (weapons, armour) through a port of
// noxsdl's material shader.
//
// Order: the renderer records its work (the floor texture, sprite uploads) into a command buffer
// it submits only when it presents, and ours must run after that work. So before our pass the
// renderer's buffer is submitted (a 1x1 read-back, which submits and waits); then ours.
//
// Drawing: opaque draws first, any order, writing depth, each pixel under the alpha cut-off
// dropped (a sprite's transparent parts don't hide what's behind); then the translucent ones
// (alpha < 255: ghosts, smoke) far to near, blended, testing depth without writing it.
#pragma once

#include "sprite_cache.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace noxsdl {

class DepthPass {
public:
    /// Null without the GPU renderer, DXIL shaders or a depth format.
    static std::unique_ptr<DepthPass> create(SDL_Renderer* renderer);
    ~DepthPass();
    DepthPass(const DepthPass&) = delete;
    DepthPass& operator=(const DepthPass&) = delete;

    struct Vertex {
        float x = 0, y = 0, z = 0, w = 1; ///< clip space
        float u = 0, v = 0;               ///< in the texture (0-1)
        float r = 1, g = 1, b = 1, a = 1;
    };
    /// What a draw is: the world's textured triangles, or particles (view3d_particle: no
    /// texture, after everything else, testing depth without writing it).
    enum class Kind : std::uint8_t { Scene, ParticlesAdded, ParticlesBlended };
    struct Draw {
        SDL_Texture* texture = nullptr;
        const Sprite* sprite = nullptr; ///< for a material sprite's colours and place
        int first = 0;                  ///< triangles: `count` vertices from `first`
        int count = 0;
        bool blend = false;             ///< translucent: after the opaque, far to near
        bool linear = false;            ///< filtered (HD art, the floor); else nearest
        float depth = 0;                ///< for the translucent's order
        Kind kind = Kind::Scene;
    };

    /// Draws the triangles into a texture `width` x `height` (black where nothing is) and
    /// returns it, for the caller to draw; nullptr if it couldn't.
    SDL_Texture* render(SDL_Renderer* renderer, int width, int height, std::span<const Vertex> vertices,
                        std::vector<Draw>& draws);

private:
    DepthPass() = default;
    bool ensureTargets(SDL_Renderer* renderer, int width, int height);
    bool ensureBuffers(std::uint32_t bytes);

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vertex_ = nullptr;
    SDL_GPUShader* plain_ = nullptr;
    SDL_GPUShader* material_ = nullptr;
    SDL_GPUGraphicsPipeline* opaquePlain_ = nullptr;
    SDL_GPUGraphicsPipeline* opaqueMaterial_ = nullptr;
    SDL_GPUGraphicsPipeline* blendPlain_ = nullptr;
    SDL_GPUGraphicsPipeline* blendMaterial_ = nullptr;
    SDL_GPUShader* particle_ = nullptr;
    SDL_GPUGraphicsPipeline* particlesAdded_ = nullptr;
    SDL_GPUGraphicsPipeline* particlesBlended_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    SDL_GPUTextureFormat depthFormat_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    SDL_Texture* scene_ = nullptr; ///< the renderer's texture we draw into
    SDL_GPUTexture* depth_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    SDL_Texture* sync_ = nullptr; ///< 1x1: read back to submit the renderer's work
    SDL_GPUBuffer* vertexBuffer_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    std::uint32_t bufferBytes_ = 0;
};

} // namespace noxsdl
