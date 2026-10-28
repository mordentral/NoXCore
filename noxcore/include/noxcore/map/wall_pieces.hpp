// Wall pieces as WallMap and SecretWalls records hold them: a facing's arms (the diagonal
// neighbours its piece joins) and the facing for a set of arms, and the secret-wall records the
// stock maps carry. Shared by the map editor's wall tool (mapedit/walls.hpp) and the level
// generator (mapgen).
//
// A wall piece's facing (WallMap direction, low 7 bits) is one of 13 shapes; each joins some of
// its four diagonal neighbours on the wall grid (its "arms"): NW (-1,-1), NE (+1,-1), SW (-1,+1),
// SE (+1,+1). Facings 11 and 12 are the window pieces of 0 and 1.
#pragma once

#include "noxcore/map/sections.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace nox::map {

/// Wall arms: the diagonal neighbours a piece joins.
enum WallArm : std::uint8_t { kArmNW = 1, kArmNE = 2, kArmSW = 4, kArmSE = 8 };

namespace detail {

// The arms of facings 0-12 (11 and 12: the window pieces of 0 and 1).
inline constexpr std::array<std::uint8_t, 13> kFacingArms = {
    kArmSW | kArmNE,          // 0: the SW-NE line
    kArmNW | kArmSE,          // 1: the NW-SE line
    15,                       // 2: cross
    kArmNW | kArmNE | kArmSW, // 3
    kArmNW | kArmNE | kArmSE, // 4
    kArmNE | kArmSE | kArmSW, // 5
    kArmNW | kArmSW | kArmSE, // 6
    kArmNW | kArmNE,          // 7
    kArmNE | kArmSE,          // 8
    kArmSE | kArmSW,          // 9
    kArmNW | kArmSW,          // 10
    kArmSW | kArmNE,          // 11: window of 0
    kArmNW | kArmSE,          // 12: window of 1
};

} // namespace detail

/// The arms of a facing's piece (0 for a facing past 12).
constexpr std::uint8_t armsOfFacing(int facing)
{
    return facing >= 0 && facing < static_cast<int>(detail::kFacingArms.size()) ? detail::kFacingArms[static_cast<std::size_t>(facing)] : 0;
}

/// The facing whose piece has these arms: one arm gives the straight piece along it, none
/// gives nothing (a lone piece keeps the facing it has).
constexpr std::optional<std::uint8_t> facingForArms(std::uint8_t arms)
{
    switch (arms & 15) {
    case 0: return std::nullopt;
    case kArmNW:
    case kArmSE: return std::uint8_t{1};
    case kArmNE:
    case kArmSW: return std::uint8_t{0};
    default: break;
    }
    for (std::uint8_t f = 0; f <= 10; ++f) {
        if (detail::kFacingArms[f] == (arms & 15)) {
            return f;
        }
    }
    return std::nullopt;
}

/// The default secret-wall record (the stock maps' commonest: shut, 3 s wait, no flags).
inline SecretWall defaultSecretWall(GridPoint pos)
{
    SecretWall s;
    s.pos = pos;
    s.openWait = 3;
    s.flags = 0;
    s.state = 1;
    return s;
}

/// A secret wall set to start open (state 3, fully open, seen by everyone: as stock maps store
/// those), or shut.
inline void setSecretStartOpen(SecretWall& s, bool open)
{
    s.state = open ? 3 : 1;
    s.openDelay = open ? 23 : 0;
    s.unknown = open ? 0xFFFFFFFFu : 0;
}

} // namespace nox::map
