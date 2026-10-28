// The netstat (ToggleNetstat, F10; GuiFps.wnd): a 32 x 32
// box at (0x79, the screen's bottom less 37) in game pixels, darkened under:
//   the frame rate at the top, worked out every 10 frames;
//   under it a strip of 30 columns, a column every 3 frames, each as tall as the frames of
//   its three that got a datagram (1 red, 2 yellow, 3 green), the newest on the right;
//   the ping at the bottom (our latency): green under 250, yellow under 500, red above.
// In the "numbers" font, white, the box outlined in (8, 8, 8).
#pragma once

#include "text.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

namespace noxsdl {

class NetStat {
public:
    bool shown() const { return shown_; }
    void toggle() { shown_ = !shown_; }

    /// One drawn frame: counts it and draws the box if shown. `datagrams` is the client's count
    /// so far; `frame` the game frame (30 a second, the strip's pace); `scale` output pixels per
    /// game pixel on a w x h screen.
    void draw(SDL_Renderer* renderer, Font* font, int w, int h, float scale, std::uint64_t datagrams,
              std::uint32_t frame, int ping);

private:
    bool shown_ = false;
    std::uint64_t frames_ = 0;
    std::uint64_t nextRate_ = 0;
    std::uint64_t rateFromMs_ = 0;
    int rate_ = 0;
    std::uint64_t datagrams_ = 0;
    std::uint32_t lastFrame_ = 0;
    int tick_ = 0;           ///< which of a column's three frames
    int column_ = 0;         ///< the column being filled
    std::array<int, 31> counts_{};
};

} // namespace noxsdl
