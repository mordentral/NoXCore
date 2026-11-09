#include "gpu_atlas.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void require(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << message << ": " << SDL_GetError() << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main()
{
    require(SDL_Init(SDL_INIT_VIDEO), "initialize video");
    SDL_Window* window = SDL_CreateWindow("Atlas upload regression", 64, 64, SDL_WINDOW_HIDDEN);
    require(window != nullptr, "create window");
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "gpu");
    require(renderer != nullptr, "create GPU renderer");
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
    SDL_GPUShader* labelShader = nullptr;
    if (std::strcmp(SDL_GetGPUDeviceDriver(device), "metal") == 0) {
        // Labels pages require a material shader; readback does not execute it.
        constexpr char code[] = "#include <metal_stdlib>\nusing namespace metal;\n"
                                "fragment float4 labels() { return float4(1.0); }\n";
        SDL_GPUShaderCreateInfo info{};
        info.code = reinterpret_cast<const Uint8*>(code);
        info.code_size = sizeof(code) - 1;
        info.entrypoint = "labels";
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 2;
        labelShader = SDL_CreateGPUShader(device, &info);
        require(labelShader != nullptr, "create labels test shader");
    }
    auto atlas = noxsdl::GpuAtlas::create(renderer, labelShader);
    require(atlas != nullptr, "BC7 GPU atlas required");

    // Different rows/blocks expose stride errors; widths exercise padding and atlas gutters.
    // Multiple uploads exercise nonzero transfer offsets and placements on a shared page.
    for (auto kind : {noxsdl::GpuAtlas::Kind::Bc7, noxsdl::GpuAtlas::Kind::Labels}) {
        if (kind == noxsdl::GpuAtlas::Kind::Labels && !atlas->takesLabels()) {
            continue;
        }
        const bool bc7 = kind == noxsdl::GpuAtlas::Kind::Bc7;
        for (int width : {4, 44, 64, 1920}) {
            constexpr int height = 12;
            const int rowBytes = bc7 ? width / 4 * 16 : width;
            std::vector<std::uint8_t> bytes(rowBytes * (bc7 ? height / 4 : height));
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = bc7 && i % 16 == 0 ? 0x40 : static_cast<std::uint8_t>((i * 17 + i / rowBytes * 53) & 255);
            }
            const noxsdl::GpuAtlas::Level level{width, height, bytes};
            std::vector<SDL_Point> corners;
            const noxsdl::GpuAtlas::Level preceding{4, 4, std::span(bytes.data(), 16)};
            require(atlas->place(kind, noxsdl::GpuAtlas::Pool::Demand, std::span(&preceding, 1), corners) >= 0,
                    "place preceding image for nonzero transfer offset");
            const int page = atlas->place(kind, noxsdl::GpuAtlas::Pool::Demand,
                                          std::span(&level, 1), corners);
            require(page >= 0, "place atlas image");
            atlas->flush();

            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            info.size = static_cast<Uint32>(bytes.size());
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &info);
            require(transfer != nullptr, "create readback buffer");
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            require(cmd != nullptr, "acquire readback command");
            SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion src{};
            src.texture = atlas->gpuTexture(page);
            src.x = corners[0].x;
            src.y = corners[0].y;
            src.w = width;
            src.h = height;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = transfer;
            SDL_DownloadFromGPUTexture(pass, &src, &dst);
            SDL_EndGPUCopyPass(pass);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            require(fence && SDL_WaitForGPUFences(device, true, &fence, 1), "wait for readback");
            void* actual = SDL_MapGPUTransferBuffer(device, transfer, false);
            require(actual != nullptr, "map readback");
            if (std::memcmp(actual, bytes.data(), bytes.size()) != 0) {
                std::cerr << (bc7 ? "BC7" : "R8") << " upload corrupted at width " << width << " on " << SDL_GetGPUDeviceDriver(device) << '\n';
                return EXIT_FAILURE;
            }
            SDL_UnmapGPUTransferBuffer(device, transfer);
            SDL_ReleaseGPUFence(device, fence);
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
    }
    std::cout << "Atlas upload bytes preserved on " << SDL_GetGPUDeviceDriver(device) << '\n';
    atlas.reset();
    if (labelShader) SDL_ReleaseGPUShader(device, labelShader);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
