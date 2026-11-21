#include "gpu_fixture.hpp"
#include "../../noxcore/checks/fixture.hpp"
#include "../src/sprite_cache.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"

#include <string_view>

// Interpose only the copy compiled into this executable. SpriteCache calls that implementation.
#define SDL_CreateGPUTransferBuffer gpucheck::createTransfer
#define SDL_MapGPUTransferBuffer gpucheck::mapTransfer
#define SDL_AcquireGPUCommandBuffer gpucheck::acquire
#define SDL_BeginGPUCopyPass gpucheck::copy
#define SDL_SubmitGPUCommandBuffer gpucheck::submit
#include "../src/gpu_atlas.cpp"

namespace {

using checks::require;
using gpucheck::Fault;
using noxsdl::GpuAtlas;

// Slot 0 has a one-pixel SD stand-in. Slots 1 and 2 exist only in the pack and must load synchronously.
void makeImages(const std::filesystem::path& dir)
{
    nox::ByteWriter bag;
    bag.u32(1); bag.u32(1); // sprite width and height
    bag.zeros(9);           // offsets and header byte
    bag.u16(0xffff);
    nox::writeFileBytes(dir / "video.bag", bag.data());

    nox::ByteWriter idx;
    idx.u32(0xFAEDBCEA); idx.u32(0); idx.u32(1); // plain index, size, one segment
    idx.u32(0); idx.u32(0); idx.u32(1);          // buffer size, reserved, one image
    idx.u32(0); idx.u32(static_cast<Uint32>(bag.size())); idx.u32(1);
    idx.u8(1); idx.rawString("a"); idx.u8(2); // full-colour sprite
    idx.u32(static_cast<Uint32>(bag.size())); idx.u32(0);
    idx.patchU32(4, static_cast<Uint32>(idx.size()));
    nox::writeFileBytes(dir / "video.idx", idx.data());

    nox::video::ImagePackWriter pack(dir / "test.nxp", 3);
    for (unsigned i = 0; i < 3; ++i) {
        const int side = i == 1 ? 4088 : 4; // with the BC7 gutters, slot 1 fills a 16 MiB batch
        nox::video::PackImage image;
        image.name = std::to_string(i);
        image.width = image.height = 1;
        image.pixelWidth = image.pixelHeight = side;
        image.type = 2;
        image.bc7.emplace_back(static_cast<std::size_t>(side) * side);
        for (std::size_t b = 0; b < image.bc7[0].size(); b += 16) image.bc7[0][b] = 0x40;
        pack.put(i, image, 1);
    }
    pack.finish();
}

// One worker and a one-block budget make the queue limit deterministic.
void checkDecoderBacklog()
{
    checks::TempDirectory temp;
    const auto path = temp.path / "small.nxp";
    nox::video::ImagePackWriter pack(path, 4);
    nox::video::PackImage image;
    image.width = image.height = 1;
    image.pixelWidth = image.pixelHeight = 4;
    image.type = 2;
    image.bc7.emplace_back(16, 0x40);
    for (unsigned i = 0; i < 4; ++i) pack.put(i, image, 1);
    pack.finish();

    noxsdl::ImageDecoder decoder(path, 1, 16);
    for (int i = 0; i < 4; ++i) {
        noxsdl::DecodeJob job;
        job.index = i;
        job.blocks = true;
        decoder.request(job, SDL_GetTicks());
    }
    noxsdl::DecodeJob floor;
    floor.kind = noxsdl::DecodeKind::Composite;
    floor.cacheKey = "floor";
    floor.width = floor.height = 1;
    floor.build = [] { return std::vector<std::uint8_t>(4, 255); };
    decoder.preload({floor});

    noxsdl::DecodeResult result;
    require(decoder.takeWait(result, 5000, false) && result.ok && result.job.key() == floor.key(),
            "a saturated demand queue starved the floor preload");
    decoder.dropRequests(); // Only work admitted before the floor completed should remain.
    require(decoder.take(result) && result.needsAtlas(), "lost deferred atlas result");
    require(!decoder.take(result), "decoded past the blocked atlas budget");
}

void checkSpriteCache(SDL_Renderer* renderer)
{
    checks::TempDirectory temp;
    makeImages(temp.path);
    nox::video::VideoBag bag(temp.path / "video.bag", temp.path / "video.idx");
    noxsdl::SpriteCache cache(renderer, bag);
    cache.setPlaceholders(true);
    const auto path = temp.path / "test.nxp";
    cache.setPack(std::make_unique<nox::video::ImagePackReader>(path), path);

    gpucheck::fault = Fault::Submit;
    const auto* first = cache.get(1);
    require(first && first->gpuBatch && !first->ready(), "large upload did not block");
    cache.beginSession({{{0}}});
    const auto* placeholder = cache.get(0);
    require(placeholder && !placeholder->hd, "second sprite must start at SD");
    const auto* immediate = cache.get(2);
    require(immediate && immediate->hd && immediate->ready(), "synchronous HD request lost while blocked");
    cache.endFrame(SDL_GetTicks());
    require(cache.preloading(), "discarded the queued sprite while blocked");

    require(!cache.hdTile(0), "expected an asynchronous floor tile");
    for (int frame = 0; frame < 3; ++frame) {
        cache.endFrame(SDL_GetTicks());
        require(!first->ready(), "atlas unexpectedly recovered during sustained failure");
    }
    cache.awaitAsked(5000);
    const auto tile = cache.hdTile(0);
    require(tile && tile->rgba.size() == 4 * 4 * 4, "floor tile stalled behind the blocked atlas");
    require(cache.preloading(), "atlas preload lost before recovery");

    gpucheck::fault = Fault::None;
    const auto deadline = SDL_GetTicks() + 5000;
    do {
        cache.endFrame(SDL_GetTicks());
        SDL_Delay(1);
    } while (cache.preloading() && SDL_GetTicks() < deadline);
    const auto* second = cache.get(0);
    require(!cache.preloading() && first->ready() && second && second->hd && second->gpuBatch && second->ready(),
            "worker result did not replace the SD placeholder after recovery");
}
} // namespace

int main(int argc, char** argv) try {
    if (argc == 2 && std::string_view(argv[1]) == "--decoder") {
        checkDecoderBacklog();
        return 0;
    }
    gpucheck::Video video;
    if (!video.renderer || !GpuAtlas::create(video.renderer, nullptr)) {
        std::cerr << "Skipping: BC7 GPU renderer unavailable\n";
        return 77;
    }
    for (auto fault : {Fault::Transfer, Fault::Map, Fault::Acquire, Fault::Copy, Fault::Submit}) {
        auto atlas = GpuAtlas::create(video.renderer, nullptr);
        std::vector<std::uint8_t> bytes(16);
        bytes[0] = 0x40;
        const GpuAtlas::Level level{4, 4, bytes};
        std::vector<SDL_Point> at;
        checks::require(atlas->place(GpuAtlas::Kind::Bc7, GpuAtlas::Pool::Demand, {&level, 1}, at) >= 0, "could not stage sprite");
        const auto batch = atlas->latestBatch();
        gpucheck::fault = fault;
        checks::require(!atlas->ready(batch) && atlas->pending(), "failed upload left sprite drawable");
        gpucheck::fault = Fault::None;
        atlas->endFrame();
        checks::require(atlas->ready(batch) && !atlas->pending(), "atlas failed to retry upload");
    }
    checkSpriteCache(video.renderer);
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
