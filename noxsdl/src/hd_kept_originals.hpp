// The images drawn from the game's own art even with Use HD Images on (sprite_cache.cpp): HD
// redraws that don't keep their original's layout where windows are placed on that layout, so the
// original's pixels are what lines up with them. Listed by the bad redraw itself (its slot's CRC),
// not by name: a pack with the redraw fixed draws it in HD, while the packs players already have,
// with the bad one, keep the original.
#pragma once

#include "noxcore/video/image_pack.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace noxsdl {

/// One bad HD encode: the slot's video.bag name and its entry's CRC (CRC-32 of the slot's raw
/// bytes, in the pack's index: nothing is read or decoded to check it).
struct HdKeptOriginal {
    std::string_view name;
    std::uint32_t crc = 0;
};

/// - UEKbase1.pcx, ShopBase: the shop's and the NPC dialog's bar (public issue #26). The redraw's
///   frame for the talker's picture is about 12 px wider on its left than the original's, with a
///   dark strip and a light line down the left of the picture where the original has the frame's
///   wood and bevel right against it; its buttons and lights sit up to 9 px left of the original's,
///   under the windows on them. Every pack up to the HD images of 2026-10-26 has it; the redraw
///   realigned onto the original in the 2026-11-10 pack (CRC 9CFC1179) is drawn in HD.
inline constexpr HdKeptOriginal kHdKeptOriginal[] = {
    {"UEKbase1.pcx", 0x18D4AA35}, // the BC7 packs (version 3), as published 2026-10-24 and 2026-10-26
    {"UEKbase1.pcx", 0xED3BE031}, // the lossless packs (versions 1 and 2) from 2026-10-05
    {"UEKbase1.pcx", 0x1DDF8ABD}, // the first HD pack (2026-10-04)
};

/// The slots of the pack whose HD image is one of `kept` (looked up once, as the pack is set).
inline std::vector<std::int32_t> hdKeptOriginalSlots(const nox::video::ImagePackReader& pack,
                                                     std::span<const HdKeptOriginal> kept = kHdKeptOriginal)
{
    std::vector<std::int32_t> slots;
    for (const HdKeptOriginal& k : kept) {
        const long slot = pack.find(k.name);
        if (slot < 0) {
            continue;
        }
        const auto& e = pack.entry(static_cast<std::size_t>(slot));
        const auto s = static_cast<std::int32_t>(slot);
        if (static_cast<std::int32_t>(e.pixelWidth) > e.width && e.crc == k.crc &&
            std::find(slots.begin(), slots.end(), s) == slots.end()) {
            slots.push_back(s);
        }
    }
    return slots;
}

} // namespace noxsdl
