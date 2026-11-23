// The game's drawing while a solo game we host stands paused (the level-up's 5 s), kept apart from
// SDL so the tests can run it.
//
// The game's client draws on through the pause. Its client update list waits, but every drawable
// drawn still runs its DRAW function, and some of those move what they draw: LevelUpDraw (and
// OblivionUpDraw) drops two rain orbs a drawn frame, RainOrbDraw lets each fall and turns it into a
// move orb on the ground, and GlowOrbMoveDraw flies that out from the player and shrinks it. So the
// orb rain falls all through the level-up's pause, over a world that stands still.
//
// Our effects step by the game's frames, and a paused game counts none: the rain stood at the top
// of the view for the whole pause and was gone with it. So while paused, those draws run by this
// clock instead, at the game's 30 frames a second of real time.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace noxsdl {

class PausedDrawClock {
public:
    /// The game's drawn frames due at `nowMs` (the render clock, ms) while `paused`: none when the
    /// pause has just begun, then one every 1/30 s. Not paused: none, and the clock starts afresh
    /// at the next pause.
    int frames(std::uint64_t nowMs, bool paused)
    {
        if (!paused) {
            startMs_.reset();
            return 0;
        }
        if (!startMs_ || nowMs < *startMs_) {
            startMs_ = nowMs;
            done_ = 0;
            return 0;
        }
        const std::uint64_t due = (nowMs - *startMs_) * kPerSecond / 1000;
        const std::uint64_t n = due - done_;
        done_ = due;
        // (a stalled frame catches up a little, not with a burst)
        return static_cast<int>(std::min(n, kMostAtOnce));
    }

private:
    static constexpr std::uint64_t kPerSecond = 30; ///< the game's frame rate
    static constexpr std::uint64_t kMostAtOnce = 3;
    std::optional<std::uint64_t> startMs_; ///< when the pause was first seen
    std::uint64_t done_ = 0;               ///< frames run since then
};

} // namespace noxsdl
