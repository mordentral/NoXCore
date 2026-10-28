// The generator's dense cell set (cell_set.hpp).
#include "noxcore/mapgen/cell_set.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace nox::mapgen {

CellSet::CellSet(CellSet&& o) noexcept
    : bits_(std::move(o.bits_))
    , count_(o.count_)
    , begin_(o.begin_)
    , end_(o.end_)
{
    o.bits_.clear();
    o.count_ = 0;
    o.begin_ = kTotal;
    o.end_ = 0;
}

CellSet& CellSet::operator=(CellSet&& o) noexcept
{
    if (this != &o) {
        bits_ = std::move(o.bits_);
        count_ = o.count_;
        begin_ = o.begin_;
        end_ = o.end_;
        o.bits_.clear();
        o.count_ = 0;
        o.begin_ = kTotal;
        o.end_ = 0;
    }
    return *this;
}

bool CellSet::insert(Cell c)
{
    if (!fits(c)) {
        throw std::out_of_range("mapgen: a cell past the grid (" + std::to_string(c.first) + ", " + std::to_string(c.second) + ")");
    }
    const std::size_t x = static_cast<std::size_t>(c.first - kOrigin);
    const std::size_t y = static_cast<std::size_t>(c.second - kOrigin);
    const std::size_t w = x * kWords + y / 64;
    const std::uint64_t bit = std::uint64_t{1} << (y % 64);
    if (bits_.empty()) {
        bits_.assign(kTotal, 0);
    } else if ((bits_[w] & bit) != 0) {
        return false;
    }
    bits_[w] |= bit;
    ++count_;
    begin_ = std::min(begin_, w);
    end_ = std::max(end_, w + 1);
    return true;
}

bool CellSet::erase(Cell c) noexcept
{
    if (!fits(c) || bits_.empty()) {
        return false;
    }
    const std::size_t x = static_cast<std::size_t>(c.first - kOrigin);
    const std::size_t y = static_cast<std::size_t>(c.second - kOrigin);
    const std::size_t w = x * kWords + y / 64;
    const std::uint64_t bit = std::uint64_t{1} << (y % 64);
    if ((bits_[w] & bit) == 0) {
        return false;
    }
    bits_[w] &= ~bit;
    --count_;
    return true;
}

void CellSet::clear() noexcept
{
    if (begin_ < end_) { // (nothing ever in it: no bits to clear)
        std::fill(bits_.begin() + static_cast<std::ptrdiff_t>(begin_), bits_.begin() + static_cast<std::ptrdiff_t>(end_), 0);
    }
    count_ = 0;
    begin_ = bits_.size();
    end_ = 0;
}

bool CellSet::operator==(const CellSet& o) const noexcept
{
    // (Both empty: equal, whether or not either has its bits yet. Else both have them.)
    return count_ == o.count_ && (count_ == 0 || bits_ == o.bits_);
}

CellMap::CellMap(const CellMap& o)
    : keys_(o.keys_)
{
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (o.pages_[i]) {
            pages_[i] = std::make_unique<Page>(*o.pages_[i]);
        }
    }
}

CellMap& CellMap::operator=(const CellMap& o)
{
    if (this != &o) {
        CellMap copy(o);
        *this = std::move(copy);
    }
    return *this;
}

void CellMap::set(Cell c, int value)
{
    keys_.insert(c); // (throws past the grid)
    const auto x = static_cast<std::size_t>(c.first - kOrigin), y = static_cast<std::size_t>(c.second - kOrigin);
    auto& page = pages_[(x / kPage) * kPages + y / kPage];
    if (!page) {
        page = std::make_unique<Page>();
        page->fill(-1);
    }
    (*page)[(x % kPage) * kPage + y % kPage] = static_cast<std::int16_t>(value);
}

void CellMap::erase(Cell c) noexcept
{
    if (keys_.erase(c)) {
        const auto x = static_cast<std::size_t>(c.first - kOrigin), y = static_cast<std::size_t>(c.second - kOrigin);
        (*pages_[(x / kPage) * kPages + y / kPage])[(x % kPage) * kPage + y % kPage] = -1;
    }
}

std::vector<Cell> CellSet::list() const
{
    std::vector<Cell> out;
    out.reserve(count_);
    for (const Cell c : *this) {
        out.push_back(c);
    }
    return out;
}

} // namespace nox::mapgen
