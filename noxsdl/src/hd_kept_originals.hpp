// The images drawn from the game's own art even with Use HD Images on (sprite_cache.cpp): HD
// redraws that don't keep their original's layout where windows are placed on that layout, so the
// original's pixels are what lines up with them. Each goes back to HD once the pack's redraw keeps
// the original's layout (its name taken out of the list).
#pragma once

#include "noxcore/video/image_pack.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace noxsdl {

/// By video.bag name (the pack's slot names).
/// - UEKbase1.pcx, ShopBase: the shop's and the NPC dialog's bar (public issue #26). The redraw's
///   frame for the talker's picture is about 12 px wider on its left than the original's, with a
///   dark strip and a light line down the left of the picture where the original has the frame's
///   wood and bevel right against it; its buttons and lights sit up to 9 px left of the original's,
///   under the windows on them.
inline constexpr std::string_view kHdKeptOriginal[] = {"UEKbase1.pcx"};

/// The slots of kHdKeptOriginal's images the pack holds in HD (looked up once, as the pack is set).
inline std::vector<std::int32_t> hdKeptOriginalSlots(const nox::video::ImagePackReader& pack)
{
    std::vector<std::int32_t> slots;
    for (const std::string_view name : kHdKeptOriginal) {
        const long slot = pack.find(name);
        if (slot >= 0) {
            const auto& e = pack.entry(static_cast<std::size_t>(slot));
            if (static_cast<std::int32_t>(e.pixelWidth) > e.width) {
                slots.push_back(static_cast<std::int32_t>(slot));
            }
        }
    }
    return slots;
}

} // namespace noxsdl
