// WallGrid (wall_grid.hpp): the walls standing on a map, by block.
#include "wall_grid.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace noxsdl {

namespace {

constexpr std::size_t blockOf(std::uint8_t x, std::uint8_t y)
{
    return static_cast<std::size_t>(y / WallGrid::kBlock) * WallGrid::kBlocks + x / WallGrid::kBlock;
}

/// A wall cell's key (MapView::wallKey's).
std::uint16_t wallKey(int x, int y)
{
    return static_cast<std::uint16_t>((x << 8) | (y & 0xFF));
}

} // namespace

void standingWalls(std::span<const nox::map::Wall> mapWalls, std::span<const std::uint16_t> breakable,
                   std::span<const std::pair<std::uint16_t, bool>> secret, const std::set<std::uint16_t>& destroyed,
                   const std::set<std::uint16_t>& opened, const std::set<std::uint16_t>& closed,
                   std::span<const std::pair<std::pair<int, int>, nox::map::Wall>> magic, std::vector<nox::map::Wall>& walls,
                   std::set<std::uint16_t>& breakableStanding)
{
    // The cells of broken walls and open secret walls; Wall-spell walls replace whatever the map has in their cell.
    std::set<std::uint16_t> gone;
    for (const auto id : destroyed) {
        if (id < breakable.size()) {
            gone.insert(breakable[id]);
        }
    }
    for (std::size_t id = 0; id < secret.size(); ++id) {
        const auto key = static_cast<std::uint16_t>(id);
        // Open when the server says so, or when the map starts it open and the server hasn't closed it.
        const bool open = opened.contains(key) || (secret[id].second && !closed.contains(key));
        if (open) {
            gone.insert(secret[id].first);
        }
    }
    std::set<std::uint16_t> raised;
    for (const auto& [cell, wall] : magic) {
        raised.insert(wallKey(cell.first, cell.second));
    }
    walls.clear();
    for (const auto& w : mapWalls) {
        const auto key = wallKey(w.x, w.y);
        if (!gone.contains(key) && !raised.contains(key)) {
            walls.push_back(w);
        }
    }
    for (const auto& [cell, wall] : magic) {
        auto w = wall;
        w.x = static_cast<std::uint8_t>(cell.first);
        w.y = static_cast<std::uint8_t>(cell.second);
        walls.push_back(w);
    }
    // The breakable walls still standing as the map has them (a Wall-spell wall raised in a broken
    // one's cell is a new wall: not breakable).
    breakableStanding.clear();
    for (const auto key : breakable) {
        if (!gone.contains(key) && !raised.contains(key)) {
            breakableStanding.insert(key);
        }
    }
}

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

void WallGrid::findAround(float left, float top, float right, float bottom, float reach, std::vector<std::uint32_t>& out) const
{
    constexpr float kWallCell = 23; // world units a wall cell
    // A world coordinate's cell, kept to just past the map's (find() clips them anyway).
    const auto cell = [](float world) {
        return static_cast<int>(std::clamp(std::floor(world / kWallCell), -2.0f, static_cast<float>(kCells + 1)));
    };
    find(cell(left - reach) - 1, cell(top - reach) - 1, cell(right + reach) + 1, cell(bottom + reach) + 1, out);
}

} // namespace noxsdl
