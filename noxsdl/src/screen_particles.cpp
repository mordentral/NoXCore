// ScreenParticles (screen_particles.hpp): the GUI's sparks, moved as the exe moves them and drawn
// with the smooth effects' glow and points.
#include "screen_particles.hpp"

#include "camera.hpp"
#include "effects_draw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

struct Colors {
    SDL_Color point;
    SDL_Color glow;
};

/// particlesLoadColor and the kinds' pairs in client_newScreenParticle.
constexpr std::array<Colors, 5> kColors{{
    {{200, 255, 255, 255}, {0x80, 0x80, 0xFF, 255}},
    {{0x32, 0x96, 0xFF, 255}, {0, 0, 0xFF, 255}},
    {{200, 255, 200, 255}, {0, 0xFF, 0, 255}},
    {{0xFF, 0xFF, 200, 255}, {0xFF, 0xFF, 0, 255}},
    {{0xFF, 200, 100, 255}, {0xFF, 0x80, 0x40, 255}},
}};

constexpr int kGlowSize = 64;

} // namespace

ScreenParticles::~ScreenParticles()
{
    if (glow_) {
        SDL_DestroyTexture(glow_);
    }
}

int ScreenParticles::rand(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    seed_ = seed_ * 1103515245u + 12345u;  // (linear congruential; its high bits used)
    return lo + static_cast<int>((seed_ >> 16) % static_cast<std::uint32_t>(hi - lo + 1));
}

void ScreenParticles::add(int kind, int x, int y, int vx, int vy, int gravity, int size, int timer, int mode,
                          bool trail)
{
    if (kind < 0 || kind > 4) {
        return;
    }
    Spark s;
    s.kind = kind;
    s.x = x * 65536;  // positions and speeds in 16.16 fixed point
    s.y = y * 65536;
    s.vx = vx * 65536;
    s.vy = vy * 65536;
    s.gravity = gravity * 65536;
    s.size = static_cast<std::uint8_t>(std::clamp(size, 0, 255));
    s.timer = static_cast<std::uint8_t>(std::clamp(timer, 0, 255));
    s.reload = s.timer;
    s.mode = static_cast<std::uint8_t>(mode);
    s.trail = trail;
    // Neither gravity nor a timer: it shrinks away on a timer of 3.
    if (s.gravity == 0 && s.timer == 0) {
        s.mode = 2;
        s.timer = 3;
        s.reload = 3;
    }
    if (sparks_.size() >= kMax) {
        sparks_.erase(sparks_.begin()); // the oldest makes room
    }
    sparks_.push_back(s);
}

bool ScreenParticles::step(Spark& s, float width, float height, std::vector<Spark>& born)
{
    const int px = s.x >> 16;
    const int py = s.y >> 16;
    // Gone once it reaches the screen's edge.
    if (px <= 0 || py <= 0 || static_cast<float>(px) >= width || static_cast<float>(py) >= height) {
        return false;
    }
    s.vy += s.gravity;
    // Each time its timer runs out: mode 1 grows (past 3 it turns to mode 2); otherwise it shrinks,
    // gone at 0.
    if (s.timer != 0 && --s.timer == 0) {
        if (s.mode == 1) {
            if (++s.size > 3) {
                s.mode = 2;
            }
        } else if (--s.size == 0) {
            return false;
        }
        s.timer = s.reload;
    }
    if (s.trail && rand(0, 10) > 7) {  // 3 times in 11: a small shrinking spark left behind
        Spark t;
        t.kind = s.kind;
        t.x = (px + rand(-2, 2)) * 65536;
        t.y = py * 65536;
        t.size = static_cast<std::uint8_t>(rand(2, 3));
        t.timer = static_cast<std::uint8_t>(rand(3, 5));
        t.reload = t.timer;
        t.mode = 2;
        born.push_back(t);
    }
    s.x += s.vx;
    s.y += s.vy;
    return true;
}

SDL_Texture* ScreenParticles::glowTexture(SDL_Renderer* renderer)
{
    if (glow_) {
        return glow_;
    }
    // A soft round glow, white, its alpha falling off from the middle.
    std::vector<std::uint32_t> px(kGlowSize * kGlowSize);
    for (int y = 0; y < kGlowSize; ++y) {
        for (int x = 0; x < kGlowSize; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / (kGlowSize / 2.0f) - 1.0f;
            const float dy = (static_cast<float>(y) + 0.5f) / (kGlowSize / 2.0f) - 1.0f;
            const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            const float a = (1.0f - d) * (1.0f - d);
            px[static_cast<std::size_t>(y * kGlowSize + x)] =
                0x00FFFFFFu | (static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24);
        }
    }
    glow_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kGlowSize, kGlowSize);
    if (glow_) {
        SDL_UpdateTexture(glow_, nullptr, px.data(), kGlowSize * 4);
        SDL_SetTextureBlendMode(glow_, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(glow_, SDL_SCALEMODE_LINEAR);
    }
    return glow_;
}

void ScreenParticles::shellPointer(SDL_Point mouse, bool pressed, const std::function<void()>& boom)
{
    // The trail: more than 8 pixels moved starts it; then, each frame, a quarter as many sparks
    // as pixels moved, scattered along the way; it stops when the pointer all but stops.
    const int dx = mouse.x - last_.x;
    const int dy = mouse.y - last_.y;
    const int d2 = dx * dx + dy * dy;
    if (!trailing_) {
        trailing_ = d2 > 0x40;  // 0x40: 8 pixels, squared
    } else {
        const int n = static_cast<int>(std::lround(std::sqrt(static_cast<double>(d2)))) / 4;
        for (int i = 0; i < n; ++i) {
            const int t = rand(0, 100);
            const int x = t * dx / 100 + last_.x;
            const int y = t * dy / 100 + last_.y;
            const int vx = rand(-5, 5);
            const int vy = rand(-7, 2);
            const int size = rand(2, 5);
            const int timer = rand(2, 5);
            add(4, x, y, vx, vy, 1, size, timer);
        }
        if (d2 < 10) {
            trailing_ = false;
        }
        last_ = mouse;
    }
    // A press: ShellMouseBoom and 75 sparks flung out round the pointer, once until let go.
    if (!pressed) {
        burst_ = false;
        return;
    }
    if (burst_) {
        return;
    }
    burst_ = true;
    if (boom) {
        boom();
    }
    for (int n = 0; n < 0x4B; ++n) {
        // The exe's 256-step table of (16 cos, 16 sin), rounded.
        const int a = rand(0, 0xFF);
        const int r = rand(6, 0xC);
        const double angle = a * 2 * std::numbers::pi / 256;
        const int c16 = static_cast<int>(std::lround(16 * std::cos(angle)));
        const int s16 = static_cast<int>(std::lround(16 * std::sin(angle)));
        const int vx = c16 * r / 16;
        const int vy = s16 * r / 16 - 6;
        const int size = rand(2, 5);
        const int timer = rand(2, 5);
        add(4, mouse.x + vx, mouse.y + vy, vx, vy, 1, size, timer, 2, true);
    }
}

void ScreenParticles::draw(SDL_Renderer* renderer, std::uint32_t frame, float scale, float offsetX, float offsetY,
                           float width, float height, bool glow)
{
    if (frame_ == 0 || frame < frame_) {
        frame_ = frame;
    }
    // client_screenParticleDraw moves each spark once a drawn frame, at the game's 30.
    for (int n = 0; frame_ < frame && n < 8; ++n, ++frame_) {  // (at most 8 a draw: a long stall doesn't replay them all)
        std::vector<Spark> born;
        std::erase_if(sparks_, [&](Spark& s) { return !step(s, width, height, born); });
        for (const auto& b : born) {
            if (sparks_.size() < kMax) {
                sparks_.push_back(b);
            }
        }
    }
    frame_ = frame;
    if (sparks_.empty()) {
        return;
    }
    // Game pixels to output pixels as a camera does world units.
    Camera camera;
    int outW = 0, outH = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &outW, &outH);
    camera.viewWidth = outW;
    camera.viewHeight = outH;
    camera.zoom = scale;
    camera.x = -offsetX / scale + static_cast<float>(outW) / (2 * scale);
    camera.y = -offsetY / scale + static_cast<float>(outH) / (2 * scale);
    SDL_Texture* tex = glow ? glowTexture(renderer) : nullptr;
    const bool smooth = fxdraw::smoothOn();
    for (const auto& s : sparks_) {
        const auto& c = kColors[static_cast<std::size_t>(s.kind)];
        // Smooth drawing takes the exact position; the exe's shapes sit on whole pixels.
        const float x = smooth ? static_cast<float>(s.x) / 65536.0f : static_cast<float>(s.x >> 16);
        const float y = smooth ? static_cast<float>(s.y) / 65536.0f : static_cast<float>(s.y >> 16);
        if (tex && s.size > 0) {
            // The glow: size + 4 round, intensity size x 32, added on.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
            fxdraw::glow(renderer, camera, tex, x, y, c.glow, static_cast<float>(s.size + 4), s.size * 32);
        }
        // The point (drawPointMB), size / 2.
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, c.point.r, c.point.g, c.point.b, 255);
        fxdraw::point(renderer, camera, x, y, s.size >> 1);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}
} // namespace noxsdl
