// The screen's white flash (MSG_FX_WHITE_FLASH), kept apart from SDL so the tests can run it.
//
// The game's handler only sets a flag; the next frame that draws the world clears the screen to
// white instead, draws nothing else of the world and clears the flag. A frame the world isn't drawn
// (a cinematic's black) leaves the flag for the next one. So a flash is one drawn frame: a game
// frame, 1/30 s, whatever the game's clock does. The level-up's flash comes as the solo game's
// pause begins, and the game's frame stays put for the 5 s pause: a flash timed by the game's
// frame would last all of it.
//
// We draw at the display's rate, so one drawn frame would be 8 ms at 120 Hz: the flash lasts what
// the game's frame lasts instead, about 1/30 s of real time. Every frame begun within 30 ms of the
// first white one is white (one begun later would show mostly past the game's 33 ms): 4 frames at
// 120 Hz, 2 at 60, 1 at 30 or less.
#pragma once

#include <cstdint>
#include <optional>

namespace noxsdl {

class WhiteFlash {
public:
    /// MSG_FX_WHITE_FLASH played: the next frame drawing the world is white.
    void trigger() { pending_ = true; }
    void clear()
    {
        pending_ = false;
        startMs_.reset();
    }
    /// Whether the frame drawn at `nowMs` (the render clock, ms) is white. `worldDrawn`: the world
    /// would be drawn this frame (not a cinematic's black); a frame without it shows no flash and
    /// leaves a pending one for later.
    bool frame(std::uint64_t nowMs, bool worldDrawn)
    {
        if (!worldDrawn) {
            return false;
        }
        if (startMs_ && nowMs >= *startMs_ && nowMs - *startMs_ < kWhiteMs) {
            return true; // (one that came meanwhile waits: the game's next frame would show it)
        }
        startMs_.reset();
        if (!pending_) {
            return false;
        }
        pending_ = false;
        startMs_ = nowMs;
        return true;
    }
    bool pending() const { return pending_; }

private:
    static constexpr std::uint64_t kWhiteMs = 30; ///< frames begun this soon after the first are white
    bool pending_ = false;
    std::optional<std::uint64_t> startMs_; ///< the shown flash's first frame
};

} // namespace noxsdl
