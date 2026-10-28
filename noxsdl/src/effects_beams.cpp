// Beams and glow orbs: lightning, chain lightning, energy bolt, green bolt, the orbs of drain
// mana, heal and charm, and sentry rays, drawn the way the 1.2b client draws them.
//
// The game redraws a beam's jagged path from scratch every frame (at 30 fps). Here each beam
// keeps one random seed per game frame and replays it against the ends' current positions, so
// the path changes at the game's rate while beams stay attached to smoothly moving objects at any
// frame rate.
#include "effects.hpp"
#include "effects_draw.hpp"

#include "noxcore/net/fx.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

namespace msg = nox::net::msg;
using nox::net::Op;

/// Beam ends are drawn this far above the ground (the -0x14 in each ray DRAW function).
constexpr float kRayHeight = 20;
/// Glow orbs are drawn this far up (GlowOrbDraw).
constexpr float kOrbHeight = 22;
/// A message ray's end is taken to be on a drawable this close to it.
constexpr float kSnapDistance = 12;

// drawLightningStep's tuning: the jitter for a long beam
// and a short one, the falloff per level (10 / (7 + 9)) and the recursion depth.
constexpr int kJitterLong = 100;
constexpr int kJitterShort = 15;
constexpr int kFalloff = 7;
constexpr int kLightningSteps = 8;

/// The colours of one lightning look (prepareLightningEffects).
struct LightningLook {
    SDL_Color glowA;   ///< first thin glow pass
    SDL_Color glowB;   ///< second thin glow pass
    SDL_Color core;    ///< the core line
    SDL_Color coreGlow; ///< the core's wide glow
    bool thinPasses;   ///< lightning and green bolts draw the two thin passes; energy bolts don't
};
constexpr LightningLook kBlueLightning{{128, 128, 255, 255}, {64, 64, 255, 255}, {255, 255, 255, 255},
                                       {128, 128, 255, 255}, true};
constexpr LightningLook kGreenLightning{{30, 160, 30, 255}, {60, 140, 60, 255}, {40, 225, 40, 255},
                                        {150, 220, 150, 255}, true};
constexpr LightningLook kEnergyBolt{{}, {}, {255, 255, 255, 255}, {255, 255, 0, 255}, false};

/// lightningProc2 + drawLightningStep: the jagged path from `a` to
/// `b` as leaf segments. The midpoint of each span moves by up to +/- jitter x (10/16)^level,
/// the jitter shrinking for short beams, until the depth for the beam's length is reached.
std::vector<std::pair<SDL_FPoint, SDL_FPoint>> lightningPath(SDL_FPoint a, SDL_FPoint b, std::uint32_t seed)
{
    std::minstd_rand rng(seed ? seed : 1);
    const auto random = [&](int lo, int hi) { return nox::util::uniform_int_distribution<int>(lo, hi)(rng); };
    const int length = static_cast<int>(std::hypot(b.x - a.x, b.y - a.y));
    int jitter = kJitterLong;
    int steps = kLightningSteps;
    if (length < 0x200) {
        jitter = (kJitterLong - kJitterShort) * length / 0x200 + kJitterShort;
        steps = length < 0x40 ? kLightningSteps - 3 : length < 0x80 ? kLightningSteps - 2 : length < 0x100 ? kLightningSteps - 1 : kLightningSteps;
    }
    std::vector<std::pair<SDL_FPoint, SDL_FPoint>> out;
    const auto split = [&](auto&& self, SDL_FPoint p, SDL_FPoint q, int depth) -> void {
        if (depth + 1 >= steps) {
            out.emplace_back(p, q);
            return;
        }
        // The displacement scales by 10^depth / 16^depth, in integers as the game does.
        int num = 1;
        int den = 1;
        for (int i = 0; i < depth; ++i) {
            num *= 10;
            den *= kFalloff + 9;
        }
        const int jx = random(-jitter, jitter);
        const int jy = random(-jitter, jitter);
        const SDL_FPoint mid{std::floor((p.x + q.x) / 2) + static_cast<float>(jx * num / den),
                             std::floor((p.y + q.y) / 2) + static_cast<float>(jy * num / den)};
        self(self, p, mid, depth + 1);
        self(self, mid, q, depth + 1);
    };
    split(split, a, b, 1);
    return out;
}

/// One beam in a lightning look (the ray DRAW functions): up to two thin glowing passes, then
/// the core line in its wide glow, each pass along its own random path. `width` adds parallel
/// core lines (energy bolts of higher level).
void drawLightning(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow, SDL_FPoint a, SDL_FPoint b,
                   const LightningLook& look, std::uint8_t width, std::uint32_t seed)
{
    // (Each pass's glow and lines go in one draw each, the lines over the glow: the game drew
    // them segment by segment into its frame, a cost-free order there but thousands of draws
    // here for a chain lightning.)
    if (look.thinPasses) {
        for (const auto& [color, pass] : {std::pair{look.glowA, 1u}, std::pair{look.glowB, 2u}}) {
            const auto path = lightningPath(a, b, seed * 3 + pass);
            fxdraw::glowPath(renderer, camera, glow, path, color, 3, 0x20);
            fxdraw::linePath(renderer, camera, path, color);
        }
    }
    // The core glow brightens with the bolt's width (0x30 at width 0).
    const int intensity = (width + 0x30) & 0xFF;
    const auto path = lightningPath(a, b, seed * 3 + 3);
    fxdraw::glowPath(renderer, camera, glow, path, look.coreGlow, 12, intensity);
    std::vector<fxdraw::Segment> lines(path.begin(), path.end());
    for (const auto& [p, q] : path) {
        // Wider bolts: more lines beside the core, across its minor axis.
        const bool flat = std::abs(q.y - p.y) < std::abs(q.x - p.x);
        // Extra lines only from width 4 up: offsets 1 .. width/2 - 1 on both sides.
        for (int i = 1; (width & 0xFE) > 2 && i < (width >> 1); ++i) {
            for (const float s : {static_cast<float>(i), static_cast<float>(-i)}) {
                const SDL_FPoint o{flat ? 0 : s, flat ? s : 0};
                lines.push_back({{p.x + o.x, p.y + o.y}, {q.x + o.x, q.y + o.y}});
            }
        }
    }
    fxdraw::linePath(renderer, camera, lines, look.core);
}

} // namespace

// A drawable's ground position this frame by net code, or a static map object's; nullopt if
// neither is known.
std::optional<SDL_FPoint> Effects::drawablePoint(std::uint16_t netCode) const
{
    if (!anchors_) {
        return std::nullopt;
    }
    const auto it = anchors_->find(netCode);
    if (it == anchors_->end()) {
        // One of the map's own objects (bit 15: Cli_GetStaticDrawableByNetCode), an obelisk.
        if ((netCode & 0x8000) && staticPoint_) {
            return staticPoint_(static_cast<std::uint16_t>(netCode & 0x7FFF));
        }
        return std::nullopt;
    }
    return SDL_FPoint{it->second.x, it->second.y};
}

Effects::RayEnd Effects::rayEnd(int x, int y) const
{
    // The nearest drawable at the point (the caster or the target), so the end moves with it.
    RayEnd end{0, static_cast<float>(x), static_cast<float>(y)};
    float best = kSnapDistance * kSnapDistance;
    if (anchors_) {
        for (const auto& [netCode, a] : *anchors_) {
            const float dx = a.x - static_cast<float>(x);
            const float dy = a.y - static_cast<float>(y);
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                end = {netCode, -dx, -dy};
            }
        }
    }
    return end;
}

// A beam end in world units: fixed, or its drawable's position plus the offset; nullopt once the
// drawable is gone.
std::optional<SDL_FPoint> Effects::endPoint(const RayEnd& end) const
{
    if (end.netCode == 0) {
        return SDL_FPoint{end.x, end.y};
    }
    const auto p = drawablePoint(end.netCode);
    if (!p) {
        return std::nullopt;
    }
    return SDL_FPoint{p->x + end.x, p->y + end.y};
}

// A message ray between two points, each snapped to a drawable within kSnapDistance; drawn until
// `until`.
void Effects::addPointRay(Ray::Kind kind, int x1, int y1, int x2, int y2, std::uint32_t until)
{
    Ray r;
    r.kind = kind;
    r.from = rayEnd(x1, y1);
    r.to = rayEnd(x2, y2);
    r.until = until;
    r.seed = static_cast<std::uint32_t>(random(1, 0x7FFFFFFF));
    // Its drawable midway between the message's points (each half chopped toward zero).
    r.mid = SDL_Point{(x2 - x1) / 2 + x1, (y2 - y1) / 2 + y1};
    r.fade = nox::client::madeAt(frame_.value_or(0));
    rays_.push_back(r);
}

void Effects::spawnOrb(std::string_view type, int x, int y, int targetX, int targetY, std::optional<int> speed)
{
    // Draw_SpawnOrbTowardPoint and the orb spawns of the *UpdateDraw functions:
    // speed 6-12 a frame unless the caller rolled its own, glow radius 3-10.
    const TypeArt* art = art_.get(type);
    Orb o;
    o.art = art;
    // GlowOrbDraw's colours by type: drain mana blue, charm green, the white
    // orbs white, anything else (heal) orange with a yellow core.
    if (type == "DrainManaOrb" || type == "BlueMoveOrb") {
        o.palette = {{0, 200, 255, 255}, {0, 0, 255, 255}};
    } else if (type == "CharmOrb") {
        o.palette = {{150, 255, 150, 255}, {0, 220, 0, 255}};
    } else if (type == "WhiteOrb" || type == "ManaBombOrb" || type == "WhiteMoveOrb") {
        o.palette = {{255, 255, 255, 255}, {200, 200, 200, 255}};
    } else {
        o.palette = {{255, 255, 0, 255}, {255, 100, 0, 255}};
    }
    o.x = o.prevX = x;
    o.y = o.prevY = y;
    o.targetX = targetX;
    o.targetY = targetY;
    o.speed = speed ? *speed : random(6, 12);
    o.radius = random(3, 10);
    o.fade = nox::client::madeAt(frame_.value_or(0));
    orbs_.push_back(o);
}

void Effects::lightningParticles(std::string_view spark, SDL_FPoint from, SDL_FPoint to, std::uint32_t frame)
{
    // Draw_SpawnLightningParticles: sparks every 8-100 units along the beam.
    const SparkType* type = sparkType(spark);
    const int length = static_cast<int>(std::hypot(to.x - from.x, to.y - from.y));
    if (!type || length <= 0) {
        return;
    }
    for (int d = random(0, length); d <= length; d += random(8, 100)) {
        const int x = static_cast<int>(from.x) + d * static_cast<int>(to.x - from.x) / length;
        const int y = static_cast<int>(from.y) + d * static_cast<int>(to.y - from.y) / length;
        Spark& s = addSpark(*type, x, y, frame);
        s.angle = static_cast<std::uint8_t>(random(0, 255));
        s.speed = random(1, 3000);
        s.death = frame + static_cast<std::uint32_t>(random(5, 20));
        s.z = static_cast<std::int16_t>(random(15, 30));
        s.velZ = static_cast<std::int8_t>(random(-4, 4));
    }
}

// Handles the beam and orb FX messages; true if `fx` was one of them (spawn() then stops).
bool Effects::rayFx(const nox::client::FxEvent& fx, std::uint32_t frame)
{
    const auto& e = fx.fx;
    // A 50% chance of one orb flying from (x, y) +/- 20 to the target, as the ray messages and
    // the *UpdateDraw functions do.
    const auto maybeOrb = [&](std::string_view type, int x, int y, int tx, int ty) {
        if (random(0, 100) < 50) {
            const int dy = random(-20, 20);
            const int dx = random(-20, 20);
            spawnOrb(type, x + dx, y + dy, tx, ty);
        }
    };
    switch (nox::net::fxOp(e)) {
    case Op::FxPlasma:
        if (const auto* m = std::get_if<msg::FxPlasma>(&e)) {
            // The plasma itself (PlasmaDraw) only shows in single-player games; its sparks
            // always do (Draw_SpawnPlasmaSparks).
            if (const SparkType* type = sparkType("BlueSpark")) {
                for (int i = 0; i < 2; ++i) {
                    Spark& s = addSpark(*type, m->x2, m->y2, frame);
                    s.angle = static_cast<std::uint8_t>(random(0, 255));
                    s.speed = random(1, 3000);
                    s.death = frame + static_cast<std::uint32_t>(random(5, 20));
                    s.z = static_cast<std::int16_t>(random(0, 20) + 10);
                    s.velZ = static_cast<std::int8_t>(random(0, 4));
                }
            }
        }
        return true;
    case Op::FxLightning:
        // Message rays last this frame only: the server sends them again every frame they're up.
        if (const auto* m = std::get_if<msg::FxLightning>(&e)) {
            addPointRay(Ray::Kind::Lightning, m->x1, m->y1, m->x2, m->y2, frame + 1);
        }
        return true;
    case Op::FxChainLightningBolt:
        if (const auto* m = std::get_if<msg::FxChainLightningBolt>(&e)) {
            addPointRay(Ray::Kind::ChainLightning, m->x1, m->y1, m->x2, m->y2, frame + 1);
        }
        return true;
    case Op::FxEnergyBolt:
        if (const auto* m = std::get_if<msg::FxEnergyBolt>(&e)) {
            addPointRay(Ray::Kind::EnergyBolt, m->x1, m->y1, m->x2, m->y2, frame + 1);
        }
        return true;
    case Op::FxDrainMana:
        if (const auto* m = std::get_if<msg::FxDrainMana>(&e)) {
            maybeOrb("DrainManaOrb", m->x2, m->y2, m->x1, m->y1);  // from the target (x2, y2) back to the caster
        }
        return true;
    case Op::FxGreaterHeal:
        if (const auto* m = std::get_if<msg::FxGreaterHeal>(&e)) {
            maybeOrb("HealOrb", m->x2, m->y2, m->x1, m->y1);
        }
        return true;
    case Op::FxCharm:
        if (const auto* m = std::get_if<msg::FxCharm>(&e)) {
            maybeOrb("CharmOrb", m->x2, m->y2, m->x1, m->y1);
            maybeOrb("CharmOrb", m->x1, m->y1, m->x2, m->y2);  // and one each way for charm
        }
        return true;
    case Op::FxGreenBolt:
        if (const auto* m = std::get_if<msg::FxGreenBolt>(&e)) {
            // GreenBoltDraw counts its duration down each frame and goes when it reaches 0.
            addPointRay(Ray::Kind::GreenBolt, m->x1, m->y1, m->x2, m->y2,
                        frame + static_cast<std::uint32_t>(std::max(1, m->duration - 1)));
        }
        return true;
    case Op::FxDurationSpell:
        if (const auto* m = std::get_if<msg::FxDurationSpell>(&e)) {
            if (m->kind >= 8) {
                // Kinds 8-14 remove the ray between the same two drawables (Cli_RemoveDurationRay).
                const auto it = std::find_if(rays_.begin(), rays_.end(), [&](const Ray& r) {
                    return r.duration && r.netCode1 == m->netCode1 && r.netCode2 == m->netCode2;
                });
                if (it != rays_.end()) {
                    rays_.erase(it);
                }
                return true;
            }
            // Cli_AddDurationRay: 1 plasma, 2 charm, 3 chain lightning, 4 energy bolt,
            // 5 drain mana, 6 heal, 7 harpoon rope. Plasma (single player only) isn't drawn yet.
            Ray r;
            switch (m->kind) {
            case 7:
                r.kind = Ray::Kind::HarpoonRope;
                break;
            case 2:
                r.kind = Ray::Kind::Charm;
                break;
            case 3:
                r.kind = Ray::Kind::ChainLightning;
                break;
            case 4:
                r.kind = Ray::Kind::EnergyBolt;
                break;
            case 5:
                r.kind = Ray::Kind::DrainMana;
                break;
            case 6:
                r.kind = Ray::Kind::Heal;
                break;
            default:
                return true;
            }
            r.duration = true;
            r.netCode1 = m->netCode1;
            r.netCode2 = m->netCode2;
            // Both ends follow their drawables (no offset).
            r.from = {m->netCode1, 0, 0};
            r.to = {m->netCode2, 0, 0};
            // EnergyBoltDraw's width byte, (level + 0x7F) * 2 as a byte: 0 at level 1, 2 more
            // a level.
            r.width = static_cast<std::uint8_t>((m->level + 0x7F) * 2);
            r.seed = static_cast<std::uint32_t>(random(1, 0x7FFFFFFF));
            // Its drawable midway between its drawables as they are now (sortOverScene works it
            // out once both are known if they aren't yet).
            if (const auto a = endPoint(r.from), b = endPoint(r.to); a && b) {
                const int ax = static_cast<int>(a->x);
                const int ay = static_cast<int>(a->y);
                r.mid = SDL_Point{(static_cast<int>(b->x) - ax) / 2 + ax, (static_cast<int>(b->y) - ay) / 2 + ay};
            }
            r.fade = nox::client::madeAt(frame);
            rays_.push_back(r);
        }
        return true;
    default:
        return false;
    }
}

// Drops the expired rays, then rolls each live ray's path for this frame and spawns its sparks
// or orbs.
void Effects::stepRays(std::uint32_t frame)
{
    std::erase_if(rays_, [frame](const Ray& r) { return !r.duration && frame >= r.until; });
    std::erase_if(sentryRays_, [frame](const SentryRay& r) { return r.frame < frame; });
    for (Ray& r : rays_) {
        // A new path this frame, and what the ray's draw or client-update function spawns.
        r.seed = static_cast<std::uint32_t>(random(1, 0x7FFFFFFF));
        const auto a = endPoint(r.from);
        const auto b = endPoint(r.to);
        if (!a || !b) {
            continue;
        }
        const int ax = static_cast<int>(a->x);
        const int ay = static_cast<int>(a->y);
        const int bx = static_cast<int>(b->x);
        const int by = static_cast<int>(b->y);
        const auto maybeOrb = [&](std::string_view type, int x, int y, int tx, int ty) {
            if (random(0, 100) < 50) {
                const int dx = random(-20, 20);
                const int dy = random(-20, 20);
                spawnOrb(type, x + dx, y + dy, tx, ty);
            }
        };
        switch (r.kind) {
        case Ray::Kind::Lightning:
        case Ray::Kind::ChainLightning:
            lightningParticles("BlueSpark", *a, *b, frame);
            break;
        case Ray::Kind::EnergyBolt:
            lightningParticles("YellowSpark", *a, *b, frame);
            break;
        case Ray::Kind::GreenBolt:
            lightningParticles("GreenSpark", *a, *b, frame);
            break;
        case Ray::Kind::DrainMana: // orbs from the second drawable to the first
            maybeOrb("DrainManaOrb", bx, by, ax, ay);
            break;
        case Ray::Kind::Heal:
            maybeOrb("HealOrb", bx, by, ax, ay);
            break;
        case Ray::Kind::Charm: // both ways (the ray's update twice)
            maybeOrb("CharmOrb", bx, by, ax, ay);
            maybeOrb("CharmOrb", ax, ay, bx, by);
            break;
        case Ray::Kind::HarpoonRope:
            break;
        }
    }
}

void Effects::stepOrbs(std::uint32_t frame)
{
    // LinearOrbUpdateDraw: `speed` units a frame toward the target; gone once within
    // 10 units or past it.
    for (Orb& o : orbs_) {
        o.prevX = o.x;
        o.prevY = o.y;
        if (o.spiral) {
            // In from `distance` to the centre over 60 frames while turning half a
            // circle; gone after that or within 9 of the centre.
            const int elapsed = static_cast<int>(frame - o.start);
            if (elapsed >= 60 || (std::abs(o.x - o.targetX) <= 9 && std::abs(o.y - o.targetY) <= 9)) {
                o.dead = true;
                continue;
            }
            const int r = o.distance - o.distance * elapsed / 60;
            const int turn = elapsed * 256 / 120;
            const int angle = (o.clockwise ? o.angle + turn : o.angle - turn) & 0xFF;
            const double a = angle * 2 * std::numbers::pi / 256;
            const int c = static_cast<int>(std::lround(16 * std::cos(a)));
            const int s = static_cast<int>(std::lround(16 * std::sin(a)));
            o.x = c * r / 16 + o.targetX;
            o.y = s * r / 16 + o.targetY;
            continue;
        }
        const int dx = o.targetX - o.x;
        const int dy = o.targetY - o.y;
        const int dist = static_cast<int>(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)) + 1;
        const int nx = o.speed * dx / dist + o.x;
        const int ny = o.speed * dy / dist + o.y;
        // Close enough, or the next step would carry it past the target (the dot product turns
        // negative).
        if (dist <= 10 || (ny - o.targetY) * (o.y - o.targetY) + (nx - o.targetX) * (o.x - o.targetX) < 0) {
            o.dead = true;
            continue;
        }
        o.x = nx;
        o.y = ny;
        if (o.shrinkEvery != 0 && --o.shrinkCountdown == 0) {
            o.shrinkCountdown = o.shrinkEvery;
            if (--o.radius == 0) {
                o.dead = true;
            }
        }
    }
    std::erase_if(orbs_, [](const Orb& o) { return o.dead; });
}

void Effects::drawHarpoonRope(SDL_Renderer* renderer, const Camera& camera, const Ray& r) const
{
    // HarpoonRopeDraw: from the caster's hand (an offset by its facing, the exe's table) to 8
    // above the target's height; a brown line with a dark one each side of it,
    // above and below when it runs flatter than 45 degrees, else left and right.
    static constexpr std::array<SDL_Point, 9> kHand{{
        {-11, -47}, {12, -47}, {27, -43}, {-28, -42}, {0, 0}, {29, -28}, {-28, -28}, {-13, -17}, {14, -16}}};
    if (!anchors_) {
        return;
    }
    const auto from = anchors_->find(r.netCode1);
    const auto to = anchors_->find(r.netCode2);
    if (from == anchors_->end() || to == anchors_->end()) {
        return;
    }
    const SDL_Point hand = kHand[std::min<std::size_t>(from->second.direction, kHand.size() - 1)];
    const SDL_FPoint a{from->second.x + static_cast<float>(hand.x), from->second.y + static_cast<float>(hand.y)};
    const SDL_FPoint b{to->second.x, to->second.y - to->second.z - 8};
    const bool flat = std::abs(b.y - a.y) < std::abs(b.x - a.x);
    const SDL_FPoint side = flat ? SDL_FPoint{0, 1} : SDL_FPoint{1, 0};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    fxdraw::line(renderer, camera, a, b, {0x90, 0x68, 0x40, 255});
    for (const float s : {-1.0f, 1.0f}) {
        fxdraw::line(renderer, camera, {a.x + side.x * s, a.y + side.y * s}, {b.x + side.x * s, b.y + side.y * s},
                     {0x18, 0x10, 0, 255});
    }
}

void Effects::drawRays(SDL_Renderer* renderer, const Camera& camera, const SightMap* sight, SDL_Texture* glow)
{
    // Each beam the scene's sort keeps (its drawable midway along it: sortOverScene), whole.
    for (const Ray& r : rays_) {
        if (!r.shown) {
            continue;
        }
        const LightningLook* look = nullptr;
        switch (r.kind) {
        case Ray::Kind::Lightning:
        case Ray::Kind::ChainLightning:
            look = &kBlueLightning;
            break;
        case Ray::Kind::EnergyBolt:
            look = &kEnergyBolt;
            break;
        case Ray::Kind::GreenBolt:
            look = &kGreenLightning;
            break;
        default:
            break; // drain, heal and charm rays draw nothing but their orbs
        }
        if (r.kind == Ray::Kind::HarpoonRope) {
            drawHarpoonRope(renderer, camera, r);
            continue;
        }
        const auto a = endPoint(r.from);
        const auto b = endPoint(r.to);
        if (!look || !a || !b) {
            continue;
        }
        drawLightning(renderer, camera, glow, {a->x, a->y - kRayHeight}, {b->x, b->y - kRayHeight}, *look,
                      r.kind == Ray::Kind::EnergyBolt ? r.width : 1, r.seed);
    }

    // Sentry rays: a faint line on the ground and a bright one up at the
    // globe's height between two violet ones, only where the eye can see.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const SentryRay& s : sentryRays_) {
        SDL_FPoint a{static_cast<float>(s.x1), static_cast<float>(s.y1)};
        SDL_FPoint b{static_cast<float>(s.x2), static_cast<float>(s.y2)};
        if (sight) {
            const auto range = sight->visibleRange(a.x, a.y, b.x, b.y);
            if (!range) {
                continue;
            }
            const SDL_FPoint d{b.x - a.x, b.y - a.y};
            b = {a.x + d.x * range->second, a.y + d.y * range->second};
            a = {a.x + d.x * range->first, a.y + d.y * range->first};
        }
        fxdraw::line(renderer, camera, a, b, {100, 40, 100, 128});
        const bool flat = std::abs(b.y - a.y) < std::abs(b.x - a.x);
        const auto up = [&](SDL_FPoint p, float dx, float dy) { return SDL_FPoint{p.x + dx, p.y - 22 + dy}; };
        fxdraw::line(renderer, camera, up(a, 0, 0), up(b, 0, 0), {255, 200, 255, 255});
        for (const float o : {-1.0f, 1.0f}) {
            fxdraw::line(renderer, camera, up(a, flat ? 0 : o, flat ? o : 0), up(b, flat ? 0 : o, flat ? o : 0),
                     {255, 0, 255, 255});
        }
    }
}

void Effects::drawOrbs(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow, float t)
{
    // GlowOrbDraw: a glow of radius + 4, a core dot of radius / 2 and a trail back to
    // where it was a frame ago; the orbs the scene's sort keeps.
    for (const Orb& o : orbs_) {
        if (!o.shown) {
            continue;
        }
        const float x = static_cast<float>(o.prevX) + static_cast<float>(o.x - o.prevX) * t;
        const float ground = static_cast<float>(o.prevY) + static_cast<float>(o.y - o.prevY) * t;
        const float y = ground - kOrbHeight;
        if (glow) {
            fxdraw::glow(renderer, camera, glow, x, y, o.palette.glow, static_cast<float>(o.radius + 4), 5 << 5);
        }
        SDL_SetRenderDrawColor(renderer, o.palette.core.r, o.palette.core.g, o.palette.core.b, 255);
        fxdraw::point(renderer, camera, x, y, o.radius >> 1);
        fxdraw::line(renderer, camera, {x, y},
                 {x - static_cast<float>(o.x - o.prevX), y - static_cast<float>(o.y - o.prevY)}, o.palette.core);
    }
}

} // namespace noxsdl
