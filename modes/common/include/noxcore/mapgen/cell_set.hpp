// A set of wall-grid cells held densely: the generator's
// floor and the like, the type its native building blocks and its scripts share. It reads like the
// std::set<Cell> it replaced (contains, insert, erase, size, walked in the same order: x, then y),
// at a bit a cell. Its bits are made at its first insert (kBytes), so an empty set costs nothing; a
// moved-from set is an empty one.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace nox::mapgen {

using Cell = std::pair<int, int>;

class CellSet {
public:
    /// The cells it can hold: kOrigin to kOrigin + kSide - 1 on both axes (the map's grid and a
    /// wide margin round it).
    static constexpr int kOrigin = -128;
    static constexpr int kSide = 512;

    /// The bits a set holds once anything has gone in.
    static constexpr std::size_t kBytes = static_cast<std::size_t>(kSide) * (kSide / 64) * sizeof(std::uint64_t);

    CellSet() noexcept = default;
    CellSet(const CellSet&) = default;
    CellSet& operator=(const CellSet&) = default;
    CellSet(CellSet&& o) noexcept;
    CellSet& operator=(CellSet&& o) noexcept;

    static bool fits(Cell c) noexcept
    {
        return c.first >= kOrigin && c.first < kOrigin + kSide && c.second >= kOrigin && c.second < kOrigin + kSide;
    }
    bool contains(Cell c) const noexcept
    {
        if (!fits(c) || bits_.empty()) {
            return false;
        }
        const std::size_t x = static_cast<std::size_t>(c.first - kOrigin);
        const std::size_t y = static_cast<std::size_t>(c.second - kOrigin);
        return ((bits_[x * kWords + y / 64] >> (y % 64)) & 1u) != 0;
    }
    /// True when it wasn't there; throws std::out_of_range for a cell past the grid.
    bool insert(Cell c);
    /// True when it was there.
    bool erase(Cell c) noexcept;
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    void clear() noexcept;
    bool operator==(const CellSet& o) const noexcept;

    /// Its cells in order (x, then y), read only.
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Cell;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Cell;

        const_iterator() = default;
        Cell operator*() const noexcept
        {
            return {static_cast<int>(word_ / kWords) + kOrigin, static_cast<int>((word_ % kWords) * 64 + std::countr_zero(rest_)) + kOrigin};
        }
        const_iterator& operator++() noexcept
        {
            rest_ &= rest_ - 1;
            settle();
            return *this;
        }
        const_iterator operator++(int) noexcept
        {
            const_iterator was = *this;
            ++*this;
            return was;
        }
        bool operator==(const const_iterator& o) const noexcept { return word_ == o.word_ && rest_ == o.rest_; }

    private:
        friend class CellSet;
        const_iterator(const CellSet* set, std::size_t word) noexcept : set_(set), word_(word)
        {
            rest_ = word_ < set_->bits_.size() ? set_->bits_[word_] : 0;
            settle();
        }
        void settle() noexcept
        {
            while (rest_ == 0 && word_ < set_->end_) {
                ++word_;
                rest_ = word_ < set_->end_ ? set_->bits_[word_] : 0;
            }
            if (rest_ == 0) {
                word_ = set_->bits_.size();
            }
        }
        const CellSet* set_ = nullptr;
        std::size_t word_ = 0;
        std::uint64_t rest_ = 0;
    };
    using iterator = const_iterator;

    const_iterator begin() const noexcept { return const_iterator(this, begin_); }
    const_iterator end() const noexcept
    {
        const_iterator it;
        it.set_ = this;
        it.word_ = bits_.size();
        return it;
    }
    std::vector<Cell> list() const;

private:
    static constexpr std::size_t kWords = kSide / 64; ///< words a column
    static constexpr std::size_t kTotal = static_cast<std::size_t>(kSide) * kWords;
    std::vector<std::uint64_t> bits_; ///< none till the first insert, then kTotal
    std::size_t count_ = 0;
    std::size_t begin_ = kTotal; ///< the first word that may be set
    std::size_t end_ = 0;        ///< one past the last
};

/// A small whole number for some cells (a wall's shape, a cell's floor): -1 for none. Held in
/// pages of 64 x 64 cells, made as they're first written.
class CellMap {
public:
    static constexpr int kOrigin = CellSet::kOrigin;
    static constexpr int kSide = CellSet::kSide;

    CellMap() = default;
    CellMap(const CellMap& o);
    CellMap& operator=(const CellMap& o);
    CellMap(CellMap&&) noexcept = default;
    CellMap& operator=(CellMap&&) noexcept = default;

    int get(Cell c) const noexcept
    {
        if (!CellSet::fits(c)) {
            return -1;
        }
        const auto x = static_cast<std::size_t>(c.first - kOrigin), y = static_cast<std::size_t>(c.second - kOrigin);
        const auto& page = pages_[(x / kPage) * kPages + y / kPage];
        return page ? (*page)[(x % kPage) * kPage + y % kPage] : -1;
    }
    bool has(Cell c) const noexcept { return get(c) >= 0; }
    /// A value from 0 to 32767; throws std::out_of_range for a cell past the grid.
    void set(Cell c, int value);
    void erase(Cell c) noexcept;
    std::size_t size() const noexcept { return keys_.size(); }
    /// The cells it holds, as a set (walked x, then y).
    const CellSet& keys() const noexcept { return keys_; }

private:
    static constexpr std::size_t kPage = 64;
    static constexpr std::size_t kPages = static_cast<std::size_t>(kSide) / kPage;
    using Page = std::array<std::int16_t, kPage * kPage>;
    std::array<std::unique_ptr<Page>, kPages * kPages> pages_;
    CellSet keys_;

public:
    /// The most its pages can hold (every one made).
    static constexpr std::size_t kMostPageBytes = kPages * kPages * sizeof(Page);
};

} // namespace nox::mapgen
