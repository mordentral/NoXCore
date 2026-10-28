// Shared internals of the map editor's document code.
#pragma once

#include "noxcore/map/xfer.hpp"
#include "noxcore/mapedit/document.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit::detail {

/// An object type's xfer function name (DefaultXfer when it has none) and CLASS/SUBCLASS bits.
using map::xfer::TypeXfer;

/// Throws FormatError for a type thing.bin doesn't have (the game fails such a load).
TypeXfer typeXfer(const things::ThingDb& things, std::string_view name);

/// A hash of a decrypted map file with its CRC field zeroed: "same content as opened".
std::uint64_t contentHash(std::span<const std::uint8_t> plain);

/// What the writer derives from content (writer.cpp): the header's wall offsets (the least wall
/// x / y; nothing without walls), the WallMap and FloorMap bounding boxes (nothing when empty).
std::optional<std::pair<std::uint32_t, std::uint32_t>> derivedWallOffsets(const MapDocument& d);
std::optional<GridBox> derivedWallBox(const MapDocument& d);
std::optional<GridBox> derivedFloorBox(const MapDocument& d);

} // namespace nox::mapedit::detail
