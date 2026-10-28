// The netstat box (net_stat.hpp): the frame rate, a strip of recent datagrams, and the ping.
#include "net_stat.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace noxsdl {

void NetStat::draw(SDL_Renderer* renderer, Font* font, int w, int h, float scale, std::uint64_t datagrams,
                   std::uint32_t frame, int ping)
{
    // The frame rate: 10 frames over the milliseconds they took (runs every frame,
    // shown or not).
    const std::uint64_t now = SDL_GetTicks();
    if (frames_ >= nextRate_) {
        nextRate_ = frames_ + 10;
        if (rateFromMs_ != 0 && now > rateFromMs_) {
            rate_ = static_cast<int>(10000 / (now - rateFromMs_));
        }
        rateFromMs_ = now;
    }
    ++frames_;
    // The strip, at the game's 30 frames a second: a frame that got a datagram adds one to the
    // column being filled; every third frame the next column starts.
    if (lastFrame_ == 0 || frame < lastFrame_) {
        lastFrame_ = frame;
    }
    for (int n = 0; lastFrame_ < frame && n < 30; ++n, ++lastFrame_) {
        if (datagrams != datagrams_) {
            datagrams_ = datagrams;
            ++counts_[static_cast<std::size_t>(column_)];
        }
        tick_ = (tick_ + 1) % 3;
        if (tick_ == 0) {
            column_ = (column_ + 1) % 31;
            counts_[static_cast<std::size_t>(column_)] = 0; // (the oldest column, reused)
        }
    }
    lastFrame_ = frame;
    if (!shown_) {
        return;
    }
    const float x = 0x79;
    const float y = std::floor(static_cast<float>(h) / scale) - 32 - 5;
    (void)w;
    const auto rect = [&](float rx, float ry, float rw, float rh, SDL_Color c) {
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        const SDL_FRect r{rx * scale, ry * scale, rw * scale, rh * scale};
        SDL_RenderFillRect(renderer, &r);
    };
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    rect(x, y, 32, 32, {0, 0, 0, 128}); // (client_drawRectFilledAlpha)
    const int size = font ? font->lineSize(8, scale) : 0;
    const auto centred = [&](const std::string& t, float ty, SDL_Color c) {
        if (font) {
            const float tw = font->measure(t, size) / scale;
            font->draw(t, (x + (32 - tw) / 2) * scale, ty * scale, size, c);
        }
    };
    centred(std::to_string(rate_), y + 3, {255, 255, 255, 255});
    const float strip = y + 14;
    rect(x + 1, strip, 30, 3, {0, 0, 0, 255});
    // The 30 finished columns, oldest first (the one being filled left out): each as tall as the frames
    // of its three that got a datagram, red for 1, yellow for 2, green for 3.
    for (int i = 1; i < 31; ++i) {
        const int n = std::min(counts_[static_cast<std::size_t>((column_ + i) % 31)], 3);
        if (n == 0) {
            continue;
        }
        const SDL_Color c = n == 1 ? SDL_Color{255, 0, 0, 255} : n == 2 ? SDL_Color{255, 255, 0, 255}
                                                                         : SDL_Color{0, 255, 0, 255};
        rect(x + static_cast<float>(i), strip + 3 - static_cast<float>(n), 1, static_cast<float>(n), c);
    }
    // Ping under 250 ms green, under 500 yellow, else red.
    const SDL_Color pingColor = ping / 250 == 0 ? SDL_Color{0, 255, 0, 255}
                              : ping / 250 == 1 ? SDL_Color{255, 255, 0, 255}
                                                : SDL_Color{255, 0, 0, 255};
    centred(std::to_string(ping), y + 0x16, pingColor);
    SDL_SetRenderDrawColor(renderer, 8, 8, 8, 255);
    const SDL_FRect border{x * scale, y * scale, 32 * scale, 32 * scale};
    SDL_RenderRect(renderer, &border);
}

} // namespace noxsdl
