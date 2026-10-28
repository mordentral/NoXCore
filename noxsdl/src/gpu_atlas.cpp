// GpuAtlas (gpu_atlas.hpp): the BC7 and material-byte pages, their shelves, and the staged uploads.
#include "gpu_atlas.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace noxsdl {

namespace {

// Page sides: the session's and a map's pages hold hundreds of images; those made as they're
// first wanted come a few at a time, so their pages are smaller.
constexpr int kBc7PageSide = 4096;         // 16 MB of BC7
constexpr int kBc7DemandPageSide = 2048;   // 4 MB
constexpr int kLabelPageSide = 2048;       // 4 MB of R8
constexpr int kLabelDemandPageSide = 1024; // 1 MB
constexpr int kBlock = 4;            // BC7's block side
constexpr int kBlockBytes = 16;
constexpr int kGutter = 4;           // one clear block
constexpr std::uint32_t kRowPitch = 256;   // Direct3D 12's texture row pitch alignment
constexpr std::uint32_t kPlacement = 512;  // and its upload placement alignment
// Staged uploads go up once they reach this, whatever else comes (so the transfer buffer stays
// this size, plus one big image at most).
constexpr std::size_t kFlushAt = std::size_t{16} << 20;

// A transparent BC7 block: mode 6 (bit 6 of the first byte), every endpoint and index 0, so
// all 16 texels are (0, 0, 0, 0).
constexpr std::uint8_t kClearBlock[kBlockBytes] = {0x40};

constexpr int roundUp(int v, int to)
{
    return (v + to - 1) / to * to;
}

constexpr std::uint32_t roundUp(std::uint32_t v, std::uint32_t to)
{
    return (v + to - 1) / to * to;
}

} // namespace

std::unique_ptr<GpuAtlas> GpuAtlas::create(SDL_Renderer* renderer, SDL_GPUShader* labelShader)
{
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(renderer);
    if (!device) {
        return nullptr; // (not the GPU renderer)
    }
    if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        SDL_Log("image pack: %s can't sample BC7; its images decoded to RGBA on the CPU", SDL_GetGPUDeviceDriver(device));
        return nullptr;
    }
    std::unique_ptr<GpuAtlas> out(new GpuAtlas);
    out->renderer_ = renderer;
    out->device_ = device;
    out->labelShader_ = labelShader;
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    out->nearest_ = SDL_CreateGPUSampler(device, &sampler);
    if (!out->nearest_) {
        out->labelShader_ = nullptr; // (no second texture for the material shader without it)
    }
    return out;
}

GpuAtlas::~GpuAtlas()
{
    clear();
    if (transfer_) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
    if (nearest_) {
        SDL_ReleaseGPUSampler(device_, nearest_);
    }
}

int GpuAtlas::pages(Kind kind) const
{
    return static_cast<int>(std::ranges::count_if(pages_, [kind](const Page& p) { return p.gpu && p.kind == kind; }));
}

int GpuAtlas::newPage(Kind kind, Pool pool, int w, int h)
{
    if (kind == Kind::Labels && !labelShader_) {
        return -1;
    }
    const bool demand = pool == Pool::Demand;
    const int side = kind == Kind::Bc7 ? (demand ? kBc7DemandPageSide : kBc7PageSide)
                                       : (demand ? kLabelDemandPageSide : kLabelPageSide);
    Page p;
    p.kind = kind;
    p.pool = pool;
    p.width = std::max(side, roundUp(w, kBlock));
    p.height = std::max(side, roundUp(h, kBlock));
    const std::uint64_t start = SDL_GetTicksNS();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kind == Kind::Bc7 ? SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(p.width);
    info.height = static_cast<Uint32>(p.height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1; // (SDL's samplers read level 0 only: each level has a place of its own)
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    p.gpu = SDL_CreateGPUTexture(device_, &info);
    if (!p.gpu) {
        if (!std::exchange(failed_, true)) {
            SDL_Log("image pack: a %dx%d page: %s", p.width, p.height, SDL_GetError());
        }
        return -1;
    }
    if (kind == Kind::Bc7) {
        // Drawn through the 2D renderer as an RGBA texture: it samples whatever format the GPU
        // texture has.
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, p.width);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, p.height);
        SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, p.gpu);
        p.wrapper = SDL_CreateTextureWithProperties(renderer_, props);
        SDL_DestroyProperties(props);
        if (p.wrapper) {
            SDL_SetTextureBlendMode(p.wrapper, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(p.wrapper, SDL_SCALEMODE_LINEAR);
        }
    } else {
        // The material shader's second texture, bound by a render state of the page's own.
        SDL_GPUTextureSamplerBinding binding{p.gpu, nearest_};
        SDL_GPURenderStateCreateInfo state{};
        state.fragment_shader = labelShader_;
        state.num_sampler_bindings = 1;
        state.sampler_bindings = &binding;
        p.state = SDL_CreateGPURenderState(renderer_, &state);
    }
    if ((kind == Kind::Bc7 && !p.wrapper) || (kind == Kind::Labels && !p.state)) {
        if (!std::exchange(failed_, true)) {
            SDL_Log("image pack: wrapping a page: %s", SDL_GetError());
        }
        SDL_ReleaseGPUTexture(device_, p.gpu);
        return -1;
    }
    const double ms = static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
    ++stats_.pagesMade;
    stats_.createMs += ms;
    stats_.worstCreateMs = std::max(stats_.worstCreateMs, ms);
    p.bytes = static_cast<std::size_t>(p.width) * static_cast<std::size_t>(p.height); // (BC7 and R8: a byte a texel)
    bytes_ += p.bytes;
    p.usedMs = SDL_GetTicks();
    auto slot = std::ranges::find_if(pages_, [](const Page& q) { return q.gpu == nullptr; });
    if (slot == pages_.end()) {
        pages_.push_back(std::move(p));
        return static_cast<int>(pages_.size() - 1);
    }
    *slot = std::move(p);
    return static_cast<int>(slot - pages_.begin());
}

bool GpuAtlas::allocate(Page& page, int w, int h, SDL_Point& at)
{
    if (w > page.width || h > page.height) {
        return false;
    }
    auto& sky = page.sky;
    const auto runEnd = [&](std::size_t j) { return j + 1 < sky.size() ? sky[j + 1].x : page.width; };
    // Each run's start as the left edge: the place's top is the lowest fill under it.
    std::size_t best = sky.size();
    int bestY = page.height;
    long bestWaste = 0;
    for (std::size_t i = 0; i < sky.size() && sky[i].x + w <= page.width; ++i) {
        const int end = sky[i].x + w;
        int y = 0;
        for (std::size_t j = i; j < sky.size() && sky[j].x < end; ++j) {
            y = std::max(y, sky[j].y);
        }
        if (y + h > page.height || y > bestY) {
            continue;
        }
        long waste = 0; // (the texels between the skyline and its bottom)
        for (std::size_t j = i; j < sky.size() && sky[j].x < end; ++j) {
            waste += static_cast<long>(y - sky[j].y) * (std::min(runEnd(j), end) - sky[j].x);
        }
        if (y < bestY || waste < bestWaste) {
            best = i;
            bestY = y;
            bestWaste = waste;
        }
    }
    if (best == sky.size()) {
        return false;
    }
    // Its run in the skyline, over the runs it covers; what's left of the last one goes on after it.
    const int x = sky[best].x;
    const int end = x + w;
    std::size_t j = best;
    int lastY = 0;
    while (j < sky.size() && sky[j].x < end) {
        lastY = sky[j].y;
        ++j;
    }
    const bool tail = j < sky.size() ? sky[j].x > end : end < page.width;
    sky.erase(sky.begin() + static_cast<std::ptrdiff_t>(best), sky.begin() + static_cast<std::ptrdiff_t>(j));
    sky.insert(sky.begin() + static_cast<std::ptrdiff_t>(best), SDL_Point{x, bestY + h});
    if (tail) {
        sky.insert(sky.begin() + static_cast<std::ptrdiff_t>(best + 1), SDL_Point{end, lastY});
    }
    // Runs as deep as their neighbour joined.
    if (best + 1 < sky.size() && sky[best + 1].y == sky[best].y) {
        sky.erase(sky.begin() + static_cast<std::ptrdiff_t>(best + 1));
    }
    if (best > 0 && sky[best - 1].y == sky[best].y) {
        sky.erase(sky.begin() + static_cast<std::ptrdiff_t>(best));
    }
    at = {x, bestY};
    return true;
}

void GpuAtlas::stage(const Page& page, const Level& level, SDL_Point at)
{
    Upload u;
    u.texture = page.gpu;
    std::size_t offset = roundUp(static_cast<std::uint32_t>(staging_.size()), kPlacement);
    if (page.kind == Kind::Bc7) {
        // The cell: the image's blocks with a clear block all round.
        const int pw = roundUp(level.width, kBlock);
        const int ph = roundUp(level.height, kBlock);
        const int bw = pw / kBlock + 2;
        const int bh = ph / kBlock + 2;
        const std::uint32_t pitch = roundUp(static_cast<std::uint32_t>(bw * kBlockBytes), kRowPitch);
        staging_.resize(offset + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(bh));
        std::uint8_t* out = staging_.data() + offset;
        const std::size_t rowBytes = static_cast<std::size_t>(pw / kBlock) * kBlockBytes;
        for (int r = 0; r < bh; ++r) {
            std::uint8_t* row = out + static_cast<std::size_t>(r) * pitch;
            const bool image = r >= 1 && r <= ph / kBlock;
            for (int b = 0; b < bw; ++b) {
                if (!image || b == 0 || b == bw - 1) {
                    std::memcpy(row + static_cast<std::size_t>(b) * kBlockBytes, kClearBlock, kBlockBytes);
                }
            }
            if (image) {
                std::memcpy(row + kBlockBytes, level.bytes.data() + static_cast<std::size_t>(r - 1) * rowBytes, rowBytes);
            }
        }
        u.pixelsPerRow = pitch / kBlockBytes * kBlock;
        u.x = at.x - kGutter;
        u.y = at.y - kGutter;
        u.w = bw * kBlock;
        u.h = bh * kBlock;
    } else {
        const std::uint32_t pitch = roundUp(static_cast<std::uint32_t>(level.width), kRowPitch);
        staging_.resize(offset + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(level.height));
        for (int r = 0; r < level.height; ++r) {
            std::memcpy(staging_.data() + offset + static_cast<std::size_t>(r) * pitch,
                        level.bytes.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(level.width),
                        static_cast<std::size_t>(level.width));
        }
        u.pixelsPerRow = pitch;
        u.x = at.x;
        u.y = at.y;
        u.w = level.width;
        u.h = level.height;
    }
    u.offset = static_cast<std::uint32_t>(offset);
    uploads_.push_back(u);
}

int GpuAtlas::place(Kind kind, Pool pool, std::span<const Level> levels, std::vector<SDL_Point>& corners)
{
    corners.clear();
    if (levels.empty()) {
        return -1;
    }
    const bool bc7 = kind == Kind::Bc7;
    // Each level's cell: BC7's padded to whole blocks with its gutter, the material bytes as they are.
    const auto cellOf = [&](const Level& l) {
        return bc7 ? SDL_Point{roundUp(l.width, kBlock) + 2 * kGutter, roundUp(l.height, kBlock) + 2 * kGutter}
                   : SDL_Point{l.width, l.height};
    };
    // All its levels on one page, or none (the skyline put back).
    const auto fit = [&](int page) {
        Page& p = pages_[static_cast<std::size_t>(page)];
        const auto sky = p.sky;
        corners.clear();
        for (const Level& l : levels) {
            const SDL_Point c = cellOf(l);
            SDL_Point at{};
            if (!allocate(p, c.x, c.y, at)) {
                p.sky = sky;
                // Nearly full (no run with an eighth of the page under it): filled no further.
                p.full = std::ranges::none_of(p.sky, [&](SDL_Point r) { return r.y < p.height - p.height / 8; });
                corners.clear();
                return false;
            }
            corners.push_back(bc7 ? SDL_Point{at.x + kGutter, at.y + kGutter} : at);
        }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            stage(p, levels[i], corners[i]);
            const std::size_t used = bc7 ? static_cast<std::size_t>(roundUp(levels[i].width, kBlock)) * roundUp(levels[i].height, kBlock)
                                         : static_cast<std::size_t>(levels[i].width) * levels[i].height;
            p.used += used;
            usedBytes_ += used;
        }
        if (staging_.size() >= kFlushAt) {
            flush();
        }
        return true;
    };
    // The pool's open pages, newest first, then a new one (sized to the levels when they're bigger).
    for (int i = static_cast<int>(pages_.size()) - 1; i >= 0; --i) {
        const Page& p = pages_[static_cast<std::size_t>(i)];
        if (p.gpu && !p.full && p.kind == kind && p.pool == pool && fit(i)) {
            return i;
        }
    }
    int w = 0;
    int h = 0;
    for (const Level& l : levels) {
        const SDL_Point c = cellOf(l);
        w = std::max(w, c.x);
        h += c.y;
    }
    const int page = newPage(kind, pool, w, h);
    return page >= 0 && fit(page) ? page : -1;
}

void GpuAtlas::closePool(Pool pool)
{
    for (Page& p : pages_) {
        if (p.gpu && p.pool == pool) {
            p.full = true;
        }
    }
}

void GpuAtlas::flush()
{
    if (uploads_.empty()) {
        return;
    }
    const std::uint64_t start = SDL_GetTicksNS();
    const auto size = static_cast<std::uint32_t>(staging_.size());
    if (!transfer_ || transferSize_ < size) {
        if (transfer_) {
            SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        }
        transferSize_ = std::max(size, std::uint32_t{4} << 20);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = transferSize_;
        transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
        if (!transfer_) {
            SDL_Log("image pack: a %u-byte transfer buffer: %s", transferSize_, SDL_GetError());
            transferSize_ = 0;
            staging_.clear();
            uploads_.clear();
            return;
        }
    }
    // (Cycled: the last flush's copy may still be on its way.)
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, true);
    SDL_GPUCommandBuffer* cmd = mapped ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if (!mapped || !cmd) {
        SDL_Log("image pack: uploading: %s", SDL_GetError());
        if (mapped) {
            SDL_UnmapGPUTransferBuffer(device_, transfer_);
        }
        staging_.clear();
        uploads_.clear();
        return;
    }
    std::memcpy(mapped, staging_.data(), size);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const Upload& u : uploads_) {
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = transfer_;
        from.offset = u.offset;
        from.pixels_per_row = u.pixelsPerRow;
        from.rows_per_layer = static_cast<Uint32>(u.h);
        SDL_GPUTextureRegion to{};
        to.texture = u.texture;
        to.x = static_cast<Uint32>(u.x);
        to.y = static_cast<Uint32>(u.y);
        to.w = static_cast<Uint32>(u.w);
        to.h = static_cast<Uint32>(u.h);
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    ++stats_.flushes;
    stats_.regions += uploads_.size();
    stats_.uploadedBytes += size;
    stats_.flushMs += static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
    staging_.clear();
    uploads_.clear();
}

void GpuAtlas::release(int page)
{
    Page& p = pages_[static_cast<std::size_t>(page)];
    if (!p.gpu) {
        return;
    }
    // (Its staged uploads go first: they name its texture.)
    if (std::ranges::any_of(uploads_, [&](const Upload& u) { return u.texture == p.gpu; })) {
        flush();
    }
    if (p.wrapper) {
        SDL_DestroyTexture(p.wrapper); // (SDL leaves a texture it was given alone)
    }
    if (p.state) {
        SDL_DestroyGPURenderState(p.state);
    }
    SDL_ReleaseGPUTexture(device_, p.gpu);
    bytes_ -= std::min(bytes_, p.bytes);
    usedBytes_ -= std::min(usedBytes_, p.used);
    p = Page{};
}

void GpuAtlas::clear()
{
    staging_.clear();
    uploads_.clear();
    for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
        release(i);
    }
    pages_.clear();
}

} // namespace noxsdl
