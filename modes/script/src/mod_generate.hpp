// nox.generate: a mod's maps, built by its own generator scripts
// (gen/*.luau, in a GenVm of their own: the gen library and the map writer, no toolbox) or by the
// engine's arena builder with the mod's parameters, ahead on a worker or at once, written as
// <mapsDir>/<name>/<name>.map and .nxz, the mod's onGenerated(tag, ok, result) told on the server's
// thread. A map's name is <mapPrefix>_<slot><TAG> (mode.json's mapPrefix; 8 letters at most, the
// game's map names'). A map change to one still building waits for it (the host's prepareMap).
// (Internal to the script modes' sources.)
#pragma once

#include "noxcore/mapgen/arena.hpp"
#include "noxcore/modes/script/gen_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace nox::things {
struct ThingDb;
}

namespace nox::modes::script {

class Generation {
public:
    /// Builds running at once, a game (more wait their turn).
    static constexpr std::size_t kRunning = 2;
    /// Builds waiting or running, a game.
    static constexpr std::size_t kMaxQueued = 8;
    /// A mod's generator's guards.
    static GenVm::Limits limits() { return {.memoryBytes = 128u << 20, .time = std::chrono::milliseconds(20'000)}; }

    /// What a build makes.
    struct Request {
        std::string tag;   ///< "a1"
        std::string name;  ///< the map's ("DD_0A1")
        bool arena = false;
        mapgen::ArenaParams arenaParams;     ///< (arena)
        std::string module, fn;              ///< (a mod's generator)
        std::vector<std::uint8_t> params;    ///< (a mod's generator) its params, value_codec's bytes
        std::uint32_t seed = 1;
        /// (a mod's generator) the map's own MapInfo: title, author, game types, players.
        std::string title, author;
        std::uint32_t gameFlags = 4;
        std::uint8_t minPlayers = 1, maxPlayers = 6;
    };
    /// What a build made.
    struct Built {
        bool ok = false;
        std::string error;
        mapgen::EncodedMap files;
        std::vector<std::uint8_t> result;   ///< (a mod's generator) what it returned, value_codec's bytes
        std::optional<mapgen::ArenaSpots> spots; ///< (arena)
    };

    /// Set as the game is configured: the mod's prefix (empty: none, nox.generate refuses), its
    /// slot, the maps' folder and thing.bin.
    void setUp(std::string prefix, int slot, std::filesystem::path mapsDir, const things::ThingDb* things);
    /// Its own generator modules (each load's; none: it has none). Builds under way keep theirs.
    void setModules(std::shared_ptr<const GenModules> modules) { modules_ = std::move(modules); }
    bool enabled() const noexcept { return !prefix_.empty(); }
    /// Nothing building and nothing to tell (a frame's poll has nothing to do).
    bool idle() const noexcept { return jobs_.empty() && told_.empty(); }
    /// This game's maps (<prefix>_<slot>...) deleted from the maps' folder; how many went.
    int sweepOwn() const;
    /// A tag's map name; none: not a tag (1-3 of a-z and 0-9, a letter first: where the slot's
    /// digits end), or the name past 8 letters.
    std::optional<std::string> nameOf(std::string_view tag) const;
    /// Whether `name` (any case) is one of this game's maps.
    bool ownsName(std::string_view name) const;

    /// The request table at `idx` read (an error for a bad one; L is the mode's thread).
    Request requestArg(lua_State* L, int idx) const;
    /// Built now, on this thread.
    Built buildNow(const Request& r) const;
    /// Built now and written (its outcome kept for nox.generating), not told: the caller has it.
    Built runNow(const Request& r, std::string_view currentMap);
    /// What onGenerated's third argument is: the generator's result (or the arena builder's
    /// spots), or the error's text.
    static void pushResult(lua_State* L, const Built& b);
    /// Queued for a worker; false: why (`error`).
    bool start(Request r, std::string& error);
    /// "building", "ready", "failed" or none, for a tag.
    std::optional<std::string_view> state(std::string_view tag) const;
    /// A tag's build dropped (its result never told); whether there was one.
    bool cancel(std::string_view tag);
    /// The finished builds written and told (onGenerated), more started; `currentMap` the map
    /// being played (never written over).
    void poll(lua_State* L, std::string_view currentMap);
    /// A map change to `name`: its build waited for and written now (prepareMap's), its
    /// onGenerated told at the next poll.
    void waitFor(std::string_view name, std::string_view currentMap);
    /// The builds let go (waited for at the program's end: finishBackground), the results dropped.
    void release();
    /// Every build waited for and dropped (the game's end).
    void stop();
    /// A finished build's result kept to be told at the next poll (configure's first map).
    void told(std::string tag, Built built);

    /// The builds let go by games that ended (waited for at the program's end).
    static void finishReleased();

private:
    struct Job {
        Request request;
        std::future<Built> future;
        bool running = false;
        bool done = false;
        Built built;
    };
    void startWaiting();
    /// A done job's files written (an error kept in its result when they can't be).
    void write(Job& job, std::string_view currentMap);

    std::string prefix_;
    int slot_ = 0;
    std::filesystem::path mapsDir_;
    const things::ThingDb* things_ = nullptr;
    std::shared_ptr<const GenModules> modules_;
    std::vector<Job> jobs_; ///< in the order they were asked for
    std::vector<std::pair<std::string, Built>> told_; ///< (results to tell at the next poll)
    /// Each tag's last outcome for nox.generating: true ready, false failed.
    std::vector<std::pair<std::string, bool>> outcomes_;
};

/// mode.json's mapPrefix: empty: fine; else why not (2-3 of A-Z).
std::string mapPrefixProblem(std::string_view prefix);

} // namespace nox::modes::script
