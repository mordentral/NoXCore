// The .map file container: header, CRC block and the list of named raw sections.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::map {

inline constexpr std::uint32_t kMagic = 0xFADEFACE;
inline constexpr std::uint32_t kMagicOld = 0xFADEBEEF; // no CRC block

/// A map section as stored: its name and undecoded contents.
struct RawSection {
    std::string name;
    std::vector<std::uint8_t> data;
};

/// The .map container: header plus the sections in file order.
///
/// Decrypted layout (offsets from the file start):
///   u32 magic
///   FADEFACE only: pad to 8; u32 crc; pad to 8 (the CRC sits alone in its own cipher block)
///   u32 wallOffsetX; u32 wallOffsetY
///   repeat { u8 nameLen (incl. NUL); char name[nameLen]; pad to 8; u64 size; u8 data[size] }
///   u8 0 (end marker); zero pad to 8
struct MapFile {
    std::uint32_t magic = kMagic;
    /// The server sends the stored value in MSG_USE_MAP and clients compare it with the value
    /// stored in their own copy (server_loadMapFile, mapValidateMB); the game never recomputes it. Kept as read
    /// here; map_save.hpp computes a fresh one when saving.
    std::uint32_t crc = 0;
    std::uint32_t wallOffsetX = 0;
    std::uint32_t wallOffsetY = 0;
    std::vector<RawSection> sections;

    /// First section with this name, or nullptr.
    const RawSection* find(std::string_view name) const;
};

/// Parses a decrypted map. Throws FormatError.
MapFile parseMapFile(std::span<const std::uint8_t> plain);

/// Serialises to decrypted bytes.
std::vector<std::uint8_t> serializeMapFile(const MapFile& map);

/// Reads and decrypts a .map file.
MapFile loadMapFile(const std::filesystem::path& path);

} // namespace nox::map
