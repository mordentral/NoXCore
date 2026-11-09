// Cinematics' order and its watch on a lasting black (cinematics.hpp), the parts that need no SDL.
//
//   ScreenQueue  the messages Cinematics takes (MSG_FADE_BEGIN, MSG_CHAPTER_END, the quest's
//                screens) and a mode's calls on the host's client (a co-op guest's relayed
//                cinematics), kept in the order they came and handled together at the next update,
//                so a fade and a call that blacks the screen apply in the order the server sent
//                them (issue #14).
//   BlackWatch   (noxcpp) a black screen nothing will end: in a game, the map not drawn with nothing
//                under way that ends or explains it (no fade, briefing, screen or widescreen bars;
//                no movie, nor the map held for its HD images) for kStuckMs of the client's running
//                time. Then Escape may open the in-game menu; and a black the game's own scripts
//                didn't put up (a map's load, a fade to black, a relayed call's) is ended: the
//                fades stopped and the map drawn.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace noxsdl {

class ScreenQueue {
public:
    /// A message (its bytes as Cinematics keeps them), or a call.
    void push(std::vector<std::uint8_t> message) { entries_.push_back({std::move(message), {}}); }
    void push(std::function<void()> call) { entries_.push_back({{}, std::move(call)}); }
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    /// Each in turn, oldest first (one pushed meanwhile too): a call made, a message given to
    /// `handle`.
    template <class Handle>
    void drain(Handle&& handle)
    {
        while (!entries_.empty()) {
            Entry e = std::move(entries_.front());
            entries_.pop_front();
            if (e.call) {
                e.call();
            } else {
                handle(e.message);
            }
        }
    }

private:
    struct Entry {
        std::vector<std::uint8_t> message;
        std::function<void()> call;
    };
    std::deque<Entry> entries_;
};

class BlackWatch {
public:
    /// How long a quiet black lasts before it's taken as stuck (the client's running time).
    static constexpr std::uint64_t kStuckMs = 5000;
    /// The most one update counts (a stall, or the main thread parked for our host's save or
    /// load, isn't the black's time).
    static constexpr std::uint64_t kStepMostMs = 250;

    struct State {
        bool inGame = false;   ///< the world view up, and not held (a movie, the map's HD hold)
        bool mapDrawn = true;  ///< gameDisableMapDraw off
        bool underWay = false; ///< a fade, the widescreen or its bars, a briefing, a screen up
        /// The black is the game's scripts' own (Blind, ImmediateBlind, a chapter screen's): never
        /// ended here, however long (Escape still opens the menu once it's stuck).
        bool scripted = false;
    };

    /// Each update. True (once) when the black has been stuck kStuckMs and isn't scripted: the
    /// caller ends it. Any update that isn't a quiet black starts the count again.
    bool update(std::uint64_t nowMs, const State& s)
    {
        const bool quiet = s.inGame && !s.mapDrawn && !s.underWay;
        const std::uint64_t step = last_ != 0 && nowMs > last_ ? std::min(nowMs - last_, kStepMostMs) : 0;
        last_ = nowMs;
        if (!quiet) {
            quietMs_ = 0;
            fired_ = false;
            return false;
        }
        quietMs_ += step;
        if (quietMs_ < kStuckMs || s.scripted || fired_) {
            return false;
        }
        fired_ = true;
        return true;
    }
    /// The quiet black has lasted kStuckMs (as of the last update).
    bool stuck() const { return quietMs_ >= kStuckMs; }
    /// How long the quiet black has lasted (0: none), as of the last update.
    std::uint64_t quietMs() const { return quietMs_; }

private:
    std::uint64_t last_ = 0;
    std::uint64_t quietMs_ = 0;
    bool fired_ = false;
};

} // namespace noxsdl
