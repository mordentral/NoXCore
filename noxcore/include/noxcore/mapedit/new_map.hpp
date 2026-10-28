// A new map for the editor's New dialog: a blank document with a floor area and a player start,
// lit by the map's own ambient.
#pragma once

#include "noxcore/mapedit/document.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

struct NewMapOptions {
    std::string title; ///< UTF-8 (as typed): stored as the map's 8-bit text, at most 63 bytes
    /// The floored area, a square of this many wall cells on a side, centred on the map
    /// (0: no floor).
    int size = 64;
    /// The floor it's covered with (thing.bin FLOR index).
    std::uint8_t floor = 0;
    /// Game types (MapInfo flags: 4 arena, 8 CTF...).
    std::uint32_t gameFlags = 4;
    /// A PlayerStart in the middle (a multiplayer map needs one).
    bool playerStart = true;
    /// Lit from the start: the map's AmbientData is kNewMapAmbient, which lights the view wherever no
    /// polygon covers it (with no polygons, everywhere). Without it the game draws the map at the
    /// AmbientData colour (0, 0, 0 here), nearly black.
    bool lit = true;
};

/// A new map's light (its AmbientData): the stock maps' bright, near-white polygon light
/// (FreezOut's whole-map "Boundary" polygon; TriLevel's, treehaus's and the quest's well-lit rooms
/// are about the same).
inline constexpr std::array<std::uint8_t, 3> kNewMapAmbient{165, 161, 140};

/// The variant the aligned tiling rule gives a floor at a full-grid tile, with the anchor at
/// (0, 0): the block of cols x rows images laid along the floor's diagonals.
std::int16_t alignedVariant(const things::ThingDb& things, std::uint8_t floor, int gx, int gy);

MapDocument newMap(const things::ThingDb& things, const NewMapOptions& options);

} // namespace nox::mapedit
