// Putting extracted (or new) PNGs into the open documents, and picking images by index, shared by
// the GUI and the command line.
#pragma once

#include "editor/documents.hpp"
#include "editor/image_files.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videotools::editor {

/// "12", "12,15,100-200" -> those indexes below `count`, ascending, no repeats. Throws
/// std::invalid_argument on anything else.
std::vector<std::int32_t> parseIndexList(std::string_view text, std::int32_t count);

/// The bag image `file` becomes in slot `index` (bagImageFromFile against the slot's current
/// image). Throws.
nox::video::DecodedImage bagImageForSlot(const ImageFile& file, std::int32_t index, BagDocument& bag,
                                         std::vector<std::string>& warnings);

/// The pack image `file` becomes in slot `index`: against the bag's image there, or past the
/// bag's images (a pack's extra slots) against the pack's own image or the sidecar. Throws.
nox::video::PackImage packImageForSlot(const ImageFile& file, std::int32_t index, BagDocument& bag, PackDocument& pack,
                                       std::vector<std::string>& warnings);

/// One file's outcome.
struct InsertOutcome {
    std::filesystem::path png;
    std::optional<Target> target;
    std::int32_t index = -1;
    std::string error;                 // empty if it went in
    std::vector<std::string> warnings;
};

/// Puts each file in its slot (fileSlot): bag images into `bag`, pack images into `pack` (skipped
/// with an error if there's none), encoded by `encode`. On the calling thread.
std::vector<InsertOutcome> insertFiles(const std::vector<ImageFile>& files, BagDocument& bag, PackDocument* pack,
                                       const PackEncodeOptions& encode);

/// Images to extract, with the documents' edits, for a worker thread (it opens the files again).
struct ExtractRequest {
    GamePaths paths;
    std::map<std::uint32_t, std::vector<std::uint8_t>> bagEdits;
    std::filesystem::path packPath; // empty: no pack, or a new one (only its edits)
    std::map<std::uint32_t, PackEdit> packEdits;
    std::vector<std::int32_t> indexes;
    bool fromBag = true;
    bool fromPack = true;
    std::filesystem::path dir;
};

struct ExtractCounts {
    std::size_t bag = 0;
    std::size_t pack = 0;
    std::size_t failed = 0;
    std::vector<std::string> errors; // the first few
};

/// Writes the requested images' files (extractBagImage / extractPackImage) into request.dir.
/// Stops early when `cancel` is set.
ExtractCounts extractImages(const ExtractRequest& request, const Progress& progress = {},
                            const std::atomic<bool>* cancel = nullptr);

} // namespace videotools::editor
