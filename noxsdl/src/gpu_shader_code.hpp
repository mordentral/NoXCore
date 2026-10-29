// The compiled shaders embedded in noxsdl (noxsdl/CMakeLists.txt builds them from shaders/*.hlsl):
// DXIL (Direct3D 12) when the build had dxc, SPIR-V (Vulkan: SDL's GPU API on Linux) when it had
// glslang. Each shader is the same HLSL in either format; the device says which one it takes.
#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

namespace noxsdl {

/// Every embedded shader: the fragment shaders by name, view3d_scene_vert the 3D view's vertex shader.
#define NOXSDL_SHADER_LIST(X)                                                                              \
    X(material) X(material_split) X(fx_disc) X(fx_rect) X(fx_glow) X(fx_glowline) X(fx_puff) X(fx_barrier) X(fx_ripple) \
    X(bloom_blur) X(bloom_bright) X(fx_light) X(gamma) X(pending) X(weather) X(view3d_scene) X(view3d_material) X(view3d_particle) \
    X(view3d_scene_vert)

enum class ShaderId {
#define NOXSDL_SHADER_ENUM(name) name,
    NOXSDL_SHADER_LIST(NOXSDL_SHADER_ENUM)
#undef NOXSDL_SHADER_ENUM
};

struct ShaderCode {
    const unsigned char* code = nullptr;
    std::size_t size = 0;
};

/// The format of ours that `device` takes: DXIL when it takes DXIL and the build has it, else
/// SPIR-V likewise; SDL_GPU_SHADERFORMAT_INVALID for neither (the callers then draw without them).
SDL_GPUShaderFormat embeddedShaderFormat(SDL_GPUDevice* device);

/// `id` in `format` (one embeddedShaderFormat returned); empty if the build hasn't it.
ShaderCode embeddedShader(ShaderId id, SDL_GPUShaderFormat format);

} // namespace noxsdl
