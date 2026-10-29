// The core of the client-side effects (see effects.hpp): queues FX messages, turns each into
// sparks, animations or particles when the render clock reaches it, steps them on the game's
// clock and draws them. Beams and orbs live in effects_beams.cpp, drawables' own effects in
// effects_objects.cpp, enchantments in effects_enchant.cpp, clouds and rains in effects_rain.cpp
// and the soft puffs in effects_puffs.cpp.
#include "effects.hpp"

#include "effects_draw.hpp"

#include "noxcore/net/fx.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace noxsdl {

namespace {

namespace msg = nox::net::msg;
using nox::net::Op;

/// sincosTable16: 256 directions of (cos, sin) x 16, rounded.
struct Direction {
    std::int32_t cos = 0;
    std::int32_t sin = 0;
};
const std::array<Direction, 256>& directions()
{
    static const auto table = [] {
        std::array<Direction, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double a = 2 * std::numbers::pi * static_cast<double>(i) / 256.0;
            t[i] = {static_cast<std::int32_t>(std::lround(16 * std::cos(a))),
                    static_cast<std::int32_t>(std::lround(16 * std::sin(a)))};
        }
        return t;
    }();
    return table;
}

/// The core and glow colours each spark DRAW function passes to the spark draw (the exe's colours).
std::optional<std::pair<SDL_Color, SDL_Color>> sparkPalette(std::string_view draw)
{
    struct Entry {
        std::string_view draw;
        SDL_Color core;
        SDL_Color glow;
    };
    static constexpr std::array<Entry, 9> kPalettes{{
        {"RedSparkDraw", {255, 255, 0, 255}, {255, 100, 0, 255}},
        {"BlueSparkDraw", {0, 200, 255, 255}, {0, 0, 255, 255}},
        {"CyanSparkDraw", {50, 255, 255, 255}, {0, 200, 200, 255}},
        {"GreenSparkDraw", {150, 255, 150, 255}, {0, 220, 0, 255}},
        {"YellowSparkDraw", {255, 255, 0, 255}, {255, 255, 0, 255}},
        {"VioletSparkDraw", {255, 200, 255, 255}, {255, 0, 255, 255}},
        {"WhiteSparkDraw", {255, 255, 255, 255}, {0, 200, 255, 255}},
        {"BlueRainSparkDraw", {255, 255, 255, 255}, {0, 200, 255, 255}},
        {"DeathBallSparkDraw", {150, 255, 150, 255}, {100, 255, 50, 255}},
    }};
    for (const auto& e : kPalettes) {
        if (e.draw == draw) {
            return std::pair{e.core, e.glow};
        }
    }
    return std::nullopt;
}

/// MulDiv(a, b, 0x10000): a 16.16 product, rounded.
std::int32_t mulFixed(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(std::llround(static_cast<double>(a) * b / 65536.0));
}

constexpr std::int32_t kParticleGravity = -0x8000; ///< -0.5 per frame
constexpr std::int32_t kParticleBounce = 0xB333;   ///< 0.7 of the speed kept
constexpr std::size_t kMaxParticles = 20000;

} // namespace

Effects::Effects(const nox::things::ThingDb& things, ThingArt& art) : things_(things), art_(art) {}

Effects::~Effects()
{
    if (glow_) {
        SDL_DestroyTexture(glow_);
    }
}

void Effects::add(nox::client::FxEvent fx)
{
    // A fall's start height is noted on arrival, not when it plays, so the fist or meteor is drawn
    // up at its start height until its queued fall begins (see fallPending_).
    if (const auto* m = std::get_if<msg::FxDeltazSpellStart>(&fx.fx)) {
        fallPending_[m->netCode] = static_cast<float>(m->startZ);
    }
    pending_.push_back(std::move(fx));
}

void Effects::setGameData(const nox::things::GameData* data, std::uint32_t gameFlags)
{
    if (numbersRead_ && data == gameData_ && gameFlags == gameFlags_) {
        return;
    }
    gameData_ = data;
    gameFlags_ = gameFlags;
    numbersRead_ = true;
    manaBombOutRadius_ = static_cast<int>(nox::things::gameNumber(data, "ManaBombOutRadius", gameFlags));
}

void Effects::clear()
{
    pending_.clear();
    fallPending_.clear();
    sparks_.clear();
    animations_.clear();
    particles_.clear();
    puffs_.clear();
    spawners_.clear();
    rays_.clear();
    orbs_.clear();
    sentryRays_.clear();
    drawableFx_.clear();
    tailLinks_.clear();
    killers_.clear();
    summons_.clear();
    bubbles_.clear();
    enchantPrev_.clear();
    vortexOrbs_.clear();
    rainOrbs_.clear();
    jiggle_ = 0;
    whiteFlash_.reset();
    anchorsAtStep_.clear();
    frame_.reset();
    sortedFrames_ = {};
}

int Effects::random(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return nox::util::uniform_int_distribution<int>(lo, hi)(rng_);
}

int Effects::randomBelow(int lo, int hi)
{
    return hi <= lo ? lo : random(lo, hi - 1);
}

std::int32_t Effects::randomFixed(int lo, int hi)
{
    if (hi <= lo) {
        return lo * 0x10000;
    }
    return lo * 0x10000 + nox::util::uniform_int_distribution<std::int32_t>(0, (hi - lo) * 0x10000 - 1)(rng_);
}

Effects::Particle& Effects::addParticle(int x, int y, int z, int life)
{
    // particleFxNew.
    Particle p;
    p.x = x * 0x10000;
    p.y = y * 0x10000;
    p.z = z * 0x10000;
    p.originX = x;
    p.originY = y;
    p.life = life;
    p.prevX = static_cast<float>(x);
    p.prevY = static_cast<float>(y - z);
    p.fade = nox::client::madeAt(frame_.value_or(0));
    if (particles_.size() >= kMaxParticles) {
        particles_.erase(particles_.begin()); // the oldest makes room, as the game's pool would grow
    }
    particles_.push_back(p);
    return particles_.back();
}

void Effects::particleFx(const nox::client::FxEvent& fx)
{
    // CliNet_OnPacket's MSG_FX_PARTICLEFX case and Draw_ParticleFx0-6.
    const auto* m = std::get_if<msg::FxParticlefx>(&fx.fx);
    if (!m || !anchors_) {
        return;
    }
    const auto it = anchors_->find(m->netCode);
    if (it == anchors_->end()) {
        return;
    }
    const Anchor& anchor = it->second;
    const std::uint16_t netCode = m->netCode;
    const int count = m->count;
    const int max = m->max;
    const nox::video::DecodedImage* image =
        anchor.image && sprites_ ? sprites_->decoded(*anchor.image) : nullptr;
    // The sprite's top-left in world units: where the drawable draws it plus the image's own offset.
    const int left = static_cast<int>(std::floor(anchor.left)) + (image ? image->offsetX : 0);
    const int top = static_cast<int>(std::floor(anchor.top)) + (image ? image->offsetY : 0);

    // partfxSwitch: `count` particles spread evenly over the image's opaque
    // pixels, in reading order.
    const auto overPixels = [&](auto&& spawn) {
        if (!image || image->rgba.empty()) {
            return;
        }
        const auto alpha = [&](int col, int row) {
            return image->rgba[(static_cast<std::size_t>(row) * image->width + col) * 4 + 3];
        };
        // Count the opaque pixels, then walk them adding `count` each time: a particle whenever the
        // running sum passes `total`, so `count` particles land evenly over the sprite.
        int total = 0;
        for (int row = 0; row < image->height; ++row) {
            for (int col = 0; col < image->width; ++col) {
                total += alpha(col, row) != 0;
            }
        }
        int acc = count;
        for (int row = 0; row < image->height && total > 0; ++row) {
            for (int col = 0; col < image->width; ++col) {
                if (alpha(col, row) == 0) {
                    continue;
                }
                acc += count;
                if (acc >= total) {
                    acc -= total;
                    spawn(left + col, top + row);
                }
            }
        }
    };

    switch (m->fxCode) {
    case 0: // image fire: glowing embers over the sprite that drop off after a while
        overPixels([&](int px, int py) {
            const int radius = randomBelow(2, 3);
            const int z = randomBelow(0, 40);
            Particle& p = addParticle(px, py + z, z, randomBelow(0, max));
            p.glow = {255, 128, 32, 255};
            p.glowRadius = static_cast<float>(radius);
            p.glowIntensity = 255;
            p.attached = netCode;
            p.delay = 50;  // frames it waits before it moves
            p.vx = randomFixed(-2, 2);
            p.gravity = kParticleGravity;
            p.bounces = true;
            p.bounce = kParticleBounce;
        });
        break;
    case 1: // sparkles: white motes rising from the object, swaying wider as they go
        for (int i = 0; i < count; ++i) {
            Particle& p = addParticle(static_cast<int>(anchor.x), static_cast<int>(anchor.y), 0, 0x80);
            p.motion = Particle::Motion::Sway;
            p.swing = randomBelow(-0x100, 0x100);
            p.attached = netCode;
            p.delay = randomBelow(0, 0x80);
            p.size = 2;
            p.vy = -0x4000 - (randomFixed(0, 1) >> 2);  // 16.16: rising 0.25-0.5 a frame
        }
        break;
    case 2: // top sparks: yellow and white motes swinging along the top of the sprite
        if (image) {
            const int half = image->width / 2;
            for (int i = count - 1; i >= 0; --i) {
                const int y = top + randomBelow(-8, -2);
                const int x = randomBelow(left + half, left + image->width);
                Particle& p = addParticle(x, y, 0, max);
                p.motion = Particle::Motion::Swing;
                p.color = (i & 1) ? SDL_Color{255, 255, 255, 255} : SDL_Color{255, 255, 0, 255};
                p.size = 2;
                p.attached = netCode;
                p.swing = randomBelow(-half, half);
            }
        }
        break;
    case 3: // converge: glows closing in on the sprite from a ring around each point
        overPixels([&](int px, int py) {
            const SDL_Color color = m->flag ? SDL_Color{64, 64, 255, 255} : SDL_Color{64, 200, 64, 255};
            const float radius = static_cast<float>(randomBelow(2, 5));
            const double a = 2 * std::numbers::pi * randomBelow(0, 0x100) / 256.0;
            const int x = px + static_cast<int>(std::cos(a) * 120);
            const int y = py + static_cast<int>(std::sin(a) * 120);
            const int life = randomBelow(1, max);
            Particle& p = addParticle(x, y, 0, life);
            p.sorted = true; // its own drawable
            p.attached = netCode;
            p.glow = color;
            p.glowRadius = radius;
            p.glowIntensity = 255;
            // Straight to the pixel over its lifetime.
            p.vx = ((px - x) * 0x10000) / std::max(life, 1);
            p.vy = ((py - y) * 0x10000) / std::max(life, 1);
        });
        break;
    case 4: // pixel rise: white pixels drifting up off the sprite
        overPixels([&](int px, int py) {
            Particle& p = addParticle(px, py, 0, randomBelow(0, 100));
            p.size = 1;
            p.delay = randomBelow(0, 0x40);
            p.vy = randomFixed(-1, 0) - 0x1999;  // 16.16: rising 0.1-1.1 a frame
        });
        break;
    case 5: // smoke: an emitter that throws off embers every frame for `max` frames
        spawners_.push_back({netCode, 1, 1, 0, max});  // attached, every frame, count 0, `max` runs
        break;
    default:
        break; // 6: an emitter that draws nothing that shows
    }
}

const Effects::SparkType* Effects::sparkType(std::string_view name)
{
    auto it = sparkTypes_.find(name);
    if (it == sparkTypes_.end()) {
        std::optional<SparkType> type;
        if (const auto* def = things_.findThing(name); def && def->draw) {
            if (const auto palette = sparkPalette(def->draw->type)) {
                type = SparkType{{palette->first, palette->second}, art_.get(name)};
            }
        }
        it = sparkTypes_.emplace(std::string(name), type).first;
    }
    return it->second ? &*it->second : nullptr;
}

// A spark at (x, y) in whole units, kept in 1/4096 units; its death is the next frame until the
// caller sets its life.
Effects::Spark& Effects::addSpark(const SparkType& type, int x, int y, std::uint32_t frame)
{
    Spark s;
    s.type = &type;
    s.fx = x << 12;
    s.fy = y << 12;
    s.prevX = static_cast<float>(x);
    s.prevY = static_cast<float>(y);
    s.start = frame;
    s.death = frame + 1;
    s.fade = nox::client::madeAt(frame);
    sparks_.push_back(s);
    return sparks_.back();
}

void Effects::sparkBurst(std::string_view name, int count, int maxSpeed, int minLife, int x, int y, std::uint32_t frame)
{
    // Draw_SpawnSparkBurst.
    const SparkType* type = sparkType(name);
    if (!type) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        Spark& s = addSpark(*type, x, y, frame);
        s.angle = static_cast<std::uint8_t>(random(0, 255));
        s.speed = random(1, maxSpeed);
        s.death = frame + static_cast<std::uint32_t>(random(minLife, 64));
        s.z = 0;
        s.velZ = static_cast<std::int8_t>(random(2, 10));
    }
}

void Effects::explosionSparks(std::string_view name, int x, int y, int size, std::uint32_t frame)
{
    // Draw_SpawnExplosionSparks: more, faster, longer-lived sparks for a bigger size.
    const SparkType* type = sparkType(name);
    if (!type) {
        return;
    }
    // Bigger explosions (size 0-255): 10-190 sparks, faster and longer-lived.
    for (int n = size * 180 / 255 + 10; n > 0; --n) {
        Spark& s = addSpark(*type, x, y, frame);
        s.angle = static_cast<std::uint8_t>(random(0, 255));
        s.speed = random(1, size * 2400 / 255 + 200);
        s.death = frame + static_cast<std::uint32_t>(random(size * 10 / 255 + 5, 96));
        s.z = static_cast<std::int16_t>(random(5, 15));
        s.velZ = static_cast<std::int8_t>(random(0, 8));
    }
}

void Effects::addAnimation(std::string_view name, int x, int y, int z, std::uint32_t frame)
{
    const TypeArt* art = art_.get(name);
    if (!art) {
        return;
    }
    // A OneShotRemove animation deletes its drawable after its last frame; anything
    // else lasts its LIFETIME, or a second.
    std::uint32_t length = 30;
    if (art->draw.kind == nox::things::DrawKind::Animate && !art->draw.animations.empty()) {
        const auto& a = art->draw.animations.front();
        const auto frames = static_cast<std::uint32_t>(a.frames.size()) * (a.frameDelay + 1u);
        using enum nox::things::AnimPlayback;
        if (a.playback == OneShotRemove || a.playback == OneShot) {
            length = frames;
        } else if (a.playback == LoopAndFade) {
            length = 2 * frames; // faded away after two loops
        }
    }
    if (const auto* def = things_.findThing(name); def && length == 30) {
        if (const auto life = def->attribute("LIFETIME")) {
            length = static_cast<std::uint32_t>(std::max(1, std::atoi(std::string(*life).c_str())));
        }
    }
    animations_.push_back({art, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), frame,
                           frame + length, static_cast<std::uint16_t>(random(0, 0xFFFF))});
    animations_.back().fade = nox::client::madeAt(frame);
}

// Plays one queued FX message at `frame`: the beam and orb messages first (effects_beams.cpp),
// then the spark, animation and other handlers by op.
void Effects::spawn(const nox::client::FxEvent& fx, std::uint32_t frame)
{
    if (rayFx(fx, frame)) {
        return;
    }
    // The handlers in CliNet_OnPacket, op by op.
    switch (nox::net::fxOp(fx.fx)) {
    case Op::FxParticlefx:
        particleFx(fx);
        break;
    case Op::FxBlueSparks:
        if (const auto* m = std::get_if<msg::FxBlueSparks>(&fx.fx)) {
            sparkBurst("BlueSpark", 50, 1000, 30, m->x, m->y, frame);  // 50 sparks, speed up to 1000 (1/256 units a frame), 30-64 frames
        }
        break;
    case Op::FxYellowSparks:
        if (const auto* m = std::get_if<msg::FxYellowSparks>(&fx.fx)) {
            sparkBurst("YellowSpark", 25, 500, 25, m->x, m->y, frame);
        }
        break;
    case Op::FxCyanSparks:
        if (const auto* m = std::get_if<msg::FxCyanSparks>(&fx.fx)) {
            sparkBurst("CyanSpark", 25, 500, 25, m->x, m->y, frame);
        }
        break;
    case Op::FxVioletSparks:
        if (const auto* m = std::get_if<msg::FxVioletSparks>(&fx.fx)) {
            sparkBurst("VioletSpark", 25, 500, 25, m->x, m->y, frame);
        }
        break;
    case Op::FxExplosion:
        if (const auto* m = std::get_if<msg::FxExplosion>(&fx.fx)) {
            addAnimation("FireBoom", m->x, m->y, 0, frame);
        }
        break;
    case Op::FxLesserExplosion:
        if (const auto* m = std::get_if<msg::FxLesserExplosion>(&fx.fx)) {
            const std::size_t before = animations_.size();
            addAnimation("MediumFireBoom", m->x, m->y, 0, frame);
            if (animations_.size() > before) {
                animations_.back().scale = burstScale(m->x, m->y, std::nullopt); // (a magic missile a mode drew larger)
            }
        }
        break;
    case Op::FxCounterspellExplosion:
        if (const auto* m = std::get_if<msg::FxCounterspellExplosion>(&fx.fx)) {
            addAnimation("CounterspellBoom", m->x, m->y, 0, frame);
        }
        break;
    case Op::FxThinExplosion:
        if (const auto* m = std::get_if<msg::FxThinExplosion>(&fx.fx)) {
            addAnimation("ThinFireBoom", m->x, m->y, 0, frame);
        }
        break;
    case Op::FxTeleport:
        if (const auto* m = std::get_if<msg::FxTeleport>(&fx.fx)) {
            addAnimation("TeleportPoof", m->x, m->y + 2, 0, frame);
        }
        break;
    case Op::FxDamagePoof:
        if (const auto* m = std::get_if<msg::FxDamagePoof>(&fx.fx)) {
            addAnimation("DamagePoof", m->x, m->y + 2, 0, frame);
        }
        break;
    case Op::FxSmokeBlast:
        if (const auto* m = std::get_if<msg::FxSmokeBlast>(&fx.fx)) {
            if (fxdraw::puffsOn()) {
                smokeBlast(m->x, m->y, frame); // soft dust instead of the sprite (effects_puffs.cpp)
            } else {
                addAnimation("Smoke", m->x, m->y, 20, frame);
            }
            for (int i = 0; i < 6; ++i) {
                const int dx = random(-15, 15);
                const int dy = random(-15, 15);
                addAnimation("Puff", m->x + dx, m->y + dy, random(5, 25), frame);
            }
        }
        break;
    case Op::FxSparkExplosion:
        if (const auto* m = std::get_if<msg::FxSparkExplosion>(&fx.fx)) {
            // The sparks, then the fireball. (A fireball a mode drew larger, of this burst's size:
            // its burst too, the sparks' count and reach by its size times the scale, the boom
            // drawn that much larger.)
            const float k = burstScale(m->x, m->y, static_cast<int>(m->size));
            explosionSparks("Spark", m->x, m->y, k == 1.0f ? m->size : static_cast<int>(static_cast<float>(m->size) * k), frame);
            const std::size_t before = animations_.size();
            addAnimation(m->size > 170 ? "FireBoom" : "MediumFireBoom", m->x, m->y, 0, frame);
            if (animations_.size() > before) {
                animations_.back().scale = k;
            }
        }
        break;
    case Op::Gauntlet:
        // The quest's (CliNet_OnPacket's MSG_GAUNTLET): 0x10 the GreenZap, a GreenBoltDraw
        // drawable from (x1, y1) to (x2, y2) for its duration (counted down by its draw, and
        // Cli_ScheduleDrawableExpire); 0x19 a warp's FireBoom and GreenSpark explosion sparks of
        // its count (Draw_SpawnExplosionSparks); 0x1A 25 GreenSparks (Draw_SpawnSparkBurst
        // 25, 500, 25).
        if (const auto* m = std::get_if<msg::gauntlet::GreenZapEffect>(&fx.fx)) {
            addPointRay(Ray::Kind::GreenBolt, m->x1, m->y1, m->x2, m->y2,
                        frame + static_cast<std::uint32_t>(std::max(1, m->duration - 1)));
        } else if (const auto* w = std::get_if<msg::gauntlet::WarpExplosionFX>(&fx.fx)) {
            addAnimation("FireBoom", w->x, w->y, 0, frame);
            explosionSparks("GreenSpark", w->x, w->y, w->count, frame);
        } else if (const auto* p = std::get_if<msg::gauntlet::PointFX>(&fx.fx)) {
            sparkBurst("GreenSpark", 25, 500, 25, p->x, p->y, frame);
        }
        break;
    case Op::FxDeathRay:
        if (const auto* m = std::get_if<msg::FxDeathRay>(&fx.fx)) {
            // Violet sparks strewn along the ray (the ray itself isn't drawn yet).
            const SparkType* type = sparkType("VioletSpark");
            const int dx = m->x2 - m->x1;
            const int dy = m->y2 - m->y1;
            const int length = static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy)));
            // A spark every 2 units along the ray.
            for (int i = 0, n = (length + 3) / 2; type && i < n; ++i) {
                Spark& s = addSpark(*type, m->x1 + i * 2 * dx / (length + 1), m->y1 + i * 2 * dy / (length + 1), frame);
                s.angle = static_cast<std::uint8_t>(random(0, 255));
                s.speed = random(1, 200);
                s.death = frame + static_cast<std::uint32_t>(random(20, 40));
                s.z = static_cast<std::int16_t>(random(15, 30));
                s.velZ = static_cast<std::int8_t>(random(-4, 4));
            }
        }
        break;
    case Op::FxSentryRay:
        if (const auto* m = std::get_if<msg::FxSentryRay>(&fx.fx)) {
            sentryRays_.push_back({m->x1, m->y1, m->x2, m->y2, frame});
            // A violet spark where the ray hits, 4 units back toward the globe.
            if (const SparkType* type = sparkType("VioletSpark")) {
                const int dx = m->x2 - m->x1;
                const int dy = m->y2 - m->y1;
                const int length = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy))));
                Spark& s = addSpark(*type, m->x2 - dx * 4 / length, m->y2 - dy * 4 / length, frame);
                s.angle = static_cast<std::uint8_t>(random(0, 255));
                s.speed = random(1, 1500);
                s.death = frame + static_cast<std::uint32_t>(random(5, 20));
                s.z = 22;
                s.velZ = static_cast<std::int8_t>(random(-4, 4));
            }
        }
        break;
    case Op::FxRicochet:
        if (const auto* m = std::get_if<msg::FxRicochet>(&fx.fx)) {
            if (const SparkType* type = sparkType("BlueSpark")) {
                for (int i = 0; i < 5; ++i) {
                    Spark& s = addSpark(*type, m->x, m->y, frame);
                    s.angle = static_cast<std::uint8_t>(random(0, 255));
                    s.speed = random(1333, 4000);
                    s.death = frame + static_cast<std::uint32_t>(random(5, 20));
                    s.z = 20;
                    s.velZ = static_cast<std::int8_t>(random(-5, 5));
                }
            }
        }
        break;
    case Op::FxManaBombCancel:
        if (const auto* m = std::get_if<msg::FxManaBombCancel>(&fx.fx)) {
            // Cyan sparks scattered over the outer radius, popping straight up.
            if (const SparkType* type = sparkType("CyanSpark")) {
                const int radius = manaBombOutRadius_;
                for (int i = 0; i < 150; ++i) {
                    const int r = std::min(random(0, radius) + radius / 4, radius);  // from a quarter of the radius out to the edge
                    const auto& d = directions()[static_cast<std::size_t>(random(0, 255))];
                    Spark& s = addSpark(*type, d.cos * r / 16 + m->x, d.sin * r / 16 + m->y, frame);
                    s.angle = 0;
                    s.speed = 0;
                    s.death = frame + static_cast<std::uint32_t>(random(30, 40));
                    s.z = 0;
                    s.velZ = static_cast<std::int8_t>(random(4, 10));
                }
            }
        }
        break;
    case Op::FxJiggle:
        if (const auto* m = std::get_if<msg::FxJiggle>(&fx.fx)) {
            jiggle_ = m->amount / 3;  // a third of the sent amount, swung down by update()
        }
        break;
    case Op::FxWhiteFlash:
        whiteFlash_ = frame;
        break;
    case Op::FxArrowTrap:
        if (const auto* m = std::get_if<msg::FxArrowTrap>(&fx.fx)) {
            if (m->variant == 1) {
                addAnimation("ArrowTrap1Smoke", m->x + 15, m->y, 0, frame);
            } else {
                addAnimation("ArrowTrap2Smoke", m->x - 3, m->y, 0, frame);
            }
        }
        break;
    case Op::FxVampirism:
        if (const auto* m = std::get_if<msg::FxVampirism>(&fx.fx)) {
            // Heal orbs from the victim (x2, y2) +/- 20 back to the caster, one per 4 points
            // drained, 1 to 8 of them.
            const int count = ((m->amount & ~3) < 0x1D ? m->amount >> 2 : 7) + 1;
            for (int i = 0; i < count; ++i) {
                const int speed = random(6, 12);
                const int dy = random(-20, 20);
                const int dx = random(-20, 20);
                spawnOrb("HealOrb", m->x2 + dx, m->y2 + dy, m->x1, m->y1, speed);
            }
        }
        break;
    case Op::FxTurnUndead:
        if (const auto* m = std::get_if<msg::FxTurnUndead>(&fx.fx)) {
            for (int dir = 0; dir < 256; dir += 6) {  // every 6 of the 256 directions: 43 killers
                killers_.push_back({m->x, m->y, static_cast<std::uint8_t>(dir), frame, nox::client::madeAt(frame)});
            }
        }
        break;
    case Op::FxDeltazSpellStart:
        if (const auto* m = std::get_if<msg::FxDeltazSpellStart>(&fx.fx)) {
            // Starts a fist or meteor falling: its height, speed and bounce for its client update.
            fallPending_.erase(m->netCode);
            DrawableFx& d = drawableFx_[m->netCode];
            d.fallZ = d.fallPrevZ = m->startZ;
            d.fallSpeed = m->deltaZ;
            d.fallBounce = m->speed;
        }
        break;
    case Op::FxSummon:
        if (const auto* m = std::get_if<msg::FxSummon>(&fx.fx)) {
            // The facing (0-255, 0 east, clockwise) to the nearest of the 8 AnimDirs.
            static constexpr std::array<std::uint8_t, 8> kAnimDir{5, 8, 7, 6, 3, 0, 1, 2};
            summons_.push_back({m->summonId, m->x, m->y, m->typeId, kAnimDir[((m->dir + 16) >> 5) & 7], frame,  // +16 >> 5: the nearest eighth of a turn
                                m->duration, static_cast<std::uint16_t>(random(0, 0xFFFF)), nox::client::madeAt(frame)});
        }
        break;
    case Op::FxSummonCancel:
        if (const auto* m = std::get_if<msg::FxSummonCancel>(&fx.fx)) {
            // Cli_CancelSummonEffect: a burst of blue sparks, and the ghost is gone.
            const auto it = std::find_if(summons_.begin(), summons_.end(),
                                         [&](const Summon& s) { return s.id == m->summonId; });
            if (it != summons_.end()) {
                sparkBurst("BlueSpark", 50, 1000, 30, it->x, it->y, frame);
                summons_.erase(it);
            }
        }
        break;
    case Op::FxShield:
        if (const auto* m = std::get_if<msg::FxShield>(&fx.fx)) {
            // Cli_AddShieldEffect: the shield facing the hit (by AnimDir; 4 has none), on the
            // target at y + 3, unless it already has one up.
            static constexpr std::array<std::string_view, 9> kShields{
                "SphericalShieldNW", "SphericalShieldN",  "SphericalShieldNE", "SphericalShieldW", "",
                "SphericalShieldE",  "SphericalShieldSW", "SphericalShieldS",  "SphericalShieldSE"};
            const auto target = anchors_ ? anchors_->find(m->netCode) : Anchors::const_iterator{};
            if (m->shieldType >= kShields.size() || kShields[m->shieldType].empty() || !anchors_ ||
                target == anchors_->end()) {
                break;
            }
            const bool up = std::any_of(animations_.begin(), animations_.end(), [&](const Animation& a) {
                return a.follow == m->netCode && a.art && a.art->name.starts_with("SphericalShield");
            });
            const std::size_t before = animations_.size();
            if (!up) {
                addAnimation(kShields[m->shieldType], static_cast<int>(target->second.x),
                             static_cast<int>(target->second.y) + 3, 0, frame);
            }
            if (animations_.size() > before) {
                animations_.back().follow = m->netCode;
            }
        }
        break;
    default:
        break;
    }
}

SDL_FPoint Effects::killerPosition(const UndeadKiller& k, double frame) const
{
    // Its sprite update adds its velocity (4 units along its direction, no damping) once per frame since
    // it started, plus one.
    const double steps = frame - static_cast<double>(k.start) + 1;
    const double angle = k.direction * 2 * std::numbers::pi / 256;
    return {static_cast<float>(k.originX + steps * 4 * std::cos(angle)),
            static_cast<float>(k.originY + steps * 4 * std::sin(angle))};
}

// One game frame of every effect, in the order the game's client update and draws run them.
void Effects::step(std::uint32_t frame)
{
    // The drawables' own effects first (the client update list runs before drawing), so what
    // they spawn gets its first step this frame too.
    stepDrawables(frame);
    stepEnchantments(frame);
    stepBubbles(frame);
    stepRainEffects(frame);
    stepPuffs(frame);

    // Turn Undead's ring: gone after 70 frames, off the map, or (its sprite update, before the
    // draw) outside the last drawn frame's sight polygon; otherwise a 15% chance a frame of a white
    // orb from near it back to the centre (UndeadKillerDraw).
    const SightMap* lastSight =
        sceneSight_ && sortedFrames_.last != 0 && sortedFrames_.last + 1 >= frame ? sceneSight_ : nullptr;
    std::erase_if(killers_, [&](const UndeadKiller& k) {
        const SDL_FPoint p = killerPosition(k, frame);
        const int x = static_cast<int>(std::lround(p.x));
        const int y = static_cast<int>(std::lround(p.y));
        if (frame - k.start > 70 || x <= 0 || y <= 0 || x >= 0x1700 || y >= 0x1700) {  // 0x1700: the map's edge in units (256 tiles of 23)
            return true;
        }
        if (lastSight) {
            const auto& view = lastSight->region().view();
            if (!lastSight->region().insidePolygon(x - view.left, y - view.top)) {
                return true;
            }
        }
        if (random(0, 100) > 85) {
            const int rx = random(-5, 5);
            const int ry = random(-5, 5);
            const int speed = random(6, 10);
            const int dy = random(-5, 5);
            spawnOrb("WhiteOrb", x + rx, y + ry + dy, k.originX, k.originY, speed);
        }
        return false;
    });

    // Each spark as its draw function moves it every frame.
    const auto& dirs = directions();
    std::vector<std::pair<int, int>> splashes;
    for (Spark& s : sparks_) {
        s.prevX = static_cast<float>(s.fx >> 12);
        s.prevY = static_cast<float>(s.fy >> 12);
        s.prevZ = s.z;
        if (s.twinkle) {
            // No motion; a random size scaled by the life left, and a colour pair.
            s.total = static_cast<std::int32_t>(s.death - s.start);
            s.remaining = static_cast<std::int32_t>(s.death - frame);
            if (s.remaining == s.total) {
                --s.remaining;
            }
            s.dead = s.remaining < 1 || s.total < 1;
            if (!s.dead) {
                s.twinkleSize = random(0, 4) * s.remaining / s.total;
                s.twinkleUseAlt = random(0, 10) >= 5;
            }
            continue;
        }
        // Along its angle: the table is x16, so speed is in 1/256 units a frame (fx is 1/4096).
        s.fx += dirs[s.angle].cos * s.speed;
        s.fy += dirs[s.angle].sin * s.speed;
        s.z = static_cast<std::int16_t>(s.z + s.velZ);
        if (s.z < 0) {
            // Bounce, keeping 9/10 of the speed, until it's too slow to leave the ground.
            s.z = static_cast<std::int16_t>(-s.z);
            s.velZ = static_cast<std::int8_t>(s.velZ * -9 / 10);
            if (s.velZ < 2) {
                s.z = 0;
                s.velZ = 0;
            }
        } else if (frame & 1) {  // gravity: a unit off velZ every other frame
            s.velZ = static_cast<std::int8_t>(s.velZ - 1);
        }
        // Life left out of its whole life, for the draw's shrink and fade.
        s.total = static_cast<std::int32_t>(s.death - s.start);
        s.remaining = static_cast<std::int32_t>(s.death - frame);
        if (s.remaining == s.total) {
            --s.remaining;
        }
        s.dead = s.remaining < 1 || s.total < 1;
        // A raindrop that bounces (velZ flipped above 4) ends in a splash.
        if (s.splash && !s.dead && s.velZ > 4) {
            s.dead = true;
            splashes.push_back({s.fx >> 12, s.fy >> 12});
        }
    }
    std::erase_if(sparks_, [](const Spark& s) { return s.dead; });
    // A blue raindrop that bounced: a white spark from where it hit.
    for (const auto& [x, y] : splashes) {
        if (const SparkType* type = sparkType("WhiteSpark")) {
            Spark& s = addSpark(*type, x, y, frame);
            s.angle = static_cast<std::uint8_t>(random(0, 255));
            s.speed = random(1, 0x64B);
            s.death = frame + static_cast<std::uint32_t>(random(10, 0x60));
            s.z = static_cast<std::int16_t>(random(5, 15));
            s.velZ = static_cast<std::int8_t>(random(0, 8));
        }
    }
    std::erase_if(animations_, [&](Animation& a) {
        if (a.follow != 0) {
            // SphericalShieldDraw: on its target, gone with it.
            const auto it = anchors_ ? anchors_->find(a.follow) : Anchors::const_iterator{};
            if (!anchors_ || it == anchors_->end()) {
                return true;
            }
            a.x = it->second.x;
            a.y = it->second.y + 3;
        }
        a.z += static_cast<float>(a.rise);
        return frame >= a.end;
    });
    // SummonEffectDraw: gone once its time is up, with a burst of blue sparks on its last frame.
    std::erase_if(summons_, [&](const Summon& s) {
        const std::uint32_t elapsed = frame - s.start;
        if (s.duration <= elapsed) {
            return true;
        }
        if (static_cast<int>(s.duration) - 1 <= static_cast<int>(elapsed)) {
            sparkBurst("BlueSpark", 50, 1000, 30, s.x, s.y, frame);
        }
        return false;
    });
    stepParticles(frame);
    stepRays(frame);
    stepOrbs(frame);
}

// Moves the MSG_FX_PARTICLEFX particles one frame (attached ones also carried by their
// drawable's movement since the last step), drops the expired ones, then runs the spawners.
void Effects::stepParticles(std::uint32_t /*frame*/)
{
    // How far each followed drawable moved since the last step (the game's drawables keep
    // their previous position for this).
    std::unordered_map<std::uint16_t, std::pair<std::int32_t, std::int32_t>> moved;
    const auto delta = [&](std::uint16_t netCode) -> std::optional<std::pair<std::int32_t, std::int32_t>> {
        if (netCode == 0 || !anchors_) {
            return std::nullopt;
        }
        if (const auto it = moved.find(netCode); it != moved.end()) {
            return it->second;
        }
        const auto a = anchors_->find(netCode);
        if (a == anchors_->end()) {
            return std::nullopt;
        }
        std::pair<std::int32_t, std::int32_t> d{0, 0};
        if (const auto last = anchorsAtStep_.find(netCode); last != anchorsAtStep_.end()) {
            d = {static_cast<std::int32_t>(std::lround((a->second.x - last->second.first) * 0x10000)),
                 static_cast<std::int32_t>(std::lround((a->second.y - last->second.second) * 0x10000))};
        }
        anchorsAtStep_[netCode] = {a->second.x, a->second.y};
        moved[netCode] = d;
        return d;
    };

    for (Particle& p : particles_) {
        p.prevX = static_cast<float>(p.x) / 0x10000;
        p.prevY = static_cast<float>(p.y - p.z) / 0x10000;
        const auto d = delta(p.attached);
        switch (p.motion) {
        case Particle::Motion::Physics: {
            // Each motion waits out its `delay` frames first; its life doesn't run meanwhile.
            if (p.delay != 0) {
                --p.delay;
                continue;
            }
            std::int32_t x = p.x + (d ? d->first : 0) + p.vx;
            std::int32_t y = p.y + (d ? d->second : 0) + p.vy;
            std::int32_t z = p.z + p.vz;
            std::int32_t vx = p.vx;
            std::int32_t vy = p.vy;
            std::int32_t vz = p.vz;
            // Below the ground: reflect, keeping `bounce` of the speed, or stop on it.
            if (z < 0) {
                if (p.bounces) {
                    // Undo this frame's drop and head back up.
                    z -= vz;
                    vz = -vz;
                    if (p.bounce != 0) {
                        vx = mulFixed(vx, p.bounce);
                        vy = mulFixed(vy, p.bounce);
                        vz = mulFixed(vz, p.bounce);
                    }
                } else {
                    z = 0;
                    vz = 0;
                }
            }
            if (p.dampX != 0) {
                vx = mulFixed(vx, p.dampX);
            }
            if (p.dampY != 0) {
                vy = mulFixed(vy, p.dampY);
            }
            if (p.dampZ != 0) {
                vz = mulFixed(vz, p.dampZ);
            }
            vz += p.gravity;
            // Expired: left where it is and removed after the loop.
            if (--p.life < 0) {
                continue;
            }
            p.x = x;
            p.y = y;
            p.z = z;
            p.vx = vx;
            p.vy = vy;
            p.vz = vz;
            break;
        }
        case Particle::Motion::Sway: {
            // The sway swings back and forth while `spread` widens it.
            if (p.delay != 0) {
                --p.delay;
                continue;
            }
            // Springs back toward the middle (speed pushed 4 a frame against the swing) while rising.
            p.swingSpeed += p.swing < 0 ? 4 : -4;
            p.swing += p.swingSpeed;
            const std::int32_t x = ((p.spread * p.swing) >> 2) + p.originX * 0x10000;
            p.spread += 0x100;
            if (--p.life < 0) {
                continue;
            }
            p.y += p.vy;
            p.x = x;
            break;
        }
        case Particle::Motion::Swing: {
            // About a point that moves with the drawable.
            if (p.delay != 0) {
                --p.delay;
                continue;
            }
            if (d) {
                // The drawable's move is 16.16; the origin is in whole units.
                p.originX += d->first / 0x10000;
                p.originY += d->second / 0x10000;
            }
            p.swingSpeed += p.swing < 0 ? 1 : -1;
            p.swing += p.swingSpeed;
            if (--p.life < 0) {
                continue;
            }
            p.x = (p.originX + p.swing) * 0x10000;
            p.y = p.originY * 0x10000;
            break;
        }
        }
    }
    std::erase_if(particles_, [](const Particle& p) { return p.life < 0; });

    // The periodic sources (the particle update's second loop); smoke is the only kind that shows.
    for (auto it = spawners_.begin(); it != spawners_.end();) {
        Spawner& s = *it;
        bool keep = false;
        if (++s.count <= s.max && anchors_) {  // count: frames run so far, up to max
            if (const auto a = anchors_->find(s.attached); a != anchors_->end()) {
                // A yellow ember in a grey glow, tossed up and bouncing.
                const int x = static_cast<int>(a->second.x);
                const int y = static_cast<int>(a->second.y);
                const int py = randomBelow(y - 2, y + 2);
                Particle& p = addParticle(randomBelow(x - 2, x + 2), py, 2, 10);
                p.sorted = true; // its own drawable
                p.color = {255, 255, 0, 255};
                p.glow = {50, 50, 50, 255};
                p.glowRadius = 10;
                p.glowIntensity = 255;
                p.size = 1;
                p.gravity = kParticleGravity;
                p.bounces = true;
                p.bounce = kParticleBounce;
                p.vx = randomFixed(-2, 2);
                p.vy = randomFixed(0, 2);
                p.vz = randomFixed(0, 2);
                keep = true;
            }
        }
        it = keep ? std::next(it) : spawners_.erase(it);
    }

    // Forget drawables that are gone.
    if (anchors_) {
        std::erase_if(anchorsAtStep_, [&](const auto& e) { return !anchors_->contains(e.first); });
    }
}

void Effects::update(double time, const Anchors& anchors, SpriteCache* sprites)
{
    anchors_ = &anchors;
    sprites_ = sprites;
    time_ = time;
    const auto target = static_cast<std::uint32_t>(std::max(0.0, std::floor(time)));
    if (!frame_ || target < *frame_ || target - *frame_ > 90) {
        frame_ = target == 0 ? 0 : target - 1; // (re)start after a gap rather than replay it
    }
    while (*frame_ < target) {
        const std::uint32_t frame = ++*frame_;
        // The shake swings to the other side one unit smaller each frame, after the camera has
        // used it, so a new jiggle shows at full size first.
        jiggle_ = jiggle_ > 0 ? 1 - jiggle_ : jiggle_ < 0 ? -1 - jiggle_ : 0;
        // Events stamped far ahead of the render clock (it restarted) play now rather than never.
        while (!pending_.empty() && (pending_.front().frame <= frame || pending_.front().frame > frame + 90)) {
            spawn(pending_.front(), frame);
            pending_.pop_front();
        }
        step(frame);
    }
    sprites_ = nullptr;
}

void Effects::setSceneDraw(const SightMap* sight, bool glow, bool fadeObjects)
{
    sceneSight_ = sight;
    sceneGlow_ = glow;
    fadeObjects_ = fadeObjects;
    sortedFrames_.sorted(drawnFrame());
    for (auto& [netCode, d] : drawableFx_) {
        d.shown = false; // (the scene's items for them say so as they're drawn)
    }
    sortOverScene();
}

void Effects::showDrawable(std::uint16_t netCode)
{
    if (const auto it = drawableFx_.find(netCode); it != drawableFx_.end()) {
        it->second.shown = true;
    }
}

bool Effects::sortEffect(nox::client::FadeState& fade, const TypeArt* art, float x, float y, float z)
{
    // (The client makes none of them a sight blocker: no key; never drawn by the generic draw.)
    bool seen = true;
    if (sceneSight_) {
        seen = art ? sceneSight_->drawableSeen(*art, 0, x, y, z, 0, {}) : sceneSight_->visible(x, y);
    }
    const std::uint32_t frame = drawnFrame();
    const bool drawn = nox::client::sortStep(fade, {.fadeObjects = fadeObjects_, .inSight = seen, .frame = frame});
    if (drawn) {
        nox::client::markDrawn(fade, frame);
    }
    return drawn;
}

const TypeArt* Effects::tailArt(int kind)
{
    static constexpr std::array<std::string_view, 4> kNames{"MagicTailLink", "MagicMissileTailLink", "ArrowTailLink",
                                                            "WeakArrowTailLink"};
    auto& a = tailArts_[static_cast<std::size_t>(kind) % kNames.size()];
    if (!a) {
        a = art_.get(kNames[static_cast<std::size_t>(kind) % kNames.size()]);
    }
    return a;
}

const TypeArt* Effects::rayArt(int kind)
{
    // By Ray::Kind; the drain, heal and charm rays draw nothing (NoDraw).
    static constexpr std::array<std::string_view, 8> kNames{"DynamicLightning", "DynamicChainLightning", "DynamicEnergyBolt",
                                                            "GreenZap",         "",                      "",
                                                            "",                 "HarpoonRope"};
    const auto i = static_cast<std::size_t>(kind) % kNames.size();
    if (!rayArts_[i] && !kNames[i].empty()) {
        rayArts_[i] = art_.get(kNames[i]);
    }
    return rayArts_[i];
}

// The scene's sort for the effects drawn over the scene rather than in it: each one's `shown`.
void Effects::sortOverScene()
{
    const float t = stepBlend();
    for (Spark& s : sparks_) {
        const float x = s.prevX + (static_cast<float>(s.fx >> 12) - s.prevX) * t;
        const float ground = s.prevY + (static_cast<float>(s.fy >> 12) - s.prevY) * t;
        s.shown = sortEffect(s.fade, s.type->art, x, ground, static_cast<float>(s.z));
    }
    for (Orb& o : orbs_) {
        const float x = static_cast<float>(o.prevX) + static_cast<float>(o.x - o.prevX) * t;
        const float ground = static_cast<float>(o.prevY) + static_cast<float>(o.y - o.prevY) * t;
        o.shown = sortEffect(o.fade, o.art, x, ground, 0);
    }
    for (TailLink& l : tailLinks_) {
        l.shown = sortEffect(l.fade, tailArt(static_cast<int>(l.kind)), l.x1, l.y1, 0);
    }
    for (Ray& r : rays_) {
        if (!r.mid) {
            // Made once both its ends are known (the game makes none without them).
            const auto a = endPoint(r.from);
            const auto b = endPoint(r.to);
            if (!a || !b) {
                r.shown = false;
                continue;
            }
            const int ax = static_cast<int>(a->x);
            const int ay = static_cast<int>(a->y);
            r.mid = SDL_Point{(static_cast<int>(b->x) - ax) / 2 + ax, (static_cast<int>(b->y) - ay) / 2 + ay};
        }
        r.shown = sortEffect(r.fade, rayArt(static_cast<int>(r.kind)), static_cast<float>(r.mid->x),
                             static_cast<float>(r.mid->y), 0);
    }
    const TypeArt* killer = killers_.empty() ? nullptr : art_.get(killerArt_, "UndeadKiller");
    for (UndeadKiller& k : killers_) {
        const SDL_FPoint p = killerPosition(k, time_);
        k.shown = sortEffect(k.fade, killer, p.x, p.y, 0);
    }
    for (Puff& p : puffs_) {
        const float x = p.prevX + (p.x - p.prevX) * t;
        const float y = p.prevY + (p.y - p.prevY) * t;
        p.shown = sortEffect(p.fade, p.art, x, y, p.prevZ + (p.z - p.prevZ) * t);
    }
}

void Effects::destroyUnseen()
{
    // After each frame's draw the game deletes the SIGHT_DESTROY drawables whose last in-sight
    // frame (their making's, until then) is before it; here when a later frame is first drawn.
    const std::uint32_t completed = sortedFrames_.completed(drawnFrame());
    if (completed == 0) {
        return;
    }
    const auto gone = [completed](const nox::client::FadeState& f) { return nox::client::sightDestroyed(f, completed); };
    std::erase_if(sparks_, [&](const Spark& s) { return s.type->art && s.type->art->sightDestroy && gone(s.fade); });
    std::erase_if(animations_, [&](const Animation& a) { return (a.sightDestroy || a.art->sightDestroy) && gone(a.fade); });
    if (!particleArt_) {
        particleArt_ = art_.get("Particle");
    }
    const bool particleGoes = particleArt_ && particleArt_->sightDestroy;
    std::erase_if(particles_, [&](const Particle& p) { return p.sorted && particleGoes && gone(p.fade); });
    std::erase_if(orbs_, [&](const Orb& o) { return o.art && o.art->sightDestroy && gone(o.fade); });
    // The client makes these SIGHT_DESTROY itself.
    std::erase_if(killers_, [&](const UndeadKiller& k) { return gone(k.fade); });
    std::erase_if(bubbles_, [&](const Bubble& b) { return gone(b.fade); });
    std::erase_if(vortexOrbs_, [&](const VortexOrb& o) { return gone(o.fade); });
    std::erase_if(rainOrbs_, [&](const RainOrb& o) {
        const TypeArt* art = art_.get(rainOrbArts_[o.blue ? 1 : 0], o.blue ? "RainOrbBlue" : "RainOrbWhite");
        return art && art->sightDestroy && gone(o.fade);
    });
    std::erase_if(puffs_, [&](const Puff& p) { return p.sightDestroy && gone(p.fade); });
}

void Effects::sceneSprites(SpriteCache& sprites, std::vector<MapView::SceneSprite>& out)
{
    destroyUnseen();
    // A sorted effect goes by the scene's sort as the drawable it is (SceneSprite::fade): its type,
    // its position (the item's light point) and height; `keep`: its DRAW doesn't fade a lingering one.
    const auto tag = [](MapView::SceneSprite& s, nox::client::FadeState& fade, const TypeArt* art, float z, bool keep) {
        s.fade = &fade;
        s.fadeGeneral = true;
        s.fadeKeepAlpha = keep;
        s.sightArt = art;
        s.sightZ = z;
    };
    // The particles with a drawable of their own sort in with the drawables at their ground y
    // (the drawable at the particle's ground spot, its height left at 0).
    if (!particleArt_) {
        particleArt_ = art_.get("Particle");
    }
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        Particle& p = particles_[i];
        if (!p.sorted) {
            continue;
        }
        p.shown = false;
        const float ground = static_cast<float>(p.y) / 0x10000;
        const float px = static_cast<float>(p.x) / 0x10000;
        MapView::SceneSprite s{ground, px, ground, nullptr, px, ground};
        tag(s, p.fade, particleArt_, 0, true);
        s.draw = [this, i](SDL_Renderer* renderer, const Camera& camera) {
            Particle& q = particles_[i];
            q.shown = true; // (and so in the bloom pass)
            const float t = stepBlend();  // how far between the last step and the next
            drawParticle(renderer, camera, sceneGlow_ ? glowTexture(renderer) : nullptr, q, t);
        };
        out.push_back(std::move(s));
    }
    // The one-shot animations, on their drawable if they follow one.
    for (auto& a : animations_) {
        Pose pose;
        pose.netCode = a.seed;
        pose.variant = a.seed;
        pose.time = time_;
        pose.animStart = a.start;
        const auto image = pickImage(*a.art, pose);
        const Sprite* s = image ? sprites.get(*image) : nullptr;
        if (!s) {
            continue;
        }
        float x = a.x;
        float y = a.y;
        if (a.follow != 0 && anchors_) {
            if (const auto it = anchors_->find(a.follow); it != anchors_->end()) {
                x = it->second.x;
                y = it->second.y + 3;  // y + 3, as SphericalShieldDraw places it
            }
        }
        out.push_back({y, x - a.art->halfWidth, y - a.art->halfHeight - a.art->z - a.z, s, x, y});
        out.back().scale = a.scale; // (a mode's burst scale: 1 for the game's)
        if (const auto alpha = fadeAlpha(*a.art, pose)) {
            out.back().alpha = *alpha; // smoke and the cloud's puffs (LoopAndFade)
        }
        // (AnimateDraw and SphericalShieldDraw go through the generic draw: lingering, it fades
        // them over their own alpha, and notes the image.)
        tag(out.back(), a.fade, a.art, a.z, !a.art->fades);
        out.back().drawnImage = &a.drawn;
        out.back().notesImage = a.art->fades;
    }

    rainSprites(out);  // the vortex and rain orbs
    // Bubbles sort by their own position, as the drawables they are in the game.
    const float blend = stepBlend();
    for (Bubble& b : bubbles_) {
        const auto x = static_cast<float>(b.x);
        const auto ground = static_cast<float>(b.y);
        const float y = ground - (static_cast<float>(b.prevZ) + static_cast<float>(b.z - b.prevZ) * blend);
        MapView::SceneSprite s{ground, x, y, nullptr, x, ground};
        tag(s, b.fade, b.art, static_cast<float>(b.z), true);
        s.draw = [this, x, y, size = b.size, palette = b.palette](SDL_Renderer* renderer, const Camera& camera) {
            fxdraw::glow(renderer, camera, glowTexture(renderer), x, y, palette.glow, static_cast<float>(size + 4),
                         (size + 3) << 5);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, palette.core.r, palette.core.g, palette.core.b, 255);
            fxdraw::point(renderer, camera, x, y, size >> 1);
        };
        out.push_back(std::move(s));
    }

    // Summons: the SummonEffect animation with its ring, and the creature's ghost.
    const TypeArt* effect = summons_.empty() ? nullptr : art_.get(summonArt_, "SummonEffect");
    const auto frame = static_cast<std::uint32_t>(std::max(0.0, time_));
    for (Summon& m : summons_) {
        const auto fx = static_cast<float>(m.x);
        const auto fy = static_cast<float>(m.y);
        // All its images are its SummonEffect drawable's (SummonEffectDraw, through the generic
        // draw: lingering, the fade replaces their alpha, the ghost's too).
        const auto add = [&](const nox::things::ImageRef& image, float x, float y, const TypeArt& art,
                             std::uint8_t alpha) {
            if (const Sprite* s = sprites.get(image)) {
                MapView::SceneSprite sprite{fy, x - art.halfWidth, y - art.halfHeight - art.z, s, fx, fy};
                sprite.alpha = alpha;
                tag(sprite, m.fade, effect, 0, false);
                sprite.drawnImage = &m.drawn;
                sprite.notesImage = true;
                out.push_back(sprite);
            }
        };
        if (effect && !effect->draw.animations.empty() && !effect->draw.animations.front().frames.empty()) {
            // Its own animation, then again at every 10th of the 256 directions, 32 units out,
            // each a frame further on (the animation drawn as Slave, frame by frame).
            const auto& anim = effect->draw.animations.front();
            Pose pose;
            pose.netCode = m.seed;
            pose.time = time_;
            pose.animStart = m.start;
            if (const auto image = pickImage(*effect, pose)) {
                add(*image, fx, fy, *effect, 255);
            }
            const auto frames = static_cast<std::uint32_t>(anim.frames.size());
            for (std::uint32_t i = 0, k = 0; i < 26; ++i, ++k) {
                if (k >= frames) {
                    k = 0;
                }
                std::uint32_t index = (m.seed + frame + k) / (anim.frameDelay + 1u);
                if (index >= frames) {
                    index %= frames;
                }
                const auto& d = directions()[i * 10];
                const float x = fx + static_cast<float>(d.cos * 2);  // the table is x16: 32 units out
                const float y = fy + static_cast<float>(d.sin * 2);
                if (x >= 0 && x < 0x1700 && y >= 0 && y < 0x1700) {  // on the map (0x1700: 256 tiles of 23 units)
                    add(anim.frames[index], x, y, *effect, 255);
                }
            }
        }
        // The creature's ghost, fading in over the summon (alpha elapsed / duration x 255).
        if (const TypeArt* ghost = art_.get(m.typeId)) {
            Pose pose;
            pose.direction = m.direction;
            pose.animState = 8;
            pose.netCode = m.seed;
            pose.time = time_;
            pose.animStart = m.start;
            if (const auto image = pickImage(*ghost, pose)) {
                const double t = std::clamp((time_ - m.start) / std::max(1u, m.duration), 0.0, 1.0);
                add(*image, fx, fy, *ghost, static_cast<std::uint8_t>(t * 255));
            }
        }
    }
}

SDL_Texture* Effects::glowTexture(SDL_Renderer* renderer)
{
    if (glow_ && glowRenderer_ == renderer) {
        return glow_;
    }
    // The glow disc the exe builds: brightest in the middle, falling linearly to nothing at the
    // edge, first down the rows and then along each row: (1 - |y|/R) * (1 - |x|/halfwidth(y)).
    // Made once at a high resolution and scaled, so it stays smooth at any zoom.
    constexpr int kSize = 128;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        const double v = (y + 0.5) / kSize * 2 - 1;
        const double half = std::sqrt(std::max(0.0, 1 - v * v));
        for (int x = 0; x < kSize; ++x) {
            const double u = (x + 0.5) / kSize * 2 - 1;
            const double f = half > 0 && std::abs(u) < half ? (1 - std::abs(v)) * (1 - std::abs(u) / half) : 0;
            auto* p = &rgba[(static_cast<std::size_t>(y) * kSize + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<std::uint8_t>(std::lround(f * 255));
        }
    }
    if (glow_) {
        SDL_DestroyTexture(glow_);
    }
    glow_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
    glowRenderer_ = renderer;
    if (glow_) {
        SDL_UpdateTexture(glow_, nullptr, rgba.data(), kSize * 4);
        SDL_SetTextureBlendMode(glow_, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(glow_, SDL_SCALEMODE_LINEAR);
    }
    return glow_;
}

// Draws one particle blended `t` (0-1) of the way from its last step to its current one.
void Effects::drawParticle(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow, const Particle& p, float t)
{
    // The glow image centred on the point, then the pixel blob in the particle colour.
    const float x = p.prevX + (static_cast<float>(p.x) / 0x10000 - p.prevX) * t;
    const float y = p.prevY + (static_cast<float>(p.y - p.z) / 0x10000 - p.prevY) * t;
    if (glow && p.glowRadius > 0 && p.glowIntensity > 0) {
        fxdraw::glow(renderer, camera, glow, x, y, p.glow, p.glowRadius, p.glowIntensity);
    }
    if (p.size <= 0) {
        return;
    }
    SDL_SetRenderDrawColor(renderer, p.color.r, p.color.g, p.color.b, 255);
    if (fxdraw::smoothOn()) {
        // The same blob, smooth at its exact position: the squares as squares, the plus as a disc.
        const SDL_Color c = p.color;
        switch (p.size) {
        case 1:
            fxdraw::smoothSquare(renderer, camera, x + 0.5f, y + 0.5f, 1, c);
            break;
        case 2:
            fxdraw::smoothSquare(renderer, camera, x + 1, y + 1, 2, c);
            break;
        case 3:
            fxdraw::smoothDisc(renderer, camera, x + 1.5f, y + 1.5f, 3, c);
            break;
        default:
            fxdraw::smoothSquare(renderer, camera, x + 0.5f, y + 0.5f, static_cast<float>((p.size / 2) * 2 + 1), c);
            break;
        }
        return;
    }
    const float cx = std::floor(x);
    const float cy = std::floor(y);
    const auto rect = [&](float rx, float ry, float w, float h) {
        const SDL_FRect r = camera.toScreen(rx, ry, w, h);
        SDL_RenderFillRect(renderer, &r);
    };
    switch (p.size) {
    case 1:
        rect(cx, cy, 1, 1);
        break;
    case 2:
        rect(cx, cy, 2, 2);
        break;
    case 3:
        rect(cx + 1, cy, 1, 1);
        rect(cx, cy + 1, 3, 1);
        rect(cx + 1, cy + 2, 1, 1);
        break;
    default: {
        const float r = static_cast<float>(p.size / 2);
        rect(cx - r, cy - r, r * 2 + 1, r * 2 + 1);
        break;
    }
    }
}

void Effects::drawParticles(SDL_Renderer* renderer, const Camera& camera, const SightMap* sight, bool glow, bool puffs,
                            bool sorted)
{
    // t: how far the render clock is between the last step and the next, for blending.
    const float t = stepBlend();
    SDL_Texture* glowTex = glow ? glowTexture(renderer) : nullptr;
    if (puffs) {
        drawPuffs(renderer, camera, t); // under the sparks
    }
    drawTailLinks(renderer, camera, t);
    drawDrawables(renderer, camera, glowTex);
    drawRays(renderer, camera, sight, glowTex);
    drawOrbs(renderer, camera, glowTex, t);
    if (const TypeArt* killer = killers_.empty() ? nullptr : art_.get(killerArt_, "UndeadKiller")) {
        // UndeadKillerDraw: a glow of radius size + 4, size = 8 - distance / 40 (1 once that
        // goes negative), and intensity 12 << 5.
        for (const UndeadKiller& k : killers_) {
            if (!k.shown) {
                continue;
            }
            const SDL_FPoint p = killerPosition(k, time_);
            const int distance = static_cast<int>(std::hypot(p.x - static_cast<float>(k.originX), p.y - static_cast<float>(k.originY)));
            int size = 8 - distance / 40;
            if (size < 0) {
                size = 1;
            }
            fxdraw::glow(renderer, camera, glowTex, p.x, p.y - killer->z, {100, 100, 255, 255},
                         static_cast<float>(size + 4), 12 << 5);
        }
    }
    // The particles, then the sparks: with smooth glows, gathered into a few draws (a chain
    // lightning keeps a thousand sparks about, an explosion hundreds of particles: one draw each
    // halved the frame rate), the dots over the glows; else one by one as the game drew them.
    const bool batched = fxdraw::DotBatch::batched();
    fxdraw::DotBatch batch;
    // (The particles without a drawable: the game's pass over the scene draws them wherever they
    // are, over the black too; noxsdl only where the eye sees their ground point, a deliberate
    // deviation. A sorted one, again for the bloom, if the scene drew it.)
    for (const Particle& p : particles_) {
        if (p.sorted && (!sorted || !p.shown)) {
            continue; // drawn in the scene (sceneSprites)
        }
        if (!p.sorted && sight && !sight->visible(static_cast<float>(p.x) / 0x10000, static_cast<float>(p.y) / 0x10000)) {
            continue; // out of sight
        }
        if (!batched) {
            drawParticle(renderer, camera, glowTex, p, t);
            continue;
        }
        // drawParticle's smooth shapes.
        const float x = p.prevX + (static_cast<float>(p.x) / 0x10000 - p.prevX) * t;
        const float y = p.prevY + (static_cast<float>(p.y - p.z) / 0x10000 - p.prevY) * t;
        if (glowTex && p.glowRadius > 0 && p.glowIntensity > 0) {
            batch.glow(camera, x, y, p.glow, p.glowRadius, p.glowIntensity);
        }
        const SDL_Color c{p.color.r, p.color.g, p.color.b, 255};
        switch (p.size) {
        case 0:
            break;
        case 1:
            batch.square(camera, x + 0.5f, y + 0.5f, 1, c);
            break;
        case 2:
            batch.square(camera, x + 1, y + 1, 2, c);
            break;
        case 3:
            batch.disc(camera, x + 1.5f, y + 1.5f, 3, c);
            break;
        default:
            if (p.size > 0) {
                batch.square(camera, x + 0.5f, y + 0.5f, static_cast<float>((p.size / 2) * 2 + 1), c);
            }
            break;
        }
    }
    for (const Spark& s : sparks_) {
        const float x = s.prevX + (static_cast<float>(s.fx >> 12) - s.prevX) * t;
        const float ground = s.prevY + (static_cast<float>(s.fy >> 12) - s.prevY) * t;
        const float z = s.prevZ + (static_cast<float>(s.z) - s.prevZ) * t;
        if (!s.shown) {
            continue; // (the scene's sort: sortOverScene)
        }
        const float y = ground - z - s.zBase;
        const auto glowAt = [&](SDL_Color color, float radius, int intensity) {
            if (batched) {
                if (glowTex) {
                    batch.glow(camera, x, y, color, radius, intensity);
                }
            } else {
                fxdraw::glow(renderer, camera, glowTex, x, y, color, radius, intensity);
            }
        };
        const auto dotAt = [&](SDL_Color color, int size) {
            if (batched) {
                batch.point(camera, x, y, size, {color.r, color.g, color.b, 255});
            } else {
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
                fxdraw::point(renderer, camera, x, y, size);
            }
        };
        if (s.twinkle) {
            // Glow and dot of this frame's random size, in one of two colour pairs.
            const Palette& p = s.twinkleUseAlt ? s.twinkleAlt : s.twinkleMain;
            if (s.twinkleSize > 0) {
                glowAt(p.glow, static_cast<float>(s.twinkleSize * 2 + 1 + 4), (s.twinkleSize + 1) << 5);
                dotAt(p.core, s.twinkleSize);
            }
            continue;
        }
        // Shrinks from 3 to a single pixel, and the glow fades, over its life.
        const int size = s.remaining * 4 / s.total;
        const int intensity = std::min(s.remaining * 5 / s.total, 4) << 5;
        const auto& p = s.type->palette;
        if (glowTex && intensity > 0) {
            // The centre adds glow colour x intensity / 256 (at most half), as the game's glow blit.
            glowAt(p.glow, static_cast<float>(size * 2 + 1 + 4), intensity);
        }
        // The core: a pixel, a 2 x 2 square or the 3-wide plus (the point shapes 1-3).
        dotAt(p.core, std::min(std::max(size, 1), 3));
    }
    if (batched) {
        batch.draw(renderer);
    }
}

void Effects::lights(std::vector<LightSource>& out, bool glowLights) const
{
    // add: the light of a thing type that gives one (LIGHTINTENSITY over 1); the seed varies its flicker.
    const auto add = [&](const TypeArt* art, float x, float y, std::uint32_t seed) {
        if (!art || art->lightIntensity <= 1) {
            return;
        }
        LightSource l;
        l.x = x;
        l.y = y;
        l.intensity = art->lightIntensity;
        l.color = art->lightColor;
        l.flicker = art->flicker;
        l.seed = seed;
        out.push_back(l);
    };
    // A glow of radius r and intensity g (the centre adds colour x g / 256) as a light on the
    // ground under it: intensity r x g / 96, at most 12 (a light adds colour x I / 32 at its
    // centre and reaches about 2.2 I units at 12), so a fresh spark lights a little pool round
    // itself and a crowd of them light an area.
    const auto addGlow = [&](float x, float y, SDL_Color color, float radius, int intensity) {
        const float i = std::min(radius * static_cast<float>(intensity) / 96, 12.0f);
        if (i <= 1) {
            return;
        }
        LightSource l;
        l.x = x;
        l.y = y;
        l.intensity = i;
        l.color = {color.r, color.g, color.b};
        out.push_back(l);
    };
    // Sparks: their type's light, else (per-pixel lighting) one from their glow.
    for (const Spark& s : sparks_) {
        const std::size_t before = out.size();
        const float x = static_cast<float>(s.fx >> 12);
        const float y = static_cast<float>(s.fy >> 12);
        add(s.type->art, x, y, s.start ^ s.death);
        if (s.lightColor && out.size() > before) {
            out.back().color = *s.lightColor; // Draw_LightSetColor on the spawned spark
        }
        // Its type gives no light: with per-pixel lighting, one from its glow instead.
        if (glowLights && out.size() == before) {
            // Its glow as drawParticles draws it.
            if (s.twinkle) {
                if (s.twinkleSize > 0) {
                    const Palette& p = s.twinkleUseAlt ? s.twinkleAlt : s.twinkleMain;
                    addGlow(x, y, p.glow, static_cast<float>(s.twinkleSize * 2 + 5), (s.twinkleSize + 1) << 5);
                }
            } else if (s.total > 0) {
                const int size = s.remaining * 4 / s.total;
                addGlow(x, y, s.type->palette.glow, static_cast<float>(size * 2 + 5),
                        std::min(s.remaining * 5 / s.total, 4) << 5);
            }
        }
    }
    if (glowLights) {
        for (const Particle& p : particles_) {
            if (p.glowRadius > 0 && p.glowIntensity > 0) {
                addGlow(static_cast<float>(p.x) / 0x10000, static_cast<float>(p.y) / 0x10000, p.glow, p.glowRadius,
                        p.glowIntensity);
            }
        }
        // Lightning and energy bolts: lights every 32 units along the beam, in the core glow's
        // colour (drawLightning), brighter for a wider bolt; they overlap into a bright band
        // about 60 units each side.
        for (const Ray& r : rays_) {
            Rgb color;
            float intensity = 24;
            switch (r.kind) {
            case Ray::Kind::Lightning:
            case Ray::Kind::ChainLightning:
                color = {128, 128, 255};
                break;
            case Ray::Kind::GreenBolt:
                color = {150, 220, 150};
                break;
            case Ray::Kind::EnergyBolt:
                color = {255, 255, 0};
                intensity = std::min(24.0f + r.width, 31.0f);
                break;
            default:
                continue;
            }
            const auto a = endPoint(r.from);
            const auto b = endPoint(r.to);
            if (!a || !b) {
                continue;
            }
            const float length = std::hypot(b->x - a->x, b->y - a->y);
            const int steps = std::max(1, static_cast<int>(length / 32));
            for (int i = 0; i <= steps; ++i) {
                const float f = static_cast<float>(i) / static_cast<float>(steps);
                LightSource l;
                l.x = a->x + (b->x - a->x) * f;
                l.y = a->y + (b->y - a->y) * f;
                l.intensity = intensity;
                l.color = color;
                out.push_back(l);
            }
        }
    }
    // Tail links glow in their trail's colour, fading with them (their *TailLinkDraw).
    for (const TailLink& l : tailLinks_) {
        if (l.kind != TailLink::Kind::Magic && l.kind != TailLink::Kind::MagicMissile) {
            continue;
        }
        const float left = static_cast<float>(l.end) - static_cast<float>(time_);
        if (left <= 0) {
            continue;
        }
        LightSource light;
        light.x = l.x1;
        light.y = l.y1;
        light.intensity = left * 20 / static_cast<float>(l.end - l.start);
        light.color = l.kind == TailLink::Kind::Magic ? Rgb{128, 128, 255} : Rgb{255, 128, 50};
        out.push_back(light);
    }
    if (const TypeArt* killer = killers_.empty() ? nullptr : art_.get(killerArt_, "UndeadKiller")) {
        for (const UndeadKiller& k : killers_) {
            const SDL_FPoint p = killerPosition(k, time_);
            add(killer, p.x, p.y, k.start ^ k.direction);
        }
    }
    for (const Animation& a : animations_) {
        add(a.art, a.x, a.y, a.seed);
    }
    if (const TypeArt* effect = summons_.empty() ? nullptr : art_.get(summonArt_, "SummonEffect")) {
        for (const Summon& m : summons_) {
            add(effect, static_cast<float>(m.x), static_cast<float>(m.y), m.seed);
        }
    }
    for (const Orb& o : orbs_) {
        add(o.art, static_cast<float>(o.x), static_cast<float>(o.y), static_cast<std::uint32_t>(o.targetX ^ o.y));
    }
}

} // namespace noxsdl
