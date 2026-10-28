// The map editor's document writer: MapDocument -> the .map (and the .nxz the map download
// serves). The derive and keep rules are listed in document.hpp.
#pragma once

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/map_save.hpp"
#include "noxcore/mapedit/document.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::mapedit {

struct Sidecar;

/// The document as map sections (header CRC not yet computed). Throws FormatError for a
/// document the game couldn't load (see validate.hpp for the checks with messages).
map::MapFile toMapFile(const MapDocument& doc, const things::ThingDb& things);

/// The objects in the order the writer writes their records: each object (by order key among
/// those of its parent), then its inventory, nested. (MapDocument::fromMapFile gives record i the
/// id i + 1 and order key i.)
std::vector<ObjId> recordOrder(const MapDocument& doc);

/// The decrypted .map bytes: the sections written with the game's write calls and a fresh
/// header CRC, or the CRC the file had when the bytes are the same as when it was opened.
std::vector<std::uint8_t> writeMapBytes(const MapDocument& doc, const things::ThingDb& things);

/// The .map as stored (encrypted) and the .nxz the map download serves (u32 stored size, then
/// the NXZ stream of the stored bytes: map_save.hpp).
using EncodedFiles = map::EncodedMapFiles;
EncodedFiles encodeFiles(const MapDocument& doc, const things::ThingDb& things);

struct SavedPaths {
    std::filesystem::path map;
    std::filesystem::path nxz;
    std::filesystem::path sidecar; // empty when none was written
};

/// Validates (throws FormatError listing the errors), then writes <mapsDir>\<name>\<name>.map
/// and .nxz, and the sidecar <name>.nxe beside them when one is given (with its "needs NoX Core"
/// record set from the map's script: script_doc.hpp, markNoxCoreNeeds). Refuses a stock map's
/// name (edits of a stock map are saved under a new name).
SavedPaths saveDocument(const MapDocument& doc, const things::ThingDb& things, const std::filesystem::path& mapsDir,
                        std::string_view name, const Sidecar* sidecar = nullptr);

} // namespace nox::mapedit
