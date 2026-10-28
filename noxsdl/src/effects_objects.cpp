// Effects drawables make on their own, with no message: the thing types' CLIENTUPDATE functions
// (the client runs them every frame for each drawable on its update list) and the DRAW
// functions that spawn or draw effects rather than a sprite. Keyed by the game's function names,
// so a type picks up its behaviour from thing.bin whether the server, the local game or a
// script created the object.
//
// Done: the fireballs' spark trails, magic missile and magic (sparks, twinkles, tail links and
// their flickering light), sparkle trails, the death ball's sparks and charge orbs, the teleport
// wake, the mana bomb's charge sparks, pixies, and arrow trails.
#include "effects.hpp"
#include "effects_draw.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

/// A vector's direction in 256ths of a turn (0 east, 64 south).
std::uint8_t directionOf(float x, float y)
{
    const double turns = std::atan2(static_cast<double>(y), static_cast<double>(x)) / (2 * std::numbers::pi);
    return static_cast<std::uint8_t>(static_cast<int>(std::lround(turns * 256)) & 0xFF);
}

/// sincosTable16 entry: (cos, sin) x 16, rounded.
std::pair<int, int> direction16(int index)
{
    const double a = 2 * std::numbers::pi * index / 256.0;
    return {static_cast<int>(std::lround(16 * std::cos(a))), static_cast<int>(std::lround(16 * std::sin(a)))};
}

constexpr std::uint8_t kFull = 255;

/// A tail link's colour for how much of its life is left (the colour tables the exe fills).
SDL_Color tailColor(int kind, int index)
{
    index = std::clamp(index, 0, 63);
    const int v = index * 255 / 63;
    switch (kind) {
    case 0: // magic: blue-white
        return {static_cast<std::uint8_t>(v / 3), static_cast<std::uint8_t>(v * 3 / 5), static_cast<std::uint8_t>(v), 128};
    case 1: { // magic missile: red when fresh, through yellow to black
        const int c = ((index & 31) * 255) >> 5;
        if (index < 32) {
            return {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c), 0, 128};
        }
        return {kFull, static_cast<std::uint8_t>(255 - c), 0, 128};
    }
    case 2: // arrow: grey
        return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v), 128};
    default: // weak arrow: dark red
        return {static_cast<std::uint8_t>(v), 50, 50, 128};
    }
}

/// Tail links sit at their type's Z (28 for all of them); arrow trails 4 units higher still.
constexpr float kTailHeight = 28;
/// A missile leaves a tail link each time it has moved this far (squared) from the last one.
constexpr float kTailSpacing2 = 200;

} // namespace

// A spark that stays put and twinkles (see Spark::twinkle), drawn `zBase` higher, with its own
// light colour.
void Effects::addTwinkle(const SparkType& type, int x, int y, int z, int zBase, int life, Palette main, Palette alt,
                         std::array<std::uint8_t, 3> light, std::uint32_t frame)
{
    Spark& s = addSpark(type, x, y, frame);
    s.twinkle = true;
    s.twinkleMain = main;
    s.twinkleAlt = alt;
    s.death = frame + static_cast<std::uint32_t>(life);
    s.z = static_cast<std::int16_t>(z);
    s.zBase = static_cast<std::int16_t>(zBase);
    s.lightColor = light;
}

std::optional<LightSource> Effects::drawableLight(std::uint16_t netCode) const
{
    const auto it = drawableFx_.find(netCode);
    return it != drawableFx_.end() ? it->second.light : std::nullopt;
}

std::optional<float> Effects::drawableZ(std::uint16_t netCode, std::uint16_t typeId) const
{
    const TypeArt* art = art_.get(typeId);
    if (!art || (art->updateFn != UpdateFn::Fist && art->updateFn != UpdateFn::Meteor)) {
        return std::nullopt;
    }
    if (const auto pending = fallPending_.find(netCode); pending != fallPending_.end()) {
        return pending->second; // its fall is on its way: held where it starts
    }
    const auto it = drawableFx_.find(netCode);
    if (it == drawableFx_.end()) {
        return 0.0f; // no fall started: the update holds it on the ground
    }
    const float t = stepBlend();
    return it->second.fallPrevZ + (it->second.fallZ - it->second.fallPrevZ) * t;
}

// One game frame of every drawable's CLIENTUPDATE and effect-making DRAW function, by name.
// Keeps each drawable's last position in drawableFx_ for the trails, and forgets the ones gone.
void Effects::stepDrawables(std::uint32_t frame)
{
    std::erase_if(tailLinks_, [frame](const TailLink& l) { return frame >= l.end; });
    if (!anchors_) {
        return;
    }
    const auto spark = [&](std::string_view name) { return sparkType(name); };
    for (const auto& [netCode, a] : *anchors_) {
        const TypeArt* art = art_.get(a.typeId);
        // Only types with a client update or one of the effect DRAW functions handled here.
        if (!art || !art->stepsEffects) {
            continue;
        }
        stepRain(a, *art, frame);
        auto [it, fresh] = drawableFx_.try_emplace(netCode);
        DrawableFx& d = it->second;
        // New, or missing for a frame or more: start its trail where it is now, not from a stale spot.
        if (fresh || d.seen + 1 < frame) {
            d.prevX = d.trailX = a.x;
            d.prevY = d.trailY = a.y;
            d.firstSeen = frame;
        }
        d.seen = frame;
        d.light.reset();
        // Where it was a frame ago (the drawable's previous position).
        const float px = d.prevX;
        const float py = d.prevY;
        const int ix = static_cast<int>(a.x);
        const int iy = static_cast<int>(a.y);
        const int dx = ix - static_cast<int>(px);
        const int dy = iy - static_cast<int>(py);
        const UpdateFn update = art->updateFn;
        const DrawFn draw = art->drawFn;
        // A tail link from the last link's end to here, once it has moved far enough.
        const auto tail = [&](TailLink::Kind kind, std::uint32_t frames) {  // frames: the link's life
            const float tx = a.x - d.trailX;
            const float ty = a.y - d.trailY;
            if (tx * tx + ty * ty > kTailSpacing2) {
                tailLinks_.push_back({kind, d.trailX, d.trailY, a.x, a.y, frame, frame + frames, nox::client::madeAt(frame)});
                d.trailX = a.x;
                d.trailY = a.y;
            }
        };

        // ---- CLIENTUPDATE
        d.fallPrevZ = d.fallZ;
        if (update == UpdateFn::Fist) {
            // Falls at 0.5 a frame per frame and bounces, keeping speed x 0.1 x the message's
            // bounce, until a bounce is slower than 2.
            d.fallZ += d.fallSpeed;
            if (d.fallZ >= 0) {
                d.fallSpeed -= 0.5f;
            } else {
                d.fallZ = 0;
                d.fallSpeed = -d.fallSpeed * d.fallBounce * 0.1f;
                if (d.fallSpeed < 2) {
                    d.fallSpeed = 0;
                }
            }
        } else if (update == UpdateFn::Meteor) {
            // Falls at 1 a frame per frame and stops on the ground.
            if (d.fallZ > 0) {
                d.fallZ += d.fallSpeed;
                d.fallSpeed -= 1;
            }
            if (d.fallZ <= 0) {
                d.fallZ = 0;
                d.fallSpeed = 0;
            }
        }
        int fireball = 0; // 1-5: Pitiful to Titan
        switch (update) {
        case UpdateFn::PitifulFireball: fireball = 1; break;
        case UpdateFn::WeakFireball: fireball = 2; break;
        case UpdateFn::Fireball: fireball = 3; break;
        case UpdateFn::StrongFireball: fireball = 4; break;
        case UpdateFn::TitanFireball: fireball = 5; break;
        default: break;
        }
        if (fireball != 0) {
            // Sparks along the way it came, one per 7 units moved, thrown backwards
            // (within +/- 25 of straight back) faster for a bigger fireball; none while it
            // lingers out of sight (Fade Objects).
            if (const SparkType* type = spark("Spark"); type && !a.lingering) {
                const std::uint8_t back = directionOf(static_cast<float>(-dx), static_cast<float>(-dy));
                for (int n = (std::abs(dy) + std::abs(dx)) / 7; n > 0; --n) {
                    const int r = random(0, 100);
                    // A random point r% of the way along the last step.
                    Spark& s = addSpark(*type, r * dx / 100 + static_cast<int>(px), r * dy / 100 + static_cast<int>(py), frame);
                    s.angle = static_cast<std::uint8_t>(random(-25, 25) + back);
                    s.speed = random(100, 300) * fireball;  // faster for a bigger fireball (1-5)
                    s.death = frame + static_cast<std::uint32_t>(random(30, 45));
                    s.z = 28;
                    s.velZ = static_cast<std::int8_t>(random(-2, 4));
                }
            }
        } else if (update == UpdateFn::MagicMissile) {
            // updDrawMagicMissile: two sparks somewhere along the last step, and a tail link.
            if (const SparkType* type = spark("Spark")) {
                for (int i = 0; i < 2; ++i) {
                    const int sx = static_cast<int>(px) + random(0, 100) * dx / 100;
                    const int sy = static_cast<int>(py) + random(0, 100) * dy / 100;
                    Spark& s = addSpark(*type, sx, sy, frame);
                    s.angle = static_cast<std::uint8_t>(random(0, 255));
                    s.speed = 0;
                    s.death = frame + static_cast<std::uint32_t>(random(3, 10));
                    s.z = 20;
                    s.velZ = static_cast<std::int8_t>(random(0, 6));
                }
            }
            tail(TailLink::Kind::MagicMissile, 10);
        } else if (update == UpdateFn::Magic) {
            // updDrawMagic: a tail link and four blue twinkles along the last step.
            tail(TailLink::Kind::Magic, 30);
            if (const SparkType* type = spark("BlueSpark")) {
                for (int k = 0; k < 4; ++k) {
                    const int jx = random(-8, 8);
                    const int jy = random(-8, 8);
                    addTwinkle(*type, jx + k * dx / 4 + static_cast<int>(px), jy + k * dy / 4 + static_cast<int>(py),
                               static_cast<int>(a.z), static_cast<int>(art->z), random(10, 20),
                               {{0, 200, 255, 255}, {0, 0, 255, 255}}, {{255, 255, 255, 255}, {0, 200, 255, 255}},
                               {128, 128, 255}, frame);
                }
            }
        } else if (update == UpdateFn::SparkleTrail) {
            // updDrawSparkleTrail: five gold twinkles along the last step.
            if (const SparkType* type = spark("BlueSpark")) {
                for (int k = 0; k < 5; ++k) {
                    const int jx = random(-3, 3);
                    const int jy = random(-3, 3);
                    addTwinkle(*type, jx + k * dx / 5 + static_cast<int>(px), jy + k * dy / 5 + static_cast<int>(py),
                               static_cast<int>(a.z), static_cast<int>(art->z), random(2, 10),
                               {{255, 255, 100, 255}, {255, 200, 0, 255}}, {{255, 255, 255, 255}, {255, 255, 100, 255}},
                               {255, 200, 75}, frame);
                }
            }
        } else if (update == UpdateFn::DeathBall || update == UpdateFn::DeathBallFragment) {
            // updDrawAddRndSpark: green sparks flung out fast, three a frame (one for a fragment).
            if (const SparkType* type = spark("DeathBallSpark")) {
                for (int n = update == UpdateFn::DeathBall ? 3 : 1; n > 0; --n) {
                    Spark& s = addSpark(*type, ix, iy, frame);
                    s.angle = static_cast<std::uint8_t>(random(0, 255));
                    s.speed = random(1000, 3000);
                    s.death = frame + static_cast<std::uint32_t>(random(10, 40));
                    s.z = 22;
                    s.velZ = static_cast<std::int8_t>(random(0, 4));
                }
            }
        } else if (update == UpdateFn::DeathBallCharge) {
            // updDrawDBallCharge: green orbs drawn in from around it, ten tries at 50% a frame.
            for (int i = 0; i < 10; ++i) {
                const auto [c, s] = direction16(random(0, 255));
                const int r = random(2, 8);
                const int sx = ix + c * r;  // the table is x16: 32-128 units out
                const int sy = iy + s * r;
                if (random(0, 100) < 50) {
                    const int speed = random(6, 10);
                    const int oy = random(-20, 20);
                    const int ox = random(-20, 20);
                    spawnOrb("CharmOrb", sx + ox, sy + oy, ix, iy);
                    orbs_.back().speed = speed;
                }
            }
        } else if (update == UpdateFn::TeleportWake) {
            // updDrawTeleportWake: a blue spark popping up near it every frame.
            if (const SparkType* type = spark("BlueSpark")) {
                const int ox = random(-5, 5);
                const int oy = random(-5, 5);
                Spark& s = addSpark(*type, ix + ox, iy + oy, frame);
                s.angle = static_cast<std::uint8_t>(random(0, 255));
                s.speed = random(1, 100);
                s.death = frame + static_cast<std::uint32_t>(random(10, 32));
                s.z = 0;
                s.velZ = static_cast<std::int8_t>(random(3, 8));
            }
        } else if (update == UpdateFn::ManaBombCharge) {
            // updDrawManabombCharge: violet sparks popping up all over the blast radius, and in its first 10
            // frames, every other frame, five pairs of orbs spiralling in from the edge.
            const int radius = manaBombOutRadius_; // (gamedata ManaBombOutRadius: setGameData)
            if ((frame & 1) && frame - d.firstSeen < 10) {
                // Five spawn angles 51 apart, turned by the frame so each pair starts somewhere new.
                for (int angle = static_cast<int>(frame % 51); angle < 256; angle += 51) {
                    const auto [c, s] = direction16(angle);
                    const int sx = c * (radius >> 4) + ix;  // the table is x16, the radius / 16: on the edge
                    const int sy = s * (radius >> 4) + iy;
                    for (const bool clockwise : {false, true}) {
                        spawnOrb("ManaBombOrb", sx, sy, ix, iy, 0);
                        Orb& o = orbs_.back();
                        o.spiral = true;
                        o.clockwise = clockwise;
                        o.angle = angle;
                        o.distance = static_cast<int>(std::hypot(sx - ix, sy - iy));
                        o.start = frame;
                    }
                }
            }
            if (const SparkType* type = spark("VioletSpark")) {
                for (int i = 0; i < 20; ++i) {
                    const int r = std::min(random(0, radius) + radius / 4, radius);  // from a quarter of the radius out to the edge
                    const auto [c, s] = direction16(random(0, 255));
                    Spark& sp = addSpark(*type, c * r / 16 + ix, s * r / 16 + iy, frame);
                    sp.angle = 0;
                    sp.speed = 0;
                    sp.death = frame + static_cast<std::uint32_t>(random(10, 30));
                    sp.z = 0;
                    sp.velZ = static_cast<std::int8_t>(random(2, 8));
                }
            }
        }

        // ---- DRAW functions with effects
        if (draw == DrawFn::Magic || draw == DrawFn::MagicMissile) {
            // A flickering size and light every frame.
            d.magicSize = random(1, 4);
            LightSource light;
            light.color = draw == DrawFn::Magic ? Rgb{200, 200, 255} : Rgb{255, 180, 50};
            light.intensity = static_cast<float>(random(0, 100));
            d.light = light;
        } else if (draw == DrawFn::Pixie) {
            // Bobs up and down at random between 0 and 35 units.
            if (random(0, 100) < 50) {
                if (d.bob >= 1) {
                    d.bob -= 1;
                }
            } else if (d.bob <= 34) {
                d.bob += 1;
            }
        } else if (draw == DrawFn::Arrow) {
            tail(TailLink::Kind::Arrow, 10);
        } else if (draw == DrawFn::WeakArrow) {
            tail(TailLink::Kind::WeakArrow, 10);
        }

        d.fromX = px;
        d.fromY = py;
        d.prevX = a.x;
        d.prevY = a.y;
    }
    std::erase_if(drawableFx_, [&](const auto& e) { return !anchors_->contains(e.first); });
}

// Draws the effects of the DRAW functions that draw something themselves (magic, pixies, spider
// spit, waypoints) rather than a sprite: for the drawables the scene's sort drew this frame
// (showDrawable), lingering ones too, at full alpha.
void Effects::drawDrawables(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow)
{
    if (!anchors_) {
        return;
    }
    for (const auto& [netCode, d] : drawableFx_) {
        if (!d.shown) {
            continue;
        }
        const auto a = anchors_->find(netCode);
        if (a == anchors_->end()) {
            continue;
        }
        const TypeArt* art = art_.get(a->second.typeId);
        if (!art) {
            continue;
        }
        const float x = a->second.x;
        const DrawFn draw = art->drawFn;
        if (draw == DrawFn::Magic || draw == DrawFn::MagicMissile) {
            // A glow (blue for magic, yellow for a missile) around a small white square.
            const float y = a->second.y - art->z - a->second.z;
            const int size = d.magicSize;
            const SDL_Color c = draw == DrawFn::Magic ? SDL_Color{0, 200, 255, 255} : SDL_Color{255, 255, 50, 255};
            fxdraw::glow(renderer, camera, glow, x, y, c, static_cast<float>(size * 2 + 1 + 4), ((size >> 1) + 3) << 5);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            const float half = static_cast<float>(size >> 1);
            const bool smooth = fxdraw::smoothOn(); // (smooth: at its exact position)
            fxdraw::fillRect(renderer, camera, (smooth ? x : std::floor(x)) - half, (smooth ? y : std::floor(y)) - half,
                             static_cast<float>(size), static_cast<float>(size));
        } else if (draw == DrawFn::Pixie) {
            // A gold glow and a streak back toward where it was (at most 20 units long).
            const float y = a->second.y - art->z - d.bob;
            fxdraw::glow(renderer, camera, glow, x, y, {255, 200, 0, 255}, 14, 4 << 5);
            float tx = d.fromX;
            float ty = d.fromY - d.bob;
            const float lx = x - tx;
            const float ly = y - ty;
            const float len2 = lx * lx + ly * ly;
            if (len2 > 400) {
                const float len = std::sqrt(len2);
                tx = x - lx * 20 / len;
                ty = y - ly * 20 / len;
            }
            fxdraw::line(renderer, camera, {x, y}, {tx, ty}, {255, 255, 100, 255});
        } else if (draw == DrawFn::SpiderSpit) {
            // A white line back to where it was, edged in grey, with a square head.
            const float lift = art->z + a->second.z;
            const SDL_FPoint head{x, a->second.y - lift};
            const SDL_FPoint tail{d.fromX, d.fromY - lift};
            const bool flat = std::abs(tail.y - head.y) < std::abs(tail.x - head.x);  // mostly horizontal: grey edges above and below, else left and right
            const SDL_FPoint side = flat ? SDL_FPoint{0, 1} : SDL_FPoint{1, 0};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            for (const float s : {-1.0f, 1.0f}) {
                fxdraw::line(renderer, camera, {head.x + side.x * s, head.y + side.y * s},
                             {tail.x + side.x * s, tail.y + side.y * s}, {0x73, 0x73, 0x73, 255});
            }
            SDL_SetRenderDrawColor(renderer, 0x73, 0x73, 0x73, 255);
            fxdraw::fillRect(renderer, camera, head.x - 1, head.y - 1, 4, 4);
            fxdraw::line(renderer, camera, head, tail, {255, 255, 255, 255});
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            fxdraw::fillRect(renderer, camera, head.x, head.y, 2, 2);
        } else if (draw == DrawFn::PlayerWaypoint) {
            // A red circle of radius 10 with a five-pointed star turning inside it
            // (each point 102/256 of a turn on from the last), half transparent.
            const float y = a->second.y;
            const SDL_Color red{255, 0, 0, 128};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            const auto at = [&](double turn256) {
                const double t = turn256 * 2 * std::numbers::pi / 256;
                return SDL_FPoint{x + static_cast<float>(std::cos(t) * 10), y + static_cast<float>(std::sin(t) * 10)};
            };
            for (int i = 0; i < 32; ++i) {  // the circle as 32 segments of 8 256ths
                fxdraw::line(renderer, camera, at(i * 8), at((i + 1) * 8), {255, 0, 0, 255});
            }
            double u = std::fmod(time_ * 2, 256);  // the star turns 2 256ths a game frame
            for (int i = 0; i < 5; ++i) {
                fxdraw::line(renderer, camera, at(u), at(u + 0x66), red);
                u += 0x66;
            }
        }
    }
}

void Effects::drawTailLinks(SDL_Renderer* renderer, const Camera& camera, float /*t*/)
{
    // The four *TailLinkDraw: a half-transparent line whose
    // colour runs down its table over the link's life; those the scene's sort keeps (its drawable
    // at the link's start).
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const TailLink& l : tailLinks_) {
        const double left = static_cast<double>(l.end) - time_;
        if (left <= 0 || !l.shown) {
            continue;
        }
        const int index = static_cast<int>(left * 64 / static_cast<double>(l.end - l.start));
        const bool arrow = l.kind == TailLink::Kind::Arrow || l.kind == TailLink::Kind::WeakArrow;
        const float lift = kTailHeight + (arrow ? 4.0f : 0.0f);
        fxdraw::line(renderer, camera, {l.x1, l.y1 - lift}, {l.x2, l.y2 - lift},
                     tailColor(static_cast<int>(l.kind), index));
    }
}

} // namespace noxsdl
