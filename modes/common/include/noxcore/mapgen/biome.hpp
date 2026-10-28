// Delving Mode's biomes: what a generated room is made of and dressed
// with. Each is drawn from the shipped maps that look that way (their floors, wall sets and the
// scenery they place most): Forest (con03a, G_Forest), Beach (So_Beach), Shop (So_Dun's
// traders), Cave (con05c), Swamp (G_Swamp, con09a), Ice (con09d), Crypt (G_Crypts), Castle
// (G_Castle), Mines (G_Mines, con03b), Lava (G_Lava's volcanic rock and lava), Desert (the sand
// and ancient ruins of So_Beach and the Ix outskirts), Grasslands (field-stone walls round open
// grass) and Dungeon.
//
// Floors: the base everywhere, patches of other floors here and there and, where the biome has
// one, a path from the players' start to the exit; outdoor biomes blend each patch and path into
// the base (FloorEdge), as the editor's maps do. Scenery: three sizes scattered over the floor
// (large ones far apart), and pieces set along the walls. A harming floor (Lava, FLOR 6: it
// burns whatever stands on it) is kept off the route, the start and the end, and nothing is
// placed on it.
#pragma once

#include "noxcore/mapgen/layout.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::mapgen {

/// What a biome's air does to the players in it (the delve's director applies it).
enum class Climate : std::uint8_t {
    Mild,
    Freezing,  ///< slowed for as long as they're there
    Scorching, ///< burnt a little, steadily, till they leave
};

/// A biome as the directors read it. (Its walls, floors, light and scenery are the generator's
/// script's alone, biomes.luau's.)
struct Biome {
    std::string_view name;
    std::string_view title; ///< as players are told it: "the Frozen Wastes"
    /// How often an act draws it, against the others' (rare ones 1).
    int weight = 3;
    Climate climate = Climate::Mild;
    /// How its levels' rooms are cut (layout.hpp): built halls, natural caverns, open clearings.
    LayoutStyle style = LayoutStyle::Natural;
    /// The chests its levels' loot goes in: none with a CHEST_ subclass (Chest1-4's 0xF00),
    /// which a quest opens only with a silver key (collide.cpp's chestCollide). Empty: the stone
    /// ones, CryptChest1-4.
    std::vector<std::string_view> chests;
};

/// A harming floor (the server burns what stands on FLOR 6, Lava).
bool harmingFloor(std::string_view floor);

/// Every biome, in a fixed order.
std::span<const Biome> biomes();
/// The biome by name (case aside), nullptr if none.
const Biome* findBiome(std::string_view name);

} // namespace nox::mapgen
