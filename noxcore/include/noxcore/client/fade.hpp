// Fade Objects (nox.cfg FadeObjects, the Advanced Options' Fade Objects checkbox): something
// seen that leaves sight stays drawn for up to a game second, fading out, before it's hidden.
//
// Each drawable keeps three marks and a frame:
//   seen       drawn in sight at least once
//   lingering  out of sight but still drawn, fading
//   told       the server said it went into shadows (MSG_OBJECT_IN_SHADOWS)
//   inSight    the last frame it was drawn neither lingering nor told (its making's at first)
//
// MSG_OBJECT_IN_SHADOWS sets all three marks on the drawable it names. With Fade Objects on
// the drawable is kept (but our own) and goes a game second later unless an update comes first
// (an update clears `told` and the pending expiry); off, it's dropped as before, unless it's ours
// or an animation that removes itself (OneShotRemove), which stay. MSG_OBJECT_OUT_OF_SIGHT
// (creatures and players behind walls, anything leaving the view) always drops it.
//
// The scene's sort runs `sortStep` on each drawable of the general list (not the BELOW things,
// not the wall hangings): with Fade Objects off nothing out of sight is drawn but our own unit.
// Out of sight and not told, one never seen or a creature or player isn't drawn, anything else
// lingers. In sight (not told), it's seen and no longer lingering. A lingering or told one more
// than a game second past its last in-sight frame isn't drawn, and stops lingering.
//
// A lingering drawable's alpha is `lingerAlpha`: 1 - (frames since its last in-sight frame) /
// game FPS, 0.001 below 0, times 255, truncated (the FPU's chop mode). Only the generic drawable
// draw sets it (`drawFades`: the DRAW functions that go through it); the others (sparks,
// bolts, magic missiles, plates) draw a lingering drawable as they always do, at full alpha.
// The Ghost's own alpha (`ghostAlpha`) is the smaller of the two while it lingers.
//
// The map's own objects (the ones the client loads itself) are drawables too: they linger and
// fade by the same rule. So are the effects the client makes itself (sparks, glow orbs, bubbles,
// tail links, beams, explosions...): their making is their first in-sight frame (`madeAt`).
// What a drawable's DRAW draws round it (name plates, health bars, team names, friend dots) goes
// with it: drawn exactly when the sort draws the drawable.
//
// SIGHT_DESTROY (thing FLAGS, or set by the client on the bubbles, vortex orbs, toxic puffs and
// Turn Undead's orbs it makes): after each frame's draw, such a drawable whose last in-sight
// frame is before that frame is deleted. So it never lingers past the one frame after it leaves
// sight (drawn that frame only with Fade Objects), and one made out of sight goes after the next.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace nox::client {

/// The game's frame rate (gameFPS): how long a drawable lingers, in frames.
inline constexpr std::uint32_t kGameFps = 30;

struct FadeState {
    std::uint32_t inSightFrame = 0; ///< the last frame drawn neither lingering nor told
    std::uint32_t expireAt = 0;     ///< told with Fade Objects on: the frame it goes (0: none pending)
    bool seen = false;
    bool lingering = false;
    bool told = false;
};

/// What the scene's sort knows of a drawable this frame.
struct SortInput {
    bool fadeObjects = false;
    bool self = false;             ///< our own unit: always in sight
    bool inSight = false;          ///< the drawable sight test
    bool creatureOrPlayer = false; ///< CLASS MONSTER or PLAYER: never lingers of itself
    std::uint32_t frame = 0;       ///< the game frame drawn
    std::uint32_t fps = kGameFps;
};

/// The scene's sort for a drawable of the general list: whether it's drawn this frame (updating
/// its marks). Frames are compared as the drawn frame less the in-sight one, none when the drawn
/// frame isn't past it (a renderer's shown frame can trail the one a drawable was made in).
constexpr bool sortStep(FadeState& s, const SortInput& in) noexcept
{
    const bool inSight = in.self || in.inSight;
    if (!in.fadeObjects && !inSight) {
        return false;
    }
    if (!s.told) {
        if (!inSight) {
            if (!s.seen || in.creatureOrPlayer) {
                return false;
            }
            s.lingering = true;
        } else {
            s.seen = true;
            s.lingering = false;
        }
    }
    if ((s.lingering || s.told) && in.frame > s.inSightFrame && in.frame - s.inSightFrame > in.fps) {
        s.lingering = false;
        return false;
    }
    return true;
}

/// A drawable was drawn at `frame`: neither lingering nor told, that's its last in-sight frame.
constexpr void markDrawn(FadeState& s, std::uint32_t frame) noexcept
{
    if (!s.lingering && !s.told) {
        s.inSightFrame = frame;
    }
}

/// The marks of a drawable made at `frame`: that's its last in-sight frame until it's drawn.
constexpr FadeState madeAt(std::uint32_t frame) noexcept
{
    FadeState s;
    s.inSightFrame = frame;
    return s;
}

/// The frames the scene's sort ran for, as a renderer drawing several times a game frame (or
/// none, in another view) sees them: the sort for a frame is over once a later one has begun.
struct SortedFrames {
    std::uint32_t last = 0;   ///< the latest frame sorted
    std::uint32_t before = 0; ///< the one sorted before it
    /// The sort runs for `frame` (any number of times).
    constexpr void sorted(std::uint32_t frame) noexcept
    {
        if (frame != last) {
            before = last;
            last = frame;
        }
    }
    /// The latest frame whose sort is over while `frame` is drawn (0: none).
    constexpr std::uint32_t completed(std::uint32_t frame) const noexcept
    {
        return last < frame ? last : last == frame ? before : 0;
    }
};

/// Whether a SIGHT_DESTROY drawable is deleted after the draw of `completedFrame`: it wasn't
/// drawn in sight that frame (nor made then).
constexpr bool sightDestroyed(const FadeState& s, std::uint32_t completedFrame) noexcept
{
    return s.inSightFrame < completedFrame;
}

/// A lingering drawable's alpha, `elapsed` frames (a fraction for a frame drawn between game
/// frames) past its last in-sight frame: 255 at once, down to 0 a game second on (none before
/// it: a shown frame trailing the in-sight one counts as 0).
constexpr std::uint8_t lingerAlpha(double elapsed, std::uint32_t fps = kGameFps) noexcept
{
    double t = 1.0 - (elapsed > 0 ? elapsed : 0.0) / static_cast<double>(fps);
    if (t < 0) {
        t = static_cast<double>(0.001f);
    }
    return static_cast<std::uint8_t>(static_cast<std::int64_t>(t * 255.0));
}

/// Whether a DRAW function draws its image through the generic drawable draw, the one that fades
/// a lingering drawable (and gives the Ghost its alpha): directly, or through StaticDraw,
/// AnimateDraw, SlaveDraw or MonsterDraw's frame draw. Asked once per type, as its art loads.
constexpr bool drawFades(std::string_view drawName) noexcept
{
    constexpr std::array<std::string_view, 27> kGeneric = {
        "StaticDraw",       "StaticRandomDraw",  "DoorDraw",          "AnimateDraw",       "ConditionalAnimateDraw",
        "MonsterGeneratorDraw", "VectorAnimateDraw", "MonsterDraw",   "MaidenDraw",        "AnimateStateDraw",
        "PlayerDraw",       "SlaveDraw",         "GlyphDraw",         "BoulderDraw",       "WeaponDraw",
        "ArmorDraw",        "WeaponAnimateDraw", "ArmorAnimateDraw",  "FlagDraw",          "BaseDraw",
        "NPCDraw",          "SphericalShieldDraw", "SummonEffectDraw", "ReleasedSoulDraw", "ArrowDraw",
        "WeakArrowDraw",    "HarpoonDraw"};
    for (const auto name : kGeneric) {
        if (name == drawName) {
            return true;
        }
    }
    return false;
}

/// The Ghost's own alpha in the generic drawable draw, by its distance (dx, dy) from the view's
/// centre: 0x80 less 0x80 for each 40000 of the squared distance, kept within 0 - 0x80 (in the
/// game's 32-bit integers). Seeing the invisible (enchantment 21), it's 255 instead: the caller's.
constexpr std::uint8_t ghostAlpha(std::int32_t dx, std::int32_t dy) noexcept
{
    const auto squared = static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(dy) * static_cast<std::uint32_t>(dy) + static_cast<std::uint32_t>(dx) * static_cast<std::uint32_t>(dx)) *
        0x80u);
    const std::int32_t a = 0x80 - squared / 40000;
    return static_cast<std::uint8_t>(a < 0 ? 0 : a > 0x80 ? 0x80 : a);
}

} // namespace nox::client
