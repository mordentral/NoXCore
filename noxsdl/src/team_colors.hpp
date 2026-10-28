// Team colours (materialGetTeamColor, from the colours loadDefColor makes): names over
// players, the minimap's team markers.
#pragma once

#include "noxcore/client/world.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace noxsdl {

/// A team's colour, or nullopt for no team.
inline std::optional<SDL_Color> teamColor(const nox::client::World& world, std::uint32_t team)
{
    // By team colour index: none, red, blue, green, cyan, yellow, violet, black, white, orange.
    static constexpr std::array<SDL_Color, 10> kTeamColors{{
        {255, 255, 255, 255},
        {255, 128, 128, 255},
        {0, 160, 255, 255},
        {128, 255, 128, 255},
        {0, 0, 255, 255},
        {255, 255, 0, 255},
        {100, 0, 0, 255},
        {0, 0, 0, 255},
        {255, 255, 255, 255},
        {240, 180, 42, 255},
    }};
    const auto t = world.teams().find(team);
    if (team == 0 || t == world.teams().end() || t->second.color >= kTeamColors.size()) {
        return std::nullopt;
    }
    return kTeamColors[t->second.color];
}

/// A flag's colour index from its material modifier's name (the table:
/// MaterialTeamRed 1 ... MaterialTeamOrange 9), 0 for none.
inline std::uint8_t flagMaterialColor(std::string_view material)
{
    static constexpr std::array<std::pair<std::string_view, std::uint8_t>, 9> kMaterials{{
        {"MaterialTeamRed", 1},
        {"MaterialTeamBlue", 2},
        {"MaterialTeamGreen", 3},
        {"MaterialTeamYellow", 5},
        {"MaterialTeamCyan", 4},
        {"MaterialTeamViolet", 6},
        {"MaterialTeamBlack", 7},
        {"MaterialTeamWhite", 8},
        {"MaterialTeamOrange", 9},
    }};
    for (const auto& [name, colour] : kMaterials) {
        if (name == material) {
            return colour;
        }
    }
    return 0;
}

} // namespace noxsdl
