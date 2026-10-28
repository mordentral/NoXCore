// SpriteCache (sprite_cache.hpp): video.bag and HD pack images made into textures on first use,
// the atlas pages small images share, mips for HD art, BC7 images on the GPU atlas, material tints
// (on the CPU or through the material shader), the session's and a map's own images decoded on
// the workers and kept, misses made in the frame, and letting go of what hasn't been drawn for a
// while.
#include "sprite_cache.hpp"

#include "pending_look.hpp"

#include "noxcore/io/file.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <set>


namespace noxsdl {

namespace {

constexpr std::size_t kSegmentCache = 32;

// The atlas pages small images share: Direct3D 12 places every texture on 64 KB, so a 40 x 60
// frame (9 KB) alone costs 64 KB, and a fight's thousands of frames most of a gigabyte. Each
// image has a 2-pixel gutter round it, so filtering never reaches a neighbour: the pixel next to
// the image repeats its edge (as a texture's clamped edge would), the one beyond is clear.
constexpr int kPageSide = 2048;
constexpr std::size_t kPageBytes = std::size_t{kPageSide} * kPageSide * 4;
constexpr int kGutter = 2;
constexpr int kShareMaxWidth = 512; // (a 256-wide image beside its material bytes)
constexpr int kShareMaxHeight = 256;

// The budget: over it, what wasn't asked for in the last few seconds goes (oldest first, pages
// whole); anything unused for a minute goes anyway. It's made again when it's next asked for.
// A map's own images (beginMap) and the session's (beginSession) are kept whatever their age,
// and count towards it: with a pack they're ~250 MB of it.
constexpr std::size_t kBudget = std::size_t{512} << 20;
// Without a pack (video.bag's images alone: a map's are some 16-40 MB of textures) a smaller one,
// so a long session over many maps doesn't fill up to the HD budget (the project's call, 2026-10-16).
constexpr std::size_t kSdBudget = std::size_t{128} << 20;
constexpr std::uint64_t kKeepMs = 3000;
constexpr std::uint64_t kForgetMs = 60000;

// A map's own images kept (beginMap), at most this much texture memory by estimate (the HD
// levels and their mips), of which the floor composites take up to kCompositeShare first: what's
// past it comes on demand like anything else, so a big map's preload can't eat the budget on its
// own. The floor tiles' pixels (memory, for compositing) have a cap of their own, and the ones not
// kept a budget like the textures'.
constexpr std::size_t kPreloadCap = std::size_t{256} << 20;
constexpr std::size_t kCompositeShare = std::size_t{96} << 20;
constexpr std::size_t kTilePreloadCap = std::size_t{96} << 20;
constexpr std::size_t kTileBudget = std::size_t{192} << 20;
// The session's images (beginSession) kept, at most this much texture memory by estimate.
constexpr std::size_t kSessionCap = std::size_t{160} << 20;

// The decodes: a placeholder asks again at most this often while it's drawn (a request not
// asked again for kStaleMs is dropped), and a frame puts in place at most kUploadBudgetNs worth
// of the finished ones (at least one), kHoldUploadBudgetNs while the map is held back.
constexpr std::uint64_t kReaskMs = 250;
constexpr std::uint64_t kStaleMs = 2000;
constexpr std::uint64_t kUploadBudgetNs = 4'000'000;
constexpr std::uint64_t kHoldUploadBudgetNs = 40'000'000;

/// The mip level for drawing `texelsAcross` texels into dst: the smallest whose texels are still
/// at least one per output pixel (so at most two per pixel, which linear filtering covers).
int mipLevel(SDL_Renderer* renderer, const Sprite& sprite, float texelsAcross, const SDL_FRect& dst)
{
    if (sprite.mips.empty() || dst.w <= 0) {
        return 0;
    }
    float sx = 1;
    float sy = 1;
    SDL_GetRenderScale(renderer, &sx, &sy);
    const float ratio = texelsAcross / (dst.w * sx);
    int level = 0;
    // Each level halves: stop at the first whose texels would be fewer than one per output pixel.
    while (level < static_cast<int>(sprite.mips.size()) && ratio / static_cast<float>(2 << level) >= 1.0f) {
        ++level;
    }
    return level;
}

/// The cache key of a tint: the image and all seven colours.
std::string tintKey(std::int32_t imageIndex, const MaterialColours& materials)
{
    std::string key = std::format("t{}", imageIndex);
    for (const auto& c : materials) {
        key += std::format(":{:02x}{:02x}{:02x}", c[0], c[1], c[2]);
    }
    return key;
}

/// An HD texture's estimated bytes with its mips (a third more) before it's decoded, from the
/// pack's entry: RGBA, the material bytes beside the image; or `bc7`, a byte a texel and as much
/// again for the material bytes.
std::size_t estimateBytes(const nox::video::PackEntry& e, bool material, bool bc7)
{
    const std::size_t texels = static_cast<std::size_t>(e.pixelWidth) * e.pixelHeight;
    return (bc7 ? texels : texels * 4) * (material ? 2 : 1) * 4 / 3;
}

/// A BC7 page's image drawn filtered: each side of `from` that lies on the image's own edge (`whole`)
/// pulled half a texel in, so the filter never reaches the clear border round it on the page (or
/// the clear pixels padding a BC7 block past its edge). A texture of its own clamps there instead;
/// the clear pixels would show as a dark seam between pieces drawn side by side (a window
/// border's) and a dark line along an opaque image's edge (a spell icon's). Half a texel of a 4x
/// image's edge is all it loses.
SDL_FRect insetPageEdges(SDL_FRect from, const SDL_FRect& whole)
{
    constexpr float kHalf = 0.5f;
    constexpr float kEps = 1e-3f;
    if (from.w > 2 * kHalf) {
        const bool left = from.x <= whole.x + kEps;
        const bool right = from.x + from.w >= whole.x + whole.w - kEps;
        if (left) {
            from.x += kHalf;
            from.w -= kHalf;
        }
        if (right) {
            from.w -= kHalf;
        }
    }
    if (from.h > 2 * kHalf) {
        const bool top = from.y <= whole.y + kEps;
        const bool bottom = from.y + from.h >= whole.y + whole.h - kEps;
        if (top) {
            from.y += kHalf;
            from.h -= kHalf;
        }
        if (bottom) {
            from.h -= kHalf;
        }
    }
    return from;
}

} // namespace

void drawSprite(SDL_Renderer* renderer, const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst)
{
    sprite.ready();
    const float tw = sprite.rect.w;
    const float th = sprite.rect.h;
    const int level = mipLevel(renderer, sprite, src ? src->w : tw, dst);
    if (level == 0) {
        SDL_SetTextureScaleMode(sprite.texture, sprite.scale);
        SDL_FRect from = sprite.source(src);
        if (sprite.gpu && sprite.scale != SDL_SCALEMODE_NEAREST) {
            from = insetPageEdges(from, sprite.rect);
        }
        SDL_RenderTexture(renderer, sprite.texture, &from, &dst);
        return;
    }
    // A texture of its own, or its place on the sprite's BC7 page.
    const SpriteLevel& mip = sprite.mips[static_cast<std::size_t>(level - 1)];
    if (mip.texture != sprite.texture) {
        // What the caller set on the sprite's texture applies to the level drawn.
        Uint8 r = 255, g = 255, b = 255, a = 255;
        SDL_GetTextureColorMod(sprite.texture, &r, &g, &b);
        SDL_GetTextureAlphaMod(sprite.texture, &a);
        SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(sprite.texture, &blend);
        SDL_SetTextureColorMod(mip.texture, r, g, b);
        SDL_SetTextureAlphaMod(mip.texture, a);
        SDL_SetTextureBlendMode(mip.texture, blend);
    }
    SDL_SetTextureScaleMode(mip.texture, SDL_SCALEMODE_LINEAR);
    const float mw = mip.rect.w;
    const float mh = mip.rect.h;
    SDL_FRect scaled = src ? SDL_FRect{mip.rect.x + src->x * mw / tw, mip.rect.y + src->y * mh / th, src->w * mw / tw,
                                       src->h * mh / th}
                           : mip.rect;
    if (sprite.gpu) {
        scaled = insetPageEdges(scaled, mip.rect);
    }
    SDL_RenderTexture(renderer, mip.texture, &scaled, &dst);
}

namespace {

/// The last composites' epoch any cache took (SpriteCache::compositeEpoch_).
std::uint64_t lastCompositeEpoch = 0;

} // namespace

SpriteCache::SpriteCache(SDL_Renderer* renderer, nox::video::VideoBag& bag)
    : renderer_(renderer)
    , bag_(bag)
    , compositeEpoch_(++lastCompositeEpoch)
{
}

void SpriteCache::compositesLetGo()
{
    compositeEpoch_ = ++lastCompositeEpoch;
    tintIndex_.clear();
}

SpriteCache::~SpriteCache()
{
    decoder_.reset(); // (the workers stopped first)
    clear();
}

void SpriteCache::release(Entry& entry)
{
    // (An atlas page's or a BC7 page's sprite owns nothing: its page goes whole.)
    if (entry.page < 0 && entry.gpuPage < 0 && entry.sprite) {
        SDL_DestroyTexture(entry.sprite->texture);
        for (const SpriteLevel& l : entry.sprite->mips) {
            SDL_DestroyTexture(l.texture);
        }
        ownBytes_ -= std::min(ownBytes_, entry.bytes);
    }
    entry.sprite.reset();
}

// Every texture and kept image let go (setPack, the destructor).
void SpriteCache::clear()
{
    for (auto& [_, e] : sprites_) {
        release(e);
    }
    for (auto& [_, e] : mono_) {
        release(e);
    }
    for (auto& [_, e] : materials_) {
        release(e);
    }
    for (auto& [_, e] : composites_) {
        release(e);
    }
    for (Page& p : pages_) {
        if (p.texture) {
            SDL_DestroyTexture(p.texture);
        }
    }
    pages_.clear();
    if (atlas_) {
        atlas_->clear();
    }
    openPage_ = -1;
    openPinnedPage_ = -1;
    ownBytes_ = 0;
    mono_.clear();
    sprites_.clear();
    composites_.clear();
    compositesLetGo();
    materials_.clear();
    shaderTints_.clear();
    decoded_.clear();
    hdTiles_.clear();
    tilePending_.clear();
    tileAskedMs_.clear();
    compositeAskedMs_.clear();
    pinWanted_.clear();
    preloadOutstanding_.clear();
    preloadLogged_ = true;
    compositeReserve_ = 0;
    pinnedPageBytes_ = 0;
    sessionJobs_.clear();
    sessionWanted_.clear();
    sessionOutstanding_.clear();
    sessionLogged_ = true;
    mapJobs_.clear();
    askedTiles_.clear();
    askedComposites_.clear();
}

void SpriteCache::touch(Entry& entry)
{
    entry.usedMs = SDL_GetTicks();
    if (entry.page >= 0) {
        pages_[static_cast<std::size_t>(entry.page)].usedMs = entry.usedMs;
    }
    if (entry.gpuPage >= 0) {
        atlas_->touch(entry.gpuPage, entry.usedMs);
    }
    if (entry.labelPage >= 0) {
        atlas_->touch(entry.labelPage, entry.usedMs);
    }
}

void SpriteCache::dropGpuPage(int page)
{
    const auto onPage = [page](const auto& kv) { return kv.second.gpuPage == page || kv.second.labelPage == page; };
    std::erase_if(sprites_, onPage);
    std::set<std::int32_t> gone;
    for (const auto& [index, e] : materials_) {
        if (e.gpuPage == page || e.labelPage == page) {
            gone.insert(index);
        }
    }
    std::erase_if(materials_, onPage);
    std::erase_if(shaderTints_, [&](const auto& kv) { return gone.contains(kv.second.first); });
    atlas_->release(page);
}

void SpriteCache::repinGpuPages()
{
    if (!atlas_) {
        return;
    }
    for (int i = 0; i < atlas_->pageCount(); ++i) {
        atlas_->setPinned(i, false);
    }
    const auto mark = [&](const auto& map) {
        for (const auto& [_, e] : map) {
            if ((e.pinned || e.session) && e.sprite) {
                if (e.gpuPage >= 0) {
                    atlas_->setPinned(e.gpuPage, true);
                }
                if (e.labelPage >= 0) {
                    atlas_->setPinned(e.labelPage, true);
                }
            }
        }
    };
    mark(sprites_);
    mark(materials_);
}

std::string SpriteCache::textureSummary() const
{
    const auto pages = std::ranges::count_if(pages_, [](const Page& p) { return p.texture != nullptr; });
    std::string out = std::format("{} MB: {} atlas pages, {} MB in textures of their own", textureBytes() >> 20, pages,
                                  ownBytes_ >> 20);
    if (atlas_) {
        const auto& st = atlas_->stats();
        out += std::format(", {} BC7 pages + {} material pages ({} MB, {} MB of it images; {} made in {:.1f} ms, worst "
                           "{:.1f} ms; {} uploads, {} MB in {:.1f} ms)",
                           atlas_->pages(GpuAtlas::Kind::Bc7), atlas_->pages(GpuAtlas::Kind::Labels), atlas_->bytes() >> 20,
                           atlas_->usedBytes() >> 20, st.pagesMade, st.createMs, st.worstCreateMs, st.flushes,
                           st.uploadedBytes >> 20, st.flushMs);
    }
    return out;
}

void SpriteCache::dropPage(int page)
{
    const auto onPage = [page](const auto& kv) { return kv.second.page == page; };
    std::erase_if(sprites_, onPage);
    std::erase_if(mono_, onPage);
    if (std::erase_if(composites_, onPage) != 0) {
        compositesLetGo();
    }
    std::set<std::int32_t> gone;
    for (const auto& [index, e] : materials_) {
        if (e.page == page) {
            gone.insert(index);
        }
    }
    std::erase_if(materials_, onPage);
    std::erase_if(shaderTints_, [&](const auto& kv) { return gone.contains(kv.second.first); });
    Page& p = pages_[static_cast<std::size_t>(page)];
    SDL_DestroyTexture(p.texture);
    p = Page{};
    if (openPage_ == page) {
        openPage_ = -1;
    }
    if (openPinnedPage_ == page) {
        openPinnedPage_ = -1;
    }
}

std::size_t SpriteCache::textureBytes() const
{
    const auto pages = static_cast<std::size_t>(std::ranges::count_if(pages_, [](const Page& p) { return p.texture != nullptr; }));
    return pages * kPageBytes + ownBytes_ + (atlas_ ? atlas_->bytes() : 0);
}

void SpriteCache::endFrame(std::uint64_t nowMs)
{
    pump(nowMs);
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        flushPage(static_cast<int>(i)); // (made and not drawn)
    }
    if (atlas_) {
        atlas_->flush(); // (the frame's BC7 uploads in one copy pass)
    }
    // The sweep below runs at most once a second.
    if (nowMs - sweptMs_ < 1000) {
        return;
    }
    sweptMs_ = nowMs;
    repinGpuPages();
    if (decoder_) {
        decoder_->dropStale(nowMs, kStaleMs);
    }
    {
        // The floor tiles' pixels (memory, not textures): the map's stay; others go once unused
        // for a minute, or for a few seconds while they're over kTileBudget (oldest first).
        const auto bytesOf = [](const TilePixels& t) {
            return t.image->rgba.size() + t.image->material.size() + t.image->edgeMask.size();
        };
        std::vector<std::pair<std::uint64_t, std::int32_t>> idle;
        std::size_t bytes = 0;
        for (const auto& [index, t] : hdTiles_) {
            if (t.image) {
                bytes += bytesOf(t);
                if (!t.pinned) {
                    idle.emplace_back(t.usedMs, index);
                }
            }
        }
        std::ranges::sort(idle);
        for (const auto& [used, index] : idle) {
            const std::uint64_t age = nowMs > used ? nowMs - used : 0;
            if (age <= kForgetMs && (bytes <= kTileBudget || age <= kKeepMs)) {
                break;
            }
            bytes -= std::min(bytes, bytesOf(hdTiles_.at(index)));
            hdTiles_.erase(index);
        }
    }
    // Everything that could go, oldest first: pages whole, the entries with textures of their own.
    // The map's own (pinned) stay.
    struct Candidate {
        std::uint64_t usedMs = 0;
        int page = -1;
        int kind = 0; ///< 0 a page, 1 sprites_, 2 mono_, 3 materials_, 4 composites_, 5 a BC7 or material page
        std::int32_t index = 0;
        std::pair<std::int32_t, std::uint32_t> monoKey{};
        std::string key;
    };
    std::vector<Candidate> candidates;
    // Pages go whole; entries only when they have a texture of their own (page < 0, not on a BC7 page).
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].texture && !pages_[i].pinned) {
            candidates.push_back({pages_[i].usedMs, static_cast<int>(i), 0});
        }
    }
    for (int i = 0; atlas_ && i < atlas_->pageCount(); ++i) {
        if (atlas_->live(i) && !atlas_->pinned(i)) {
            candidates.push_back({atlas_->usedMs(i), i, 5});
        }
    }
    const auto own = [](const Entry& e) { return e.page < 0 && e.gpuPage < 0 && e.sprite && !e.pinned && !e.session; };
    for (const auto& [index, e] : sprites_) {
        if (own(e)) {
            candidates.push_back({e.usedMs, -1, 1, index});
        }
    }
    for (const auto& [key, e] : mono_) {
        if (own(e)) {
            candidates.push_back({e.usedMs, -1, 2, 0, key});
        }
    }
    for (const auto& [index, e] : materials_) {
        if (own(e)) {
            candidates.push_back({e.usedMs, -1, 3, index});
        }
    }
    for (const auto& [key, e] : composites_) {
        if (e.page < 0 && e.sprite && !e.pinned) {
            candidates.push_back({e.usedMs, -1, 4, 0, {}, key});
        }
    }
    std::ranges::sort(candidates, {}, &Candidate::usedMs);
    std::size_t total = textureBytes();
    const std::size_t budget = pack_ ? kBudget : kSdBudget;
    for (const Candidate& c : candidates) {
        // Stop at the first that may stay: idle under kForgetMs, and either the cache is within budget
        // or it was used in the last kKeepMs.
        const std::uint64_t idle = nowMs > c.usedMs ? nowMs - c.usedMs : 0;
        if (idle <= kForgetMs && (total <= budget || idle <= kKeepMs)) {
            break; // (the rest are newer)
        }
        if (c.kind == 0) {
            dropPage(c.page);
            total -= std::min(total, kPageBytes);
            continue;
        }
        if (c.kind == 5) {
            total -= std::min(total, atlas_->pageBytes(c.page));
            dropGpuPage(c.page);
            continue;
        }
        // Its entry in the map its kind names, let go and erased (a material image's tinted copies too).
        Entry* e = nullptr;
        if (c.kind == 1) {
            e = &sprites_.at(c.index);
        } else if (c.kind == 2) {
            e = &mono_.at(c.monoKey);
        } else if (c.kind == 4) {
            e = &composites_.at(c.key);
        } else {
            e = &materials_.at(c.index);
        }
        total -= std::min(total, e->bytes);
        release(*e);
        if (c.kind == 1) {
            sprites_.erase(c.index);
        } else if (c.kind == 2) {
            mono_.erase(c.monoKey);
        } else if (c.kind == 4) {
            composites_.erase(c.key);
            compositesLetGo();
        } else {
            materials_.erase(c.index);
            std::erase_if(shaderTints_, [&](const auto& kv) { return kv.second.first == c.index; });
        }
    }
}

bool SpriteCache::decodesLater(std::int32_t index) const
{
    return (placeholders_ || deferHd_) && decoder_ && index >= 0 && static_cast<std::size_t>(index) < bag_.images().size();
}

DecodeJob SpriteCache::jobFor(DecodeKind kind, std::int32_t index) const
{
    DecodeJob job;
    job.kind = kind;
    job.index = index;
    if (atlas_ && pack_ && index >= 0 && static_cast<std::uint32_t>(index) < pack_->size() &&
        pack_->entry(static_cast<std::size_t>(index)).codec == nox::video::kCodecBc7) {
        // The blocks as they are for the BC7 pages: a plain image, or a material one when the
        // material shader can read its material bytes from a page of their own.
        job.blocks = kind == DecodeKind::Plain || (kind == DecodeKind::Material && atlas_->takesLabels());
    }
    return job;
}

SpriteCache::Entry SpriteCache::makeNow(const DecodeJob& job)
{
    const std::uint64_t start = SDL_GetTicksNS();
    Entry e = makeHdEntry(buildDecodeResult(job, packImage(job.index, job.blocks)));
    ++loadStats_.misses;
    loadStats_.missMs += static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
    return e;
}

void SpriteCache::ask(Entry& entry, const DecodeJob& job)
{
    const std::uint64_t now = SDL_GetTicks();
    if (entry.askedMs != 0 && now - entry.askedMs < kReaskMs) {
        return;
    }
    entry.askedMs = now;
    decoder_->request(job, now);
}

SpriteCache::Entry SpriteCache::makeAtlasEntry(const DecodeResult& result, GpuAtlas::Pool pool)
{
    Entry e;
    // Every level on one BC7 page; a material image's material bytes on a material page.
    std::vector<GpuAtlas::Level> levels;
    for (const TextureLevel& l : result.levels) {
        levels.push_back({l.width, l.height, l.bytes});
    }
    std::vector<SDL_Point> at;
    const int page = atlas_->place(GpuAtlas::Kind::Bc7, pool, levels, at);
    if (page < 0) {
        return e;
    }
    std::vector<SDL_Point> labelAt;
    int labelPage = -1;
    if (result.job.kind == DecodeKind::Material) {
        std::vector<GpuAtlas::Level> labels;
        for (std::size_t i = 0; i < result.labels.size() && i < result.levels.size(); ++i) {
            labels.push_back({result.levels[i].width, result.levels[i].height, result.labels[i]});
        }
        labelPage = labels.size() == result.levels.size() ? atlas_->place(GpuAtlas::Kind::Labels, pool, labels, labelAt) : -1;
        if (labelPage < 0) {
            return e; // (its levels' place stays unused till the page goes)
        }
    }
    const auto rectOf = [](SDL_Point p, const TextureLevel& l) {
        return SDL_FRect{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(l.width), static_cast<float>(l.height)};
    };
    SDL_Texture* texture = atlas_->texture(page);
    Sprite s{texture, result.width, result.height, result.offsetX, result.offsetY};
    s.rect = rectOf(at[0], result.levels[0]);
    s.hd = true;
    s.scale = SDL_SCALEMODE_LINEAR;
    s.owner = this;
    s.gpu = true;
    if (labelPage >= 0) {
        s.labelState = atlas_->labelState(labelPage);
        s.labelTexture = atlas_->gpuTexture(labelPage);
        s.labels = rectOf(labelAt[0], result.levels[0]);
    }
    for (std::size_t i = 1; i < result.levels.size(); ++i) {
        s.mips.push_back({texture, rectOf(at[i], result.levels[i]),
                          labelPage >= 0 ? rectOf(labelAt[i], result.levels[i]) : SDL_FRect{}});
    }
    e.sprite = std::move(s);
    e.gpuPage = page;
    e.labelPage = labelPage;
    ++made_;
    if (++hdLoaded_ <= 3) {
        SDL_Log("image pack: %d (%s) in high definition as BC7, %dx%d for %.0fx%.0f, %zu levels", result.job.index,
                result.name.c_str(), result.levels[0].width, result.levels[0].height, result.width, result.height,
                result.levels.size());
    }
    return e;
}

SpriteCache::Entry SpriteCache::makeHdEntry(const DecodeResult& result, GpuAtlas::Pool pool)
{
    Entry e;
    if (!result.ok || result.levels.empty()) {
        return e;
    }
    if (result.levels.front().bc7) {
        return atlas_ ? makeAtlasEntry(result, pool) : e;
    }
    // Each level a texture of its own, uploaded as the decode left it (its format and pitch).
    const auto upload = [&](const TextureLevel& level) -> SDL_Texture* {
        SDL_Texture* t = SDL_CreateTexture(renderer_, level.format, SDL_TEXTUREACCESS_STATIC, level.width, level.height);
        if (t) {
            SDL_UpdateTexture(t, nullptr, level.bytes.data(), level.pitch);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            e.bytes += level.bytes.size();
        }
        return t;
    };
    const TextureLevel& top = result.levels.front();
    SDL_Texture* texture = upload(top);
    if (!texture) {
        return e;
    }
    // Drawn at its original size, filtered: the extra pixels make it sharper.
    Sprite s{texture, result.width, result.height, result.offsetX, result.offsetY};
    s.rect = {0, 0, static_cast<float>(top.width), static_cast<float>(top.height)};
    s.hd = true;
    s.scale = SDL_SCALEMODE_LINEAR;
    for (std::size_t i = 1; i < result.levels.size(); ++i) {
        SDL_Texture* mip = upload(result.levels[i]);
        if (!mip) {
            break;
        }
        const TextureLevel& l = result.levels[i];
        s.mips.push_back({mip, {0, 0, static_cast<float>(l.width), static_cast<float>(l.height)}});
    }
    e.sprite = std::move(s);
    ownBytes_ += e.bytes;
    ++made_;
    if (++hdLoaded_ <= 3) {
        SDL_Log("image pack: %d (%s) in high definition, %dx%d for %.0fx%.0f", result.job.index, result.name.c_str(),
                result.job.kind == DecodeKind::Material ? top.width / 2 : top.width, top.height, result.width,
                result.height);
    }
    return e;
}

void SpriteCache::install(DecodeResult&& r)
{
    const DecodeJob::Key key = r.job.key();
    const bool session = sessionWanted_.contains(key);
    const bool pin = pinWanted_.contains(key) || session; // (made if it's wanted, as the map's are)
    if (preloadOutstanding_.erase(key)) {
        preloadDecodeMs_ += r.decodeMs;
        decoder_->cancelPreload(key); // (it may have come as asked for, its preload job still queued)
    }
    if (sessionOutstanding_.erase(key)) {
        sessionDecodeMs_ += r.decodeMs;
        decoder_->cancelPreload(key);
    }
    const std::int32_t index = r.job.index;
    if (r.job.kind == DecodeKind::Tile) {
        // A floor tile's pixels: in place of the pending one, or kept for the map's floor.
        if (tilePending_.erase(index) || (pin && !hdTiles_.contains(index))) {
            auto& t = hdTiles_[index];
            t.image = r.ok && r.pixels ? std::make_shared<const nox::video::PackImage>(std::move(*r.pixels)) : nullptr;
            t.usedMs = SDL_GetTicks();
            t.pinned = pin;
            tileAskedMs_.erase(index);
        }
        return;
    }
    if (r.job.kind == DecodeKind::Composite) {
        // Built pixels: a new composite (one that couldn't be built stays empty, the SD one drawn).
        compositeAskedMs_.erase(r.job.cacheKey);
        auto [it, made] = composites_.try_emplace(r.job.cacheKey);
        if (!made) {
            return; // (made meanwhile)
        }
        if (r.ok) {
            const TextureLevel& level = r.levels.front();
            it->second = makeEntry(level.bytes, level.width, level.height, 0, 0, true, r.job.pin && compositePinAllowed());
            touch(it->second);
        }
        return;
    }
    // The entry it's for: a placeholder waiting for it, or (one of the map's) a new one.
    Entry* e = nullptr;
    bool fresh = false;
    switch (r.job.kind) {
    case DecodeKind::Plain: {
        auto [it, made] = pin ? sprites_.try_emplace(index) : std::pair{sprites_.find(index), false};
        e = it != sprites_.end() ? &it->second : nullptr;
        fresh = made;
        break;
    }
    case DecodeKind::Mono: {
        const auto monoKey = std::pair{index, r.job.colour};
        const auto it = mono_.find(monoKey);
        e = it != mono_.end() ? &it->second : nullptr;
        break;
    }
    case DecodeKind::Tint: {
        const auto it = composites_.find(r.job.cacheKey);
        e = it != composites_.end() ? &it->second : nullptr;
        break;
    }
    case DecodeKind::Material: {
        auto [it, made] = pin ? materials_.try_emplace(index) : std::pair{materials_.find(index), false};
        e = it != materials_.end() ? &it->second : nullptr;
        fresh = made;
        break;
    }
    case DecodeKind::Tile:
    case DecodeKind::Composite:
        break; // (put in place above)
    }
    if (!e) {
        return; // (let go meanwhile, nobody waiting)
    }
    if (!fresh && !e->placeholder) {
        // Already HD (made as asked, or before the map was loaded): kept from now on if it's the
        // map's or the session's.
        if (e->sprite && e->sprite->hd) {
            e->pinned = e->pinned || pinWanted_.contains(key);
            e->session = e->session || session;
        }
        return;
    }
    Entry made = makeHdEntry(r, session ? GpuAtlas::Pool::Session : pin ? GpuAtlas::Pool::Map : GpuAtlas::Pool::Demand);
    if (!made.sprite) {
        // Not to be had: the original it has stays (none for a new one).
        e->placeholder = false;
        if (fresh) {
            if (r.job.kind == DecodeKind::Plain) {
                sprites_.erase(index);
            } else {
                materials_.erase(index);
            }
        }
        return;
    }
    // The placeholder's texture goes now, between frames (a page's place stays till the page goes).
    release(*e);
    const std::uint64_t used = fresh ? SDL_GetTicks() : e->usedMs;
    *e = std::move(made);
    e->usedMs = used;
    e->pinned = pinWanted_.contains(key);
    e->session = session;
    if (r.job.kind == DecodeKind::Material) {
        // Its shaded copies held the old texture: made again from this one as they're next asked for.
        std::erase_if(shaderTints_, [&](const auto& kv) { return kv.second.first == index; });
    }
}

void SpriteCache::pump(std::uint64_t nowMs)
{
    if (!decoder_) {
        return;
    }
    const std::uint64_t start = SDL_GetTicksNS();
    const std::uint64_t budget = holding_ ? kHoldUploadBudgetNs : kUploadBudgetNs;
    DecodeResult r;
    while (decoder_->take(r)) {
        install(std::move(r));
        if (SDL_GetTicksNS() - start >= budget) {
            break;
        }
    }
    // An image's texture bytes: its own textures, or its levels' share of its BC7 page (by estimate).
    const auto bytesOf = [&](std::int32_t index, const Entry& e, bool material) {
        return e.gpuPage >= 0 && pack_ && static_cast<std::uint32_t>(index) < pack_->size()
                   ? estimateBytes(pack_->entry(static_cast<std::size_t>(index)), material, true)
                   : e.bytes;
    };
    if (!sessionLogged_ && sessionOutstanding_.empty()) {
        sessionLogged_ = true;
        std::size_t bytes = 0;
        std::size_t images = 0;
        for (const auto& [index, e] : sprites_) {
            if (e.session && e.sprite) {
                bytes += bytesOf(index, e, false);
                ++images;
            }
        }
        for (const auto& [index, e] : materials_) {
            if (e.session && e.sprite) {
                bytes += bytesOf(index, e, true);
                ++images;
            }
        }
        SDL_Log("session images: %zu in, %zu MB of textures (estimated %zu MB), %llu ms (workers' decode time %.0f ms); "
                "textures %s",
                images, bytes >> 20, sessionEstimate_ >> 20, static_cast<unsigned long long>(nowMs - sessionStartMs_),
                sessionDecodeMs_, textureSummary().c_str());
    }
    if (!preloadLogged_ && preloadOutstanding_.empty()) {
        preloadLogged_ = true;
        // What the map keeps: its pinned textures (pages of floor composites made so far included).
        std::size_t bytes = 0;
        std::size_t images = 0;
        for (const auto& [index, e] : sprites_) {
            if (e.pinned && e.sprite) {
                bytes += bytesOf(index, e, false);
                ++images;
            }
        }
        for (const auto& [index, e] : materials_) {
            if (e.pinned && e.sprite) {
                bytes += bytesOf(index, e, true);
                ++images;
            }
        }
        std::size_t tiles = 0;
        std::size_t tileBytes = 0;
        for (const auto& [_, t] : hdTiles_) {
            if (t.image && t.pinned) {
                ++tiles;
                tileBytes += t.image->rgba.size() + t.image->material.size() + t.image->edgeMask.size();
            }
        }
        SDL_Log("map preload %s: %zu images, %zu MB of textures (estimated %zu MB) + %zu floor tiles (%zu MB in memory), "
                "%llu ms (workers' decode time %.0f ms); %zu past the cap left on demand; textures %s",
                preloadName_.c_str(), images, bytes >> 20, preloadEstimate_ >> 20, tiles, tileBytes >> 20,
                static_cast<unsigned long long>(nowMs - preloadStartMs_), preloadDecodeMs_, preloadBeyondCap_,
                textureSummary().c_str());
    }
}

void SpriteCache::dropStandIns()
{
    // (Only the map editor's draws make them without --hd-placeholders: setDeferHd. In a game
    // there are none, and this does nothing.)
    if (placeholders_) {
        return;
    }
    const auto drop = [this](auto& map) {
        for (auto it = map.begin(); it != map.end();) {
            if (it->second.placeholder && !it->second.session) {
                release(it->second);
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };
    drop(sprites_);
    drop(mono_);
    std::set<std::int32_t> materials;
    for (const auto& [index, e] : materials_) {
        if (e.placeholder && !e.session) materials.insert(index);
    }
    drop(materials_);
    std::erase_if(shaderTints_, [&](const auto& kv) { return materials.contains(kv.second.first); });
    drop(composites_); // (the CPU's tints standing in)
    compositesLetGo();
}

void SpriteCache::beginMap(const MapImages& images)
{
    // The originals' decoded pixels kept for compositing and tinting (decoded()) go with the last
    // map, so they don't pile up over a session; what this map uses is decoded again from
    // video.bag as it's drawn. (Nobody keeps a pointer into them: every user copies or is done
    // within its call.)
    decoded_.clear();
    if (!pack_ || !decoder_) {
        return; // (no workers: everything made as it's asked for, as without a pack)
    }
    dropStandIns(); // (the map editor's originals standing in: a game's view draws none)
    // The last map's let go to the budget: no longer pinned, its pinned pages ordinary ones.
    const auto unpin = [](auto& map) {
        for (auto& [_, e] : map) {
            e.pinned = false;
        }
    };
    unpin(sprites_);
    unpin(materials_);
    unpin(composites_);
    unpin(mono_);
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].pinned) {
            flushPage(static_cast<int>(i));
            pages_[i].pinned = false;
            if (openPinnedPage_ == static_cast<int>(i)) {
                pages_[i].strip = {};
                pages_[i].full = true; // (filled no further: new pinned images start a page of their own)
            }
        }
    }
    openPinnedPage_ = -1;
    pinnedPageBytes_ = 0;
    if (atlas_) {
        atlas_->closePool(GpuAtlas::Pool::Map); // (this map's images start pages of their own)
    }
    pinWanted_.clear();
    preloadOutstanding_.clear();
    // (Every request is dropped below: what's still wanted is asked again the next time it's
    // drawn, not kReaskMs after its last ask. A tile left waiting meanwhile would have the floor's
    // wait, awaitAsked, sit out its whole deadline for a decode nobody asked for.)
    compositeAskedMs_.clear();
    tileAskedMs_.clear();
    askedTiles_.clear();
    askedComposites_.clear();
    decoder_->dropRequests();
    // The last map's floor tiles' pixels go (what this map shares stays).
    std::set<std::int32_t> tiles;
    for (const auto& img : images.images) {
        if (img.tile) {
            tiles.insert(img.index);
        }
    }
    std::erase_if(hdTiles_, [&](const auto& kv) { return !tiles.contains(kv.first) && !tilePending_.contains(kv.first); });
    for (auto& [_, t] : hdTiles_) {
        t.pinned = false;
    }

    // Within the caps: the floor composites' share of the textures' first, then the images in the
    // order given; the floor tiles' pixels (CPU memory) by a cap of their own.
    compositeReserve_ = std::min(images.compositeBytes, kCompositeShare);
    std::size_t budget = compositeReserve_;
    std::size_t tileBudget = 0;
    std::size_t wantedImageBytes = 0; // (all of them, for the log)
    std::size_t wantedTileBytes = 0;
    preloadEstimate_ = 0;
    preloadBeyondCap_ = 0;
    preloadDecodeMs_ = 0;
    std::vector<DecodeJob> jobs;
    std::set<DecodeJob::Key> seen;
    for (const auto& img : images.images) {
        DecodeJob job;
        job.index = img.index;
        std::size_t bytes = 0;
        if (img.tile) {
            if (hdTileScale(img.index) <= 1) {
                continue;
            }
            job.kind = DecodeKind::Tile;
            job.blocks = false; // (its pixels decoded: the floor composites from them)
            if (!seen.insert(job.key()).second) {
                continue;
            }
            // (CPU pixels, kept for compositing: their composites have the textures' reserve.)
            const std::size_t pixels = pack_->entry(static_cast<std::size_t>(img.index)).rawSize;
            wantedTileBytes += pixels;
            if (tileBudget + pixels > kTilePreloadCap) {
                ++preloadBeyondCap_;
                continue;
            }
            tileBudget += pixels;
            pinWanted_.insert(job.key());
            if (const auto it = hdTiles_.find(job.index); it != hdTiles_.end() && it->second.image) {
                it->second.pinned = true;
                continue; // (in already)
            }
            preloadOutstanding_.insert(job.key());
            jobs.push_back(std::move(job));
            continue;
        } else {
            const auto* e = hdEntry(img.index);
            if (!e) {
                continue;
            }
            // A tinted thing's image with material pixels goes to the material shader (its CPU tints
            // are by colours: those come on demand).
            const bool material = img.tinted && materialState_ && (e->planes & nox::video::kPlaneMaterial);
            job = jobFor(material ? DecodeKind::Material : DecodeKind::Plain, img.index);
            bytes = estimateBytes(*e, material, job.blocks);
        }
        const auto key = job.key();
        if (!seen.insert(key).second) {
            continue;
        }
        if (sessionWanted_.contains(key)) {
            continue; // (the session's: kept already, or on its way)
        }
        wantedImageBytes += bytes;
        if (budget + bytes > kPreloadCap) {
            ++preloadBeyondCap_;
            continue;
        }
        budget += bytes;
        preloadEstimate_ += bytes;
        pinWanted_.insert(key);
        // Already in (HD from before): kept from now on; else decoded.
        auto& map = job.kind == DecodeKind::Material ? materials_ : sprites_;
        if (const auto it = map.find(job.index); it != map.end() && it->second.sprite && it->second.sprite->hd) {
            it->second.pinned = true;
            continue;
        }
        preloadOutstanding_.insert(key);
        jobs.push_back(std::move(job));
    }
    preloadName_ = images.name;
    preloadStartMs_ = SDL_GetTicks();
    preloadQueued_ = jobs.size();
    preloadLogged_ = images.images.empty(); // (no map: nothing to say)
    mapJobs_ = std::move(jobs);
    queuePreloads();
    if (images.images.empty()) {
        return;
    }
    SDL_Log("map preload %s: %zu queued (%zu kept already); textures %zu MB of the map's %zu MB images + %zu MB of its "
            "%zu MB floor composites, floor tiles' pixels %zu MB of %zu MB; %zu past the caps",
            images.name.c_str(), preloadQueued_, pinWanted_.size() - preloadQueued_, preloadEstimate_ >> 20,
            wantedImageBytes >> 20, compositeReserve_ >> 20, images.compositeBytes >> 20, tileBudget >> 20,
            wantedTileBytes >> 20, preloadBeyondCap_);
}

void SpriteCache::queuePreloads()
{
    // The session's first (wanted on every map), then the map's: those not in yet.
    std::vector<DecodeJob> jobs;
    for (const DecodeJob& j : sessionJobs_) {
        if (sessionOutstanding_.contains(j.key())) {
            jobs.push_back(j);
        }
    }
    for (const DecodeJob& j : mapJobs_) {
        if (preloadOutstanding_.contains(j.key())) {
            jobs.push_back(j);
        }
    }
    decoder_->preload(std::move(jobs));
}

void SpriteCache::beginSession(const MapImages& images)
{
    if (!pack_ || !decoder_) {
        return; // (no workers: everything made as it's asked for)
    }
    for (auto& [_, e] : sprites_) {
        e.session = false;
    }
    for (auto& [_, e] : materials_) {
        e.session = false;
    }
    sessionJobs_.clear();
    sessionWanted_.clear();
    sessionOutstanding_.clear();
    sessionEstimate_ = 0;
    sessionDecodeMs_ = 0;
    std::size_t wanted = 0;
    std::size_t beyond = 0;
    std::size_t already = 0;
    for (const auto& img : images.images) {
        const auto* e = hdEntry(img.index);
        if (!e) {
            continue; // (not upscaled: video.bag's, made as it's drawn)
        }
        const bool material = img.tinted && materialState_ && (e->planes & nox::video::kPlaneMaterial);
        DecodeJob job = jobFor(material ? DecodeKind::Material : DecodeKind::Plain, img.index);
        if (sessionWanted_.contains(job.key())) {
            continue;
        }
        const std::size_t bytes = estimateBytes(*e, material, job.blocks);
        wanted += bytes;
        if (sessionEstimate_ + bytes > kSessionCap) {
            ++beyond;
            continue;
        }
        sessionEstimate_ += bytes;
        sessionWanted_.insert(job.key());
        auto& map = material ? materials_ : sprites_;
        if (const auto it = map.find(job.index); it != map.end() && it->second.sprite && it->second.sprite->hd) {
            it->second.session = true;
            ++already;
            continue;
        }
        sessionOutstanding_.insert(job.key());
        sessionJobs_.push_back(std::move(job));
    }
    sessionStartMs_ = SDL_GetTicks();
    sessionLogged_ = sessionOutstanding_.empty();
    queuePreloads(); // (a map's preload, if any, goes on after these)
    SDL_Log("session images: %zu queued (%zu kept already), %zu MB of textures by estimate of the %zu MB wanted; %zu past the cap",
            sessionJobs_.size(), already, sessionEstimate_ >> 20, wanted >> 20, beyond);
}

int SpriteCache::hdTileScale(std::int32_t imageIndex) const
{
    if (!pack_ || imageIndex < 0 || static_cast<std::uint32_t>(imageIndex) >= pack_->size()) {
        return 1;
    }
    const auto& e = pack_->entry(static_cast<std::size_t>(imageIndex));
    if (!e.present() || e.width <= 0 || static_cast<std::int32_t>(e.pixelWidth) <= e.width ||
        e.pixelWidth % static_cast<std::uint32_t>(e.width) != 0) {
        return 1;
    }
    return static_cast<int>(e.pixelWidth / static_cast<std::uint32_t>(e.width));
}

std::shared_ptr<const nox::video::PackImage> SpriteCache::hdTile(std::int32_t imageIndex)
{
    const TilePixels* t = hdTilePixels(imageIndex);
    return t ? t->image : nullptr;
}

const SpriteCache::TilePixels* SpriteCache::hdTilePixels(std::int32_t imageIndex)
{
    if (!pack_ || imageIndex < 0 || static_cast<std::uint32_t>(imageIndex) >= pack_->size()) {
        return nullptr;
    }
    // Looked up once per image (again after a let-go); stays empty when the pack holds it only at
    // its own size.
    auto [it, created] = hdTiles_.try_emplace(imageIndex);
    if (created && hdTileScale(imageIndex) > 1) {
        if (decoder_ && static_cast<std::size_t>(imageIndex) < bag_.images().size()) {
            // From the workers: empty (pending) till it's in (the drawing waits for it: awaitAsked).
            tilePending_.insert(imageIndex);
            hdTilePending(imageIndex);
        } else if (auto p = packImage(imageIndex)) {
            it->second.image = std::make_shared<const nox::video::PackImage>(std::move(*p));
        }
    }
    it->second.usedMs = SDL_GetTicks();
    return &it->second;
}

bool SpriteCache::hdTilePending(std::int32_t imageIndex)
{
    if (!tilePending_.contains(imageIndex)) {
        return false;
    }
    askedTiles_.insert(imageIndex);
    // Asked for again while it's wanted (kReaskMs apart), so it doesn't go stale.
    const std::uint64_t now = SDL_GetTicks();
    auto [it, created] = tileAskedMs_.try_emplace(imageIndex, 0);
    if (created || now - it->second >= kReaskMs) {
        it->second = now;
        DecodeJob job;
        job.kind = DecodeKind::Tile;
        job.index = imageIndex;
        decoder_->request(job, now);
    }
    return true;
}

void SpriteCache::setPack(std::unique_ptr<nox::video::ImagePackReader> pack, const std::filesystem::path& path)
{
    decoder_.reset(); // (the workers stopped before anything they'd finish for goes)
    clear();
    atlas_.reset();
    pack_ = std::move(pack);
    packNamed_.clear();
    if (pack_) {
        // BC7 images to the GPU as they are, when the renderer's device takes them.
        bool bc7 = false;
        for (std::uint32_t i = 0; i < pack_->size() && !bc7; ++i) {
            bc7 = pack_->entry(i).present() && pack_->entry(i).codec == nox::video::kCodecBc7;
        }
        if (bc7) {
            atlas_ = GpuAtlas::create(renderer_, materialState_ ? splitShader_ : nullptr);
            SDL_Log("image pack: BC7 images %s", atlas_ ? "uploaded as BC7 to shared pages" : "decoded to RGBA (no BC7 on this renderer)");
        }
    }
    if (pack_ && !path.empty()) {
        const int threads = ImageDecoder::defaultThreads();
        decoder_ = std::make_unique<ImageDecoder>(path, threads);
        SDL_Log("image pack: HD images decoded on %d worker thread%s%s", threads, threads == 1 ? "" : "s",
                placeholders_ ? " (the originals drawn till they're in: --hd-placeholders)" : "");
    }
}

const Sprite* SpriteCache::packNamed(const std::string& name)
{
    if (!pack_) {
        return nullptr;
    }
    auto it = packNamed_.find(name);
    if (it == packNamed_.end()) {
        // (The pack's slots by name: a scan of them all, so kept.)
        const long slot = pack_->find(name);
        it = packNamed_.emplace(name, slot >= 0 ? std::optional<std::int32_t>(static_cast<std::int32_t>(slot)) : std::nullopt).first;
    }
    return it->second ? get(*it->second) : nullptr;
}

std::unique_ptr<nox::video::ImagePackReader> SpriteCache::openPack(const std::filesystem::path& pack,
                                                                   const std::filesystem::path& videoIdx)
{
    try {
        auto reader = std::make_unique<nox::video::ImagePackReader>(pack);
        // Its slots are video.idx's images: a pack made from another version's would put the
        // wrong pictures on things.
        if (reader->sourceIdxCrc() != 0) {
            const auto idx = nox::readFileBytes(videoIdx);
            const std::uint32_t crc = nox::video::packCrc(idx);
            if (crc != reader->sourceIdxCrc()) {
                SDL_Log("image pack %s: made from another video.idx (CRC %08X, the game's is %08X); not used",
                        pack.string().c_str(), reader->sourceIdxCrc(), crc);
                return nullptr;
            }
        }
        std::uint32_t upscaled = 0;
        for (std::uint32_t i = 0; i < reader->size(); ++i) {
            const auto& e = reader->entry(i);
            if (e.present() && static_cast<std::int32_t>(e.pixelWidth) > e.width) {
                ++upscaled;
            }
        }
        SDL_Log("image pack %s: %u images, %u in high definition", pack.string().c_str(), reader->size(), upscaled);
        return reader;
    } catch (const std::exception& ex) {
        SDL_Log("image pack %s: %s", pack.string().c_str(), ex.what());
        return nullptr;
    }
}

std::optional<nox::video::PackImage> SpriteCache::packImage(std::int32_t index, bool blocks)
{
    if (!pack_ || index < 0 || static_cast<std::uint32_t>(index) >= pack_->size() ||
        !pack_->entry(static_cast<std::size_t>(index)).present()) {
        return std::nullopt;
    }
    try {
        return pack_->read(static_cast<std::size_t>(index), false, blocks);
    } catch (const std::exception& ex) {
        SDL_Log("image pack slot %d: %s", index, ex.what());
        return std::nullopt;
    }
}

std::vector<std::int32_t> SpriteCache::hdImages() const
{
    std::vector<std::int32_t> out;
    for (std::size_t i = 0; pack_ && i < std::min<std::size_t>(pack_->size(), bag_.images().size()); ++i) {
        if (hdEntry(static_cast<std::int32_t>(i))) {
            out.push_back(static_cast<std::int32_t>(i));
        }
    }
    return out;
}

const nox::video::PackEntry* SpriteCache::hdEntry(std::int32_t index) const
{
    if (!pack_ || index < 0 || static_cast<std::uint32_t>(index) >= pack_->size()) {
        return nullptr;
    }
    const auto& e = pack_->entry(static_cast<std::size_t>(index));
    // (Floor tiles and edges stay at their size: the floor is composited in world units.)
    return e.present() && static_cast<std::int32_t>(e.pixelWidth) > e.width && e.type != 0 && e.type != 1 ? &e : nullptr;
}

// A video.bag segment's decompressed bytes, from a short most-recently-used list (kSegmentCache).
const std::vector<std::uint8_t>& SpriteCache::segment(std::uint32_t index)
{
    for (auto it = segments_.begin(); it != segments_.end(); ++it) {
        if (it->first == index) {
            segments_.splice(segments_.begin(), segments_, it);
            return segments_.front().second;
        }
    }
    segments_.emplace_front(index, bag_.readSegment(index));
    if (segments_.size() > kSegmentCache) {
        segments_.pop_back();
    }
    return segments_.front().second;
}

// A texture of its own for the pixels (nullopt if it can't be made).
std::optional<Sprite> SpriteCache::makeSprite(const std::vector<std::uint8_t>& rgba, int width, int height,
                                              float offsetX, float offsetY)
{
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    SDL_Texture* tex =
        SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!tex) {
        return std::nullopt;
    }
    SDL_UpdateTexture(tex, nullptr, rgba.data(), width * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    // Original art: keep pixels crisp when zoomed. Upscaled art will use linear filtering.
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    Sprite s{tex, static_cast<float>(width), static_cast<float>(height), offsetX, offsetY};
    s.rect = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    return s;
}

SpriteCache::Entry SpriteCache::makeEntry(const std::vector<std::uint8_t>& rgba, int width, int height, float offsetX,
                                          float offsetY, bool shareable, bool pinned)
{
    Entry e;
    if (width <= 0 || height <= 0) {
        return e;
    }
    ++made_;
    // (The map's pinned images fill pages of their own, which stay till the next map.)
    int& open = pinned ? openPinnedPage_ : openPage_;
    if (shareable && width <= kShareMaxWidth && height <= kShareMaxHeight) {
        // A place on the page being filled, its shelves left to right, then a new shelf below,
        // then a new page (a freed one's slot first).
        // Twice at most: the second time on a new page, after the open one filled up.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (open < 0 || pages_[static_cast<std::size_t>(open)].full) {
                auto slot = std::ranges::find_if(pages_, [](const Page& p) { return p.texture == nullptr; });
                if (slot == pages_.end()) {
                    pages_.emplace_back();
                    slot = pages_.end() - 1;
                }
                slot->texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kPageSide, kPageSide);
                if (!slot->texture) {
                    open = -1;
                    break; // (a texture of its own instead)
                }
                SDL_SetTextureBlendMode(slot->texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(slot->texture, SDL_SCALEMODE_NEAREST);
                slot->pinned = pinned;
                if (pinned) {
                    pinnedPageBytes_ += kPageBytes;
                }
                open = static_cast<int>(slot - pages_.begin());
            }
            // Into the shelf's strip with its gutter (clear) round it: no page is cleared whole, a
            // neighbour's gutter is clear pixels too, and the shelf's new images go up together
            // (flushPage) when the page is next drawn from.
            Page& p = pages_[static_cast<std::size_t>(open)];
            const int paddedWidth = width + 2 * kGutter;
            const int paddedHeight = height + 2 * kGutter;
            // No room left on this shelf: upload it and start the next shelf under it.
            if (p.shelfX + paddedWidth > kPageSide) {
                flushPage(open);
                p.shelfY += p.shelfHeight;
                p.shelfX = 0;
                p.shelfHeight = 0;
                std::ranges::fill(p.strip, std::uint8_t{0});
            }
            // No room for another shelf: the page is full; round again for a new one.
            if (p.shelfY + paddedHeight > kPageSide) {
                flushPage(open);
                p.full = true;
                p.strip = {};
                continue;
            }
            if (p.strip.empty()) {
                // The shelf's staging pixels: the page's width by the tallest a shelf can be.
                p.strip.assign(static_cast<std::size_t>(kPageSide) * (kShareMaxHeight + 2 * kGutter) * 4, 0);
            }
            const SDL_Rect block{p.shelfX, p.shelfY, paddedWidth, paddedHeight};
            const SDL_Rect at{p.shelfX + kGutter, p.shelfY + kGutter, width, height};
            p.shelfX += width + kGutter; // (the next one's gutter is this one's)
            // (Its whole gutter below kept in this shelf: the next shelf's strip starts cleared.)
            p.shelfHeight = std::max(p.shelfHeight, height + 2 * kGutter);
            const auto stripAt = [&](int x, int y) {
                return p.strip.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y - p.shelfY) * kPageSide + static_cast<std::size_t>(x)) * 4);
            };
            for (int y = 0; y < height; ++y) {
                std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4),
                            static_cast<std::size_t>(width) * 4, stripAt(at.x, at.y + y));
                // Its edge pixels one out into the gutter (each side's own half of the shared
                // gutter), so a scaled draw's filtering at its edge blends with itself, as an
                // image's own texture does at its clamped edge, not with clear pixels: drawn
                // side by side (a window border's pieces) no seam shows between them.
                std::copy_n(stripAt(at.x, at.y + y), 4, stripAt(at.x - 1, at.y + y));
                std::copy_n(stripAt(at.x + width - 1, at.y + y), 4, stripAt(at.x + width, at.y + y));
            }
            // The top and bottom rows the same (with the corners just made).
            std::copy_n(stripAt(at.x - 1, at.y), static_cast<std::size_t>(width + 2) * 4, stripAt(at.x - 1, at.y - 1));
            std::copy_n(stripAt(at.x - 1, at.y + height - 1), static_cast<std::size_t>(width + 2) * 4, stripAt(at.x - 1, at.y + height));
            // Grow the rectangle flushPage uploads to take it in.
            if (p.dirty.w == 0) {
                p.dirty = block;
            } else {
                SDL_GetRectUnion(&p.dirty, &block, &p.dirty);
            }
            Sprite s{p.texture, static_cast<float>(width), static_cast<float>(height), offsetX, offsetY};
            s.rect = {static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(width), static_cast<float>(height)};
            s.owner = this;
            s.page = open;
            e.sprite = s;
            e.page = open;
            e.pinned = pinned;
            return e;
        }
    }
    // Too big to share, or no page could be made: a texture of its own.
    e.sprite = makeSprite(rgba, width, height, offsetX, offsetY);
    if (e.sprite) {
        e.bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        ownBytes_ += e.bytes;
        e.pinned = pinned;
    }
    return e;
}

void SpriteCache::flushPage(int page)
{
    if (page < 0 || static_cast<std::size_t>(page) >= pages_.size()) {
        return;
    }
    Page& p = pages_[static_cast<std::size_t>(page)];
    if (p.dirty.w == 0 || !p.texture || p.strip.empty()) {
        return;
    }
    // (The strip's rows start at the shelf's top, which the dirty rectangle is within.)
    const std::uint8_t* from = p.strip.data() + (static_cast<std::size_t>(p.dirty.y - p.shelfY) * kPageSide + static_cast<std::size_t>(p.dirty.x)) * 4;
    SDL_UpdateTexture(p.texture, &p.dirty, from, kPageSide * 4);
    p.dirty = {};
}

void Sprite::ready() const
{
    if (owner && page >= 0) {
        owner->flushPage(page);
    }
    if (owner && gpu) {
        owner->flushGpu();
    }
}

const nox::video::DecodedImage* SpriteCache::decoded(std::int32_t imageIndex)
{
    // Only what the pack improves comes from it: a slot video.bag has is read from video.bag
    // unless the pack holds it in high definition (get()); a same-size frame the pack adds after
    // video.bag's images (an in-between) is read here.
    const bool bagImage = imageIndex >= 0 && static_cast<std::size_t>(imageIndex) < bag_.images().size();
    const bool packOnly = !bagImage && pack_ && imageIndex >= 0 && static_cast<std::uint32_t>(imageIndex) < pack_->size();
    if (!bagImage && !packOnly) {
        return nullptr;
    }
    auto [it, created] = decoded_.try_emplace(imageIndex);
    // A same-size frame only the pack holds: its pixels, as a decoded image.
    if (created && packOnly) {
        const auto& e = pack_->entry(static_cast<std::size_t>(imageIndex));
        if (e.present() && static_cast<std::int32_t>(e.pixelWidth) == e.width) {
            if (auto p = packImage(imageIndex)) {
                nox::video::DecodedImage d;
                d.type = p->type;
                d.width = p->width;
                d.height = p->height;
                d.offsetX = p->offsetX;
                d.offsetY = p->offsetY;
                d.unknown = p->headerByte;
                d.rgba = std::move(p->rgba);
                d.material = std::move(p->material);
                d.edgeMask = std::move(p->edgeMask);
                it->second = std::move(d);
                return &*it->second;
            }
        }
    }
    // From video.bag: its segment (decompressed, kept in a short list), then the image within it.
    if (created && bagImage) {
        try {
            const auto& info = bag_.images()[static_cast<std::size_t>(imageIndex)];
            const auto& data = segment(info.segment);
            it->second = nox::video::decodeImage(std::span(data).subspan(info.offset, info.size), info.type);
        } catch (const std::exception& ex) {
            SDL_Log("image %d: %s", imageIndex, ex.what());
        }
    }
    return it->second ? &*it->second : nullptr;
}

const Sprite* SpriteCache::get(std::int32_t imageIndex)
{
    auto [it, created] = sprites_.try_emplace(imageIndex);
    Entry& e = it->second;
    if (!created) {
        // (Made before: its decode job only wanted for a stand-in's ask.)
        if (!e.sprite) {
            return nullptr;
        }
        if (e.placeholder) {
            ask(e, jobFor(DecodeKind::Plain, imageIndex));
        }
        touch(e);
        return &*e.sprite;
    }
    const DecodeJob job = jobFor(DecodeKind::Plain, imageIndex);
    {
        // Upscaled in the pack: its pack pixels in a texture of its own (not on a page), with
        // mips, or on the BC7 pages; made here and now (the session's and the map's come from
        // the workers ahead), or with placeholders from the workers, the original meanwhile.
        const bool hd = hdEntry(imageIndex) != nullptr;
        if (hd && !decodesLater(imageIndex)) {
            e = makeNow(job);
        }
        // Else the original from video.bag (or a same-size frame the pack adds), on a page.
        if (!e.sprite) {
            if (const auto* image = decoded(imageIndex)) {
                const bool keepPixels = image->type == 0 || image->type == 1 || !image->material.empty();
                e = makeEntry(image->rgba, image->width, image->height, static_cast<float>(image->offsetX),
                              static_cast<float>(image->offsetY), true);
                // Sprites are drawn from their texture from now on; tiles, edges and material-shaded
                // sprites keep their pixels for compositing and tinting.
                if (!keepPixels) {
                    decoded_.erase(imageIndex);
                }
                e.placeholder = hd && decodesLater(imageIndex);
            }
        }
        // No original to stand in after all: the HD one here and now.
        if (!e.sprite && hd && decodesLater(imageIndex)) {
            e = makeNow(job);
        }
    }
    return serve(e, job);
}

const Sprite* SpriteCache::serve(Entry& e, const DecodeJob& job)
{
    if (!e.sprite) {
        return nullptr;
    }
    if (e.placeholder) {
        ask(e, job);
    }
    touch(e);
    return &*e.sprite;
}

// A thing.bin image reference as a video.bag index: its own index, or its name looked up once
// (-1 when there's no such image).
std::int32_t SpriteCache::indexOf(const nox::things::ImageRef& ref)
{
    if (ref.index >= 0) {
        return ref.index;
    }
    auto [it, created] = namedIndex_.try_emplace(ref.name, -1);
    if (created) {
        it->second = static_cast<std::int32_t>(bag_.findImage(ref.name));
    }
    return it->second;
}

const Sprite* SpriteCache::get(const nox::things::ImageRef& ref)
{
    return get(indexOf(ref));
}

const Sprite* SpriteCache::mono(const nox::things::ImageRef& ref, SDL_Color colour)
{
    const std::int32_t index = indexOf(ref);
    const std::uint32_t rgb = (std::uint32_t{colour.r} << 16) | (std::uint32_t{colour.g} << 8) | colour.b;
    DecodeJob job;
    job.kind = DecodeKind::Mono;
    job.index = index;
    job.colour = rgb;
    auto [it, created] = mono_.try_emplace(std::pair{index, rgb});
    Entry& e = it->second;
    // Asked for before: as it came out then (no sprite: it couldn't be made).
    if (!created) {
        return serve(e, job);
    }
    const Sprite* base = get(index);
    if (!base) {
        return nullptr;
    }
    // An HD image: the pack's pixels, from the workers when the original can stand in meanwhile.
    const bool hd = hdEntry(index) != nullptr;
    if (hd && !decodesLater(index)) {
        e = makeNow(job);
        if (!e.sprite) {
            return nullptr;
        }
        touch(e);
        return &*e.sprite;
    }
    // Else video.bag's pixels.
    const auto* d = decoded(index);
    if (!d || d->rgba.empty()) {
        return nullptr;
    }
    std::vector<std::uint8_t> rgba = d->rgba;
    const int w = d->width;
    const int h = d->height;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        // Brightness as 0.30 R + 0.59 G + 0.11 B in 8-bit fixed point (0x4C, 0x96, 0x1C of 256).
        const unsigned lum = (rgba[i] * 0x4Cu + rgba[i + 1] * 0x96u + rgba[i + 2] * 0x1Cu) >> 8;
        rgba[i] = static_cast<std::uint8_t>(lum * colour.r >> 8);
        rgba[i + 1] = static_cast<std::uint8_t>(lum * colour.g >> 8);
        rgba[i + 2] = static_cast<std::uint8_t>(lum * colour.b >> 8);
    }
    e = makeEntry(rgba, w, h, base->offsetX, base->offsetY, true);
    if (!e.sprite) {
        return nullptr;
    }
    e.sprite->width = base->width;
    e.sprite->height = base->height;
    if (hd) {
        e.placeholder = true; // (the HD one asked for)
        ask(e, job);
    }
    touch(e);
    return &*e.sprite;
}

const Sprite* SpriteCache::pendingCopy(std::int32_t imageIndex, std::uint8_t look)
{
    // (Cached with the composites: let go under the budget like them, made again when next drawn.)
    const std::string key = "pending:" + std::to_string(imageIndex) + ":" + std::to_string(look);
    if (const auto it = composites_.find(key); it != composites_.end()) {
        if (!it->second.sprite) {
            return nullptr;
        }
        touch(it->second);
        return &*it->second.sprite;
    }
    const Sprite* base = get(imageIndex);
    const auto* d = base ? decoded(imageIndex) : nullptr;
    Entry& e = composites_[key];
    if (!d || d->rgba.empty()) {
        return nullptr; // (kept as "can't": an image only the pack has)
    }
    std::vector<std::uint8_t> rgba = d->rgba;
    pending::apply(rgba, d->width, static_cast<pending::Look>(look));
    e = makeEntry(rgba, d->width, d->height, base->offsetX, base->offsetY, true);
    if (!e.sprite) {
        return nullptr;
    }
    e.sprite->width = base->width;
    e.sprite->height = base->height;
    touch(e);
    return &*e.sprite;
}

void SpriteCache::renderPending(const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst, std::uint8_t look)
{
    if (sprite.tint && materialState_ && (!sprite.labelTexture || sprite.labelState)) {
        render(sprite, src, dst, look); // (the material shader greys it as it colours it)
        return;
    }
    if (!pendingState_ || look == 0) {
        render(sprite, src, dst);
        return;
    }
    const float uniforms[4] = {static_cast<float>(look), 0, 0, 0};
    SDL_SetGPURenderStateFragmentUniforms(pendingState_, 0, uniforms, sizeof(uniforms));
    SDL_SetGPURenderState(renderer_, pendingState_);
    drawSprite(renderer_, sprite, src, dst);
    SDL_SetGPURenderState(renderer_, nullptr);
}

const Sprite* SpriteCache::tinted(const nox::things::ImageRef& ref, const Materials& materials)
{
    return tinted(indexOf(ref), materials);
}

const Sprite* SpriteCache::tinted(std::int32_t imageIndex, const Materials& materials)
{
    // Cached by image and all seven colours.
    const TintKey tint{imageIndex, materials};
    const bool hd = hdEntry(imageIndex) != nullptr;
    if (!materialState_) {
        // The CPU's shading: a texture per image and colours, its material pixels grey * colour.
        const Sprite* plain = get(imageIndex);
        if (!plain) {
            return nullptr;
        }
        const auto job = [&](const std::string& key) {
            DecodeJob j;
            j.kind = DecodeKind::Tint;
            j.index = imageIndex;
            j.colours = materials;
            j.cacheKey = key;
            return j;
        };
        // Found by its image and colours (made before, not let go since): its key needn't be made.
        if (const auto known = tintIndex_.find(tint); known != tintIndex_.end()) {
            Entry& e = known->second->second;
            if (!e.sprite) {
                return nullptr;
            }
            if (e.placeholder) {
                ask(e, job(known->second->first));
            }
            touch(e);
            return &*e.sprite;
        }
        const std::string key = tintKey(imageIndex, materials);
        auto [it, created] = composites_.try_emplace(key);
        if (created) {
            Entry& e = it->second;
            // An HD image: from the workers when the original can stand in meanwhile.
            if (hd && !decodesLater(imageIndex)) {
                e = makeNow(job(key));
                if (!e.sprite) {
                    composites_.erase(it); // (no material pixels: the plain sprite, the key not kept)
                    return plain;
                }
            } else {
                const auto* image = decoded(imageIndex);
                // No material pixels: the plain sprite (the key isn't kept).
                if (!image || image->rgba.empty() || image->material.empty()) {
                    composites_.erase(it);
                    return plain;
                }
                std::vector<std::uint8_t> rgba = image->rgba;
                const auto& material = image->material;
                // Each pixel's material byte picks which of the colours shades it (0: none).
                shadeMaterials(rgba.data(), material.data(), material.size(), materials);
                e = makeEntry(rgba, image->width, image->height, plain->offsetX, plain->offsetY, true);
                if (e.sprite) {
                    e.sprite->width = plain->width;
                    e.sprite->height = plain->height;
                    e.placeholder = hd; // (the HD one asked for below)
                }
            }
        }
        tintIndex_.emplace(tint, it);
        return serve(it->second, job(key));
    }
    // The material shader's: one place for the image whatever its colours, the image then its
    // material bytes side by side, the colours given as it's drawn. (No plain sprite is made for
    // it: only its pixels, the pack's for an upscaled image, else video.bag's.)
    const DecodeJob job = jobFor(DecodeKind::Material, imageIndex);
    auto [mit, fresh] = materials_.try_emplace(imageIndex);
    Entry& m = mit->second;
    if (fresh) {
        // The pack's for an upscaled image (from the workers when the original can stand in
        // meanwhile), else the original's.
        if (hd && !decodesLater(imageIndex)) {
            m = makeNow(job);
        }
        if (!m.sprite) {
            if (const auto* image = decoded(imageIndex); image && !image->rgba.empty() && !image->material.empty()) {
                const int w = image->width;
                const int h = image->height;
                // Twice as wide: the image, then its material bytes.
                m = makeEntry(besideMaterial(image->rgba, image->material, w, h), w * 2, h,
                              static_cast<float>(image->offsetX), static_cast<float>(image->offsetY), true);
                if (m.sprite) {
                    m.sprite->width = static_cast<float>(w);
                    m.sprite->height = static_cast<float>(h);
                    m.placeholder = hd && decodesLater(imageIndex);
                }
            }
        }
    }
    if (!m.sprite) {
        return get(imageIndex); // (no material pixels: as it is)
    }
    if (m.placeholder) {
        ask(m, job);
    }
    touch(m);
    // A copy per set of colours, sharing the image's one texture.
    auto [s, made] = shaderTints_.try_emplace(tint);
    if (made) {
        s->second = {imageIndex, *m.sprite};
        s->second.second.tint = materials;
    }
    return &s->second.second;
}

void SpriteCache::render(const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst, std::uint8_t pendingLook)
{
    if (!sprite.tint || !materialState_) {
        drawSprite(renderer_, sprite, src, dst);
        return;
    }
    // A BC7 sprite's material bytes on a page of their own: the split shader through that page's
    // render state; without one (it couldn't be made), as it is.
    SDL_GPURenderState* const state = sprite.labelTexture ? sprite.labelState : materialState_;
    if (!state) {
        drawSprite(renderer_, sprite, src, dst);
        return;
    }
    sprite.ready();
    // The material shader (shaders/material.frag.hlsl, material_split.frag.hlsl): the colours, the
    // filtering, the image's size and its corner in the texture (a page's place, or the mip level
    // drawn) and in the material texture, then the draw from the image.
    float w = sprite.imageWidth();
    float h = sprite.rect.h;
    SDL_Texture* texture = sprite.texture;
    SDL_FRect corner{sprite.rect.x, sprite.rect.y, w, h};
    SDL_FRect labels = sprite.labels;
    SDL_FRect from = src ? SDL_FRect{corner.x + src->x, corner.y + src->y, src->w, src->h} : corner;
    SDL_ScaleMode mode = sprite.scale;
    // Drawn small: a mip level instead (a texture of its own, or its place on the BC7 page), the
    // caller's colour and alpha copied to it.
    if (const int level = mipLevel(renderer_, sprite, src ? src->w : w, dst); level > 0) {
        const SpriteLevel& mip = sprite.mips[static_cast<std::size_t>(level - 1)];
        const float mw = sprite.labelTexture ? mip.rect.w : mip.rect.w / 2;
        const float mh = mip.rect.h;
        corner = {mip.rect.x, mip.rect.y, mw, mh};
        labels = mip.labels;
        from = src ? SDL_FRect{corner.x + src->x * mw / w, corner.y + src->y * mh / h, src->w * mw / w, src->h * mh / h}
                   : corner;
        if (mip.texture != sprite.texture) {
            Uint8 r = 255, g = 255, b = 255, a = 255;
            SDL_GetTextureColorMod(sprite.texture, &r, &g, &b);
            SDL_GetTextureAlphaMod(sprite.texture, &a);
            SDL_SetTextureColorMod(mip.texture, r, g, b);
            SDL_SetTextureAlphaMod(mip.texture, a);
        }
        texture = mip.texture;
        w = mw;
        h = mh;
        mode = SDL_SCALEMODE_LINEAR;
    }
    float texW = 0;
    float texH = 0;
    SDL_GetTextureSize(texture, &texW, &texH);
    struct {
        float colour[7][4];
        float params[4];
        float place[4];
        float labels[4];
    } uniforms{};
    for (std::size_t i = 0; i < 7; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            uniforms.colour[i][c] = static_cast<float>((*sprite.tint)[i][c]);
        }
    }
    uniforms.params[0] = mode == SDL_SCALEMODE_NEAREST ? 0.0f : 1.0f;  // 1: filtered (linear), 0: nearest
    uniforms.params[1] = w;  // the image's size in the texture drawn from
    uniforms.params[2] = h;
    uniforms.params[3] = static_cast<float>(pendingLook); // (0: none, as always)
    uniforms.place[0] = corner.x;
    uniforms.place[1] = corner.y;
    uniforms.place[2] = texW;
    uniforms.place[3] = texH;
    uniforms.labels[0] = labels.x; // (material_split only)
    uniforms.labels[1] = labels.y;
    // (material.frag.hlsl's cbuffer is the first three: it's given just those.)
    const auto size = static_cast<Uint32>(sprite.labelTexture ? sizeof(uniforms) : offsetof(decltype(uniforms), labels));
    SDL_SetTextureScaleMode(texture, mode);
    SDL_SetGPURenderStateFragmentUniforms(state, 0, &uniforms, size);
    SDL_SetGPURenderState(renderer_, state);
    SDL_RenderTexture(renderer_, texture, &from, &dst);
    SDL_SetGPURenderState(renderer_, nullptr);
}

const Sprite* SpriteCache::composite(const std::string& key, int width, int height,
                                     const std::function<std::vector<std::uint8_t>()>& build, bool pin,
                                     CompositeHint* hint)
{
    // (Let go under the budget like the rest, unless pinned; made again by the builder when next
    // asked for.)
    Entry* found = hint && hint->epoch == compositeEpoch_ ? hint->entry : nullptr;
    if (!found) {
        auto [it, created] = composites_.try_emplace(key);
        if (created) {
            it->second = makeEntry(build(), width, height, 0, 0, true, pin && compositePinAllowed());
        }
        found = &it->second;
        if (hint) {
            *hint = {compositeEpoch_, found};
        }
    }
    Entry& e = *found;
    if (!e.sprite) {
        return nullptr;
    }
    touch(e);
    return &*e.sprite;
}

const Sprite* SpriteCache::compositeLater(const std::string& key, int width, int height,
                                          const std::function<Builder()>& prepare, bool pin, bool* pending,
                                          CompositeHint* hint)
{
    if (!decoder_) {
        return composite(key, width, height, prepare(), pin, hint);
    }
    Entry* found = hint && hint->epoch == compositeEpoch_ ? hint->entry : nullptr;
    if (!found) {
        if (const auto it = composites_.find(key); it != composites_.end()) {
            found = &it->second;
            if (hint) {
                *hint = {compositeEpoch_, found};
            }
        }
    }
    if (found) {
        if (!found->sprite) {
            return nullptr; // (couldn't be built)
        }
        touch(*found);
        return &*found->sprite;
    }
    if (pending) {
        *pending = true;
    }
    askedComposites_.insert(key);
    // Asked for (again every kReaskMs while it's wanted, so it doesn't go stale); its builder made
    // only then.
    const std::uint64_t now = SDL_GetTicks();
    auto [asked, created] = compositeAskedMs_.try_emplace(key, 0);
    if (created || now - asked->second >= kReaskMs) {
        asked->second = now;
        DecodeJob job;
        job.kind = DecodeKind::Composite;
        job.cacheKey = key;
        job.width = width;
        job.height = height;
        job.pin = pin;
        job.build = prepare();
        decoder_->request(job, now);
    }
    return nullptr;
}

bool SpriteCache::awaitAsked(std::uint64_t maxMs)
{
    // What's still on its way of what was asked for (the rest came meanwhile).
    const auto outstanding = [&] {
        std::erase_if(askedTiles_, [&](std::int32_t index) { return !tilePending_.contains(index); });
        std::erase_if(askedComposites_, [&](const std::string& key) { return composites_.contains(key); });
        return !askedTiles_.empty() || !askedComposites_.empty();
    };
    if (!decoder_ || !outstanding()) {
        askedTiles_.clear();
        askedComposites_.clear();
        return false;
    }
    const std::uint64_t start = SDL_GetTicksNS();
    const std::uint64_t deadline = start + maxMs * 1'000'000;
    const std::size_t tiles = askedTiles_.size();
    const std::size_t composites = askedComposites_.size();
    DecodeResult r;
    std::size_t taken = 0;
    double workMs = 0;
    while (outstanding()) {
        const std::uint64_t now = SDL_GetTicksNS();
        if (now >= deadline || !decoder_->takeWait(r, std::max<std::uint64_t>(1, (deadline - now) / 1'000'000))) {
            SDL_Log("image pack: waited %llu ms for %zu floor tiles and %zu composites; drawn as they are",
                    static_cast<unsigned long long>(maxMs), askedTiles_.size(), askedComposites_.size());
            break;
        }
        ++taken;
        workMs += r.decodeMs;
        install(std::move(r));
    }
    askedTiles_.clear();
    askedComposites_.clear();
    const double ms = static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
    ++loadStats_.waits;
    loadStats_.waitMs += ms;
    if (ms >= 50) {
        SDL_Log("image pack: the floor waited %.0f ms for %zu tiles' pixels and %zu composites (%zu results put in, the "
                "workers' time %.0f ms)",
                ms, tiles, composites, taken, workMs);
    }
    return true;
}

bool SpriteCache::compositePinAllowed() const
{
    // While the map's reserve for its floor composites lasts, a page at a time (the open one filled).
    return pack_ && (pinnedPageBytes_ < compositeReserve_ ||
                     (openPinnedPage_ >= 0 && !pages_[static_cast<std::size_t>(openPinnedPage_)].full));
}

} // namespace noxsdl
