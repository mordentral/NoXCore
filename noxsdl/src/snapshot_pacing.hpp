// Match Server follows the server's updates: base frames and movement substeps (snapshotCount).
// It allows a redraw after 50 ms without an update (a pause or a stalled connection).
#pragma once

#include <algorithm>
#include <cstdint>

namespace noxsdl {

/// Options' Smoothing as noxcore.cfg stores it: 0-3, 3 being Match Server. 4 (Match Server's
/// place in its first build, beside the old Lock to 30 FPS) loads as 3.
constexpr int smoothingSetting(int stored)
{
    return std::clamp(stored, 0, 3);
}

constexpr bool snapshotFrameDue(std::uint32_t update, std::uint32_t drawn,
                                std::uint64_t nowMs, std::uint64_t drawnAtMs)
{
    return update != drawn || nowMs - drawnAtMs >= 50;
}

} // namespace noxsdl
