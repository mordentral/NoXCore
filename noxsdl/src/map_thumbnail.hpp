// Maps drawn whole from above, small, to pick between (noxcpp's map vote): the floor with its
// edges, the walls and the objects the client loads itself (ObjectSet::ClientLoaded), fully lit,
// laid out as MapView lays them out. Drawn in software on a worker thread from its own video.bag
// reader, every source pixel averaged into the picture's (a box filter: no shimmer at a twelfth of
// the size), so asking for one costs the game nothing; the finished picture goes up as one texture.
#pragma once

#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace noxsdl {

class MapThumbnails {
public:
    /// Maps are looked for in `gameDir`/maps, then in `mapCache` (downloads); pictures are at most
    /// `maxSide` pixels on their longer side.
    MapThumbnails(std::filesystem::path gameDir, std::filesystem::path mapCache, const nox::things::ThingDb& things,
                  int maxSide = 512);
    ~MapThumbnails();
    MapThumbnails(const MapThumbnails&) = delete;
    MapThumbnails& operator=(const MapThumbnails&) = delete;

    struct Picture {
        SDL_Texture* texture = nullptr; ///< null while it's being drawn, or if it can't be
        bool done = false;              ///< drawn, or given up on (no such map here)
        float worldWidth = 0;           ///< the area it shows, in world units
        float worldHeight = 0;
    };
    /// Map `name`'s picture, asked for the first time it's named. Call from the render thread.
    const Picture& get(SDL_Renderer* renderer, const std::string& name);
    /// Lets the textures go (the vote's over).
    void clear();

    /// The RGBA pixels of map `path` drawn so it fits `maxSide` (straight alpha), and the world
    /// area they show. Empty if the map can't be read. (What the worker runs; tests call it.)
    struct Pixels {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
        float worldWidth = 0;
        float worldHeight = 0;
    };
    static Pixels draw(const std::filesystem::path& path, const nox::things::ThingDb& things,
                       const std::filesystem::path& gameDir, int maxSide);

private:
    void run();
    std::filesystem::path find(const std::string& name) const;

    std::filesystem::path gameDir_;
    std::filesystem::path mapCache_;
    const nox::things::ThingDb& things_;
    int maxSide_;
    std::map<std::string, Picture> pictures_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;         ///< names waiting to be drawn
    std::set<std::string> asked_;           ///< asked for since the last clear(), not yet drawn
    std::map<std::string, Pixels> drawn_;   ///< drawn, waiting to be uploaded
    bool stop_ = false;
    std::thread worker_;
};

} // namespace noxsdl
