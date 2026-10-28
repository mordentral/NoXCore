#include "image_source.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/game_dir.hpp"

#include <span>
#include <string>

namespace videotools {

namespace {
constexpr std::size_t kCachedSegments = 16;
}

GamePaths GamePaths::inDir(const std::filesystem::path& gameDir)
{
    return {gameDir / "video.bag", gameDir / "video.idx", gameDir / "thing.bin"};
}

std::filesystem::path defaultGameDir(const std::filesystem::path& exeDir)
{
    const std::filesystem::path configured = NOXVIDEO_DEFAULT_GAME_DIR;
    if (!configured.empty()) {
        return configured;
    }
    return nox::io::findGameDir(exeDir).value_or(std::filesystem::path());
}

std::filesystem::path locateGameDir(const std::filesystem::path& exeDir)
{
    const auto found = nox::io::locateGameDir(exeDir, std::filesystem::path(NOXVIDEO_DEFAULT_GAME_DIR));
    return found ? found->dir : std::filesystem::path();
}

std::filesystem::path besideConfiguredGameDir(std::string_view name)
{
    const std::filesystem::path configured = NOXVIDEO_DEFAULT_GAME_DIR;
    return configured.empty() ? std::filesystem::path() : configured.parent_path() / name;
}

ImageSource::ImageSource(const GamePaths& paths) : bag_(paths.bag, paths.idx) {}

const std::vector<std::uint8_t>& ImageSource::segment(std::uint32_t index)
{
    for (auto it = segments_.begin(); it != segments_.end(); ++it) {
        if (it->first == index) {
            segments_.splice(segments_.begin(), segments_, it);
            return segments_.front().second;
        }
    }
    segments_.emplace_front(index, bag_.readSegment(index));
    if (segments_.size() > kCachedSegments) {
        segments_.pop_back();
    }
    return segments_.front().second;
}

std::vector<std::uint8_t> ImageSource::imageBytes(std::int32_t index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= bag_.images().size()) {
        throw nox::FormatError("no image #" + std::to_string(index) + " in video.bag");
    }
    const auto& img = bag_.images()[static_cast<std::size_t>(index)];
    const auto& data = segment(img.segment);
    if (std::size_t{img.offset} + img.size > data.size()) {
        throw nox::FormatError("video.bag: image " + img.name + " runs past its segment");
    }
    return {data.begin() + img.offset, data.begin() + img.offset + img.size};
}

nox::video::DecodedImage ImageSource::decode(std::int32_t index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= bag_.images().size()) {
        throw nox::FormatError("no image #" + std::to_string(index) + " in video.bag");
    }
    const auto& img = bag_.images()[static_cast<std::size_t>(index)];
    const auto& data = segment(img.segment);
    if (std::size_t{img.offset} + img.size > data.size()) {
        throw nox::FormatError("video.bag: image " + img.name + " runs past its segment");
    }
    return nox::video::decodeImage(std::span(data).subspan(img.offset, img.size), img.type);
}

} // namespace videotools
