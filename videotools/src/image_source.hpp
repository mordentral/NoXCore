// video.bag images decoded on request, keeping a few decompressed segments (neighbouring frames
// usually share one). Not thread-safe: give each thread its own.
#pragma once

#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <cstdint>
#include <filesystem>
#include <list>
#include <string_view>
#include <utility>
#include <vector>

namespace videotools {

struct GamePaths {
    std::filesystem::path bag;
    std::filesystem::path idx;
    std::filesystem::path thingBin;

    /// video.bag, video.idx and thing.bin in a game folder.
    static GamePaths inDir(const std::filesystem::path& gameDir);
};

/// The game folder: the build's NOX_GAME_DIR, else (left empty) the installed game as noxsdl
/// finds it from `exeDir` (noxcore io/game_dir.hpp); empty if neither.
std::filesystem::path defaultGameDir(const std::filesystem::path& exeDir = {});

/// The game folder found the way noxsdl finds it (noxcore io/game_dir.hpp: NOX_DATA, the program's
/// folder, the current folder, the installs), the build's NOX_GAME_DIR only as the last resort;
/// empty if none. For programs that run from a package on any PC.
std::filesystem::path locateGameDir(const std::filesystem::path& exeDir);

/// `name` (video_export, ai) beside the build's NOX_GAME_DIR; empty if that was left empty.
std::filesystem::path besideConfiguredGameDir(std::string_view name);

class ImageSource {
public:
    explicit ImageSource(const GamePaths& paths);

    const nox::video::VideoBag& bag() const { return bag_; }

    /// Decodes one image. Throws FormatError / std::system_error.
    nox::video::DecodedImage decode(std::int32_t index);
    /// One image's bytes as stored. Throws FormatError / std::system_error.
    std::vector<std::uint8_t> imageBytes(std::int32_t index);

private:
    const std::vector<std::uint8_t>& segment(std::uint32_t index);

    nox::video::VideoBag bag_;
    std::list<std::pair<std::uint32_t, std::vector<std::uint8_t>>> segments_; // most recent first
};

} // namespace videotools
