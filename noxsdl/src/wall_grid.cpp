// WallGrid (wall_grid.hpp): the walls standing on a map, by block.
#include "wall_grid.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace noxsdl {

namespace {

constexpr std::size_t blockOf(std::uint8_t x, std::uint8_t y)
{
    return static_cast<std::size_t>(y / WallGrid::kBlock) * WallGrid::kBlocks + x / WallGrid::kBlock;
}

} // namespace

void WallGrid::build(std::span<const nox::map::Wall> walls)
{
    // A counting sort by block: each block's walls stay in the list's order.
    start_.fill(0);
    for (const auto& w : walls) {
        ++start_[blockOf(w.x, w.y) + 1];
    }
    for (std::size_t b = 1; b < start_.size(); ++b) {
        start_[b] += start_[b - 1];
    }
    entries_.resize(walls.size());
    std::array<std::uint32_t, kBlocks * kBlocks> next{};
    std::copy(start_.begin(), start_.end() - 1, next.begin());
    for (std::size_t i = 0; i < walls.size(); ++i) {
        const auto& w = walls[i];
        entries_[next[blockOf(w.x, w.y)]++] = {static_cast<std::uint32_t>(i), w.x, w.y};
    }
    count_ = walls.size();
    marks_.assign((walls.size() + 63) / 64, 0);
}

void WallGrid::find(int x0, int y0, int x1, int y1, std::vector<std::uint32_t>& out) const
{
    out.clear();
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, kCells - 1);
    y1 = std::min(y1, kCells - 1);
    if (x0 > x1 || y0 > y1 || entries_.empty()) {
        return;
    }
    // Each wall in the cells marked by its index, then the marks read in index order: the list's
    // order, as a walk of the whole list finds them (the blocks are visited row by row). Reading
    // them clears them again.
    std::uint32_t lo = ~0u;
    std::uint32_t hi = 0;
    for (int by = y0 / kBlock; by <= y1 / kBlock; ++by) {
        for (int bx = x0 / kBlock; bx <= x1 / kBlock; ++bx) {
            const std::size_t b = static_cast<std::size_t>(by) * kBlocks + static_cast<std::size_t>(bx);
            for (std::uint32_t e = start_[b]; e < start_[b + 1]; ++e) {
                const Entry& en = entries_[e];
                if (en.x >= x0 && en.x <= x1 && en.y >= y0 && en.y <= y1) {
                    marks_[en.index >> 6] |= std::uint64_t{1} << (en.index & 63);
                    lo = std::min(lo, en.index);
                    hi = std::max(hi, en.index);
                }
            }
        }
    }
    if (lo > hi) {
        return;
    }
    for (std::uint32_t word = lo >> 6; word <= hi >> 6; ++word) {
        std::uint64_t m = std::exchange(marks_[word], 0);
        while (m != 0) {
            out.push_back(word * 64 + static_cast<std::uint32_t>(std::countr_zero(m)));
            m &= m - 1;
        }
    }
}

} // namespace noxsdl
