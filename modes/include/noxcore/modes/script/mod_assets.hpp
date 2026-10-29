// A mod's pictures and sounds: the files under its folder's assets\, decoded on the host (while
// the mod is hosted; a mods folder's listing checks their headers alone), so a client never parses
// a PNG or a WAV from a server: it's sent the pixels and samples (ui_proto.hpp's asset subs) and
// checks the same caps again.
//
//   mods\<id>\assets\icons\sword.png    an image "icons/sword": RGBA8, at most 1024 x 1024
//   mods\<id>\assets\ding.wav           a sound "ding": PCM 8 or 16 bit, mono or stereo, up to
//                                       48 kHz, kept as PCM16
//
// A name is the file's path under assets\ without its extension, lowercase a-z, 0-9, _ and /
// (at most kMaxAssetName). Anything else there is refused with the reason (the mod is listed as
// broken), but for the files Windows leaves in folders (desktop.ini, Thumbs.db) and dot files.
// Usable without a host (the map editor's mod projects load a folder's for their UI preview).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes::script {

/// The caps, checked at a mod's load and again by a client on receipt.
inline constexpr int kMaxAssetSide = 1024;            ///< an image's width and height at most
inline constexpr std::size_t kMaxAssets = 256;        ///< pictures and sounds in all
inline constexpr std::size_t kMaxImageBytes = 16u << 20; ///< every image decoded (RGBA8)
inline constexpr std::size_t kMaxSoundBytes = 8u << 20;  ///< every sound decoded (PCM16)
inline constexpr std::uint32_t kMaxSoundRate = 48000;
inline constexpr std::size_t kMaxAssetName = 64;

struct ModAsset {
    enum class Kind : std::uint8_t { Image = 1, Sound = 2 };
    std::string name;
    Kind kind = Kind::Image;
    // An image's:
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; ///< width x height x 4, rows top first
    // A sound's:
    std::uint32_t rate = 0;
    std::uint8_t channels = 0;
    std::vector<std::int16_t> samples; ///< interleaved

    /// Its decoded bytes (what the caps count).
    std::size_t bytes() const noexcept { return kind == Kind::Image ? rgba.size() : samples.size() * 2; }
};

struct ModAssets {
    std::vector<ModAsset> list; ///< by name (each name once, in byte order): an asset's index is its place
    std::size_t imageBytes = 0;
    std::size_t soundBytes = 0;
    /// What's in it (names, sizes, pixels, samples): a reload sends a set again only when this moved.
    std::uint64_t hash = 0;

    /// The index of the asset `name` of `kind`; -1: none.
    int find(std::string_view name, ModAsset::Kind kind) const;
    /// imageBytes, soundBytes and hash worked out from `list`.
    void tally();
};

/// Whether `name` may be an asset's (a-z, 0-9, _ and /, no empty part, at most kMaxAssetName).
bool validAssetName(std::string_view name);

/// Why `a` breaks a cap or the rules for its kind (an image's sides and pixels, a sound's rate,
/// channels and samples, the name); empty: it doesn't.
std::string checkAsset(const ModAsset& a);

/// A folder's assets (`assetsDir`: a mod's assets\) read and decoded. No folder: an empty set.
/// `assets` null: `error` says which file and why (a bad name, a bad or unsupported file, a cap).
struct ModAssetsResult {
    std::shared_ptr<const ModAssets> assets;
    std::string error;
};
ModAssetsResult loadModAssets(const std::filesystem::path& assetsDir);

/// The same rules and caps checked from the files' headers alone (names, count, a PNG's IHDR
/// sides, a WAV's format and data sizes; nothing decoded or kept): a mods folder's listing, its
/// broken mods with their reasons. Empty: fine (a file whose data is bad shows at loadModAssets).
std::string checkModAssets(const std::filesystem::path& assetsDir);

/// The newest write time of the files under `assetsDir` (none: file_time_type::min()): a reload's test.
std::filesystem::file_time_type newestAsset(const std::filesystem::path& assetsDir);

/// A PNG decoded to RGBA8 (a mod asset's rules: at most kMaxAssetSide a side); false: why.
bool decodeAssetPng(std::span<const std::uint8_t> file, ModAsset& out, std::string& error);
/// A WAV decoded to PCM16 (PCM 8 or 16 bit, mono or stereo, up to kMaxSoundRate); false: why.
bool decodeAssetWav(std::span<const std::uint8_t> file, ModAsset& out, std::string& error);

} // namespace nox::modes::script
