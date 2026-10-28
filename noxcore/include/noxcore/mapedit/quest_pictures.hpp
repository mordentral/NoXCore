// The pictures a quest map's intro can show (MapInfo version 3's quest picture: parity row M4).
//
// The game's quest screens (Gui_Brief_ShowQuestStart / Gui_Brief_ShowQuestNextLevel) load the
// picture by name with gLoadImg: thing.bin's named images (its IMAG list, from video.bag),
// compared exactly; a name it hasn't gets WarriorChapterBegin8. So any named image works; the
// briefing pictures are the full-screen ones (612 x 467 or 640 x 480): the chapters' and the
// quest's own, which the stock quest maps use.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
}
namespace nox::video {
class VideoBag;
struct DecodedImage;
} // namespace nox::video

namespace nox::mapedit {

/// One of thing.bin's named images.
struct QuestPicture {
    std::string name;
    int width = 0; ///< its first frame's size in video.bag (0 when it can't be read)
    int height = 0;
    /// A full-screen picture (at least 600 x 440): what the briefing screens are drawn with.
    bool fullScreen = false;
    /// The stock maps whose quest intro shows it.
    std::vector<std::string> usedBy;
};

/// The quest picture each map in `mapsDir` (its <name>/<name>.map files) names in its MapInfo
/// (version 3, non-empty), by map name; unreadable maps skipped.
std::map<std::string, std::string> questPicturesOfMaps(const std::filesystem::path& mapsDir);

/// Every named image in thing.bin, in its order, with its size from `bag` and the maps using it
/// (`used`: questPicturesOfMaps'). `onImage` (if given) gets each image decoded (its index in the
/// list), for thumbnails.
std::vector<QuestPicture> questPictures(const things::ThingDb& things, video::VideoBag& bag, const std::map<std::string, std::string>& used = {},
                                        const std::function<void(std::size_t, const video::DecodedImage&)>& onImage = {});

} // namespace nox::mapedit
