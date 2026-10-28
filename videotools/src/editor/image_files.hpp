// Single images to and from files, for the editor: PNG plus a JSON sidecar holding what the PNG
// can't (slot, type, draw offset, logical size), and grey mask PNGs for the material and edge
// planes (value = code * 32, as the catalog export writes them).
//
//   <dir>/<stem>.png         the image (straight alpha; material pixels hold their grey level)
//   <dir>/<stem>.json        Sidecar
//   <dir>/<stem>.mask.png    material plane, if the image has one (0 plain, n = slot n - 1)
//   <dir>/<stem>.edge.png    tile edges' plane (0 leave, 1 own colour, 2 floor blended in)
// The stem is "<index>_<name>" for a video.bag image and "<index>_<name>.hd" for a pack image,
// the index padded to six digits.
//
// Putting a PNG back: its sidecar (if any) gives the draw offset and, for a pack image, the
// logical size; the masks beside it give the planes. What's missing comes from the slot's
// current image.
#pragma once

#include "noxcore/video/image_pack.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace videotools::editor {

enum class Target { Bag, Pack };

std::string_view targetName(Target t); // "bag", "pack"

struct Sidecar {
    Target target = Target::Bag;
    std::int32_t index = -1;
    std::string name;
    int type = 0;
    int width = 0;  // logical size (original pixels)
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
    int headerByte = 0;
    int pixelWidth = 0; // stored size (the PNG's)
    int pixelHeight = 0;
    std::string materialMask; // file names beside the PNG, empty if none
    std::string edgeMask;
};

/// Reads a sidecar. Throws std::runtime_error.
Sidecar readSidecar(const std::filesystem::path& json);
void writeSidecar(const std::filesystem::path& json, const Sidecar& s);

/// "<index>_<name>" (name made file-safe), with ".hd" for a pack image.
std::string fileStem(std::int32_t index, std::string_view name, Target target);

struct WrittenFiles {
    std::filesystem::path png; // empty for a 0 x 0 image (only the sidecar is written)
    std::filesystem::path json;
    std::filesystem::path mask;
    std::filesystem::path edge;
};

/// Writes a video.bag image's files into `dir` (created).
WrittenFiles extractBagImage(const nox::video::DecodedImage& image, std::int32_t index, std::string_view name,
                             const std::filesystem::path& dir);
/// Writes a pack image's files into `dir` (created).
WrittenFiles extractPackImage(const nox::video::PackImage& image, std::int32_t index, const std::filesystem::path& dir);

/// A PNG to put back, with what lies beside it.
struct ImageFile {
    std::filesystem::path png;
    std::optional<Sidecar> sidecar;
    std::filesystem::path mask; // existing mask files, else empty
    std::filesystem::path edge;
};

/// The PNG with its sidecar and masks found beside it (<stem>.json etc.).
ImageFile findImageFile(const std::filesystem::path& png);

/// Every extracted PNG under `dir` (not masks), with their sidecars, sorted by path.
std::vector<ImageFile> findImageFiles(const std::filesystem::path& dir);

/// The slot and target a file is for: its sidecar's, else from its name ("<index>_...png" for
/// the bag, "<index>_....hd.png" for the pack); nullopt if neither says.
std::optional<std::pair<Target, std::int32_t>> fileSlot(const ImageFile& file);

/// A grey mask PNG (code * 32) as codes, at the expected size. Throws std::runtime_error if it
/// isn't that size.
std::vector<std::uint8_t> readMask(const std::filesystem::path& png, int width, int height);
void writeMask(const std::filesystem::path& png, int width, int height, const std::vector<std::uint8_t>& codes);

/// The video.bag image a PNG becomes in a slot whose current image is `current`: the slot's
/// type and header byte, the PNG's size and pixels, the sidecar's offsets (else the current
/// ones), the masks beside it (else the current planes, when the size matches). Warnings (things
/// dropped or guessed) go to `warnings`. Throws std::runtime_error / FormatError.
nox::video::DecodedImage bagImageFromFile(const ImageFile& file, const nox::video::DecodedImage& current,
                                          std::vector<std::string>& warnings);

/// The pack image a PNG becomes for slot `index`, whose video.bag image is `bagImage` and whose
/// pack image (if the pack has one) is `packImage`: stored at the PNG's size; logical size and
/// offsets from the sidecar, else the pack image's, else the bag image's; type and header byte
/// the bag image's; material from the mask beside it, else the bag image's plane enlarged.
/// Tile edges get their colour spread past their pixels, as the HD pack's are.
nox::video::PackImage packImageFromFile(const ImageFile& file, const nox::video::DecodedImage& bagImage,
                                        std::string_view name, const std::optional<nox::video::PackImage>& packImage,
                                        std::vector<std::string>& warnings);

} // namespace videotools::editor
