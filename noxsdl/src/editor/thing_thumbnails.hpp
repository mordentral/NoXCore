// The content browser's thumbnails: each thing type's picture, drawn small on a worker thread from
// its own video.bag reader (or the HD pack's, when the game draws from it), as the map vote's
// thumbnails are (map_thumbnail.hpp), and kept in a disk cache in the user's editor folder so later
// starts have them at once.
//
// The picture is the type's still (its first frame, facing south: a creature's idle), or its
// menu icon for what has none and for weapons and armour. A type the game draws in colours is
// drawn in the colours the game gives a new one (its `looks` entry: the fresh record's, made by
// the caller with ObjectFactory and player_art.hpp's recordLook): an NPC as NPCDraw stacks
// NewPlayer's layers in its six colours, a maiden's still in hers, a weapon's or armour's icon in
// its definition's and modifiers' colours (as the inventory draws it), each material pixel shaded
// as SpriteCache's CPU tint shades it (shadeMaterials). It is box-filtered into a kSize square,
// centred. Finished pictures go up into atlas pages (kPage square textures of kSize cells) on the
// render thread, a few a frame. The worker draws what the browser shows first, then every
// placeable type. The disk cache keys each picture by its type and its look's hash, so a type
// whose fresh colours change is drawn again.
#pragma once

#include "player_art.hpp"

#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace noxsdl::editor {

class ThumbSource; // (the worker's own image readers: thing_thumbnails.cpp)

class ThingThumbnails {
public:
    static constexpr int kSize = 64; ///< pixels a side
    static constexpr int kPage = 1024;
    static constexpr std::uint32_t kCacheVersion = 2; // (2: each picture keyed by its look's hash too)

    /// `imagePack`: the HD pack to draw from (empty: video.bag). `cacheDir`: where the disk cache
    /// is read from and written to (empty: none written). `looks`: by thing index, the colours the
    /// game draws a new one of the type in (see above; `modifiers` must be given with them).
    ThingThumbnails(const nox::things::ThingDb& things, std::filesystem::path gameDir, std::filesystem::path imagePack,
                    std::filesystem::path cacheDir, const nox::things::ModifierDb* modifiers = nullptr,
                    std::map<std::size_t, RecordLook> looks = {});
    ~ThingThumbnails();
    ThingThumbnails(const ThingThumbnails&) = delete;
    ThingThumbnails& operator=(const ThingThumbnails&) = delete;

    /// Where a type's picture is: its atlas page and the uv rectangle. No texture while it's on
    /// its way (asked for now, first in line); `none` when the type has no picture.
    struct Cell {
        SDL_Texture* texture = nullptr;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        bool none = false;
    };
    Cell get(std::size_t thing);
    /// Uploads finished pictures (at most `budget` a call). Call once a frame on the render thread.
    void pump(SDL_Renderer* renderer, int budget = 48);
    /// Pictures still to draw (the status bar's "thumbnails...").
    std::size_t pending();

    /// One type's picture: kSize x kSize RGBA (straight alpha), empty if it has none.
    struct Pixels {
        std::vector<std::uint8_t> rgba;
    };
    /// What the worker draws (static so tests could call it with their own readers). `look`: the
    /// colours to draw it in (null: as it is; `modifiers` given with it).
    static Pixels draw(const nox::things::ThingDb& things, std::size_t thing, ThumbSource& source, const RecordLook* look = nullptr,
                       const nox::things::ModifierDb* modifiers = nullptr);

private:
    void run();
    std::uint64_t cacheKey() const;
    std::filesystem::path cacheFile() const;
    void loadCache();
    void saveCache();
    /// The look's hash the picture of `thing` is drawn for (0: as it is).
    std::uint32_t lookHash(std::size_t thing) const;

    const nox::things::ThingDb& things_;
    std::filesystem::path gameDir_;
    std::filesystem::path imagePack_;
    std::filesystem::path cacheDir_;
    const nox::things::ModifierDb* modifiers_ = nullptr;
    std::map<std::size_t, RecordLook> looks_; ///< (read-only once made: the worker reads it too)

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::size_t> queue_;            ///< things waiting to be drawn (front first)
    std::set<std::size_t> queued_;
    std::map<std::size_t, Pixels> drawn_;      ///< drawn (or read from the cache), waiting to go up
    std::map<std::size_t, Pixels> all_;        ///< every picture made or read (the cache's content)
    bool dirty_ = false;                       ///< all_ has pictures the disk cache hasn't
    bool stop_ = false;
    bool loaded_ = false;

    // Render thread only.
    struct Slot {
        int page = -1;
        int index = 0;
        bool none = false;
    };
    std::map<std::size_t, Slot> slots_;
    std::vector<SDL_Texture*> pages_;
    int used_ = 0; ///< cells handed out
    std::thread worker_;
};

} // namespace noxsdl::editor
