// The image pack's images decoded off the drawing thread: a few worker threads, each with its own
// ImagePackReader on the pack, read and decompress an image and make what its textures are made
// from (the image's levels, each half the one before, tinted or in one colour as asked; a BC7
// image's levels as the pack stores them, nothing decoded). The drawing thread takes the results
// between frames and only makes and uploads the textures (SpriteCache): the session's and each
// map's images, ahead of their being drawn. They also build what the caller hands them to build
// (an HD floor tile composited with its edges, ~1 ms each and hundreds at once when a new part of
// a map comes into view), while the frame that wants them waits for them (takeWait).
//
// Two queues: images asked for while drawing (newest first: what's on screen now), and a map's
// images queued as it loads (in order, after anything asked for). A request not asked again for
// a while is dropped (dropStale), as are all of them when the map changes (dropRequests).
//
// No SDL renderer or texture calls here: workers touch only their reader and CPU buffers.
#pragma once

#include "noxcore/video/image_pack.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace noxsdl {

/// RGB per material slot, 1-6 (slot 0 and unset slots stay white). Material pixels hold a grey
/// intensity the game multiplies by the slot's colour (player skin and hair, cloth colours).
using MaterialColours = std::array<std::array<std::uint8_t, 3>, 7>;

/// What a decode makes of a pack image.
enum class DecodeKind : std::uint8_t {
    Plain,    ///< the image and its smaller levels (SpriteCache::get)
    Mono,     ///< in one colour (SpriteCache::mono)
    Tint,     ///< its material pixels shaded on the CPU (SpriteCache::tinted without the shader)
    Material, ///< the image beside its material bytes, for the material shader
    Tile,     ///< a floor tile's or edge's pixels, kept on the CPU for the floor's compositing
    Composite, ///< pixels the caller's function builds (an HD floor tile and its edges), no pack read
};

/// A decode asked for. Two jobs with the same key make the same thing.
struct DecodeJob {
    DecodeKind kind = DecodeKind::Plain;
    std::int32_t index = -1;
    std::uint32_t colour = 0; ///< Mono: 0xRRGGBB
    MaterialColours colours{}; ///< Tint
    std::string cacheKey;      ///< Tint and Composite: the key it's kept under
    /// Composite: builds width x height RGBA pixels on a worker, from what it holds (copies or
    /// shared pixels nothing changes: never the caller's state).
    std::function<std::vector<std::uint8_t>()> build;
    int width = 0;
    int height = 0;
    bool pin = false; ///< Composite: one of the map's own (SpriteCache keeps it while the map's reserve lasts)
    /// Plain / Material: a kCodecBc7 image's levels handed over as BC7 blocks (the GPU atlas takes
    /// them), not decoded to RGBA. (Not in the key: SpriteCache sets it the same way every time.)
    bool blocks = false;

    using Key = std::tuple<DecodeKind, std::int32_t, std::uint32_t, std::string>;
    Key key() const { return {kind, index, colour, cacheKey}; }
};

/// One texture level as it goes up: RGBA (straight alpha), or with `bc7` the pack's BC7 blocks as
/// they are (rows of 4 x 4 blocks covering width x height padded to multiples of 4, the padding
/// clear), for the GPU atlas.
struct TextureLevel {
    SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA32;
    int width = 0; ///< in texels (BC7: the level's own size, not the padded one)
    int height = 0;
    int pitch = 0; ///< bytes a row (for a block format, a row of blocks)
    std::vector<std::uint8_t> bytes;
    bool bc7 = false;
};

/// A finished decode. `ok` false: the pack couldn't give it (the slot's empty or corrupt, or a
/// Material / Tint job's image has no material pixels).
struct DecodeResult {
    DecodeJob job;
    bool ok = false;
    float width = 0; ///< logical size and offset (original pixels)
    float height = 0;
    float offsetX = 0;
    float offsetY = 0;
    std::string name;
    /// Level 0 then each half the one before, as many as the drawing wants (none under 8
    /// texels). A Material job's levels hold the image and its material bytes side by side, as
    /// the material shader's textures do; BC7 levels don't (their material bytes are below).
    std::vector<TextureLevel> levels;
    /// A BC7 Material job's material bytes, a plane per level (each level's size, one byte a
    /// texel; the smaller ones halved as labels: each 2x2 block's first).
    std::vector<std::vector<std::uint8_t>> labels;
    std::optional<nox::video::PackImage> pixels; ///< Tile: the image itself
    double decodeMs = 0; ///< the worker's time on it (read, decompress, levels)
};

/// Shades an image's material pixels (RGBA, straight alpha) by `colours`, as the game's material
/// run does: each pixel's material byte 0 leaves it, byte n (from 1) picks colour n - 1, which
/// multiplies its grey: grey * colour >> 8. (SpriteCache's CPU tints, the decoders' and the map
/// editor's thumbnails all shade through this.)
void shadeMaterials(std::uint8_t* rgba, const std::uint8_t* material, std::size_t pixels, const MaterialColours& colours);

/// The image (w x h RGBA) beside its material bytes (in red), twice as wide: the material
/// shader's texture layout.
std::vector<std::uint8_t> besideMaterial(const std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material,
                                         int w, int h);

/// Makes a job's result from the pack image (nullopt: not in the pack). Any thread: the same
/// work as a worker's, for the drawing thread's own decodes (no SD original to stand in).
DecodeResult buildDecodeResult(const DecodeJob& job, std::optional<nox::video::PackImage> image);

class ImageDecoder {
public:
    /// Starts `threads` workers on the pack at `pack` (each opens its own reader).
    ImageDecoder(std::filesystem::path pack, int threads);
    ~ImageDecoder();
    ImageDecoder(const ImageDecoder&) = delete;
    ImageDecoder& operator=(const ImageDecoder&) = delete;

    /// The workers for this machine: half its cores, 1 to 4 (the drawing and the hosted game's
    /// threads keep the rest).
    static int defaultThreads();

    /// Asked for while drawing: before everything queued earlier. Asking again for one waiting
    /// moves it to the front and keeps it from going stale; one being decoded, or done and not yet
    /// taken, is left as it is.
    void request(const DecodeJob& job, std::uint64_t nowMs);
    /// The map's images, decoded in this order once nothing asked for is waiting (replaces the
    /// last map's list).
    void preload(std::vector<DecodeJob> jobs);
    /// A preload job no longer needed (its image came another way).
    void cancelPreload(const DecodeJob::Key& key);
    /// Every request dropped (a new map: what's wanted is asked again as it's drawn).
    void dropRequests();
    /// Requests not asked for in `maxIdleMs` dropped (a map image asked for goes back on the
    /// preload list instead).
    void dropStale(std::uint64_t nowMs, std::uint64_t maxIdleMs);
    /// The oldest finished decode, if any: those asked for while drawing before the preload's.
    bool take(DecodeResult& out);
    /// The same, waiting up to `maxMs` for one to finish (false: none came).
    bool takeWait(DecodeResult& out, std::uint64_t maxMs);

private:
    struct Want {
        DecodeJob job;
        std::uint64_t seq = 0;
        std::uint64_t askedMs = 0;
        bool mapImage = false; ///< it was on the preload list
    };
    void run();

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_; ///< a decode done (takeWait)
    std::map<DecodeJob::Key, Want> wanted_;
    std::map<std::uint64_t, DecodeJob::Key> order_; ///< wanted_ by seq: the last asked last
    std::deque<DecodeJob> preload_;
    std::set<DecodeJob::Key> preloadKeys_; ///< preload_'s still wanted
    std::set<DecodeJob::Key> busy_;        ///< being decoded, or finished and not taken
    std::deque<DecodeResult> done_;        ///< finished: asked for while drawing (taken first)
    std::deque<DecodeResult> donePreload_; ///< finished: the map's preload
    std::size_t doneBytes_ = 0; ///< in done_ and donePreload_
    std::uint64_t seq_ = 0;
    std::atomic<int> waiting_{0}; ///< frames in takeWait (the workers' priority goes by it)
    bool stop_ = false;
    std::vector<std::thread> workers_;
};

} // namespace noxsdl
