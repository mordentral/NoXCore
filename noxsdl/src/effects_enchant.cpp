// Enchantment effects: what the game draws round every drawable for its enchantments, before
// its sprite and after it, called from the drawable loop for
// players and monsters alike.
//
//   behind    anchored (14): a blue glow at the feet; the far half of the protection orbits;
//             the reflective shield (27) when facing away (AnimDir 0-2)
//   in front  confused, held, charmed or anti-magic (3, 5, 28, 29): the ConfusedBirdies
//             animation over the head (blue for anti-magic); vampirism (13): ten magenta dots;
//             the near half of the protection orbits; shield (26): SphericalShieldAnim at half
//             alpha; the reflective shield otherwise
//
// With HD Spell FX the Force Field (shield) is the fx_barrier sphere and the reflective shield the
// fx_ripple water disc, each in its sprite's place.
//   orbits    protection from fire, poison and electricity (17, 18, 20; client_drawXxxProtect): two glowing
//             dots with tails, opposite each other, circling 32 units out; poison's and
//             electricity's circles are smaller (x 0.707) and tilted opposite ways
//   spawned   while drawn, and here once a game frame: bubbles (BubbleDraw) for
//             haste (9, when moving), running (8), slow (4) and infravision (21), and white
//             sparks for shock (22)
//
// Nothing shows on an invisible drawable unless it's us, a teammate, or we see the invisible.
#include "effects.hpp"
#include "effects_draw.hpp"
#include "reflect_ripple.hpp"

#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

constexpr std::uint32_t bit(int n)
{
    return 1u << n;
}

constexpr std::uint32_t kConfused = bit(3);
constexpr std::uint32_t kSlowed = bit(4);
constexpr std::uint32_t kHeld = bit(5);
constexpr std::uint32_t kRun = bit(8);
constexpr std::uint32_t kHasted = bit(9);
constexpr std::uint32_t kVampirism = bit(13);
constexpr std::uint32_t kAnchored = bit(14);
constexpr std::uint32_t kProtectFire = bit(17);
constexpr std::uint32_t kProtectPoison = bit(18);
constexpr std::uint32_t kProtectShock = bit(20);
constexpr std::uint32_t kInfravision = bit(21);
constexpr std::uint32_t kShock = bit(22);
constexpr std::uint32_t kShield = bit(26);
constexpr std::uint32_t kReflectiveShield = bit(27);
constexpr std::uint32_t kCharming = bit(28);
constexpr std::uint32_t kAntiMagic = bit(29);

constexpr std::uint8_t kRunState = 6;

/// Offsets by AnimDir while running: the birdies and the shield.
constexpr std::array<SDL_Point, 9> kBirdiesRun{
    {{-14, 0}, {0, 0}, {8, 0}, {-18, 12}, {0, 0}, {16, 15}, {-14, 20}, {0, 22}, {16, 15}}};
constexpr std::array<SDL_Point, 9> kShieldRun{
    {{-7, 0}, {-3, 0}, {2, -2}, {-10, 0}, {0, 0}, {5, 0}, {-5, 10}, {0, 12}, {4, 10}}};
/// The reflective shield's offset and type by AnimDir (loadReflSheild).
constexpr std::array<SDL_Point, 9> kReflectOffset{
    {{-18, -18}, {0, -18}, {18, -18}, {-18, 0}, {0, 0}, {18, 0}, {-18, 18}, {0, 18}, {18, 18}}};
constexpr std::array<std::string_view, 9> kReflectType{
    "ReflectiveShieldNW", "ReflectiveShieldN",  "ReflectiveShieldNE", "ReflectiveShieldW", "",
    "ReflectiveShieldE",  "ReflectiveShieldSW", "ReflectiveShieldS",  "ReflectiveShieldSE"};

SDL_Point offsetFor(const std::array<SDL_Point, 9>& table, std::uint8_t dir)
{
    return table[std::min<std::size_t>(dir, table.size() - 1)];
}

/// Draws a sprite at a world point (before its own offset) with a colour multiply and alpha,
/// as the game's animated-image draw does.
void drawSpriteAt(SDL_Renderer* renderer, const Camera& camera, const Sprite& s, float x, float y, SDL_Color mod,
                  std::uint8_t alpha)
{
    const SDL_FRect dst = camera.toScreen(x + s.offsetX, y + s.offsetY, s.width, s.height);
    s.scale = s.hd ? SDL_SCALEMODE_LINEAR : camera.artScaleMode();
    SDL_SetTextureColorMod(s.texture, mod.r, mod.g, mod.b);
    SDL_SetTextureAlphaMod(s.texture, alpha);
    drawSprite(renderer, s, nullptr, dst);
    SDL_SetTextureColorMod(s.texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(s.texture, 255);
}

} // namespace

void Effects::addBubble(BubbleKind kind, int x, int y, int z, int size, int period, int rise, int life, std::uint32_t frame)
{
    // the dot's colour by type; the glow is the type's light colour.
    struct Type {
        std::string_view name;
        SDL_Color core;
    };
    static constexpr std::array<Type, 8> kTypes{{
        {"LightBlueBubbleParticle", {200, 200, 255, 255}},
        {"RedBubbleParticle", {255, 128, 128, 255}},
        {"WhiteBubbleParticle", {255, 255, 255, 255}},
        {"OrangeBubbleParticle", {255, 100, 50, 255}},
        {"GreenBubbleParticle", {64, 255, 64, 255}},
        {"VioletBubbleParticle", {255, 100, 255, 255}},
        {"LightVioletBubbleParticle", {255, 200, 255, 255}},
        {"YellowBubbleParticle", {255, 255, 200, 255}},
    }};
    const auto k = static_cast<std::size_t>(kind);
    Bubble b;
    const SDL_Color core = kTypes[k].core;
    b.palette.core = core;
    b.palette.glow = core;
    if (const TypeArt* art = art_.get(bubbleArts_[k], kTypes[k].name)) {
        b.palette.glow = {art->lightColor[0], art->lightColor[1], art->lightColor[2], 255};
        b.art = art;
    }
    b.fade = nox::client::madeAt(frame);
    b.x = x;
    b.y = y;
    b.z = b.prevZ = z;
    b.size = size;
    b.counter = b.period = period;
    b.rise = rise;
    b.deadline = frame + static_cast<std::uint32_t>(life);
    bubbles_.push_back(b);
}

const nox::things::Animation* Effects::namedAnimation(NamedAnimation& slot, std::string_view name)
{
    // The first entry of the video image list called `name` whose animation has frames.
    if (!slot.looked) {
        slot.looked = true;
        for (const auto& image : things_.images) {
            if (image.name == name && image.animation && !image.animation->frames.empty()) {
                slot.animation = &*image.animation;
                break;
            }
        }
    }
    return slot.animation;
}

// Once a game frame: the bubbles and sparks enchantments spawn round each drawable (the
// "spawned" row above). `moved`: it changed whole-unit position since the last frame.
void Effects::stepEnchantments(std::uint32_t frame)
{
    if (!anchors_) {
        return;
    }
    // Forget the positions of drawables that have gone.
    std::erase_if(enchantPrev_, [&](const auto& e) { return !anchors_->contains(e.first); });
    for (const auto& [netCode, a] : *anchors_) {
        const int x = static_cast<int>(std::lround(a.x));
        const int y = static_cast<int>(std::lround(a.y));
        const auto prev = enchantPrev_.find(netCode);
        const bool moved = prev != enchantPrev_.end() && prev->second != std::pair{x, y};
        enchantPrev_[netCode] = {x, y};
        if (a.buffs == 0 || !a.effectsShown) {  // effectsShown: see Anchor (invisible things)
            continue;
        }
        const TypeArt* art = art_.get(a.typeId);
        const int zMax = art ? static_cast<int>(art->zSizeMax) : 0;
        const int radius = art ? static_cast<int>(art->extentRadius) : 0;
        const int z = static_cast<int>(a.z);
        // Two bubbles a go, near the feet (the game adds the height to y here).
        const auto pair = [&](BubbleKind first, BubbleKind second) {
            for (const auto type : {first, second}) {
                const int life = random(3, 5);
                const int period = random(3, 6);
                const int size = random(2, 4);
                const int by = random(-10, 10) + z + y;
                const int bx = random(-10, 10) + x;
                addBubble(type, bx, by, 1, size, period, 0, life, frame);
            }
        };
        if (a.buffs & kHasted) {
            // Haste shows only while moving: two pairs a frame, none standing still.
            for (int n = moved ? 2 : 0; n > 0; --n) {
                pair(BubbleKind::White, BubbleKind::LightBlue);
            }
        }
        if (a.buffs & kRun) {
            // Running: one pair standing, two while moving.
            for (int n = moved ? 2 : 1; n > 0; --n) {
                pair(BubbleKind::Red, BubbleKind::Orange);
            }
        }
        if (a.buffs & kShock) {
            // Draw_SpawnPlasmaSparks: two white sparks from the body.
            if (const SparkType* type = sparkType("WhiteSpark")) {
                for (int i = 0; i < 2; ++i) {
                    Spark& s = addSpark(*type, x, y, frame);
                    s.angle = static_cast<std::uint8_t>(random(0, 255));
                    s.speed = random(1, 3000);
                    s.death = frame + static_cast<std::uint32_t>(random(5, 20));
                    s.z = static_cast<std::int16_t>(random(0, 20) + z);
                    s.velZ = static_cast<std::int8_t>(random(0, 4));
                }
            }
        }
        if (a.buffs & kSlowed) {
            // Yellow bubbles sinking from anywhere up to half its height.
            for (int i = 0; i < 2; ++i) {
                const int life = random(3, 5);
                const int period = random(3, 6);
                const int size = random(1, 2);
                const int bz = random(0, zMax / 2);
                const int by = random(-radius, radius) + y;
                const int bx = random(-radius, radius) + x;
                addBubble(BubbleKind::Yellow, bx, by, bz, size, period, -5, life, frame);
            }
        }
        if (a.buffs & kInfravision) {
            // A green bubble rising from over its head.
            const int life = random(2, 3);
            const int period = random(3, 6);
            const int size = random(2, 4);
            const int bz = random(zMax + 8, zMax + 12);
            const int by = random(-10, 10) + y;
            const int bx = random(-6, 6) + x;
            addBubble(BubbleKind::Green, bx, by, bz, size, period, 1, life, frame);
        }
    }
}

// Steps every bubble one frame; drops the dead ones and any that sank below the ground.
void Effects::stepBubbles(std::uint32_t frame)
{
    // BubbleDraw, once a frame.
    std::erase_if(bubbles_, [&](Bubble& b) {
        // Life up: switch to dying, shrinking a size every 4 frames.
        if (b.phase != 3 && b.deadline <= frame) {
            b.phase = 3;
            b.counter = b.period = 4;
        }
        if (b.phase == 3 && b.size == 0) {
            return true;
        }
        b.prevZ = b.z;
        // Drifts by `rise` (negative: sinks) on three frames in four.
        if ((frame & 3) != 0) {
            b.z += b.rise;
        }
        // Every `period` frames the size steps: up to 5 then down to 0 and back (phases 1 and 2),
        // or down to nothing while dying.
        if (b.counter != 0 && --b.counter == 0) {
            if (b.phase == 1) {
                if (++b.size > 4) {
                    b.phase = 2;
                }
            } else if (b.phase == 2) {
                if (--b.size == 0) {
                    b.phase = 1;
                }
            } else if (--b.size == 0) {
                return true;
            }
            b.counter = b.period;
        }
        return b.z < 0;
    });
}

// Adds one drawable's enchantment scene items (the header's table): those behind its sprite
// when `front` is false, those in front when true. Called twice per drawable.
void Effects::enchantSprites(const Anchor& a, std::uint16_t netCode, bool front, SpriteCache& sprites,
                             std::vector<MapView::SceneSprite>& out)
{
    if (a.buffs == 0 || !a.effectsShown) {
        return;
    }
    const TypeArt* art = art_.get(a.typeId);
    const float zMax = art ? art->zSizeMax : 0;
    const float radius = art ? art->extentRadius : 0;
    // Scene items sort at the drawable's own ground y, so they go with its sprite.
    const auto push = [&](std::function<void(SDL_Renderer*, const Camera&)> draw) {
        MapView::SceneSprite s{a.y, a.x, a.y, nullptr, a.x, a.y};
        s.draw = std::move(draw);
        out.push_back(std::move(s));
    };
    // sprite: a thing type's current image at a world point, sorted with the drawable.
    const auto sprite = [&](const TypeArt& t, float x, float y) {
        Pose pose;
        pose.netCode = netCode;
        pose.time = time_;
        if (const auto image = pickImage(t, pose)) {
            if (const Sprite* s = sprites.get(*image)) {
                out.push_back({a.y, x - t.halfWidth, y - t.halfHeight - t.z, s, a.x, a.y});
            }
        }
    };
    // animation: a named animation's current frame at a world point, colour-multiplied and faded.
    const auto animation = [&](const nox::things::Animation* anim, float x, float y, SDL_Color mod, std::uint8_t alpha) {
        if (!anim) {
            return;
        }
        Pose pose;
        pose.time = time_;
        const auto index = animationFrame(anim->playback, anim->frames.size(), anim->frameDelay, pose);
        if (const Sprite* s = sprites.get(anim->frames[std::min(index, anim->frames.size() - 1)])) {
            push([s, x, y, mod, alpha](SDL_Renderer* renderer, const Camera& camera) {
                drawSpriteAt(renderer, camera, *s, x, y, mod, alpha);
            });
        }
    };
    // reflect: the reflective shield for the facing. AnimDir 4 is the middle of the 3x3 table
    // (no facing) and has none.
    const auto reflect = [&] {
        const std::size_t dir = std::min<std::size_t>(a.direction, kReflectType.size() - 1);
        const std::string_view type = kReflectType[dir];
        const TypeArt* t = type.empty() ? nullptr : art_.get(reflectArts_[dir], type);
        if (!t) {
            return;
        }
        const SDL_Point off = offsetFor(kReflectOffset, a.direction);
        const float x = a.x + static_cast<float>(off.x);
        const float y = a.y + static_cast<float>(off.y) + a.z;
        // HD Spell FX: the fx_ripple water disc of the facing's shape (reflect_ripple.hpp) about
        // the middle of the art's first image, where the game draws it: in the sprite's place in
        // the scene, for as long and wherever the sprite would be.
        // (Off for now by decision, 2026-10-29: the front's ripples came out too large and too 3D
        // against the original's; the original sprite until it's reworked.)
        constexpr bool kRippleShield = false;
        const auto shape = kRippleShield && fxdraw::rippleOn() ? shieldfx::rippleShape(a.direction) : std::nullopt;
        const nox::things::ImageRef* first = shape ? pickImage(*t, Pose{}) : nullptr;
        const Sprite* firstSprite = first ? sprites.get(*first) : nullptr;
        if (!firstSprite) {
            sprite(*t, x, y);
            return;
        }
        const float left = x - t->halfWidth;
        const float top = y - t->halfHeight - t->z;
        const float cx = left + firstSprite->offsetX + firstSprite->width / 2;
        const float cy = top + firstSprite->offsetY + firstSprite->height / 2;
        // The frame the game would show stays on the item for the 3D view, which draws sprites
        // (the 2D scene draws `draw`).
        Pose pose;
        pose.netCode = netCode;
        pose.time = time_;
        const nox::things::ImageRef* image = pickImage(*t, pose);
        MapView::SceneSprite item{a.y, left, top, image ? sprites.get(*image) : nullptr, a.x, a.y};
        item.draw = [this, cx, cy, s = *shape, netCode, lx = a.x, ly = a.y](SDL_Renderer* renderer, const Camera& camera) {
            // Lit as the sprite is, by the light at the unit's feet.
            SDL_Color light{255, 255, 255, 255};
            if (light_) {
                const Rgb l = light_->sample(lx, ly);
                light = {l[0], l[1], l[2], 255};
            }
            fxdraw::ripple(renderer, camera, cx, cy, s.major, s.minor, s.tiltDegrees, light,
                           shieldfx::rippleLoop(time_, netCode), time_ / 30, s.back);
        };
        out.push_back(std::move(item));
    };
    const bool facingAway = a.direction <= 2;  // AnimDir 0-2: NW, N, NE

    // The protection orbits: the dots whose circle position is behind the centre go behind.
    // The orbit angle in 256ths: 10 a game frame, offset by the net code so drawables differ.
    const double spin = std::fmod((static_cast<double>(netCode) + time_) * 5, 128) * 2;
    const auto orbit = [&](int offset, int tilt, SDL_Color glow, SDL_Color core) {
        const float scale = tilt == 0 ? 1.0f : 0.70710677f;  // the tilted circles are 1 / sqrt(2) the size
        for (int k = 0; k < 2; ++k) {
            const double angle = spin + offset + k * 128;  // k = 1: the opposite dot
            const auto at = [&](double turn256) {
                const double t = turn256 * 2 * std::numbers::pi / 256;
                return SDL_FPoint{a.x + static_cast<float>(std::cos(t) * 32) * scale,
                                  a.y + static_cast<float>(std::sin(t) * 32) * scale};
            };
            SDL_FPoint head = at(angle);
            SDL_FPoint tail = at(angle - 6);  // the tail trails 6 256ths behind
            const bool behind = head.y < a.y;
            if (behind == front) {
                continue;
            }
            float lift = 0;
            // Tilted circles: one side lifted, the other dropped, by the x offset.
            if (tilt == 1) {
                lift = (a.x - head.x) * 35 / 32;
            } else if (tilt == 2) {
                lift = (head.x - a.x) * 35 / 32;
            }
            // Lifted 20 off the ground, plus the drawable's own height.
            head.y += lift - 20 - a.z;
            tail.y += lift - 20 - a.z;
            push([this, head, tail, glow, core](SDL_Renderer* renderer, const Camera& camera) {
                // client_drawXxxProtectParticle.
                fxdraw::glow(renderer, camera, glowTexture(renderer), head.x, head.y, glow, 14, 12 << 5);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer, core.r, core.g, core.b, 255);
                fxdraw::point(renderer, camera, head.x, head.y, 3);
                fxdraw::line(renderer, camera, head, tail, core);
            });
        }
    };

    if (!front) {
        if (a.buffs & kAnchored) {
            push([this, x = a.x, y = a.y](SDL_Renderer* renderer, const Camera& camera) {
                fxdraw::glow(renderer, camera, glowTexture(renderer), x, y, {0, 160, 255, 255}, 34, 31 << 5);
            });
        }
    }
    if (a.buffs & kProtectFire) {
        orbit(0, 0, {255, 0, 0, 255}, {255, 128, 128, 255});
    }
    if (a.buffs & kProtectPoison) {
        orbit(0x55, 1, {0, 255, 0, 255}, {128, 255, 128, 255});  // 0x55: a third of a turn on from fire's
    }
    if (a.buffs & kProtectShock) {
        orbit(0xAA, 2, {0, 160, 255, 255}, {255, 255, 255, 255});  // 0xAA: two thirds
    }
    // Behind items done; everything below is in front.
    if (!front) {
        if ((a.buffs & kReflectiveShield) && facingAway) {
            reflect();
        }
        return;
    }

    const bool running = a.animState == kRunState;
    if (a.buffs & (kConfused | kHeld | kAntiMagic | kCharming)) {  // the birdies
        SDL_Point off{};
        if (a.player && running) {
            off = offsetFor(kBirdiesRun, a.direction);
        }
        const SDL_Color mod = (a.buffs & kAntiMagic) ? SDL_Color{0, 160, 255, 255} : SDL_Color{255, 255, 255, 255};
        // Over the head: the thing's Z (the drawable's ZVal2: NewPlayer's 22) and ZSIZE top above
        // its feet.
        const float drawZ = art ? art->z : 0;
        animation(namedAnimation(birdies_, "ConfusedBirdies"), a.x + static_cast<float>(off.x) - 64,
                  a.y + 5 - drawZ - a.z - zMax + static_cast<float>(off.y) - 64, mod, 255);
    }
    if (a.buffs & kVampirism) {
        // Ten magenta dots over the body, new every frame.
        drawRng_.seed(static_cast<std::uint32_t>(time_) * 7919u + netCode);  // same dots for every draw of one frame
        const auto roll = [&](int lo, int hi) {
            return hi <= lo ? lo : nox::util::uniform_int_distribution<int>(lo, hi)(drawRng_);
        };
        const int r = static_cast<int>(radius);
        for (int i = 0; i < 10; ++i) {
            const int size = roll(1, 2);
            const float x = a.x + static_cast<float>(roll(-r, r));
            const float y = a.y + a.z + static_cast<float>(roll(-10 - static_cast<int>(zMax), 0));
            const int glowSize = roll(3, 4) + size;
            push([this, x, y, size, glowSize](SDL_Renderer* renderer, const Camera& camera) {
                fxdraw::glow(renderer, camera, glowTexture(renderer), x, y, {255, 0, 255, 255},
                             static_cast<float>(glowSize + 4), (size + 2) << 5);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(renderer, 255, 180, 255, 255);
                fxdraw::point(renderer, camera, x, y, size);
            });
        }
    }
    if (a.buffs & kShield) {
        const auto* anim = fxdraw::barrierOn() ? namedAnimation(shieldAnim_, "SphericalShieldAnim") : nullptr;
        if (const Sprite* s = anim ? sprites.get(anim->frames.front()) : nullptr) {
            // HD Spell FX: the fx_barrier sphere, the animation's frames' size, straight over the feet
            // (the drawn position the body's sprite is drawn from), whatever the facing or pose:
            // none of the old art's running offset by direction, which jumps it about as a runner
            // turns. Its height over the feet is the old art's standing centre's, kBarrierDrop lower
            // (that centre sits that far above the body's middle).
            constexpr float kBarrierDrop = 4;
            const float cx = a.x;
            const float cy = a.y - 90 - a.z + s->offsetY + s->height / 2 + kBarrierDrop;
            const float r = std::min(s->width, s->height) / 2;
            push([this, cx, cy, r, netCode](SDL_Renderer* renderer, const Camera& camera) {
                fxdraw::barrier(renderer, camera, cx, cy, r, {90, 190, 255, 255}, time_ / 30, netCode);
            });
        } else {
            // The game's animation at (x - 64, y - 90 - ZVal), shifted by direction while running.
            const SDL_Point off = running ? offsetFor(kShieldRun, a.direction) : SDL_Point{};
            animation(namedAnimation(shieldAnim_, "SphericalShieldAnim"), a.x - 64 + static_cast<float>(off.x),
                      a.y - 90 - a.z + static_cast<float>(off.y), {255, 255, 255, 255}, 0x80);
        }
    }
    if ((a.buffs & kReflectiveShield) && !facingAway) {
        reflect();
    }
}

} // namespace noxsdl
