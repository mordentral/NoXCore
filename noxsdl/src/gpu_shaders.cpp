// Our fragment shaders for SDL's GPU renderer (gpu_shaders.hpp). Their DXIL / SPIR-V blobs are
// compiled and embedded by the build (noxsdl/CMakeLists.txt, gpu_shader_code.hpp) when it can
// make them.
#include "gpu_shaders.hpp"

#include "gpu_shader_code.hpp"

#include <cstddef>

namespace noxsdl {

// One fragment shader and the render state that uses it; false (logged) if either fails.
bool GpuShaders::add(SDL_Renderer* renderer, const char* name, ShaderId id, std::uint32_t uniformBuffers,
                     SDL_GPURenderState*& state)
{
    const ShaderCode code = embeddedShader(id, format_);
    SDL_GPUShaderCreateInfo info{};
    info.code = code.code;
    info.code_size = code.size;
    info.entrypoint = "main";
    info.format = format_;
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 1; // the texture SDL binds for every textured draw
    info.num_uniform_buffers = uniformBuffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) {
        SDL_Log("gpu: %s shader: %s", name, SDL_GetError());
        return false;
    }
    shaders_.push_back(shader);
    SDL_GPURenderStateCreateInfo create{};
    create.fragment_shader = shader;
    state = SDL_CreateGPURenderState(renderer, &create);
    if (!state) {
        SDL_Log("gpu: %s render state: %s", name, SDL_GetError());
        return false;
    }
    return true;
}

std::unique_ptr<GpuShaders> GpuShaders::create(SDL_Renderer* renderer)
{
#if defined(NOXSDL_HAVE_DXIL) || defined(NOXSDL_HAVE_SPIRV)
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
    if (!device) {
        return nullptr; // not the GPU renderer
    }
    const SDL_GPUShaderFormat format = embeddedShaderFormat(device);
    if (format == SDL_GPU_SHADERFORMAT_INVALID) {
        SDL_Log("gpu: %s takes none of our shaders' formats; drawing without our shaders", SDL_GetGPUDeviceDriver(device));
        return nullptr;
    }
    std::unique_ptr<GpuShaders> out(new GpuShaders);
    out->device_ = device;
    out->format_ = format;
    // Every shader or none: one missing and the renderer draws without our shaders.
    if (!out->add(renderer, "material", ShaderId::material, 1, out->material_) ||  // (each one's number: its uniform buffers)
        !out->add(renderer, "fx_disc", ShaderId::fx_disc, 0, out->disc_) ||
        !out->add(renderer, "fx_rect", ShaderId::fx_rect, 0, out->rect_) ||
        !out->add(renderer, "fx_glow", ShaderId::fx_glow, 0, out->glow_) ||
        !out->add(renderer, "fx_glowline", ShaderId::fx_glowline, 1, out->glowLine_) ||
        !out->add(renderer, "fx_puff", ShaderId::fx_puff, 0, out->puff_) ||
        !out->add(renderer, "fx_barrier", ShaderId::fx_barrier, 1, out->barrier_) ||
        !out->add(renderer, "fx_ripple", ShaderId::fx_ripple, 1, out->ripple_) ||
        !out->add(renderer, "bloom_blur", ShaderId::bloom_blur, 1, out->blur_) ||
        !out->add(renderer, "bloom_bright", ShaderId::bloom_bright, 0, out->bright_) ||
        !out->add(renderer, "fx_light", ShaderId::fx_light, 0, out->light_) ||
        !out->add(renderer, "gamma", ShaderId::gamma, 1, out->gamma_)) {
        return nullptr;
    }
    // A map editor session's pending look: on its own (without it the sprite cache greys copies
    // on the CPU), never taking the others with it.
    if (!out->add(renderer, "pending", ShaderId::pending, 1, out->pending_)) {
        out->pending_ = nullptr;
    }
    // nox.weather's look: on its own too (without it no weather is drawn).
    if (!out->add(renderer, "weather", ShaderId::weather, 1, out->weather_)) {
        out->weather_ = nullptr;
    }
    // The material shader for BC7 images: no render state of its own (GpuAtlas makes one per
    // material page, which binds the page as its second texture).
    const ShaderCode splitCode = embeddedShader(ShaderId::material_split, format);
    SDL_GPUShaderCreateInfo split{};
    split.code = splitCode.code;
    split.code_size = splitCode.size;
    split.entrypoint = "main";
    split.format = format;
    split.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    split.num_samplers = 2; // the BC7 page SDL binds, the material page
    split.num_uniform_buffers = 1;
    out->materialSplit_ = SDL_CreateGPUShader(device, &split);
    if (out->materialSplit_) {
        out->shaders_.push_back(out->materialSplit_);
    } else {
        SDL_Log("gpu: material_split shader: %s (BC7 material sprites shaded on the CPU)", SDL_GetError());
    }
    SDL_Log("gpu: shaders on %s", SDL_GetGPUDeviceDriver(device));
    return out;
#else
    (void)renderer;
    return nullptr;
#endif
}

GpuShaders::~GpuShaders()
{
    for (SDL_GPURenderState* state : {material_, disc_, rect_, glow_, glowLine_, puff_, barrier_, ripple_, blur_, bright_, light_, gamma_, pending_, weather_}) {
        if (state) {
            SDL_DestroyGPURenderState(state);
        }
    }
    for (SDL_GPUShader* shader : shaders_) {
        SDL_ReleaseGPUShader(device_, shader);
    }
}

} // namespace noxsdl
