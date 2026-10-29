// The Reflective Shield's water disc (HD Spell FX, fxdraw::ripple) by the unit's facing: the shape
// of the game's ReflectiveShield* art for that facing, measured from its images. The art is one
// round disc of water standing in front of the unit, seen from the front (S, SE, SW: rings of
// ripples), from behind (N, NE, NW: water seen through) or edge on (E, W: a sliver). The disc is
// drawn about the middle of the facing's first image, where the game draws that image (Effects'
// enchantSprites), so it follows the shield's place, facing and life as the sprite does.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace noxsdl::shieldfx {

struct RippleShape {
    float major = 0;       ///< the ellipse's long axis, world units across
    float minor = 0;       ///< its short axis
    float tiltDegrees = 0; ///< the long axis off upright, its top to the right
    bool back = false;     ///< seen from behind
};

/// The disc for AnimDir `dir` (0 NW, 1 N, 2 NE, 3 W, 5 E, 6 SW, 7 S, 8 SE); none for 4 (no facing:
/// no shield there either) or past 8.
inline std::optional<RippleShape> rippleShape(std::uint8_t dir)
{
    // The art's images: N / S 42 x 48 (the disc 48 high, 41 wide), the diagonals 30 x 53 (54 by
    // 27, leaning 16 degrees), E / W 6 x 56.
    static constexpr std::array<RippleShape, 9> kShapes{{
        {54, 27, 16, true},   // NW
        {48, 41, 0, true},    // N
        {54, 27, -16, true},  // NE
        {56, 6, 0, false},    // W
        {},                   // (none)
        {56, 6, 0, false},    // E
        {54, 27, -16, false}, // SW
        {48, 41, 0, false},   // S
        {54, 27, 16, false},  // SE
    }};
    if (dir == 4 || dir >= kShapes.size()) {
        return std::nullopt;
    }
    return kShapes[dir];
}

/// The rings' loop (0-1) at game time `time` for the unit `netCode`: a ring outward for each loop
/// of the game's eight-frame animation, out of step per unit as its frames are.
inline float rippleLoop(double time, std::uint16_t netCode)
{
    constexpr double kFrames = 8; // (the ReflectiveShield* animations: 8 frames, a game frame each)
    const double t = (static_cast<double>(netCode) + (time > 0 ? time : 0)) / kFrames;
    return static_cast<float>(t - static_cast<double>(static_cast<std::int64_t>(t)));
}

} // namespace noxsdl::shieldfx
