// (noxcpp) What the game controller keeps from frame to frame in a game (App::padPlay): the aim
// (pad_aim.hpp), what the frame's drawables gave it, the pad's pointer and the free cursor. The
// functions that use it are pad_play.hpp's.
#pragma once

#include "pad/pad_aim.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace noxsdl::pad {

struct PlayState {
    Targeter targeter;
    RightStick right;
    /// liveSprites gathers the frame's seen creatures and takeable things near us (the pad aims),
    /// measured from `from` (our spot as the server last put us).
    bool collecting = false;
    WorldPoint from;
    std::vector<AimCandidate> enemies; ///< the enemies in sight, last frame's drawables
    struct Near {
        std::uint16_t netCode = 0;
        WorldPoint at;
    };
    std::vector<Near> near; ///< seen drawables near us a hand may take (items, NPCs, useable things)

    /// The pad's pointer (mousePos while the pad is in use), in the game's output pixels.
    bool hasPointer = false;
    float pointerX = 0;
    float pointerY = 0;

    /// The free cursor (FreeCursor): on, its place (output pixels), how long it's been pushed
    /// hard, and its buttons held (bit 0 left, bit 1 right).
    bool freeCursor = false;
    float freeX = 0;
    float freeY = 0;
    float freeHeld = 0;
    std::uint8_t freeButtons = 0;

    /// This frame's aim: the target (and where it is), the reticle, the aim point, the way.
    std::optional<std::uint16_t> target;
    std::optional<WorldPoint> targetAt;
    std::optional<WorldPoint> reticle;
    std::optional<WorldPoint> aimAt;
    StickVec dir{1, 0};
    int pendingStep = 0; ///< TargetNext / Previous pressed since the last frame

    /// A cast's aim held through its chant (the server takes the cursor when it goes off).
    std::uint64_t castUntilMs = 0;
    std::optional<WorldPoint> castAt;
    std::optional<std::uint16_t> castTarget;

    /// What A takes now: a drawable or a useable map object, with the hand it gets.
    struct Interact {
        std::uint8_t cursor = 0; ///< CursorType's number
        std::optional<std::uint16_t> drawable;
        std::uint32_t extent = 0; ///< a useable map object's (0: a drawable)
        WorldPoint at;
        float radius = 6;
    };
    std::optional<Interact> interact;
    /// A pile's things A took lately (left out of the next pick for a moment).
    std::array<std::uint32_t, 8> tried{};
    std::size_t triedCount = 0;
    std::uint64_t triedUntilMs = 0;

    /// Everything aimed or held forgotten (the pad put down, a game left); the free cursor kept.
    void dropAim()
    {
        targeter.clear();
        right.clear();
        target.reset();
        targetAt.reset();
        reticle.reset();
        aimAt.reset();
        pendingStep = 0;
        castUntilMs = 0;
        castAt.reset();
        castTarget.reset();
        interact.reset();
        hasPointer = false;
    }
};

} // namespace noxsdl::pad
