// The health bar beside a creature its player watches (one on its monitored list: MSG 0xDB,
// Cli_IsCreatureMonitored), drawn by MonsterDraw (so MaidenDraw
// too) and NPCDraw after the creature: a 2-wide column beside it, twice its radius
// right of its feet, as tall as its ZSIZE span (at least 30), centred on half its ZSIZE's top
// and bottom over its feet less its z; black, filled from the bottom to health x height / most
// health, red (255, 0, 0) or green while poisoned (0, 255, 0). None while
// its most health is 0. (Math_FloatToInt truncates: the FPU is in chop mode.)
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace noxsdl {

struct CreatureBar {
    int x = 0;      ///< the column's left, from the creature's feet
    int top = 0;    ///< its top, from the creature's feet less its z
    int height = 0; ///< the black column's height
    int filled = 0; ///< the coloured part, up from its bottom
};

/// The health bar's column for a creature of EXTENT `radius` and ZSIZE `zSizeMin` / `zSizeMax`.
inline std::optional<CreatureBar> creatureBar(float radius, float zSizeMin, float zSizeMax, std::uint16_t health,
                                              std::uint16_t maxHealth)
{
    if (maxHealth == 0) {
        return std::nullopt;
    }
    CreatureBar b;
    b.x = static_cast<int>(radius + radius);
    b.height = std::max(30, static_cast<int>(zSizeMax - zSizeMin));
    const int middle = static_cast<int>(zSizeMin + zSizeMax);
    b.top = -(b.height / 2) - middle / 2;
    b.filled = static_cast<int>(health) * b.height / static_cast<int>(maxHealth);
    return b;
}

} // namespace noxsdl
