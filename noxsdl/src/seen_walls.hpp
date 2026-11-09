// The minimap's seen walls (MapView's): a bit a wall cell (MapView::wallKey: x << 8 | y), set the
// first time the wall is drawn on screen (what a solo save keeps as the wall's byte 0xC), and each
// cell listed as it's first set, for a co-op guest's report to its host (takeDrawn: a guest's
// minimap is kept by the campaign, coop_proto.hpp's kSeenReport). Cells given back to us
// (markSeen: the campaign's for a co-op guest) are set without being listed. No SDL: the tests' too.
#pragma once

#include <bitset>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl {

class SeenWalls {
public:
    /// The wall at `key` drawn: seen, and listed if it wasn't already.
    void drawn(std::uint16_t key)
    {
        if (!bits_.test(key)) {
            bits_.set(key);
            drawn_.push_back(key);
        }
    }
    /// Cells seen before (by us, kept elsewhere), added: not listed.
    void markSeen(std::span<const std::uint16_t> keys)
    {
        for (const std::uint16_t k : keys) {
            bits_.set(k);
        }
    }
    bool test(std::uint16_t key) const { return bits_.test(key); }
    /// The cells first drawn since the last call, in the order they were (each once: a map's walls
    /// at most).
    std::vector<std::uint16_t> takeDrawn() { return std::exchange(drawn_, {}); }

private:
    std::bitset<65536> bits_;
    std::vector<std::uint16_t> drawn_;
};

} // namespace noxsdl
