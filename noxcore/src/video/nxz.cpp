// After OpenNox libs nxz (MIT, Copyright (c) 2022 NoX World), checked against GAME.EXE; see
// THIRD_PARTY_NOTICES.txt.
#include "noxcore/video/nxz.hpp"

#include "noxcore/io/byte_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace nox::video {

namespace {

// Symbols: 0x00-0xFF literal bytes, 0x100-0x107 match lengths 4-11, 0x108-0x10F longer
// lengths (kLengths), then kRebuild and kEnd.
constexpr int kSymbols = 0x112;
constexpr std::uint16_t kRebuild = 0x110;
constexpr std::uint16_t kEnd = 0x111;
constexpr std::size_t kWindow = 0x10000;

struct BitsOffset {
    int bits;
    int offset;
};

// Initial code classes: 4-bit class -> index range.
constexpr BitsOffset kInitialClasses[16] = {
    {2, 0x00}, {3, 0x04}, {3, 0x0C}, {4, 0x14}, {4, 0x24}, {4, 0x34}, {4, 0x44}, {4, 0x54},
    {4, 0x64}, {4, 0x74}, {4, 0x84}, {4, 0x94}, {4, 0xA4}, {5, 0xB4}, {5, 0xD4}, {5, 0xF4},
};

// Long match lengths for symbols 0x108..0x10F.
constexpr BitsOffset kLengths[8] = {
    {1, 0x008}, {2, 0x00A}, {3, 0x00E}, {4, 0x016}, {5, 0x026}, {6, 0x046}, {7, 0x086}, {8, 0x106},
};

// Distance classes: 3-bit class, then bits + 9 more bits; offset in units of 512.
constexpr BitsOffset kDistances[8] = {
    {0, 0x00}, {0, 0x01}, {1, 0x02}, {2, 0x04}, {3, 0x08}, {4, 0x10}, {5, 0x20}, {6, 0x40},
};

/// A rebuild's model step, the decoder's and the encoder's alike: the symbols sorted by frequency
/// (descending count, then descending index) and the counts halved.
void resortSymbols(std::array<std::int16_t, kSymbols>& counts, std::array<std::uint16_t, kSymbols>& symbols)
{
    std::array<std::pair<std::int16_t, std::uint16_t>, kSymbols> order;
    for (int i = 0; i < kSymbols; ++i) {
        order[i] = {counts[i], static_cast<std::uint16_t>(i)};
        counts[i] = static_cast<std::int16_t>(counts[i] >> 1);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a > b; });
    for (int i = 0; i < kSymbols; ++i) {
        symbols[i] = order[i].second;
    }
}

// Initial symbol order: the 16 short lengths, then 0, ' ', '0', 0xFF, then the rest.
constexpr std::array<std::uint16_t, kSymbols> makeInitialSymbols()
{
    std::array<std::uint16_t, kSymbols> s{};
    int pos = 0;
    for (int i = 0; i < 16; ++i) {
        s[pos++] = static_cast<std::uint16_t>(0x100 + i);
    }
    for (std::uint16_t v : {std::uint16_t{0x00}, std::uint16_t{0x20}, std::uint16_t{0x30}, std::uint16_t{0xFF}}) {
        s[pos++] = v;
    }
    for (int v = 1; v <= 0x111; ++v) {
        if (v == 0x20 || v == 0x30 || v == 0xFF || (v >= 0x100 && v < 0x110)) {
            continue;
        }
        s[pos++] = static_cast<std::uint16_t>(v);
    }
    return s;
}

// The decoder: each symbol is a 4-bit class, then that class's bits give an index into
// symbols_, which is kept sorted by how often each symbol has been seen (rebuilt on kRebuild).
class Decoder {
public:
    Decoder(std::span<const std::uint8_t> in, std::size_t size)
        : in_(in)
    {
        // (Room set aside for what the input can plausibly hold, not for whatever size the caller
        // read from the data: a .nxz map's u32 or a bag index's says gigabytes in a few bytes.
        // More grows as it's decoded.)
        out_.reserve(std::min<std::size_t>(size, in.size() * 16 + 65536));
        std::copy(std::begin(kInitialClasses), std::end(kInitialClasses), classes_.begin());
    }

    std::vector<std::uint8_t> run(std::size_t size)
    {
        while (out_.size() < size) {
            // Decode one symbol: class, then its index within the class's range.
            const BitsOffset cls = classes_[bits(4)];
            const std::uint32_t index = cls.offset + (cls.bits ? bits(cls.bits) : 0);
            if (index >= kSymbols) {
                throw FormatError("nxz: invalid symbol index");
            }
            const std::uint16_t sym = symbols_[index];
            counts_[sym] = static_cast<std::int16_t>(counts_[sym] + 1);

            if (sym < 0x100) {
                put(static_cast<std::uint8_t>(sym));
            } else if (sym == kRebuild) {
                rebuild();
            } else if (sym == kEnd) {
                // End of a chunk, not of the data: the decoder's caller (the .nxz reader and
                // the like) calls again while input remains, with the model and window kept and the
                // bit accumulator cleared, so the next chunk
                // starts at the next unconsumed byte.
                if (out_.size() >= size || pos_ >= in_.size()) {
                    break;
                }
                acc_ = 0;
                count_ = 0;
            } else {
                copyMatch(sym, size);
            }
        }
        if (out_.size() != size) {
            throw FormatError("nxz: data ended before the expected size");
        }
        return std::move(out_);
    }

private:
    /// MSB-first bits from a 32-bit accumulator refilled a byte at a time.
    std::uint32_t bits(int n)
    {
        while (count_ < n) {
            if (pos_ >= in_.size()) {
                throw FormatError("nxz: compressed data ended early");
            }
            acc_ |= std::uint32_t{in_[pos_++]} << (24 - count_);
            count_ += 8;
        }
        const std::uint32_t v = acc_ >> (32 - n);
        acc_ <<= n;
        count_ -= n;
        return v;
    }

    void put(std::uint8_t b)
    {
        out_.push_back(b);
        // The window keeps the last 64 KiB written, for matches.
        window_[wpos_++ % kWindow] = b;
    }

    /// Re-sorts symbols by frequency (descending count, then index), halves the counts and
    /// reads new code classes as cumulative unary bit counts.
    void rebuild()
    {
        resortSymbols(counts_, symbols_);
        int width = 0;
        int offset = 0;
        // Each class's width is the previous one's plus the number of 0 bits before the next 1.
        for (auto& cls : classes_) {
            while (bits(1) == 0) {
                ++width;
            }
            cls = {width, offset};
            offset += 1 << width;
        }
    }

    void copyMatch(std::uint16_t sym, std::size_t size)
    {
        // Lengths start at 4: 0x100-0x107 add 0-7; 0x108-0x10F add kLengths' offset plus extra bits.
        std::uint32_t length = 4;
        if (sym < 0x108) {
            length += sym - 0x100;
        } else {
            const BitsOffset l = kLengths[sym - 0x108];
            length += l.offset + bits(l.bits);
        }
        // Distance: a 3-bit class, then (class bits + 9) bits added to the class's offset * 512.
        const BitsOffset d = kDistances[bits(3)];
        const std::uint32_t distance = bits(d.bits + 9) + d.offset * 0x200u;
        if (out_.size() + length > size) {
            throw FormatError("nxz: match runs past the expected size");
        }
        // Byte by byte, so overlapping matches repeat as in the game.
        for (std::uint32_t i = 0; i < length; ++i) {
            put(window_[(wpos_ - distance) % kWindow]);
        }
    }

    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    std::uint32_t acc_ = 0;
    int count_ = 0;

    std::vector<std::uint8_t> out_;
    std::array<std::uint8_t, kWindow> window_{};
    std::uint32_t wpos_ = 0;

    std::array<std::uint16_t, kSymbols> symbols_ = makeInitialSymbols();
    std::array<std::int16_t, kSymbols> counts_{};
    std::array<BitsOffset, 16> classes_{};
};

} // namespace

// The encoder keeps the decoder's model (symbols_, counts_, classes_) and performs the same
// updates, so the two stay in step by construction. Its free choices are when to emit a rebuild
// (here: on a doubling schedule up to every 4096 symbols, so the code adapts early and the
// s16 counts stay far from overflow) and the 16 class widths a rebuild announces (here: the
// non-decreasing widths minimising the expected bits for the current counts, by a small DP).
// Matches come from a hash-chain search and are taken only when their exact bit cost under the
// current classes beats the literals they replace.
namespace {

constexpr std::size_t kMaxMatch = 4 + 0x106 + 0xFF; // 521: symbol 0x10F's farthest reach

class Encoder {
public:
    explicit Encoder(std::span<const std::uint8_t> in)
        : in_(in)
    {
        std::copy(std::begin(kInitialClasses), std::end(kInitialClasses), classes_.begin());
        refreshLookups();
        head_.assign(std::size_t{1} << kHashBits, -1);
        prev_.assign(in_.size(), -1);
    }

    // Greedy: at each position take the best match if it costs no more bits than its literals.
    std::vector<std::uint8_t> run()
    {
        std::size_t pos = 0;
        while (pos < in_.size()) {
            std::size_t len = 0;
            std::uint32_t dist = 0;
            findMatch(pos, len, dist);
            if (len >= 4 && matchCost(len, dist) <= literalsCost(pos, len)) {
                emitMatch(len, dist);
                // Matched positions go into the hash chains too (all but the last 3 bytes, which can't hash 4).
                for (std::size_t i = 0; i < len && pos + i + 4 <= in_.size(); ++i) {
                    insert(pos + i);
                }
                pos += len;
            } else {
                emitSymbol(in_[pos]);
                if (pos + 4 <= in_.size()) {
                    insert(pos);
                }
                ++pos;
            }
            if (sinceRebuild_ >= rebuildAt_) {
                rebuild();
            }
        }
        emitSymbol(kEnd);
        // Flush the last partial byte, padded with zero bits.
        if (pending_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(acc_ << (8 - pending_)));
            pending_ = 0;
        }
        return std::move(out_);
    }

private:
    // --- bit writer (MSB-first, as the decoder reads) ---

    void putBits(std::uint32_t v, int n)
    {
        if (n == 0) {
            return;
        }
        acc_ = (acc_ << n) | (v & ((1u << n) - 1));
        pending_ += n;
        while (pending_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(acc_ >> (pending_ - 8)));
            pending_ -= 8;
        }
    }

    // --- model, in step with the decoder ---

    /// The symbol's class and index under the current table, then its count up one.
    void emitSymbol(std::uint16_t sym)
    {
        const std::uint16_t index = indexOf_[sym];
        const std::uint8_t c = classOfIndex_[index];
        putBits(c, 4);
        putBits(static_cast<std::uint32_t>(index) - static_cast<std::uint32_t>(classes_[c].offset), classes_[c].bits);
        counts_[sym] = static_cast<std::int16_t>(counts_[sym] + 1);
        ++sinceRebuild_;
    }

    int symbolCost(std::uint16_t sym) const { return 4 + classes_[classOfIndex_[indexOf_[sym]]].bits; }

    /// Mirrors Decoder::rebuild: kRebuild through the old classes, the same sort and halving,
    /// then the widths this encoder chose, as cumulative unary.
    void rebuild()
    {
        emitSymbol(kRebuild);
        resortSymbols(counts_, symbols_);
        const std::array<int, 16> widths = chooseWidths();
        int width = 0;
        int offset = 0;
        for (int c = 0; c < 16; ++c) {
            putBits(0, widths[c] - width);
            putBits(1, 1);
            width = widths[c];
            classes_[c] = {width, offset};
            offset += 1 << width;
        }
        refreshLookups();
        sinceRebuild_ = 0;
        rebuildAt_ = std::min(rebuildAt_ * 2, 4096);
    }

    /// Non-decreasing class widths covering every index, minimising sum(count x (4 + width))
    /// for the counts as just halved and ordered: f(c, covered, width) by exhaustion, widths
    /// at most kMaxWidth (one class of 2^9 already covers all 274 indexes).
    static constexpr int kMaxWidth = 9;
    std::array<int, 16> chooseWidths() const
    {
        std::array<std::int32_t, kSymbols + 1> prefix{};
        for (int i = 0; i < kSymbols; ++i) {
            prefix[i + 1] = prefix[i] + counts_[symbols_[i]];
        }
        const auto rangeCount = [&](int from, int to) {
            return prefix[std::clamp(to, 0, kSymbols)] - prefix[std::clamp(from, 0, kSymbols)];
        };
        // best[c][w]: classes c..15 all of width >= w, from coverage kSymbols (clamped); filled
        // for every starting offset by walking offsets backwards would be large, but coverage
        // only matters clamped to kSymbols, and each width choice fixes the offsets, so search
        // states are (class, clamped offset, width). Memoised over a dense cube.
        constexpr int kOffsets = kSymbols + 1; // clamped
        static constexpr std::int64_t kInf = std::numeric_limits<std::int64_t>::max() / 2;
        std::vector<std::int64_t> best(std::size_t{17} * kOffsets * (kMaxWidth + 1), -1);
        std::vector<std::int8_t> pick(best.size(), -1);
        const auto at = [&](int c, int off, int w) -> std::size_t {
            return (static_cast<std::size_t>(c) * kOffsets + static_cast<std::size_t>(off)) * (kMaxWidth + 1) +
                   static_cast<std::size_t>(w);
        };
        const auto solve = [&](auto&& self, int c, int off, int w) -> std::int64_t {
            if (c == 16) {
                return off >= kSymbols ? 0 : kInf;
            }
            std::int64_t& memo = best[at(c, off, w)];
            if (memo >= 0) {
                return memo;
            }
            memo = kInf;
            for (int pw = w; pw <= kMaxWidth; ++pw) {
                const int next = std::min(off + (1 << pw), kSymbols);
                const std::int64_t cost = static_cast<std::int64_t>(4 + pw) * rangeCount(off, next);
                const std::int64_t rest = self(self, c + 1, next, pw);
                if (rest < kInf && cost + rest < memo) {
                    memo = cost + rest;
                    pick[at(c, off, w)] = static_cast<std::int8_t>(pw);
                }
            }
            return memo;
        };
        solve(solve, 0, 0, 0);
        std::array<int, 16> widths{};
        int off = 0;
        int w = 0;
        for (int c = 0; c < 16; ++c) {
            w = pick[at(c, off, w)];
            widths[c] = w;
            off = std::min(off + (1 << w), kSymbols);
        }
        return widths;
    }

    /// indexOf_ and classOfIndex_ anew from symbols_ and classes_.
    void refreshLookups()
    {
        for (int i = 0; i < kSymbols; ++i) {
            indexOf_[symbols_[i]] = static_cast<std::uint16_t>(i);
        }
        for (int c = 0; c < 16; ++c) {
            const int from = classes_[c].offset;
            const int to = std::min(from + (1 << classes_[c].bits), kSymbols);
            for (int i = from; i < to; ++i) {
                classOfIndex_[i] = static_cast<std::uint8_t>(c);
            }
        }
    }

    // --- matches ---

    static constexpr int kHashBits = 15;
    static constexpr int kMaxChain = 64;

    std::uint32_t hashAt(std::size_t pos) const
    {
        std::uint32_t v;
        std::memcpy(&v, in_.data() + pos, 4);
        return (v * 0x9E3779B1u) >> (32 - kHashBits);
    }

    void insert(std::size_t pos)
    {
        const std::uint32_t h = hashAt(pos);
        prev_[pos] = head_[h];
        head_[h] = static_cast<std::int32_t>(pos);
    }

    // Longest earlier match for in_[pos..] within 64 KiB, looking at most kMaxChain candidates.
    void findMatch(std::size_t pos, std::size_t& bestLen, std::uint32_t& bestDist) const
    {
        bestLen = 0;
        if (pos + 4 > in_.size()) {
            return;
        }
        const std::size_t most = std::min(kMaxMatch, in_.size() - pos);
        std::int32_t cand = head_[hashAt(pos)];
        for (int depth = 0; depth < kMaxChain && cand >= 0; ++depth, cand = prev_[static_cast<std::size_t>(cand)]) {
            const std::size_t c = static_cast<std::size_t>(cand);
            if (pos - c > 0xFFFF) {
                break;
            }
            // A candidate can only beat bestLen if it also matches at bestLen; skip it cheaply otherwise.
            if (bestLen != 0 && in_[c + bestLen] != in_[pos + bestLen]) {
                continue;
            }
            std::size_t len = 0;
            while (len < most && in_[c + len] == in_[pos + len]) {
                ++len;
            }
            if (len > bestLen) { // chains are nearest-first, so a tie keeps the shorter distance
                bestLen = len;
                bestDist = static_cast<std::uint32_t>(pos - c);
                if (len == most) {
                    break;
                }
            }
        }
    }

    int literalsCost(std::size_t pos, std::size_t len) const
    {
        int cost = 0;
        for (std::size_t i = 0; i < len; ++i) {
            cost += symbolCost(in_[pos + i]);
        }
        return cost;
    }

    /// Length symbol (0x100.. plain, 0x108.. with kLengths' extra bits), then the 3-bit distance
    /// class and its kDistances[d].bits + 9 bits.
    static std::uint16_t lengthSymbol(std::size_t len, int& extraBits, std::uint32_t& extra)
    {
        const std::uint32_t l = static_cast<std::uint32_t>(len) - 4;
        if (l < 8) {
            extraBits = 0;
            extra = 0;
            return static_cast<std::uint16_t>(0x100 + l);
        }
        for (int j = 0;; ++j) {
            if (l < static_cast<std::uint32_t>(kLengths[j].offset) + (1u << kLengths[j].bits)) {
                extraBits = kLengths[j].bits;
                extra = l - static_cast<std::uint32_t>(kLengths[j].offset);
                return static_cast<std::uint16_t>(0x108 + j);
            }
        }
    }

    static int distanceClass(std::uint32_t dist)
    {
        for (int c = 0;; ++c) {
            if (dist < static_cast<std::uint32_t>(kDistances[c].offset) * 0x200u + (1u << (kDistances[c].bits + 9))) {
                return c;
            }
        }
    }

    int matchCost(std::size_t len, std::uint32_t dist) const
    {
        int extraBits = 0;
        std::uint32_t extra = 0;
        const std::uint16_t sym = lengthSymbol(len, extraBits, extra);
        return symbolCost(sym) + extraBits + 3 + 9 + kDistances[distanceClass(dist)].bits;
    }

    void emitMatch(std::size_t len, std::uint32_t dist)
    {
        int extraBits = 0;
        std::uint32_t extra = 0;
        const std::uint16_t sym = lengthSymbol(len, extraBits, extra);
        emitSymbol(sym);
        putBits(extra, extraBits);
        const int d = distanceClass(dist);
        putBits(static_cast<std::uint32_t>(d), 3);
        putBits(dist - static_cast<std::uint32_t>(kDistances[d].offset) * 0x200u, kDistances[d].bits + 9);
    }

    std::span<const std::uint8_t> in_;
    std::vector<std::uint8_t> out_;
    std::uint32_t acc_ = 0;
    int pending_ = 0;

    std::array<std::uint16_t, kSymbols> symbols_ = makeInitialSymbols();
    std::array<std::int16_t, kSymbols> counts_{};
    std::array<BitsOffset, 16> classes_{};
    std::array<std::uint16_t, kSymbols> indexOf_{};
    std::array<std::uint8_t, kSymbols> classOfIndex_{};
    int sinceRebuild_ = 0;
    int rebuildAt_ = 512;

    std::vector<std::int32_t> head_;
    std::vector<std::int32_t> prev_;
};

} // namespace

std::vector<std::uint8_t> nxzDecompress(std::span<const std::uint8_t> data, std::size_t size)
{
    return Decoder(data, size).run(size);
}

std::vector<std::uint8_t> nxzCompress(std::span<const std::uint8_t> data)
{
    return Encoder(data).run();
}

} // namespace nox::video
