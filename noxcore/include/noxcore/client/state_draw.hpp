// The DRAW functions that pick their image from the drawable's own state rather than from its
// payload's kind alone (things/draw.hpp): BoulderDraw, from how far and which way it has moved
// while drawn; MonsterGeneratorDraw, from the generator's xStatus. And GlyphDraw, which picks its
// image as AnimateDraw does but its alpha from how far it is from us. The renderers resolve the draw
// function once per type and keep the per-drawable part.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace nox::client {

/// BoulderDraw's marks on its drawable (zero when it's made): where it was when its roll last
/// stepped (both 0: never drawn yet), and the roll, image `set + frame` of its 32: frame 0-15
/// within the set of 16 that starts at `set` (0 or 16).
struct BoulderRoll {
    std::int32_t lastX = 0;
    std::int32_t lastY = 0;
    std::uint32_t frame = 0;
    std::uint32_t set = 0;

    constexpr std::uint32_t image() const noexcept { return set + frame; }
};

/// One BoulderDraw of a drawable at (x, y), as each draw of it steps it; returns the image it
/// draws. Drawn the first time, it notes where it is. Once it is 10 units or more from where its
/// roll last stepped, the roll steps a frame: forward when it moved south (dy > 0), back
/// otherwise (wrapping within the 16), in the second set when it moved north-west or south-east
/// (dx and dy both above 0, or neither), the first otherwise; then that is where it last stepped.
/// Less than 10 units away, nothing changes (so a slow roll still turns, a frame per 10 units).
constexpr std::uint32_t boulderDraw(BoulderRoll& roll, std::int32_t x, std::int32_t y) noexcept
{
    if (roll.lastX == 0 && roll.lastY == 0) {
        roll.lastX = x;
        roll.lastY = y;
    }
    const std::int32_t dx = x - roll.lastX;
    const std::int32_t dy = y - roll.lastY;
    if (dy * dy + dx * dx >= 100) {
        roll.set = (dx < 1) == (dy < 1) ? 16u : 0u;
        if (dy >= 1) {
            roll.frame = roll.frame + 1 > 15 ? 0 : roll.frame + 1;
        } else {
            roll.frame = roll.frame == 0 ? 15 : roll.frame - 1;
        }
        roll.lastX = x;
        roll.lastY = y;
    }
    return roll.image();
}

/// MonsterGeneratorDraw: which of the generator's five animations it draws for its xStatus:
/// 1 with bit 0x100 (hurt to two thirds), else 2 with 0x200 (to a third), else 3 with 0x400 or
/// 0x800, else 0.
constexpr std::size_t generatorAnimation(std::uint32_t xStatus) noexcept
{
    if (xStatus & 0x100u) {
        return 1;
    }
    if (xStatus & 0x200u) {
        return 2;
    }
    return (xStatus & 0xC00u) ? 3 : 0;
}

/// ...whether it draws its fifth animation over that one (looping by net code and game frame):
/// only while neither 0x400 nor 0x800 is set...
constexpr bool generatorOverlay(std::uint32_t xStatus) noexcept { return (xStatus & 0xC00u) == 0; }

/// ...and with 0x800 it holds that animation's last frame.
constexpr bool generatorHoldsLast(std::uint32_t xStatus) noexcept { return (xStatus & 0x800u) != 0; }

/// What GlyphDraw (a trap glyph's DRAW) looks at: game flag 2 (the game has a client of its own;
/// the game sets it as it starts and on each map load and never clears it, so it is set wherever
/// a client draws), whether we have our own player's drawable, the glyph drawable's flag
/// 0x40000000 (a drawable of the GUI's: the inventory's and shop's pictures, never one in the
/// world), whether our drawable has INFRAVISION (enchantment 21), and the glyph's offset from
/// our drawable (both the drawables' positions as the server sent them, not the smoothed ones).
struct GlyphView {
    bool clientFlag = true;
    bool ownDrawable = false;
    bool gui = false;
    bool infravision = false;
    std::int32_t dx = 0;
    std::int32_t dy = 0;
};

/// GlyphDraw: the alpha it draws the glyph's animation at (through AnimateDraw and the generic
/// drawable draw), or nothing where it draws nothing. With game flag 2 and our drawable: a GUI
/// drawable at 255; else under INFRAVISION at 255 (128 at a colour depth under 16 bits: never
/// ours); else only within 150 units of us (dx^2 + dy^2 < 22500), at 200 - 200 (dx^2 + dy^2) /
/// 22500 in whole numbers, the quotient truncated (200 on us, 1 at the edge). Without the flag or our drawable the
/// game takes the low byte of the drawable's address for the alpha (original_bugs.md OB-346;
/// it can't happen there: it draws no world without our drawable); ours draws it at 255.
/// The colour GlyphDraw sets under INFRAVISION (grey) is overwritten by the generic draw's own
/// lighting before anything is drawn, so it changes nothing. A lingering glyph (Fade Objects)
/// takes the fade's alpha instead, as any drawable the generic draw fades.
constexpr std::optional<std::uint8_t> glyphAlpha(const GlyphView& v) noexcept
{
    if (!v.clientFlag || !v.ownDrawable || v.gui || v.infravision) {
        return std::uint8_t{255};
    }
    const auto ux = static_cast<std::uint32_t>(v.dx);
    const auto uy = static_cast<std::uint32_t>(v.dy);
    const auto squared = static_cast<std::int32_t>(uy * uy + ux * ux); // (the game's 32-bit integers)
    if (squared >= 22500) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(200 - squared * 200 / 22500);
}

} // namespace nox::client
