#include "noxcore/video/tile_edges.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace nox::video {

namespace {

// The tuning (matched by eye against the upscaled floors): the blur that keeps a border crisp and
// its contrast curve, the wider blur a dither is read through, and how many lone pixels round a
// pixel make a dither.
constexpr float kSharpSigma = 0.5f;
constexpr float kSharpLow = 0.15f;
constexpr float kSharpHigh = 0.6f;
constexpr float kSoftSigma = 1.0f;
constexpr float kDitherSigma = 1.2f;
constexpr float kDitherFrom = 0.1f;
constexpr float kDitherSpan = 0.15f;

/// A separable Gaussian blur of a side x side map, the edges clamped.
std::vector<float> blur(const std::vector<float>& src, int side, float sigma)
{
    const int r = static_cast<int>(std::ceil(3 * sigma));
    std::vector<float> k(static_cast<std::size_t>(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i) {
        k[static_cast<std::size_t>(i + r)] = std::exp(-static_cast<float>(i * i) / (2 * sigma * sigma));
        sum += k[static_cast<std::size_t>(i + r)];
    }
    for (float& v : k) {
        v /= sum;
    }
    const auto at = [side](int x, int y) { return static_cast<std::size_t>(std::clamp(y, 0, side - 1)) * side + std::clamp(x, 0, side - 1); };
    std::vector<float> tmp(src.size());
    std::vector<float> out(src.size());
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            float v = 0;
            for (int i = -r; i <= r; ++i) {
                v += k[static_cast<std::size_t>(i + r)] * src[at(x + i, y)];
            }
            tmp[at(x, y)] = v;
        }
    }
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            float v = 0;
            for (int i = -r; i <= r; ++i) {
                v += k[static_cast<std::size_t>(i + r)] * tmp[at(x, y + i)];
            }
            out[at(x, y)] = v;
        }
    }
    return out;
}

/// A blur over the inside only (normalised by how much of the inside it covered), so values
/// outside count for nothing and the inside's carry on past its rim.
std::vector<float> blurInside(const std::vector<float>& src, const std::vector<float>& inside, int side, float sigma)
{
    std::vector<float> masked(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        masked[i] = src[i] * inside[i];
    }
    const std::vector<float> num = blur(masked, side, sigma);
    const std::vector<float> den = blur(inside, side, sigma);
    std::vector<float> out(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        out[i] = den[i] > 1e-4f ? num[i] / den[i] : 0.0f;
    }
    return out;
}

/// Catmull-Rom weights of the four samples round a point t (0..1) past the second.
void cubicWeights(float t, float w[4])
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2 * t2 - t);
    w[1] = 0.5f * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5f * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

float smoothstep(float low, float high, float v)
{
    const float t = clamp01((v - low) / (high - low));
    return t * t * (3 - 2 * t);
}

} // namespace

EdgeWeights edgeWeights(std::span<const std::uint8_t> mask, int side, int scale, std::span<const std::uint8_t> inside)
{
    const auto cells = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    if (side <= 0 || scale <= 0 || mask.size() != cells || (!inside.empty() && inside.size() != cells)) {
        throw std::invalid_argument("edgeWeights: mask or inside isn't side x side");
    }
    std::vector<float> in(cells, 1.0f);
    for (std::size_t i = 0; i < inside.size(); ++i) {
        in[i] = inside[i] ? 1.0f : 0.0f;
    }
    // How much a pixel's neighbourhood dithers, 0 (a plain border) .. 1 (a dither): how many pixels
    // round it are a dither's: differing from 3 or 4 of their (inside) 4-neighbours, alone or in a
    // checker. A border, even a stepped one, or a line one pixel wide (straight or diagonal) isn't.
    std::vector<float> differ(cells, 0.0f);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * side + x;
            const auto count = [&](std::initializer_list<std::pair<int, int>> offsets, bool same, int& n) {
                int c = 0;
                for (const auto& [dx, dy] : offsets) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side || in[static_cast<std::size_t>(ny) * side + nx] == 0) {
                        continue;
                    }
                    ++n;
                    c += (mask[static_cast<std::size_t>(ny) * side + nx] == mask[i]) == same ? 1 : 0;
                }
                return c;
            };
            int n = 0;
            int diagonals = 0;
            const int d = count({{1, 0}, {-1, 0}, {0, 1}, {0, -1}}, false, n);
            const int alike = count({{1, 1}, {-1, 1}, {1, -1}, {-1, -1}}, true, diagonals);
            // A lone pixel (no like neighbour round it at all) or a checker (its like ones on both
            // diagonals); not a diagonal line (like ones along one diagonal only).
            differ[i] = n && 4 * d >= 3 * n && (alike == 0 || alike >= 3) ? 1.0f : 0.0f;
        }
    }
    std::vector<float> dither = blurInside(differ, in, side, kDitherSigma);
    for (float& v : dither) {
        v = clamp01((v - kDitherFrom) / kDitherSpan);
    }
    // Each value's coverage, through the narrow blur and the wide one.
    std::vector<float> sharp[2];
    std::vector<float> soft[2];
    for (int k = 0; k < 2; ++k) {
        std::vector<float> cover(cells);
        for (std::size_t i = 0; i < cells; ++i) {
            cover[i] = mask[i] == k + 1 ? 1.0f : 0.0f;
        }
        sharp[k] = blurInside(cover, in, side, kSharpSigma);
        soft[k] = blurInside(cover, in, side, kSoftSigma);
    }

    EdgeWeights w;
    w.size = side * scale;
    const auto pixels = static_cast<std::size_t>(w.size) * static_cast<std::size_t>(w.size);
    w.own.resize(pixels);
    w.floor.resize(pixels);
    for (int y = 0; y < w.size; ++y) {
        // The output pixel's place in mask pixels (their centres at .5), and the 4 x 4 round it.
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f;
        const int v0 = static_cast<int>(std::floor(v));
        float wy[4];
        cubicWeights(v - static_cast<float>(v0), wy);
        for (int x = 0; x < w.size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f;
            const int u0 = static_cast<int>(std::floor(u));
            float wx[4];
            cubicWeights(u - static_cast<float>(u0), wx);
            const auto sample = [&](const std::vector<float>& m) {
                float s = 0;
                for (int j = 0; j < 4; ++j) {
                    const std::size_t row = static_cast<std::size_t>(std::clamp(v0 - 1 + j, 0, side - 1)) * side;
                    for (int i = 0; i < 4; ++i) {
                        s += wy[j] * wx[i] * m[row + static_cast<std::size_t>(std::clamp(u0 - 1 + i, 0, side - 1))];
                    }
                }
                return s;
            };
            const float d = clamp01(sample(dither));
            float out[2];
            for (int k = 0; k < 2; ++k) {
                out[k] = (1 - d) * smoothstep(kSharpLow, kSharpHigh, clamp01(sample(sharp[k]))) + d * clamp01(sample(soft[k]));
            }
            if (const float total = out[0] + out[1]; total > 1) {
                out[0] /= total;
                out[1] /= total;
            }
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w.size) + static_cast<std::size_t>(x);
            w.own[i] = out[0];
            w.floor[i] = out[1];
        }
    }
    return w;
}

void spreadColour(std::span<std::uint8_t> rgba, std::span<const std::uint8_t> known, int width, int height)
{
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || rgba.size() != pixels * 4 || known.size() != pixels) {
        throw std::invalid_argument("spreadColour: sizes don't match");
    }
    // Breadth first from every known pixel at once: each other pixel takes its colour from the
    // neighbour that reached it first (a nearest known pixel, by steps).
    std::vector<std::uint8_t> done(known.begin(), known.end());
    std::deque<std::size_t> queue;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (done[i]) {
            queue.push_back(i);
        }
    }
    while (!queue.empty()) {
        const std::size_t i = queue.front();
        queue.pop_front();
        const int x = static_cast<int>(i % static_cast<std::size_t>(width));
        const int y = static_cast<int>(i / static_cast<std::size_t>(width));
        for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                continue;
            }
            const std::size_t n = static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) + static_cast<std::size_t>(nx);
            if (done[n]) {
                continue;
            }
            done[n] = 1;
            std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), 3, rgba.begin() + static_cast<std::ptrdiff_t>(n * 4));
            queue.push_back(n);
        }
    }
}

} // namespace nox::video
