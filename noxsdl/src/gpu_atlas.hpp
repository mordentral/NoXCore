// The image pack's BC7 images on the GPU (SpriteCache, kCodecBc7 entries): SDL's 2D renderer has
// no compressed formats, so the pages are SDL_GPUTextures made on the renderer's own device, each
// BC7 page wrapped as an SDL_Texture (SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER) for drawing.
//
// Pages, not a texture per image: making a texture costs ~0.1 ms and sometimes 5-15 ms on
// Direct3D 12 under load, and an HD sprite has up to five levels. A page is 4096 x 4096 BC7
// (16 MB; 2048 for the images made as they're first wanted); every level of a sprite goes onto
// the same page, packed bottom-left on the page's skyline at multiples of 4 (BC7's blocks), its
// padded size as the pack stores it, with a clear block's gutter all round, so filtering never
// reaches a neighbour or the unused texels between. The material shader's material bytes
// (lossless, one byte a texel) go on pages of their own (R8, 2048 x 2048), which that shader
// samples as its second texture: each has its own render state binding it (SDL's render states
// take their extra textures when they're made).
//
// Uploads are staged on the CPU and go up together in one copy pass a flush, each row padded to
// Direct3D 12's 256-byte pitch and each region to its 512-byte placement, so SDL never copies them
// again. A flush is its own command buffer, submitted before the renderer's frame (at present),
// so the GPU has the pixels before anything drawn from them.
//
// Pages are filled in pools (the session's, the map's, the rest), so a page holds images that go
// together; a page is let go whole (SpriteCache decides which). Render thread only.
#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace noxsdl {

class GpuAtlas {
public:
    enum class Kind : std::uint8_t {
        Bc7,    ///< BC7 colour levels (drawn through an SDL_Texture)
        Labels, ///< material bytes, R8 (the material shader's second texture)
    };
    /// What a page is filled with: kept for the session, for the map, or let go when idle.
    enum class Pool : std::uint8_t { Session, Map, Demand };
    static constexpr int kPools = 3;

    /// The atlas on the renderer's GPU device, or nullptr: not the GPU renderer, or its device
    /// can't sample BC7 (logged). `labelShader`: the material shader that takes the material
    /// bytes as a second texture (nullptr: no Labels pages).
    static std::unique_ptr<GpuAtlas> create(SDL_Renderer* renderer, SDL_GPUShader* labelShader);
    ~GpuAtlas();
    /// Whether it makes Labels pages (a material shader to read them).
    bool takesLabels() const { return labelShader_ != nullptr; }
    GpuAtlas(const GpuAtlas&) = delete;
    GpuAtlas& operator=(const GpuAtlas&) = delete;

    /// A level going up: BC7, `bytes` its blocks as the pack lays them out (rows of 4 x 4 blocks
    /// covering width x height padded to multiples of 4); Labels, width x height bytes.
    struct Level {
        int width = 0;
        int height = 0;
        std::span<const std::uint8_t> bytes;
    };
    /// Places the levels on one page of `kind` in `pool` (the pool's open pages, else a new one)
    /// and stages their upload. Returns the page, or -1 (logged once) if no page could be made;
    /// `corners` gets each level's top-left texel.
    int place(Kind kind, Pool pool, std::span<const Level> levels, std::vector<SDL_Point>& corners);
    /// The pool's pages filled no further: its new images start pages of their own (a map's:
    /// the next map's don't go onto its pages).
    void closePool(Pool pool);

    SDL_Texture* texture(int page) const { return pages_[static_cast<std::size_t>(page)].wrapper; }
    SDL_GPUTexture* gpuTexture(int page) const { return pages_[static_cast<std::size_t>(page)].gpu; }
    /// A Labels page's render state for the 2D material shader (its texture bound second).
    SDL_GPURenderState* labelState(int page) const { return pages_[static_cast<std::size_t>(page)].state; }
    int pageCount() const { return static_cast<int>(pages_.size()); }
    bool live(int page) const { return pages_[static_cast<std::size_t>(page)].gpu != nullptr; }
    Kind kind(int page) const { return pages_[static_cast<std::size_t>(page)].kind; }
    Pool pool(int page) const { return pages_[static_cast<std::size_t>(page)].pool; }
    /// Live pages of a kind.
    int pages(Kind kind) const;

    /// Last drawn from, and whether something on it is kept (SpriteCache sets them).
    void touch(int page, std::uint64_t ms) { pages_[static_cast<std::size_t>(page)].usedMs = ms; }
    std::uint64_t usedMs(int page) const { return pages_[static_cast<std::size_t>(page)].usedMs; }
    void setPinned(int page, bool pinned) { pages_[static_cast<std::size_t>(page)].pinned = pinned; }
    bool pinned(int page) const { return pages_[static_cast<std::size_t>(page)].pinned; }

    /// Anything staged and not uploaded yet.
    bool pending() const { return !uploads_.empty(); }
    /// The staged uploads in one copy pass (nothing staged: nothing done).
    void flush();
    /// A page's textures let go (its wrapper destroyed, then the GPU textures; SDL keeps them till
    /// the GPU is done with them). Its slot is reused.
    void release(int page);
    /// Every page let go.
    void clear();

    /// The pages' memory, in bytes, and how much of it the images themselves fill (their padded
    /// levels: the rest is gutters and what packing leaves).
    std::size_t bytes() const { return bytes_; }
    std::size_t usedBytes() const { return usedBytes_; }
    std::size_t pageBytes(int page) const { return pages_[static_cast<std::size_t>(page)].bytes; }
    /// Since start-up: pages made and the time making them took, uploads (copy passes) and bytes.
    struct Stats {
        std::size_t pagesMade = 0;
        double createMs = 0;
        double worstCreateMs = 0;
        std::size_t flushes = 0;
        std::size_t regions = 0;
        std::size_t uploadedBytes = 0;
        double flushMs = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    GpuAtlas() = default;
    struct Page {
        Kind kind = Kind::Bc7;
        Pool pool = Pool::Demand;
        SDL_GPUTexture* gpu = nullptr;
        SDL_Texture* wrapper = nullptr;
        SDL_GPURenderState* state = nullptr;
        int width = 0;
        int height = 0;
        /// The skyline: left to right, where each run starts (x) and how far down it's filled
        /// (y), each run reaching the next's x (the last the page's width).
        std::vector<SDL_Point> sky{{0, 0}};
        bool full = false; ///< filled no further (closePool, or nearly full)
        bool pinned = false;
        std::uint64_t usedMs = 0;
        std::size_t bytes = 0;
        std::size_t used = 0; ///< bytes its images fill
    };
    struct Upload {
        SDL_GPUTexture* texture = nullptr;
        std::uint32_t offset = 0;
        std::uint32_t pixelsPerRow = 0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };
    /// A new page (a freed slot reused), at least w x h; -1 if it couldn't be made.
    int newPage(Kind kind, Pool pool, int w, int h);
    /// A w x h place on the page's skyline, as high up as it goes (then wasting least under it),
    /// or false.
    static bool allocate(Page& page, int w, int h, SDL_Point& at);
    /// The level's cell staged for upload at `at`.
    void stage(const Page& page, const Level& level, SDL_Point at);

    SDL_Renderer* renderer_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* labelShader_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    std::vector<Page> pages_;
    std::vector<std::uint8_t> staging_;
    std::vector<Upload> uploads_;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    std::uint32_t transferSize_ = 0;
    std::size_t bytes_ = 0;
    std::size_t usedBytes_ = 0;
    bool failed_ = false; ///< a page couldn't be made (logged once)
    Stats stats_;
};

} // namespace noxsdl
