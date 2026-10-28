// Effects some objects keep making while they exist: toxic clouds' puffs, vortices' circling
// orbs, the orb rain over a player who levels up, and blue rain.
//
//   CloudUpdateDraw /       every other frame, a GreenPuff (or one time in four-ish a
//   SmallCloudUpdateDraw    GreenSmoke) somewhere within 75 (35) of the cloud, rising 1-3 a
//                           frame for 10-32 frames (updDrawCloud, SmallCloudUpdateDraw)
//   VortexSourceClient-     every frame a WhiteVortexOrb 50 out at a random angle; see
//   Update                  VortexOrb (VortexDraw)
//   LevelUpDraw /           every frame two RainOrbWhite (RainOrbBlue) dropping from the top of
//   OblivionUpDraw          the view onto the player; see RainOrb (thing_falling_sparks_draw)
//   BlueRainDraw            every frame two BlueRainSparks falling from the top of the view
//                           that splash into white sparks (BlueRainSparkDraw)
#include "effects.hpp"
#include "effects_draw.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

/// sincosTable16 entry times `r`, over 16 and rounded toward zero, as the game's
/// `(v * r + (v * r >> 31 & 15)) >> 4`.
std::pair<int, int> circle16(int angle, int r)
{
    const double a = 2 * std::numbers::pi * (angle & 0xFF) / 256.0;
    const int c = static_cast<int>(std::lround(16 * std::cos(a)));
    const int s = static_cast<int>(std::lround(16 * std::sin(a)));
    return {c * r / 16, s * r / 16};
}

constexpr SDL_Color kVortexBack{170, 170, 170, 255};
constexpr SDL_Color kVortexGlow{200, 200, 200, 255};
constexpr SDL_Color kWhite{255, 255, 255, 255};

} // namespace

// One game frame of one drawable's cloud, vortex or rain making (called from stepDrawables).
void Effects::stepRain(const Anchor& a, const TypeArt& art, std::uint32_t frame)
{
    const int x = static_cast<int>(a.x);
    const int y = static_cast<int>(a.y);
    const UpdateFn update = art.updateFn;
    const DrawFn draw = art.drawFn;
    const int viewTop = static_cast<int>(view_.y);

    if (update == UpdateFn::Cloud || update == UpdateFn::SmallCloud) {
        const int reach = update == UpdateFn::Cloud ? 75 : 35;
        if (fxdraw::puffsOn()) {
            // Soft puffs instead (effects_puffs.cpp), two a frame.
            cloudPuff(x, y, reach, frame);
            cloudPuff(x, y, reach, frame);
        } else if (frame & 1) {
            // Classic: one puff every other frame at a random point within reach; 8 rolls in 11 make a
            // GreenPuff, the rest GreenSmoke.
            const int angle = random(0, 255);
            const int distance = random(0, reach);
            const bool puff = random(0, 10) > 2;
            const auto [dx, dy] = circle16(angle, distance);
            const std::size_t before = animations_.size();
            addAnimation(puff ? "GreenPuff" : "GreenSmoke", x + dx, y + dy, 0, frame);
            // (addAnimation adds nothing for a type it can't find, so only touch a new entry.)
            if (animations_.size() > before) {
                animations_.back().rise = random(1, 3);
                animations_.back().end = frame + static_cast<std::uint32_t>(random(10, 32));
                animations_.back().sightDestroy = true; // (the cloud update makes them SIGHT_DESTROY)
            }
        }
    } else if (update == UpdateFn::VortexSource) {
        // One orb a frame on the 50-unit circle at a random angle, circling either way.
        const int angle = random(0, 255);
        const auto [dx, dy] = circle16(angle, 50);
        VortexOrb o;
        o.cx = x;
        o.cy = y;
        o.sortX = x + dx;
        o.sortY = y + dy;
        o.fade = nox::client::madeAt(frame);
        o.angle = angle;
        o.step = random(2, 3);
        if (random(0, 100) > 50) {
            o.step = -o.step;
        }
        vortexOrbs_.push_back(o);
    }

    if (draw == DrawFn::LevelUp || draw == DrawFn::OblivionUp) {
        for (int i = 0; i < 2; ++i) {
            RainOrb o;
            o.blue = draw == DrawFn::OblivionUp;
            o.x = random(-15, 15) + x;
            o.y = random(-15, 15) + y;
            // Starts as high as the drawable is below the view's top edge, so it drops in from the top.
            o.z = o.prevZ = o.y - viewTop;
            // Negative: falling, 8-12 units a frame.
            o.velZ = -random(8, 12);
            o.size = random(3, 10);  // glow radius size + 4, dot size / 3
            o.targetX = x;
            o.targetY = y;
            o.fade = nox::client::madeAt(frame);
            rainOrbs_.push_back(o);
        }
    } else if (draw == DrawFn::BlueRain) {
        // (Not in games with flag 0x200000, which the effects don't see.)
        if (const SparkType* type = sparkType("BlueRainSpark")) {
            for (int i = 0; i < 2; ++i) {
                const int sx = random(-10, 10) + x;
                const int sy = random(-10, 10) + y;
                Spark& s = addSpark(*type, sx, sy, frame);
                // Straight down from the view's top at 5 a frame, no ground motion; gone after 90-120 frames.
                s.angle = 0;
                s.speed = 0;
                s.death = frame + static_cast<std::uint32_t>(random(90, 120));
                s.z = static_cast<std::int16_t>(sy - viewTop);
                s.velZ = -5;
                s.splash = true;
            }
        }
    }
}

// Moves the vortex and rain orbs one frame; landed rain orbs turn into move orbs.
void Effects::stepRainEffects(std::uint32_t /*frame*/)
{
    // VortexDraw moves its orb after drawing it: a step round, a unit up, a smaller circle.
    std::erase_if(vortexOrbs_, [&](VortexOrb& o) {
        o.angle = (o.angle + o.step) & 0xFF;
        ++o.z;
        // The circle shrinks by an eighth of the height (0.0025 x 50 a unit).
        o.radius = 50 - static_cast<int>(std::lround(o.z * 0.0025 * 50));
        const auto [dx, dy] = circle16(o.angle, o.radius);
        const float sx = static_cast<float>(o.cx + dx);
        const float sy = static_cast<float>(o.cy + dy - o.z);
        const bool onScreen = sx > view_.x && sx < view_.x + view_.w && sy > view_.y && sy < view_.y + view_.h;
        return o.radius <= 0 || (view_.w > 0 && !onScreen);  // until the view is known (w 0), only the radius ends it
    });
    // RainOrbDraw: falls at its own speed; on the ground it flies off as a move orb, away from
    // the player to 150 units out.
    std::vector<RainOrb> landed;
    std::erase_if(rainOrbs_, [&](RainOrb& o) {
        if (o.z < 1) {
            landed.push_back(o);
            return true;
        }
        o.prevZ = o.z;
        o.z += o.velZ;
        return false;
    });
    for (const RainOrb& o : landed) {
        // Off along the line from the player through where it landed.
        const double angle = std::atan2(static_cast<double>(o.y - o.targetY), static_cast<double>(o.x - o.targetX));
        const int tx = static_cast<int>(std::lround(std::cos(angle) * 150 + o.targetX));
        const int ty = static_cast<int>(std::lround(std::sin(angle) * 150 + o.targetY));
        spawnOrb(o.blue ? "BlueMoveOrb" : "WhiteMoveOrb", o.x, o.y + 20, tx, ty, random(6, 8)); // (always adds one)
        orbs_.back().shrinkEvery = orbs_.back().shrinkCountdown = 1;  // shrinks every frame
    }
}

// The vortex and rain orbs as scene items, each sorted at its own ground y and going by the scene's
// sort as the drawables they are (VortexDraw and RainOrbDraw don't fade a lingering one).
void Effects::rainSprites(std::vector<MapView::SceneSprite>& out)
{
    const TypeArt* vortexArt = vortexOrbs_.empty() ? nullptr : art_.get(vortexOrbArt_, "WhiteVortexOrb");
    const auto tag = [](MapView::SceneSprite& s, nox::client::FadeState& fade, const TypeArt* art, float z) {
        s.fade = &fade;
        s.fadeGeneral = true;
        s.fadeKeepAlpha = true;
        s.sightArt = art;
        s.sightZ = z;
    };
    for (VortexOrb& o : vortexOrbs_) {
        // Grey and smaller on the far side of the source, white on the near side, with a line
        // back to where it was two steps ago.
        const auto [dx, dy] = circle16(o.angle, o.radius);
        const auto [tx, ty] = circle16(o.angle - 2 * o.step, o.radius);
        const bool far = dy < 0;
        const SDL_FPoint p{static_cast<float>(o.cx + dx), static_cast<float>(o.cy + dy - o.z)};
        const SDL_FPoint t{static_cast<float>(o.cx + tx), static_cast<float>(o.cy + ty - o.z)};
        const SDL_Color lineColor = ty < 0 ? kVortexBack : kWhite;
        // (Its drawable stays where it was made, rising: lit and sight-tested there.)
        MapView::SceneSprite s{static_cast<float>(o.sortY), p.x, p.y, nullptr, static_cast<float>(o.sortX),
                               static_cast<float>(o.sortY)};
        tag(s, o.fade, vortexArt, static_cast<float>(o.z));
        s.draw = [this, p, t, far, lineColor](SDL_Renderer* renderer, const Camera& camera) {
            SDL_Texture* glow = glowTexture(renderer);
            if (far) {
                fxdraw::glow(renderer, camera, glow, p.x, p.y, kVortexBack, 6, 4 << 5);
            } else {
                fxdraw::glow(renderer, camera, glow, p.x, p.y, kVortexGlow, 7, 5 << 5);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            const SDL_Color dot = far ? kVortexBack : kWhite;
            SDL_SetRenderDrawColor(renderer, dot.r, dot.g, dot.b, 255);
            fxdraw::point(renderer, camera, p.x, p.y, far ? 2 : 3);
            fxdraw::line(renderer, camera, p, t, lineColor);
        };
        out.push_back(std::move(s));
    }
    for (RainOrb& o : rainOrbs_) {
        // A glow with a streak up to where it was a frame ago, and a white dot.
        const SDL_Color glowColor = o.blue ? SDL_Color{0, 0, 255, 255} : SDL_Color{200, 200, 200, 255};
        const SDL_FPoint p{static_cast<float>(o.x), static_cast<float>(o.y - o.z)};
        const SDL_FPoint was{p.x, static_cast<float>(o.y - o.prevZ)};
        MapView::SceneSprite s{static_cast<float>(o.y), p.x, p.y, nullptr, p.x, static_cast<float>(o.y)};
        tag(s, o.fade, art_.get(rainOrbArts_[o.blue ? 1 : 0], o.blue ? "RainOrbBlue" : "RainOrbWhite"), static_cast<float>(o.z));
        s.draw = [this, p, was, glowColor, size = o.size](SDL_Renderer* renderer, const Camera& camera) {
            fxdraw::glow(renderer, camera, glowTexture(renderer), p.x, p.y, glowColor, static_cast<float>(size + 4),
                         5 << 5);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            fxdraw::line(renderer, camera, p, was, glowColor);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            fxdraw::point(renderer, camera, p.x, p.y, size / 3);
        };
        out.push_back(std::move(s));
    }
}

} // namespace noxsdl
