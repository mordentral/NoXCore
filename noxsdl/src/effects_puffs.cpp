// Soft puffs of gas and smoke, drawn by the fx_puff shader: noxsdl's own look for what the game
// draws with small hard-edged sprites (smooth effects only; --stock-effects keeps the sprites).
//
//   toxic cloud    (CloudUpdateDraw / SmallCloudUpdateDraw) the game puts a GreenPuff or
//                  GreenSmoke within 75 (35) of the cloud every other frame, rising 1-3 a frame
//                  for 10-32 frames, fading (LoopAndFade). Here two soft green puffs
//                  within the same reach every frame, moving as the game's do: straight up at
//                  1-3 a frame for 10-32 frames, swelling a little as they fade.
//   smoke blast    (MSG_FX_SMOKE_BLAST: the fist's landing, three of them round it) the game
//                  plays the Smoke animation 20 up, where it stays, fading over 34 frames
//                  (LoopAndFade, 17 frames twice). Here a cluster of grey-brown dust puffs there,
//                  spreading only as far as the sprite does, swelling and thinning away as long.
//
// Each puff is seen as the drawable it stands in for (the scene's sort, Effects::sortOverScene):
// a cloud's as a GreenPuff, which the cloud makes SIGHT_DESTROY; a blast's as the Smoke, which
// lingers out of sight with Fade Objects, fading as the generic draw fades it.
#include "effects.hpp"
#include "effects_draw.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

float randomUnit(int r) // 0-1000 -> 0-1
{
    return static_cast<float>(r) / 1000.0f;
}

} // namespace

// One soft toxic-cloud puff within `reach` of (x, y) (sqrt of the roll spreads them evenly over
// the disc).
void Effects::cloudPuff(int x, int y, int reach, std::uint32_t frame)
{
    const bool big = reach > 50;
    const double angle = randomUnit(random(0, 1000)) * 2 * std::numbers::pi;
    const float distance = std::sqrt(randomUnit(random(0, 1000))) * static_cast<float>(reach) * 0.85f;
    Puff p;
    p.x = x + static_cast<float>(std::cos(angle)) * distance;
    p.y = y + static_cast<float>(std::sin(angle)) * distance;
    // As the game's puffs move: from the ground straight up, 1-3 a frame.
    p.z = 0;
    p.vx = 0;
    p.vy = 0;
    p.vz = static_cast<float>(random(1, 3));
    p.radius = static_cast<float>(big ? random(12, 20) : random(8, 13));
    p.grow = big ? 0.3f : 0.2f;
    // Emerald, as the game's puffs, some lighter.
    const float mix = randomUnit(random(0, 1000));
    p.color = {static_cast<std::uint8_t>(40 + 70 * mix), static_cast<std::uint8_t>(165 + 50 * mix),
               static_cast<std::uint8_t>(60 + 40 * mix), static_cast<std::uint8_t>(random(120, 175))};
    p.start = frame;
    p.end = frame + static_cast<std::uint32_t>(random(10, 32)); // (as long as the game's)
    p.seed = puffSeed_++;
    p.prevX = p.x;
    p.prevY = p.y;
    p.prevZ = p.z;
    p.prevRadius = p.radius;
    // Seen as the game's GreenPuff, which the cloud makes SIGHT_DESTROY.
    p.art = art_.get("GreenPuff");
    p.sightDestroy = true;
    p.fade = nox::client::madeAt(frame);
    puffs_.push_back(p);
}

// The soft dust cluster of a smoke blast at (x, y).
void Effects::smokeBlast(int x, int y, std::uint32_t frame)
{
    for (int i = 0; i < 16; ++i) {
        // Where the game's Smoke is (20 up, staying there), spreading no further than its sprite
        // (about 25 round): a little outward push that dies away (drag), no rise.
        const double angle = randomUnit(random(0, 1000)) * 2 * std::numbers::pi;
        const float speed = static_cast<float>(random(10, 50)) / 100.0f;
        Puff p;
        p.x = static_cast<float>(x + random(-8, 8));
        p.y = static_cast<float>(y + random(-8, 8));
        p.z = static_cast<float>(random(12, 28));
        p.vx = static_cast<float>(std::cos(angle)) * speed;
        p.vy = static_cast<float>(std::sin(angle)) * speed;
        p.vz = 0;
        p.drag = 0.93f;
        p.radius = static_cast<float>(random(12, 20));
        p.grow = 0.5f;
        const auto grey = static_cast<std::uint8_t>(random(120, 165));
        p.color = {grey, static_cast<std::uint8_t>(grey - 8), static_cast<std::uint8_t>(grey - 22),
                   static_cast<std::uint8_t>(random(170, 220))};
        p.start = frame;
        p.end = frame + static_cast<std::uint32_t>(random(28, 34)); // (the Smoke's 34 frames)
        p.seed = puffSeed_++;
        p.prevX = p.x;
        p.prevY = p.y;
        p.prevZ = p.z;
        p.prevRadius = p.radius;
        // Seen as the game's Smoke at the blast (20 up): it lingers and fades.
        p.art = art_.get("Smoke");
        p.fade = nox::client::madeAt(frame);
        puffs_.push_back(p);
    }
}

// Moves the puffs one frame: drag slows them and their swelling eases off.
void Effects::stepPuffs(std::uint32_t frame)
{
    std::erase_if(puffs_, [frame](const Puff& p) { return frame >= p.end; });
    for (Puff& p : puffs_) {
        p.prevX = p.x;
        p.prevY = p.y;
        p.prevZ = p.z;
        p.prevRadius = p.radius;
        p.x += p.vx;
        p.y += p.vy;
        p.z += p.vz;
        p.vx *= p.drag;
        p.vy *= p.drag;
        p.vz *= p.drag;
        p.radius += p.grow;
        p.grow *= 0.97f;
    }
}

// Draws the puffs the scene's sort keeps (sortOverScene), blended `t` between steps, faded by age
// and lit by the scene's light; a lingering smoke puff fades out as the game's Smoke would.
void Effects::drawPuffs(SDL_Renderer* renderer, const Camera& camera, float t)
{
    if (puffs_.empty() || !fxdraw::puffsOn()) {
        return;
    }
    for (const Puff& p : puffs_) {
        if (!p.shown) {
            continue;
        }
        const float x = p.prevX + (p.x - p.prevX) * t;
        const float y = p.prevY + (p.y - p.prevY) * t;
        const float z = p.prevZ + (p.z - p.prevZ) * t;
        const float radius = p.prevRadius + (p.radius - p.prevRadius) * t;
        // Fading in over its first fifth, out over the rest.
        const float life = static_cast<float>(p.end - p.start);
        const float age = (static_cast<float>(time_) - static_cast<float>(p.start)) / std::max(life, 1.0f);
        const float fade = age < 0.2f ? std::max(age, 0.0f) / 0.2f : std::max(0.0f, (1.0f - age) / 0.8f);
        SDL_Color c = p.color;
        if (light_) {
            // Lit as the scene's sprites are, at its base.
            const Rgb l = light_->sample(x, y);
            c.r = static_cast<std::uint8_t>(c.r * l[0] / 255);
            c.g = static_cast<std::uint8_t>(c.g * l[1] / 255);
            c.b = static_cast<std::uint8_t>(c.b * l[2] / 255);
        }
        c.a = static_cast<std::uint8_t>(std::clamp(static_cast<float>(p.color.a) * fade, 0.0f, 255.0f));
        if (p.fade.lingering && p.art && p.art->fades) {
            c.a = static_cast<std::uint8_t>(
                c.a * nox::client::lingerAlpha(time_ - static_cast<double>(p.fade.inSightFrame)) / 255);
        }
        if (c.a == 0) {
            continue;
        }
        fxdraw::puff(renderer, camera, x, y - z, radius, c, p.seed);
    }
}

} // namespace noxsdl
