// The original frame lock follows the game's base frames; Match Server also follows movement
// substeps. Both allow a redraw after 50 ms without an update (a pause or a stalled connection).
#pragma once

#include <cstdint>

namespace noxsdl {

constexpr std::uint32_t renderUpdateCount(bool matchServer, std::uint32_t frames, std::uint32_t snapshots)
{
    return matchServer ? snapshots : frames;
}

constexpr bool snapshotFrameDue(std::uint32_t update, std::uint32_t drawn,
                                std::uint64_t nowMs, std::uint64_t drawnAtMs)
{
    return update != drawn || nowMs - drawnAtMs >= 50;
}

} // namespace noxsdl
