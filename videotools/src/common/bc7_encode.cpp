#include "common/bc7_encode.hpp"

#include <rdo_bc_encoder.h>

#include <cstring>
#include <mutex>
#include <stdexcept>

namespace videotools {

namespace {

/// As noxsdl's halve() (sprite_cache.cpp): half the size rounding up, each pixel its 2x2 block's
/// average weighted by alpha (so clear pixels don't darken edges), alpha the plain average.
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& rgba, int w, int h, int& ow, int& oh)
{
    ow = (w + 1) / 2;
    oh = (h + 1) / 2;
    std::vector<std::uint8_t> out(std::size_t(ow) * std::size_t(oh) * 4);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            unsigned sum[3] = {0, 0, 0};
            unsigned alpha = 0;
            unsigned count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = 2 * x + dx;
                    const int sy = 2 * y + dy;
                    if (sx >= w || sy >= h) {
                        continue;
                    }
                    const auto* p = &rgba[(std::size_t(sy) * std::size_t(w) + std::size_t(sx)) * 4];
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += p[c] * unsigned{p[3]};
                    }
                    alpha += p[3];
                    ++count;
                }
            }
            auto* o = &out[(std::size_t(y) * std::size_t(ow) + std::size_t(x)) * 4];
            for (int c = 0; c < 3; ++c) {
                o[c] = alpha ? std::uint8_t((sum[c] + alpha / 2) / alpha) : 0;
            }
            o[3] = std::uint8_t((alpha + count / 2) / count);
        }
    }
    return out;
}

// bc7enc_rdo's init rewrites global tables (rgbcx::init) on every encoder: one at a time.
std::mutex g_initMutex;

} // namespace

std::vector<MipLevel> mipChain(const std::vector<std::uint8_t>& rgba, int width, int height)
{
    std::vector<MipLevel> out{{rgba, width, height}};
    while (int(out.size()) - 1 < kBc7MaxMips && out.back().width / 2 >= kBc7MinMipSide &&
           out.back().height / 2 >= kBc7MinMipSide) {
        MipLevel next;
        next.rgba = halve(out.back().rgba, out.back().width, out.back().height, next.width, next.height);
        out.push_back(std::move(next));
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> encodeBc7Chain(const std::vector<std::uint8_t>& rgba, int width, int height,
                                                      float rdoLambda)
{
    std::vector<std::vector<std::uint8_t>> levels;
    for (const MipLevel& m : mipChain(rgba, width, height)) {
        const int pw = (m.width + 3) / 4 * 4;
        const int ph = (m.height + 3) / 4 * 4;
        utils::image_u8 img{static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph)}; // padding stays clear
        for (int y = 0; y < m.height; ++y) {
            std::memcpy(&img(0, std::uint32_t(y)), &m.rgba[std::size_t(y) * std::size_t(m.width) * 4],
                        std::size_t(m.width) * 4);
        }
        rdo_bc::rdo_bc_params params;
        params.m_use_bc7e = false;              // bc7enc.cpp (MIT); bc7e.ispc isn't built
        params.m_bc7enc_reduce_entropy = rdoLambda > 0;
        params.m_rdo_lambda = rdoLambda;
        params.m_lookback_window_size = 256;
        params.m_custom_lookback_window_size = true;
        params.m_rdo_multithreading = false;    // callers encode images in parallel instead
        params.m_status_output = false;
        rdo_bc::rdo_bc_encoder encoder;
        {
            const std::lock_guard lock(g_initMutex);
            if (!encoder.init(img, params)) {
                throw std::runtime_error("bc7enc_rdo: init failed");
            }
        }
        if (!encoder.encode()) {
            throw std::runtime_error("bc7enc_rdo: encode failed");
        }
        const auto* blocks = static_cast<const std::uint8_t*>(encoder.get_blocks());
        levels.emplace_back(blocks, blocks + encoder.get_total_blocks_size_in_bytes());
    }
    return levels;
}

} // namespace videotools
