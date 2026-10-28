// The GUI's sparks (client\Draw\PartScr.c): points of light with a glow that fly over the
// screen, not the world (client_newScreenParticle, drawn and moved by client_screenParticleDraw each
// frame). The spellbook's awards throw them (the book's edges, the icon leaving the page, its
// trail to the spell bar), as does the trap bar's arrival.
//
//   colours    five kinds, each a point colour over a glow colour (particlesLoadColor): 0 pale blue,
//              1 blue, 2 green, 3 yellow, 4 orange.
//   motion     16.16 fixed point: the velocity is added each frame, gravity to the y velocity.
//   size       drawn as a point of radius size / 2 over a glow of size + 4. With a timer, each
//              time it runs out the size grows (mode 1, to 4, then turns to shrinking) or
//              shrinks; at 0 the spark is gone. One without gravity or timer gets timer 3,
//              shrinking. One with `trail` drops a small still spark now and then (2 in 10).
//   off screen gone.
//   drawing   the motion is the exe's; the drawing is the smooth effects' (effects_draw.hpp): the
//              glow worked out per pixel by fx_glow and the point as an anti-aliased disc, at
//              the spark's exact position, with the GPU renderer's shaders; else the exe's
//              stamped glow and pixel shapes.
//   pointer    the shell menus' pointer throws them too (the shell's main loop): moving more than 8
//              pixels starts a trail, a spark every 4 pixels of the way, orange, falling; a
//              press bursts 75 outward in a ring with ShellMouseBoom.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace noxsdl {

class ScreenParticles {
public:
    ScreenParticles() = default;
    ~ScreenParticles();
    ScreenParticles(const ScreenParticles&) = delete;
    ScreenParticles& operator=(const ScreenParticles&) = delete;

    /// A spark at (x, y) in game pixels, moving (vx, vy) a frame. client_newScreenParticle's arguments in order.
    void add(int kind, int x, int y, int vx, int vy, int gravity, int size, int timer = 0, int mode = 0,
             bool trail = false);
    void clear() { sparks_.clear(); }
    bool empty() const { return sparks_.empty(); }

    /// Moves them on to `frame` (30 a second) and draws them: game pixel (x, y) at output
    /// (offsetX + x * scale, offsetY + y * scale), on a screen `width` x `height` game pixels.
    void draw(SDL_Renderer* renderer, std::uint32_t frame, float scale, float offsetX, float offsetY, float width,
              float height, bool glow);

    /// One frame of the shell pointer's sparks (the shell's main loop), the mouse at `mouse` in the menus'
    /// 640 x 480 pixels with the left button `pressed`; `boom` plays ShellMouseBoom with a press's
    /// burst.
    void shellPointer(SDL_Point mouse, bool pressed, const std::function<void()>& boom);

    /// Visual randomness for the callers (Math_RandIntVisual): lo..hi inclusive.
    int rand(int lo, int hi);

private:
    struct Spark {
        int kind = 0;
        std::int32_t x = 0, y = 0;   ///< 16.16
        std::int32_t vx = 0, vy = 0; ///< 16.16 a frame
        std::int32_t gravity = 0;
        std::uint8_t size = 0;
        std::uint8_t timer = 0;
        std::uint8_t reload = 0;
        std::uint8_t mode = 0; ///< 1 growing, else shrinking
        bool trail = false;
    };
    static constexpr std::size_t kMax = 2048;

    /// One frame of client_screenParticleDraw for a spark: false once it's gone.
    bool step(Spark& s, float width, float height, std::vector<Spark>& born);
    SDL_Texture* glowTexture(SDL_Renderer* renderer);

    std::vector<Spark> sparks_;
    SDL_Point last_{};      ///< the pointer where the trail last reached
    bool trailing_ = false; ///< the pointer's trail running (a move of over 8 pixels starts it)
    bool burst_ = false;    ///< a press's burst thrown, until it's let go
    std::uint32_t frame_ = 0;
    std::uint32_t seed_ = 0x5EED;
    SDL_Texture* glow_ = nullptr;
};

} // namespace noxsdl
