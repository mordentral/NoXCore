// The quest intro's pictures (quest_pictures.hpp).
#include "noxcore/mapedit/quest_pictures.hpp"

#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/sprite.hpp"
#include "noxcore/video/video_bag.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>

namespace nox::mapedit {

std::map<std::string, std::string> questPicturesOfMaps(const std::filesystem::path& mapsDir)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(mapsDir, ec)) {
        std::string ext = e.path().extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!e.is_regular_file() || ext != ".map") continue;
        try {
            const auto info = map::decodeSection<map::MapInfo>(map::loadMapFile(e.path()));
            if (info.version < 3) continue;
            const std::string name = info.questGraphic.substr(0, info.questGraphic.find('\0')); // (a C string)
            if (!name.empty()) out[e.path().stem().string()] = name;
        } catch (const std::exception&) {
        }
    }
    return out;
}

std::vector<QuestPicture> questPictures(const things::ThingDb& things, video::VideoBag& bag, const std::map<std::string, std::string>& used,
                                        const std::function<void(std::size_t, const video::DecodedImage&)>& onImage)
{
    std::vector<QuestPicture> out;
    out.reserve(things.images.size());
    // A few decompressed segments at a time (named images sit together in a handful).
    std::optional<std::uint32_t> segIndex;
    std::vector<std::uint8_t> seg;
    for (const auto& def : things.images) {
        QuestPicture p;
        p.name = def.name;
        for (const auto& [mapName, picture] : used) {
            if (picture == def.name) p.usedBy.push_back(mapName);
        }
        std::optional<things::ImageRef> ref = def.image;
        if (!ref && def.animation && !def.animation->frames.empty()) ref = def.animation->frames.front();
        if (ref) {
            const long index = ref->index >= 0 ? ref->index : bag.findImage(ref->name);
            if (index >= 0 && static_cast<std::size_t>(index) < bag.images().size()) {
                try {
                    const auto& info = bag.images()[static_cast<std::size_t>(index)];
                    if (segIndex != info.segment) {
                        seg = bag.readSegment(info.segment);
                        segIndex = info.segment;
                    }
                    if (static_cast<std::size_t>(info.offset) + info.size <= seg.size()) {
                        const auto d = video::decodeImage(std::span(seg).subspan(info.offset, info.size), info.type);
                        p.width = d.width;
                        p.height = d.height;
                        if (onImage && d.width > 0 && d.height > 0) onImage(out.size(), d);
                    }
                } catch (const std::exception&) {
                }
            }
        }
        p.fullScreen = p.width >= 600 && p.height >= 440;
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace nox::mapedit
