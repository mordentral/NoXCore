// nox.after, nox.every and nox.cancelTimer: a script's functions
// run on later game frames, after the frame's queued events and before tick(frame), by their due
// frame and then the order they were made. Each run is its own protected call on a fresh budget;
// an error is reported and an `every` keeps its beat. A reload drops them all (their functions are
// the old code's). (Internal to the script modes' sources.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace nox::modes::script {

class Scheduler {
public:
    static constexpr std::size_t kMaxPending = 4096;  ///< timers waiting at once, a game
    static constexpr int kRunsPerFrame = 256;          ///< runs a frame (the rest the next, in order)
    static constexpr std::uint32_t kMaxFrames = 30u * 3600u * 24u; ///< a day of frames

    /// The toolbox's: (frames, fn, ...) from the stack; its id pushed. `now`: the frame it counts
    /// from.
    int add(lua_State* L, std::uint32_t now, bool repeat);
    /// A pending timer dropped; whether it was pending.
    bool cancel(lua_State* L, std::uint32_t id);
    /// The timers due by `frame` run (at most kRunsPerFrame).
    void run(lua_State* L, std::uint32_t frame);
    /// Every timer dropped (a reload's, the game's end).
    void clear(lua_State* L);
    std::size_t pending() const noexcept { return live_.size(); }

private:
    struct Due {
        std::uint32_t frame;
        std::uint64_t order; ///< (made first, run first)
        std::uint32_t id;
        bool operator>(const Due& o) const { return frame != o.frame ? frame > o.frame : order > o.order; }
    };
    struct Timer {
        int ref = 0;            ///< {fn, args...} (a table the registry holds)
        int nargs = 0;
        std::uint32_t every = 0; ///< 0: once
        std::uint32_t at = 0;    ///< its due frame (an entry of due_ for another is stale)
        std::uint64_t order = 0;
    };
    /// The heap again from the live timers alone (cancelled ones' entries dropped).
    void compact();

    std::priority_queue<Due, std::vector<Due>, std::greater<>> due_;
    std::unordered_map<std::uint32_t, Timer> live_; ///< (by id: a cancelled one isn't here)
    std::uint32_t nextId_ = 1;
    std::uint64_t nextOrder_ = 1;
};

} // namespace nox::modes::script
