// Writing maps the way GAME.EXE does, including the header CRC.
#pragma once

#include "noxcore/map/map_file.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::map {

/// The CRC a FADEFACE map header holds: GAME.EXE's per-call crypt CRC (crypto::updateCrc) over
/// every field write of the map save (mapSaveMap), from the magic to the end marker. Padding,
/// the CRC block and section/object sizes aren't counted.
///
/// Every section is re-encoded with the game's call boundaries, so the map must decode fully;
/// `things` supplies each object type's xfer. Throws FormatError otherwise.
std::uint32_t computeMapCrc(const MapFile& map, const things::ThingDb& things);

/// Serialises to decrypted bytes with a freshly computed CRC (map.crc is ignored).
std::vector<std::uint8_t> serializeMapFileWithCrc(const MapFile& map, const things::ThingDb& things);

/// Encrypts and writes a .map file with a freshly computed CRC.
void saveMapFileWithCrc(const std::filesystem::path& path, const MapFile& map, const things::ThingDb& things);

/// A map's two files as a host serves them: the .map as stored (encrypted) and the .nxz the map
/// download sends (u32 size of the stored .map, then the NXZ stream of those bytes: the exe's
/// .nxz reader's layout).
struct EncodedMapFiles {
    std::vector<std::uint8_t> map;
    std::vector<std::uint8_t> nxz;
};
/// The pair for `plain`, a decrypted .map with its CRC in place (serializeMapFileWithCrc's).
EncodedMapFiles encodeMapBytes(std::vector<std::uint8_t> plain);

/// Writes a whole file beside `path` (<path>.tmp), then moves it over the old one: a failed write
/// never leaves half a file. Throws FormatError ("<who>: can't write <path>.tmp") when the write
/// fails.
void writeFileReplacing(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string_view who);
/// Writes the pair as <mapsDir>\<name>\<name>.map and .nxz (each writeFileReplacing), creating
/// the folder. Returns the .map's path.
std::filesystem::path writeMapFolder(const std::filesystem::path& mapsDir, std::string_view name, const EncodedMapFiles& files,
                                     std::string_view who);

} // namespace nox::map
