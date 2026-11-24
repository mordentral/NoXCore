// HD Spell FX's water base (water_base.hpp).
#include "water_base.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace noxsdl::water {

namespace {

constexpr std::array<float, 3> kLuma{0.299f, 0.587f, 0.114f};

int floorMod(int a, int m)
{
    const int r = a % m;
    return r < 0 ? r + m : r;
}

// A box blur of radius r over a picture that repeats (side x side, 3 floats a texel), in place.
void boxBlur(std::vector<float>& img, int side, int r)
{
    std::vector<float> tmp(img.size());
    const float n = static_cast<float>(2 * r + 1);
    for (int pass = 0; pass < 2; ++pass) { // across, then down
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                for (int c = 0; c < 3; ++c) {
                    float sum = 0;
                    for (int k = -r; k <= r; ++k) {
                        const int sx = pass == 0 ? floorMod(x + k, side) : x;
                        const int sy = pass == 0 ? y : floorMod(y + k, side);
                        sum += img[(static_cast<std::size_t>(sy) * side + sx) * 3 + c];
                    }
                    tmp[(static_cast<std::size_t>(y) * side + x) * 3 + c] = sum / n;
                }
            }
        }
        img.swap(tmp);
    }
}

} // namespace

std::optional<Base> buildBase(std::span<const std::uint8_t* const> images, int cols, int rows, int frames)
{
    if (cols <= 0 || cols != rows || frames <= 0 || images.size() != static_cast<std::size_t>(cols * rows * frames) ||
        std::ranges::any_of(images, [](const std::uint8_t* p) { return p == nullptr; })) {
        return std::nullopt;
    }
    const int period = kTile * cols;
    const int half = kTile / 2;
    Base out;
    out.period = period;
    out.frames = frames;
    const int block = period + 2 * kApron;
    out.block = block;
    out.width = 2 * block;
    out.height = frames * block;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 255);
    out.lo = {1, 1, 1};
    out.hi = {0, 0, 0};
    const std::size_t texels = static_cast<std::size_t>(period) * period;
    std::vector<std::vector<float>> pictures(static_cast<std::size_t>(frames));
    std::vector<std::vector<float>> lows(static_cast<std::size_t>(frames));
    for (int f = 0; f < frames; ++f) {
        auto& pic = pictures[static_cast<std::size_t>(f)];
        pic.assign(texels * 3, 0);
        for (int y = 0; y < period; ++y) {
            for (int x = 0; x < period; ++x) {
                // The tile whose diamond holds the texel's middle: diamond centres sit at
                // (12, 35) + u (23, 23) + v (23, -23), anchor (0, 0). A pixel on the diamond's
                // stepped rim may be clear in that tile's image: then a neighbour's.
                const float qx = static_cast<float>(x) + 0.5f - 12;
                const float qy = static_cast<float>(y) + 0.5f - 35;
                const int u0 = static_cast<int>(std::floor((qx + qy) / kTile + 0.5f));
                const int v0 = static_cast<int>(std::floor((qx - qy) / kTile + 0.5f));
                const std::uint8_t* px = nullptr;
                for (const auto [du, dv] : {std::pair{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int u = u0 + du;
                    const int v = v0 + dv;
                    const int gx = u + v;
                    const int gy = u - v;
                    const int lx = x - (half * gx - 11);
                    const int ly = y - (half * gy + 12);
                    if (lx < 0 || ly < 0 || lx >= kTile || ly >= kTile) {
                        continue;
                    }
                    const int variant = floorMod(u, cols) + cols * floorMod(-v, rows);
                    const std::uint8_t* p = images[static_cast<std::size_t>(f * cols * rows + variant)] +
                                            (static_cast<std::size_t>(ly) * kTile + lx) * 4;
                    if (p[3] != 0) {
                        px = p;
                        break;
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    const float value = px ? px[c] / 255.0f : 0.0f;
                    pic[(static_cast<std::size_t>(y) * period + x) * 3 + c] = value;
                    out.lo[c] = std::min(out.lo[c], value);
                    out.hi[c] = std::max(out.hi[c], value);
                }
            }
        }
        auto& low = lows[static_cast<std::size_t>(f)];
        low = pic;
        boxBlur(low, period, 2);
        boxBlur(low, period, 2);
        // Each block with its apron: texel (x, y) of the block is the picture's (x - kApron, y -
        // kApron), wrapped.
        for (int y = 0; y < block; ++y) {
            for (int x = 0; x < block; ++x) {
                const std::size_t i = static_cast<std::size_t>(floorMod(y - kApron, period)) * period + floorMod(x - kApron, period);
                std::uint8_t* a = &out.rgba[((static_cast<std::size_t>(f) * block + y) * out.width + x) * 4];
                std::uint8_t* b = a + static_cast<std::size_t>(block) * 4;
                for (int c = 0; c < 3; ++c) {
                    a[c] = static_cast<std::uint8_t>(std::lround(pic[i * 3 + c] * 255));
                    b[c] = static_cast<std::uint8_t>(std::lround(std::clamp(low[i * 3 + c], 0.0f, 1.0f) * 255));
                }
            }
        }
    }

    // The detail (picture less low-pass): its colours' covariance, its main direction (power
    // iteration), its luminance's spread, and each frame's likeness to the next.
    std::array<double, 9> cov{};
    std::array<double, 3> mean{};
    const double count = static_cast<double>(texels) * frames;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < texels * 3; ++i) {
            mean[i % 3] += pictures[static_cast<std::size_t>(f)][i] - lows[static_cast<std::size_t>(f)][i];
        }
    }
    for (double& m : mean) {
        m /= count;
    }
    std::vector<std::vector<double>> luma(static_cast<std::size_t>(frames), std::vector<double>(texels));
    double lumaSq = 0;
    double lumaSum = 0;
    for (int f = 0; f < frames; ++f) {
        const auto& pic = pictures[static_cast<std::size_t>(f)];
        const auto& low = lows[static_cast<std::size_t>(f)];
        for (std::size_t i = 0; i < texels; ++i) {
            std::array<double, 3> d{};
            double l = 0;
            for (int c = 0; c < 3; ++c) {
                d[c] = pic[i * 3 + c] - low[i * 3 + c] - mean[c];
                l += kLuma[c] * (pic[i * 3 + c] - low[i * 3 + c]);
            }
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    cov[r * 3 + c] += d[r] * d[c];
                }
            }
            luma[static_cast<std::size_t>(f)][i] = l;
            lumaSum += l;
            lumaSq += l * l;
        }
    }
    std::array<double, 3> dir{1, 1, 1};
    for (int it = 0; it < 64; ++it) {
        std::array<double, 3> next{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                next[r] += cov[r * 3 + c] * dir[c];
            }
        }
        const double len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (len <= 0) {
            break;
        }
        for (int c = 0; c < 3; ++c) {
            dir[c] = next[c] / len;
        }
    }
    double dirLuma = 0;
    for (int c = 0; c < 3; ++c) {
        dirLuma += kLuma[c] * dir[c];
    }
    if (std::abs(dirLuma) < 1e-6) {
        dir = {1, 1, 1};
        dirLuma = 1;
    }
    for (int c = 0; c < 3; ++c) {
        out.grain[c] = static_cast<float>(dir[c] / dirLuma);
    }
    const double lumaMean = lumaSum / count;
    out.detail = static_cast<float>(std::sqrt(std::max(0.0, lumaSq / count - lumaMean * lumaMean)));
    double rho = 0;
    for (int f = 0; f < frames; ++f) {
        const auto& a = luma[static_cast<std::size_t>(f)];
        const auto& b = luma[static_cast<std::size_t>((f + 1) % frames)];
        double ma = 0, mb = 0;
        for (std::size_t i = 0; i < texels; ++i) {
            ma += a[i];
            mb += b[i];
        }
        ma /= static_cast<double>(texels);
        mb /= static_cast<double>(texels);
        double ab = 0, aa = 0, bb = 0;
        for (std::size_t i = 0; i < texels; ++i) {
            ab += (a[i] - ma) * (b[i] - mb);
            aa += (a[i] - ma) * (a[i] - ma);
            bb += (b[i] - mb) * (b[i] - mb);
        }
        rho += aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 1.0;
    }
    out.rho = frames > 1 ? static_cast<float>(std::clamp(rho / frames, 0.0, 1.0)) : 1.0f;
    return out;
}

std::pair<int, int> learnAnchor(std::span<const PlacedTile> tiles, int cols, int rows)
{
    if (cols <= 0 || rows <= 0) {
        return {0, 0};
    }
    std::map<std::pair<int, int>, int> votes;
    for (const PlacedTile& t : tiles) {
        // Full-grid place from the top-left (23 gx - 11, 23 gy + 12); u and v from it.
        const int gx = (t.x + 11) / (kTile / 2);
        const int gy = (t.y - 12) / (kTile / 2);
        if (((gx + gy) & 1) != 0) {
            continue; // (not on the tiles' grid)
        }
        const int u = (gx + gy) / 2;
        const int v = (gx - gy) / 2;
        const int col = floorMod(t.variant, cols);
        const int row = floorMod(t.variant / cols, rows);
        ++votes[{floorMod(col - u, cols), floorMod(row + v, rows)}];
    }
    std::pair<int, int> best{0, 0};
    int most = 0;
    for (const auto& [anchor, n] : votes) {
        if (n > most) {
            best = anchor;
            most = n;
        }
    }
    return best;
}

PlacedTile Coverage::placedOf(const Tile& t)
{
    return {static_cast<int>(std::lround(t.x)), static_cast<int>(std::lround(t.y)), t.variant};
}

void Coverage::mark(const Tile& t, int delta)
{
    if (!t.wet) {
        return;
    }
    wet_ = delta > 0 ? wet_ + 1 : wet_ - 1;
    // (Full-grid place from the top-left (23 gx - 11, 23 gy + 12), one past for gy -1.)
    constexpr float kHalf = static_cast<float>(kTile / 2);
    const int gx = static_cast<int>(std::lround((t.x + 11) / kHalf));
    const int gy = static_cast<int>(std::lround((t.y - 12) / kHalf)) + 1;
    if (gx >= 0 && gy >= 0 && gx < kGrid && gy < kGrid) {
        auto& cell = grid_[static_cast<std::size_t>(gy) * kGrid + static_cast<std::size_t>(gx)];
        cell = static_cast<std::uint8_t>(cell + delta);
    }
}

void Coverage::learn(std::span<const PlacedTile> placed, int cols, int rows)
{
    const auto [a, b] = learnAnchor(placed, cols, rows);
    offset_ = anchorOffset(a, b, kTile * cols);
}

bool Coverage::change(const std::optional<Tile>& before, const std::optional<Tile>& after)
{
    if (before) {
        mark(*before, -1);
    }
    if (after) {
        mark(*after, 1);
    }
    const bool wasBase = before && before->base;
    const bool isBase = after && after->base;
    if (wasBase != isBase) {
        return true;
    }
    if (!wasBase) {
        return false; // (neither is one of the anchor's)
    }
    const PlacedTile b = placedOf(*before);
    const PlacedTile a = placedOf(*after);
    return b.x != a.x || b.y != a.y || b.variant != a.variant;
}

std::pair<int, int> anchorOffset(int a, int b, int period)
{
    // (a, b) moves the variants a steps along u and -b along v: u is (23, 23) in the world, v (23, -23).
    const int half = kTile / 2;
    if (period <= 0) {
        return {0, 0};
    }
    return {floorMod(half * (a - b), period), floorMod(half * (a + b), period)};
}

} // namespace noxsdl::water
