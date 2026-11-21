#include "gpu_fixture.hpp"
#include "../src/gpu_shader_code.hpp"

// Only this test translation unit interposes faults on the real device.
#define SDL_MapGPUTransferBuffer gpucheck::mapTransfer
#define SDL_AcquireGPUCommandBuffer gpucheck::acquire
#define SDL_BeginGPUCopyPass gpucheck::copy
#define SDL_BeginGPURenderPass gpucheck::render
#define SDL_SubmitGPUCommandBuffer gpucheck::submit
#include "../src/view3d_gpu.cpp"

int main() try {
    gpucheck::Video video;
    if (!video.renderer || noxsdl::embeddedShaderFormat(SDL_GetGPURendererDevice(video.renderer)) == SDL_GPU_SHADERFORMAT_INVALID) {
        std::cerr << "Skipping: GPU renderer with embedded shader support unavailable\n";
        return 77;
    }
    auto depth = noxsdl::DepthPass::create(video.renderer);
    gpucheck::require(depth != nullptr, "could not create depth pass");
    std::vector<noxsdl::DepthPass::Vertex> vertices(3);
    std::vector<noxsdl::DepthPass::Draw> draws;
    using gpucheck::Fault;
    for (auto fault : {Fault::Map, Fault::Acquire, Fault::Copy, Fault::Render, Fault::Submit}) {
        gpucheck::fault = fault;
        gpucheck::require(!depth->render(video.renderer, 64, 64, vertices, draws), "failed depth pass returned a scene");
        gpucheck::fault = Fault::None;
        gpucheck::require(depth->render(video.renderer, 64, 64, vertices, draws), "depth pass did not recover");
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
