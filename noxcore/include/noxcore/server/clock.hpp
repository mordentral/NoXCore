// The server's clock: the game's 30 Hz frames, each split into `multiplier` substeps (1-4: 30,
// 60, 90 or 120 Hz).
//
// A frame's first substep is the classic frame, whole: everything the game decides (scripts and
// their timers, AI, cooldowns, damage, random rolls) and every object's motion, as GAME.EXE
// does, so the state at each frame, and what a classic client is sent of it, is the stock
// server's, with no added delay. The substeps after it carry the important objects (characters,
// spells, missiles) on by k/multiplier of the frame's motion, for a client that asked for the
// rate (noxsdl); the next frame goes on from the classic position. Low-priority objects
// (elevators, effects, sounds) only change on the frame.
#pragma once

#include <cstdint>

namespace nox::server {

constexpr int kFrameRate = 30;
constexpr int kMaxMultiplier = 4;

class SimClock {
public:
    explicit SimClock(int multiplier = 1) noexcept;

    int multiplier() const noexcept { return multiplier_; }
    /// Takes effect at the next frame (a frame keeps the substeps it started with).
    void setMultiplier(int multiplier) noexcept;

    /// Frames begun so far (the exe's gameFrame): 0 before the first.
    std::uint32_t frame() const noexcept { return frame_; }
    /// The substep about to run within the current frame, 0..multiplier-1.
    int substep() const noexcept { return substep_; }
    /// True when the next advance() begins a frame.
    bool atFrameStart() const noexcept { return substep_ == 0; }

    /// Moves on a substep (beginning a frame when it's the first); returns the substep that ran.
    /// With `countFrame` false a frame begun isn't counted (a solo game paused: the frame stands).
    int advance(bool countFrame = true) noexcept;

    /// Substeps run per second.
    int rate() const noexcept { return kFrameRate * multiplier_; }
    /// Seconds a substep lasts.
    double stepSeconds() const noexcept { return 1.0 / rate(); }

private:
    int multiplier_ = 1;
    int nextMultiplier_ = 1;
    std::uint32_t frame_ = 0;
    int substep_ = 0;
};

/// Where an important object is shown at substep `k` (0-based, of `m`) of a frame: its classic
/// position `start` (substep 0) carried on by k/m of `perFrame`, the motion it's making.
inline float substepPosition(float start, float perFrame, int k, int m) noexcept
{
    return k <= 0 ? start : start + perFrame * static_cast<float>(k) / static_cast<float>(m);
}

} // namespace nox::server
