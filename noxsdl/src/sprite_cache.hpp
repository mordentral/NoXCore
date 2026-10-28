// video.bag images as SDL textures, decoded on first use; with Options' Use HD Images, the image
// pack's (.nxp, noxcore video/image_pack.hpp) upscaled images over them: only what the pack holds
// in high definition (and the in-between frames it adds) comes from it, the rest from video.bag.
//
// A Sprite's size and offsets are *logical*: the original image's pixels, i.e. world units. The
// texture may have more pixels than that (an image the pack stores 2x or 4x); drawing always
// uses the logical rectangle, so higher-resolution art drops in without layout changes, filtered
// smoothly where the original's pixels stay crisp. decoded() is always the original resolution
// (the floor's compositing and the effects' sprite tracing work in world units).
//
// With a pack, an image it holds in HD is never drawn as video.bag's original: the images any map
// can need (beginSession: players, items, effects, the interface) are decoded on worker threads
// (image_decoder.hpp) at start-up and kept for the session; a map's own (beginMap) as it loads,
// kept till the next map (the map isn't shown till they're in: preloading()); anything else is
// read and made in the frame that first wants it, and a floor tile's HD composite is built on the
// workers while that frame waits (awaitAsked). `--hd-placeholders` brings back the old way for
// comparison: the original drawn till the workers' decode is in.
//
// A kCodecBc7 pack's images go to the GPU as BC7 (gpu_atlas.hpp: shared pages, a quarter of
// RGBA's memory, nothing decoded); a material image's material bytes on pages of their own for
// the material shader. Without the GPU renderer, or a device without BC7, or a version 2 pack,
// they're RGBA textures as before.
#pragma once

#include "gpu_atlas.hpp"
#include "image_decoder.hpp"

#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/image_pack.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace noxsdl {

class SpriteCache;

/// A smaller level of an HD sprite: its pixels' place (a texture of its own, all of it, or its
/// place on a BC7 page), and a BC7 material sprite's material bytes' place on their page.
struct SpriteLevel {
    SDL_Texture* texture = nullptr;
    SDL_FRect rect{};
    SDL_FRect labels{};
};

struct Sprite {
    SDL_Texture* texture = nullptr;
    float width = 0; ///< logical size (original pixels = world units)
    float height = 0;
    float offsetX = 0; ///< the image's draw offset from its header (sprites; 0 for tiles)
    float offsetY = 0;
    /// Set for a sprite the material shader colours as it's drawn (GPU renderer): its pixels
    /// hold the image and its material bytes side by side. Draw it with SpriteCache::render.
    std::optional<MaterialColours> tint;
    /// The texture has more pixels than the logical size (an upscaled pack image): draw it
    /// filtered whatever the zoom.
    bool hd = false;
    /// An HD texture's smaller copies, each half the one before (area-averaged): drawn at a
    /// fraction of its pixels (a 4x spell icon on a 1080p screen is about 2.5 texels a pixel),
    /// linear filtering alone skips texels and the edges come out jagged. drawSprite picks one.
    std::vector<SpriteLevel> mips;
    /// Its pixels in `texture`: all of it, or its place on an atlas page small images share
    /// (SpriteCache: Direct3D 12 gives every texture at least 64 KB, most frames are 5-20 KB).
    SDL_FRect rect{};
    /// How it's filtered when drawn (drawSprite / SpriteCache::render set it on the texture each
    /// draw, the texture perhaps being a shared page): set this, not the texture's.
    mutable SDL_ScaleMode scale = SDL_SCALEMODE_NEAREST;
    /// On an atlas page: the cache and the page, whose new images go up in one upload when one
    /// of them is first drawn (ready()).
    SpriteCache* owner = nullptr;
    int page = -1;
    /// On a BC7 page (GpuAtlas): its staged uploads go up before it's first drawn (ready()).
    bool gpu = false;
    /// A BC7 material sprite's material bytes: on a page of their own (at `labels`, its mips'
    /// at theirs), which the material shader reads through the page's render state (2D) or
    /// texture (the 3D view). Unset, a material sprite has them beside its image.
    SDL_GPURenderState* labelState = nullptr;
    SDL_GPUTexture* labelTexture = nullptr;
    SDL_FRect labels{};

    /// The image's width in its texture: half its place when its material bytes are beside it.
    float imageWidth() const { return tint && !labelTexture ? rect.w / 2 : rect.w; }

    /// The rectangle in `texture` for `src` in the sprite's own pixels (null: all of it).
    SDL_FRect source(const SDL_FRect* src) const
    {
        return src ? SDL_FRect{rect.x + src->x, rect.y + src->y, src->w, src->h} : rect;
    }
    /// Its pixels uploaded (a page's pending ones in one go); drawSprite and
    /// SpriteCache::render call it, and anything drawing `texture` itself must.
    void ready() const;
};

/// Draws a sprite's texture (its whole image when `src` is null), or for an HD sprite drawn
/// smaller than its pixels the mip level nearest above the drawn size, with the texture's colour
/// and alpha modulation and blend mode. Use it (or SpriteCache::render) for every sprite draw.
void drawSprite(SDL_Renderer* renderer, const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst);

class SpriteCache {
    struct Entry; // (below)

public:
    SpriteCache(SDL_Renderer* renderer, nox::video::VideoBag& bag);
    ~SpriteCache();
    SpriteCache(const SpriteCache&) = delete;
    SpriteCache& operator=(const SpriteCache&) = delete;

    /// The sprite for a video.bag image index, or nullptr if it can't be decoded.
    const Sprite* get(std::int32_t imageIndex);
    /// The sprite for a thing.bin image reference (an index, or a named image).
    const Sprite* get(const nox::things::ImageRef& ref);
    /// An image the HD pack has of its own by name ("Folder/Name": noxcpp's, no game image
    /// under it), or nullptr (no pack, or none of that name).
    const Sprite* packNamed(const std::string& name);

    using Materials = MaterialColours;

    /// The image with its material pixels shaded by `materials`; the plain sprite if it has none.
    /// With a material shader (setMaterialShader) the shading happens as it's drawn.
    const Sprite* tinted(std::int32_t imageIndex, const Materials& materials);
    const Sprite* tinted(const nox::things::ImageRef& ref, const Materials& materials);
    /// The image in one colour: each pixel's brightness (0.30 R + 0.59 G + 0.11 B, the exe's
    /// table) times `colour` (one of the blitter's modes), as the
    /// game draws a creature its summon cage points at.
    const Sprite* mono(const nox::things::ImageRef& ref, SDL_Color colour);

    /// The GPU renderer's material shader (GpuShaders::material), or nullptr to shade on the CPU;
    /// `split` the one for BC7 images, their material bytes in a texture of their own
    /// (GpuShaders::materialSplit; null: BC7 material images go up as RGBA). Before setPack.
    void setMaterialShader(SDL_GPURenderState* state, SDL_GPUShader* split = nullptr)
    {
        materialState_ = state;
        splitShader_ = split;
    }
    /// Draws a sprite (its whole image when `src` is null), through the material shader if it
    /// has a tint. `pendingLook` (a map editor session's: pending_look.hpp, 0 none) greys it in the
    /// same pass.
    void render(const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst, std::uint8_t pendingLook = 0);

    /// The GPU renderer's pending shader (GpuShaders::pending), or nullptr: the pending looks
    /// then come from pendingCopy's CPU copies.
    void setPendingShader(SDL_GPURenderState* state) { pendingState_ = state; }
    bool hasPendingShader() const { return pendingState_ != nullptr; }
    /// Whether renderPending shades this sprite on the GPU (else draw its pendingCopy).
    bool pendingInShader(const Sprite& sprite) const
    {
        return pendingState_ != nullptr || (sprite.tint && materialState_ && (!sprite.labelTexture || sprite.labelState));
    }
    /// Draws a sprite in a pending look (1 pending, 2 ghost) through the pending shader (a tinted
    /// sprite through the material shader's), with the texture's colour and alpha modulation as
    /// set. Without the shader, as render (draw pendingCopy's sprite instead).
    void renderPending(const Sprite& sprite, const SDL_FRect* src, const SDL_FRect& dst, std::uint8_t look);
    /// The CPU fallback: the image in a pending look (greyed; a ghost darker and hatched) from its
    /// original pixels, made once and cached like tinted()'s; nullptr if it can't be made.
    const Sprite* pendingCopy(std::int32_t imageIndex, std::uint8_t look);

    /// The decoded pixels of an image (kept for compositing tiles and edges), or nullptr.
    const nox::video::DecodedImage* decoded(std::int32_t imageIndex);
    /// A floor tile's (or edge's) upscaled pixels from the pack (pixelWidth > width), or nullptr:
    /// MapView composites HD floors from them (shared: a composite built on a worker holds them).
    /// Also nullptr while its decode is on the way (hdTilePending).
    std::shared_ptr<const nox::video::PackImage> hdTile(std::int32_t imageIndex);
    /// hdTile()'s pixels (none: not upscaled, or still pending), when last asked for, and whether
    /// they're one of the map's (kept till the next map).
    struct TilePixels {
        std::shared_ptr<const nox::video::PackImage> image;
        std::uint64_t usedMs = 0;
        bool pinned = false;
    };
    /// hdTile() without a copy of the pixels' handle (MapView's look over every tile in view each
    /// frame): its entry, or nullptr where hdTile gives nullptr whatever the image (no pack, an
    /// index past it). Good till endFrame.
    const TilePixels* hdTilePixels(std::int32_t imageIndex);
    /// Whether hdTile's pixels for the image are still being decoded (asked for again as it's
    /// called): a tile composited now would come out partly SD, so MapView draws the SD one.
    bool hdTilePending(std::int32_t imageIndex);
    /// How many times the pack upscales a floor tile or edge (1: not at all), from its index alone.
    int hdTileScale(std::int32_t imageIndex) const;
    std::int32_t indexOf(const nox::things::ImageRef& ref);

    /// Where a composite was found, kept by the caller (a floor tile) so the next call with the
    /// same key finds it without looking the key up: good while the cache has let go of no
    /// composite since (and only with this cache).
    struct CompositeHint {
        std::uint64_t epoch = 0;
        Entry* entry = nullptr;
    };
    /// A texture built by the caller from RGBA pixels, cached under `key` (e.g. an edged tile).
    /// `pin`: one of the map's own (an HD floor tile), kept till the next map while the map's
    /// share of the preload cap lasts. `hint` (if given, kept with `key`): where it was last found.
    const Sprite* composite(const std::string& key, int width, int height,
                            const std::function<std::vector<std::uint8_t>()>& build, bool pin = false,
                            CompositeHint* hint = nullptr);
    /// The same, its pixels built on the decode workers (without workers, here and now as
    /// composite()): nullptr till they're in. `prepare` gives the builder, called only when a build
    /// is asked for; what the builder uses must be its own (copies, or shared pixels nothing
    /// changes), as it runs on another thread.
    /// `pending` (if given) is set while it's on its way (awaitAsked waits for it).
    using Builder = std::function<std::vector<std::uint8_t>()>;
    const Sprite* compositeLater(const std::string& key, int width, int height, const std::function<Builder()>& prepare,
                                 bool pin = false, bool* pending = nullptr, CompositeHint* hint = nullptr);

    /// Whether the drawing waits for the HD images it asks the workers for (a floor's tiles and
    /// composites: awaitAsked) rather than drawing the originals meanwhile.
    bool waitsForHd() const { return decoder_ && !placeholders_ && !deferHd_; }
    /// The map editor's own view (never a game's): while on, an HD image not made yet is asked of
    /// the workers and its original drawn meanwhile, and the floor doesn't wait for its HD tiles
    /// (as --hd-placeholders, for the editor's draws alone): a first look over a big map doesn't
    /// stop for hundreds of decodes in one frame. The originals it made stand in only till their
    /// HD images are in, or till the next beginMap (a game's map), which drops any still
    /// standing in, so a game's view never draws one.
    void setDeferHd(bool on) { deferHd_ = on; }
    /// Waits, up to `maxMs`, for the floor tiles' pixels and composites asked for since the last
    /// call, putting in whatever the workers finish meanwhile. The drawing calls it, then draws
    /// what it was missing. Returns whether anything was waited for.
    bool awaitAsked(std::uint64_t maxMs);
    /// What was asked for so far forgotten (not waited for: asked ahead of being seen).
    void clearAsked()
    {
        askedTiles_.clear();
        askedComposites_.clear();
    }
    /// --hd-placeholders: an HD image not decoded yet drawn as video.bag's original meanwhile, as
    /// before (comparisons and tests). Before setPack.
    void setPlaceholders(bool on) { placeholders_ = on; }

    /// The images a map uses itself, in the order they're wanted (MapView::preloadList).
    struct MapImages {
        struct Image {
            std::int32_t index = -1;
            bool tile = false;   ///< a floor tile or edge (hdTile's pixels)
            bool tinted = false; ///< drawn with material colours (tinted(): weapons, armour, NPCs)
        };
        std::vector<Image> images;
        /// The HD floor composites the map's tiles make (their texture bytes, estimated).
        std::size_t compositeBytes = 0;
        std::string name; ///< for the log
    };
    /// A map loaded: its HD images queued for decoding and kept, with the floor composites they
    /// make, until the next call (exempt from the budget and idle let-go), as far as kPreloadCap
    /// allows; the rest come on demand. The last map's are let go to the budget as usual.
    /// Without a pack, only the originals' decoded pixels (decoded()) are let go.
    void beginMap(const MapImages& images);
    /// The images any map can need (sessionImages in map_preload.cpp): queued ahead of any map's
    /// and kept for the session (till setPack), as far as kSessionCap allows.
    void beginSession(const MapImages& images);
    /// The session's or the map's images still on their way: the map isn't shown meanwhile.
    bool preloading() const { return !preloadOutstanding_.empty() || !sessionOutstanding_.empty(); }
    /// While the map is held back (nothing drawn of it), endFrame puts in more a frame.
    void setHolding(bool holding) { holding_ = holding; }

    std::size_t loaded() const { return sprites_.size(); }
    /// Whether get(index) would hand out what's made already (nothing read or made), and the same
    /// for composite(key): MapView's make budget (setFloorMakeBudget) skips what isn't.
    bool cached(std::int32_t index) const { return sprites_.contains(index); }
    bool cachedComposite(const std::string& key) const { return composites_.contains(key); }
    /// video.bag's images the pack holds in HD (not floor tiles or edges), by index.
    std::vector<std::int32_t> hdImages() const;
    /// The pack's entry for an image it holds in HD, else nullptr; and its name in the pack.
    const nox::video::PackEntry* hdPackEntry(std::int32_t index) const { return hdEntry(index); }
    std::string_view packName(std::int32_t index) const
    {
        return pack_ ? pack_->name(static_cast<std::size_t>(index)) : std::string_view{};
    }

    /// After the frame's present: HD decodes finished on the workers put in place (a placeholder's
    /// Sprite then holds its HD texture), then what wasn't drawn for a while is let go (whole atlas
    /// pages, HD textures) when over the budget, and anything unused for a minute, the map's own
    /// aside; it's made again when it's next asked for. Sprite pointers from before stay good
    /// till this call, and textures they hold may go in it.
    void endFrame(std::uint64_t nowMs);
    /// Texture memory held: atlas pages and the textures of their own, in bytes.
    std::size_t textureBytes() const;
    /// Images made into textures so far (first uses and remakes after a let-go: --frame-stats).
    std::size_t made() const { return made_; }
    /// Images read and made in the frame that wanted them (no SD stand-in), the time that took,
    /// and the drawing's waits for floor tiles (awaitAsked), since the last call (--frame-stats).
    struct LoadStats {
        std::size_t misses = 0;
        double missMs = 0;
        std::size_t waits = 0;
        double waitMs = 0;
    };
    LoadStats takeLoadStats() { return std::exchange(loadStats_, {}); }
    /// The textures that hold the images: atlas pages, BC7 pages and material pages (for the log).
    std::string textureSummary() const;
    /// A page's images made since its last upload, uploaded (Sprite::ready).
    void flushPage(int page);
    /// The BC7 pages' staged uploads sent (Sprite::ready).
    void flushGpu()
    {
        if (atlas_) {
            atlas_->flush();
        }
    }

    /// Images from this pack from now on (nullptr: video.bag's again). Drops every texture made
    /// so far, so call it between frames. With the pack's `path`, its HD images are decoded on
    /// worker threads (each opens the file again); without, on the drawing thread as they're asked.
    void setPack(std::unique_ptr<nox::video::ImagePackReader> pack, const std::filesystem::path& path = {});
    bool usingPack() const { return pack_ != nullptr; }
    /// Checks a pack against the game's video.idx (the CRC it was built from) and opens it;
    /// nullptr, with the reason logged, if it can't be used.
    static std::unique_ptr<nox::video::ImagePackReader> openPack(const std::filesystem::path& pack,
                                                                  const std::filesystem::path& videoIdx);

private:
    /// A made sprite (or one that couldn't be: nullopt), where it lives and when it was last
    /// asked for. On a page (page >= 0) the page owns the texture; else the entry owns its
    /// texture and mips (`bytes` of them).
    struct Entry {
        std::optional<Sprite> sprite;
        int page = -1;
        std::uint64_t usedMs = 0;
        std::size_t bytes = 0;
        /// The SD original standing in for an HD image being decoded; replaced when it's in.
        bool placeholder = false;
        /// One of the map's images (beginMap): never let go till the next map.
        bool pinned = false;
        /// One of the session's (beginSession): never let go.
        bool session = false;
        std::uint64_t askedMs = 0; ///< when its decode was last asked for (a placeholder's)
        /// On BC7 pages (GpuAtlas): its levels' page and its material bytes' page (or -1).
        int gpuPage = -1;
        int labelPage = -1;
    };
    /// An atlas page: filled shelf by shelf, let go whole. The shelf being filled is kept in
    /// `strip` too (rows from shelfY), its new images uploaded together (`dirty`) when the page is
    /// next drawn from or the shelf ends: an upload costs ~0.2 ms however small.
    struct Page {
        SDL_Texture* texture = nullptr;
        int shelfX = 0;
        int shelfY = 0;
        int shelfHeight = 0;
        bool full = false;
        bool pinned = false; ///< holds the map's floor composites: kept till the next map
        std::uint64_t usedMs = 0;
        std::vector<std::uint8_t> strip;
        SDL_Rect dirty{};
    };

    /// Slot `index` from the pack (any resolution), if the pack has it; `blocks`: a BC7 image's
    /// blocks only (DecodeJob::blocks).
    std::optional<nox::video::PackImage> packImage(std::int32_t index, bool blocks = false);
    /// A decode of `kind` for the image, its blocks asked for when the GPU atlas takes them.
    DecodeJob jobFor(DecodeKind kind, std::int32_t index) const;
    /// The job read and made here and now (a miss: no stand-in), timed for --frame-stats.
    Entry makeNow(const DecodeJob& job);
    /// An HD sprite on BC7 pages from a decode of BC7 levels (no sprite if a page couldn't be made).
    Entry makeAtlasEntry(const DecodeResult& result, GpuAtlas::Pool pool);
    /// Every sprite on a BC7 or material page forgotten, then the page.
    void dropGpuPage(int page);
    /// Each BC7 and material page marked kept if anything kept is on it (the sweep's).
    void repinGpuPages();
    /// The pack's entry for an image it holds upscaled (HD), else nullptr.
    const nox::video::PackEntry* hdEntry(std::int32_t index) const;
    /// Whether an HD image is decoded on the workers: there are workers, and video.bag has the
    /// image to draw meanwhile (an image only the pack has is decoded when asked, as before).
    bool decodesLater(std::int32_t index) const;
    /// The job asked for again, at most every kReaskMs per entry (keeps it from going stale).
    void ask(Entry& entry, const DecodeJob& job);
    /// An HD sprite's texture and mips from a decode (`entry.bytes` counted), or its place on the
    /// BC7 pages of `pool` for BC7 levels; no sprite if it can't be made.
    Entry makeHdEntry(const DecodeResult& result, GpuAtlas::Pool pool = GpuAtlas::Pool::Demand);
    /// A finished decode put in place: a placeholder replaced, or a map image kept.
    void install(DecodeResult&& result);
    /// Decodes taken from the workers, a few milliseconds' worth a frame; the map preload's
    /// progress logged once it's done.
    void pump(std::uint64_t nowMs);
    void clear();
    const std::vector<std::uint8_t>& segment(std::uint32_t index);
    /// A sprite of the pixels: on an atlas page when it's small and `shareable` (a pinned page
    /// when `pinned`), else a texture of its own (`entry.bytes` counted).
    Entry makeEntry(const std::vector<std::uint8_t>& rgba, int width, int height, float offsetX, float offsetY,
                    bool shareable, bool pinned = false);
    std::optional<Sprite> makeSprite(const std::vector<std::uint8_t>& rgba, int width, int height, float offsetX,
                                     float offsetY);
    /// Marks an entry (and its page) used now.
    void touch(Entry& entry);
    /// An entry handed out: nullptr if it couldn't be made, else touched, a stand-in asking again
    /// for its HD image (`job`).
    const Sprite* serve(Entry& entry, const DecodeJob& job);
    /// Its texture and mips destroyed, unless a page holds it.
    void release(Entry& entry);
    /// Every sprite on `page` forgotten, then the page's texture.
    void dropPage(int page);

    SDL_Renderer* renderer_;
    nox::video::VideoBag& bag_;
    std::map<std::int32_t, Entry> sprites_;
    std::map<std::pair<std::int32_t, std::uint32_t>, Entry> mono_; ///< (image, 0xRRGGBB)
    std::map<std::int32_t, std::optional<nox::video::DecodedImage>> decoded_;
    std::map<std::string, Entry> composites_; ///< composite()'s (floor tiles) and the CPU's tints
    /// composites_' entries handed out as hints (CompositeHint) and found by tint (tintIndex_) stay
    /// good till one is let go: then this changes (never the same in two caches) and tintIndex_ is
    /// forgotten. compositesLetGo() after any erase from composites_ but of an entry just made.
    std::uint64_t compositeEpoch_ = 0;
    void compositesLetGo();
    /// The image and the seven colours of a tint (the CPU's in composites_, the shader's in
    /// shaderTints_), its key there.
    using TintKey = std::pair<std::int32_t, MaterialColours>;
    std::map<TintKey, std::map<std::string, Entry>::iterator> tintIndex_; ///< the CPU's tints in composites_
    std::map<std::string, std::int32_t> namedIndex_;
    std::map<std::string, std::optional<std::int32_t>> packNamed_; ///< packNamed()'s slots by name
    SDL_GPURenderState* materialState_ = nullptr;
    SDL_GPUShader* splitShader_ = nullptr;
    SDL_GPURenderState* pendingState_ = nullptr; ///< setPendingShader
    std::unique_ptr<nox::video::ImagePackReader> pack_;
    /// The BC7 pages (a pack with kCodecBc7 entries on the GPU renderer with BC7), else null.
    std::unique_ptr<GpuAtlas> atlas_;
    bool placeholders_ = false; ///< --hd-placeholders
    bool deferHd_ = false;      ///< setDeferHd (the map editor's draws)
    /// The originals standing in for HD images (placeholder entries) dropped, unless
    /// --hd-placeholders keeps them (beginMap: a game's view never draws one).
    void dropStandIns();
    bool holding_ = false;      ///< setHolding
    /// What the drawing asked the workers for since the last awaitAsked: floor tiles' pixels and composites.
    std::set<std::int32_t> askedTiles_;
    std::set<std::string> askedComposites_;
    LoadStats loadStats_;
    std::map<std::int32_t, TilePixels> hdTiles_; ///< hdTile()'s, by image
    int hdLoaded_ = 0; ///< upscaled images made into textures (the first few are logged)
    std::map<std::int32_t, Entry> materials_; ///< image + material bytes side by side, by image
    /// The shader-tinted sprites (by image and colours), sharing their image's materials_ entry.
    std::map<TintKey, std::pair<std::int32_t, Sprite>> shaderTints_;
    std::vector<Page> pages_;
    int openPage_ = -1;      ///< the page being filled
    int openPinnedPage_ = -1; ///< the pinned page being filled (the map's floor composites)
    std::size_t ownBytes_ = 0; ///< the entries' own textures and mips
    std::uint64_t sweptMs_ = 0; ///< endFrame's last look (once a second)
    std::size_t made_ = 0;

    // The workers (with a pack's path) and what's on the way from them.
    std::unique_ptr<ImageDecoder> decoder_;
    std::set<std::int32_t> tilePending_;                 ///< hdTile's being decoded
    std::map<std::int32_t, std::uint64_t> tileAskedMs_;  ///< and when each was last asked for
    std::map<std::string, std::uint64_t> compositeAskedMs_; ///< compositeLater's being built, when last asked for
    /// Whether a new composite may be pinned: the map's reserve for them not used up (a page at a time).
    bool compositePinAllowed() const;
    // The map's preload (beginMap).
    std::set<DecodeJob::Key> pinWanted_;          ///< the map's images kept as they come in
    std::set<DecodeJob::Key> preloadOutstanding_; ///< those not in yet
    std::string preloadName_;
    std::uint64_t preloadStartMs_ = 0;
    std::size_t preloadQueued_ = 0;
    std::size_t preloadBeyondCap_ = 0; ///< left on demand: past the cap
    std::size_t preloadEstimate_ = 0;  ///< the kept images' estimated texture bytes
    double preloadDecodeMs_ = 0;       ///< the workers' time on them
    bool preloadLogged_ = true;
    std::size_t compositeReserve_ = 0;  ///< the cap's share for the map's floor composites
    std::size_t pinnedPageBytes_ = 0;   ///< pinned pages made for them so far
    // The session's (beginSession): its jobs (queued again ahead of each map's while any is
    // outstanding), those wanted and those not in yet.
    std::vector<DecodeJob> sessionJobs_;
    std::set<DecodeJob::Key> sessionWanted_;
    std::set<DecodeJob::Key> sessionOutstanding_;
    std::uint64_t sessionStartMs_ = 0;
    std::size_t sessionEstimate_ = 0;
    double sessionDecodeMs_ = 0;
    bool sessionLogged_ = true;
    std::vector<DecodeJob> mapJobs_; ///< the map's preload jobs (beginMap)
    /// The decoder's preload list: the session's outstanding jobs, then the map's.
    void queuePreloads();
    // A few decompressed segments, most recently used first.
    std::list<std::pair<std::uint32_t, std::vector<std::uint8_t>>> segments_;
};

} // namespace noxsdl
