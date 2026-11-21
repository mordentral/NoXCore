// Hardware checks return CTest's skip code when the runner has no GPU renderer.
#pragma once

#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

namespace gpucheck {

inline void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Video {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Video()
    {
        if (SDL_Init(SDL_INIT_VIDEO)) window = SDL_CreateWindow("GPU regression check", 96, 96, SDL_WINDOW_HIDDEN);
        if (window) renderer = SDL_CreateRenderer(window, "gpu");
    }
    ~Video()
    {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

enum class Fault { None, Transfer, Map, Acquire, Copy, Render, Submit };
inline Fault fault = Fault::None;

inline SDL_GPUTransferBuffer* createTransfer(SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* info)
{
    return fault == Fault::Transfer ? nullptr : SDL_CreateGPUTransferBuffer(device, info);
}
inline void* mapTransfer(SDL_GPUDevice* device, SDL_GPUTransferBuffer* buffer, bool cycle)
{
    return fault == Fault::Map ? nullptr : SDL_MapGPUTransferBuffer(device, buffer, cycle);
}
inline SDL_GPUCommandBuffer* acquire(SDL_GPUDevice* device)
{
    return fault == Fault::Acquire ? nullptr : SDL_AcquireGPUCommandBuffer(device);
}
inline SDL_GPUCopyPass* copy(SDL_GPUCommandBuffer* cmd)
{
    return fault == Fault::Copy ? nullptr : SDL_BeginGPUCopyPass(cmd);
}
inline SDL_GPURenderPass* render(SDL_GPUCommandBuffer* cmd, const SDL_GPUColorTargetInfo* targets, Uint32 count,
                                 const SDL_GPUDepthStencilTargetInfo* depth)
{
    return fault == Fault::Render ? nullptr : SDL_BeginGPURenderPass(cmd, targets, count, depth);
}
inline bool submit(SDL_GPUCommandBuffer* cmd)
{
    if (fault != Fault::Submit) return SDL_SubmitGPUCommandBuffer(cmd);
    SDL_CancelGPUCommandBuffer(cmd);
    return false;
}

} // namespace gpucheck
