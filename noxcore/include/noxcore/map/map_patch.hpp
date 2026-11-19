// Small changes to a map that leave every other section exactly as stored: its compiled script
// replaced, its exits' destinations listed and renamed, and the map written back with a fresh
// header CRC. For tools that change one part of a map (a script converter, noxmap) without
// re-encoding the rest. Library calls only: nothing in the game runs them.
#pragma once

#include "noxcore/map/map_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::map {

/// Replaces the compiled script in the map's ScriptObject section (`code`: a script file's bytes,
/// script::ScriptFile::serialize's), keeping the section's version and its place. Only that
/// section changes. Throws FormatError when the map has no ScriptObject.
void replaceScriptObject(MapFile& map, std::span<const std::uint8_t> code);

/// The longest exit destination the game takes (its reader's buffer holds 80 bytes with the NUL).
inline constexpr std::size_t kMaxExitMapName = 79;

/// One exit in ObjectData: an object whose type's xfer is ExitXfer.
struct MapExit {
    std::size_t record = 0;   ///< its index in ObjectData's records
    std::string type;         ///< its object type (ObjectTOC)
    std::uint32_t extent = 0; ///< its map-unique number
    float x = 0;              ///< where it stands
    float y = 0;
    std::string mapName;      ///< where it leads: ExitXfer's name up to its first NUL ("War02a.map", "War01A.map:Entry")
    float toX = 0;            ///< ExitXfer's point: a solo map's return point, a warp gate's way out
    float toY = 0;
};

/// Every exit, in record order. Records older than the typed xfer layouts (version below 0x3C)
/// aren't listed. `things` gives each object type's xfer. Throws FormatError for a map whose
/// ObjectTOC or ObjectData doesn't decode, or a type thing.bin doesn't have.
std::vector<MapExit> listExits(const MapFile& map, const things::ThingDb& things);

/// Renames exit destinations: each exit whose name (MapExit::mapName's) `rename` maps to a new
/// name (nothing, or the same name: left alone) is re-encoded through the xfer code with it, the
/// stored NUL after the name (and anything after that) kept as it was. Returns how many
/// changed. Only ObjectData changes, and in it only those records' bytes, plus the alignment of
/// the records after a record whose size changed (the stored leftover bytes in a pad whose length
/// changes become zeros, as the game writes them). Throws FormatError for a new name longer than
/// kMaxExitMapName, or longer than the game's buffer takes with the bytes the exit stores after its
/// name (the NUL and anything past it: 80 bytes in all), or with a NUL in it, or an exit record
/// that doesn't re-encode to its stored bytes (nothing is changed then).
std::size_t renameExits(MapFile& map, const things::ThingDb& things,
                        const std::function<std::optional<std::string>(std::string_view)>& rename);

/// The map as decrypted bytes with every section copied as stored, byte for byte, and the header's
/// CRC computed afresh (computeMapCrc: GAME.EXE's save CRC over the decoded sections, so the map
/// must decode fully). Throws FormatError otherwise.
std::vector<std::uint8_t> serializeMapKeepingSections(const MapFile& map, const things::ThingDb& things);

/// serializeMapKeepingSections, encrypted and written to `path` (writeFileReplacing: a failed write
/// leaves the old file).
void saveMapKeepingSections(const std::filesystem::path& path, const MapFile& map, const things::ThingDb& things);

} // namespace nox::map
