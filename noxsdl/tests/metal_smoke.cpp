// Requires a Mac graphical session; no game files or test/cheat commands.
#include "gpu_shader_code.hpp"
#include "gpu_shaders.hpp"

#include <cstdio>

int main()
{
    using namespace noxsdl;
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "metal");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("NoX Metal smoke", 64, 64, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, SDL_GPU_RENDERER) : nullptr;
    SDL_GPUDevice* device = renderer ? SDL_GetGPURendererDevice(renderer) : nullptr;
    bool ok = device && embeddedShaderFormat(device) == SDL_GPU_SHADERFORMAT_MSL;
    if (ok) {
        auto shaders = GpuShaders::create(renderer);
        ok = shaders != nullptr;
        // Include the optional/3D shaders, not only the mandatory 2D effects.
        struct Case { ShaderId id; unsigned samplers, uniforms; SDL_GPUShaderStage stage; };
        const Case cases[] = {
            {ShaderId::pending, 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::weather, 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::material_split, 2, 1, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::view3d_scene, 1, 1, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::view3d_material, 2, 1, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::view3d_particle, 0, 0, SDL_GPU_SHADERSTAGE_FRAGMENT},
            {ShaderId::view3d_scene_vert, 0, 0, SDL_GPU_SHADERSTAGE_VERTEX},
        };
        for (const auto& c : cases) {
            const auto code = embeddedShader(c.id, SDL_GPU_SHADERFORMAT_MSL);
            SDL_GPUShaderCreateInfo info{};
            info.code = code.code;
            info.code_size = code.size;
            info.entrypoint = code.entrypoint;
            info.format = SDL_GPU_SHADERFORMAT_MSL;
            info.stage = c.stage;
            info.num_samplers = c.samplers;
            info.num_uniform_buffers = c.uniforms;
            auto* shader = SDL_CreateGPUShader(device, &info);
            if (!shader) {
                std::fprintf(stderr, "shader %d: %s\n", static_cast<int>(c.id), SDL_GetError());
                ok = false;
            } else {
                SDL_ReleaseGPUShader(device, shader);
            }
        }
        // Exercise command submission and readback on the actual Metal backend.
        ok = SDL_SetRenderDrawColor(renderer, 17, 83, 191, 255) && ok;
        ok = SDL_RenderClear(renderer) && ok;
        SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
        Uint8 r = 0, g = 0, b = 0, a = 0;
        ok = pixels && SDL_ReadSurfacePixel(pixels, 0, 0, &r, &g, &b, &a) &&
             r == 17 && g == 83 && b == 191 && a == 255 && ok;
        SDL_DestroySurface(pixels);
        std::printf("Metal shaders and readback: %s; window density %.1f\n", ok ? "PASS" : "FAIL",
                    SDL_GetWindowPixelDensity(window));
    } else {
        std::fprintf(stderr, "Metal renderer: %s\n", SDL_GetError());
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}
