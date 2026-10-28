// Client-side visual effects: the short-lived local drawables the game spawns for FX messages
// (CliNet_OnPacket). The server sends only "sparks here" or "explosion there"; the
// client makes up the particles.
//
// Two kinds so far:
//   sparks      glowing dots flung out at random (the *SparkDraw types): each moves
//               along an angle at a speed in 1/4096 units, bounces on the ground, shrinks and
//               fades out by its death frame
//   animations  one-shot animated things (FireBoom, DamagePoof, Smoke, ...) drawn like any
//               other object until their animation or LIFETIME ends
//
// Effects run on the game's clock: they are stepped once per server frame with the game's
// integer maths, and drawn blended between their last two steps so motion stays smooth at any
// frame rate. Events are spawned when the render clock (which runs a little behind the newest
// snapshot) reaches the frame they arrived in, so they line up with the objects that caused them.
//
// In the game they're drawables, so they're seen as drawables are (client/fade.hpp): the scene's
// sort makes the drawable sight test on each (at its position, with its type's shape and ZSIZE),
// and with Fade Objects one seen that leaves sight lingers a game second (at full alpha, but for
// the animations, which go through the generic draw and fade). Those flagged SIGHT_DESTROY
// (sparks, glow orbs, bubbles, vortex and rain orbs, the toxic cloud's puffs, Turn Undead's orbs,
// a particle's own drawable) are deleted once a frame is drawn without them in sight. The
// particles of the particle engine that have no drawable are drawn over the scene wherever they
// are, as the game's pass over the scene draws them (no sight test).
#pragma once

#include "camera.hpp"
#include "lighting.hpp"
#include "map_view.hpp"
#include "sight.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/client/fade.hpp"
#include "noxcore/client/world.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noxsdl {

/// Where a drawable is and what it shows, for effects that follow it or trace its sprite.
struct Anchor {
    float x = 0; ///< on the ground, as drawn this frame
    float y = 0;
    float left = 0; ///< the sprite's draw position (before the image's own offset)
    float top = 0;
    std::optional<std::int32_t> image; ///< video.bag index of the image it shows (a player's body)
    std::uint16_t typeId = 0; ///< its thing type (for its DRAW / CLIENTUPDATE effects)
    float z = 0;              ///< its height above the ground
    std::uint8_t direction = 0; ///< its AnimDir (the harpoon rope leaves from the caster's hand)
    std::uint32_t buffs = 0;    ///< its enchantments (bit n = enchantment n)
    std::uint8_t animState = 0; ///< its AnimInd (a running player's effects shift, state 6)
    bool player = false;
    /// Whether its enchantment effects show: not on an invisible thing unless it's us, a
    /// teammate, or we see the invisible.
    bool effectsShown = true;
    /// Lingering out of sight (Fade Objects, client/fade.hpp): a fireball's CLIENTUPDATE throws no sparks then.
    bool lingering = false;
};
using Anchors = std::unordered_map<std::uint16_t, Anchor>;

class Effects {
    friend class View3D; // draws the effects in 3D (view3d.hpp)
public:
    Effects(const nox::things::ThingDb& things, ThingArt& art);
    ~Effects();
    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;

    /// Queues an FX message; it plays when update() reaches its frame.
    void add(nox::client::FxEvent fx);
    /// Advances to the game frame `time` (the interpolated render clock): spawns the queued
    /// effects that are due and steps everything once per whole frame passed. `anchors` are the
    /// drawables this frame, by net code; they must stay valid until the next update (beams
    /// follow them while drawing).
    void update(double time, const Anchors& anchors, SpriteCache* sprites);
    /// Where a map object the client loaded itself is, by extent: what a net code with bit 15
    /// names (an obelisk at a Drain Mana ray's end), as it has no drawable in the anchors.
    void setStaticPoint(std::function<std::optional<SDL_FPoint>(std::uint16_t)> point) { staticPoint_ = std::move(point); }
    /// Drops everything (a new map).
    void clear();
    /// The view this frame: rains start at its top, and vortex orbs go when they leave it.
    void setView(const Camera& camera)
    {
        view_ = {camera.left(), camera.top(), camera.right() - camera.left(), camera.bottom() - camera.top()};
    }

    /// The animated effects, as sprites to sort into the scene.
    void sceneSprites(SpriteCache& sprites, std::vector<MapView::SceneSprite>& out);
    /// The enchantment effects the game draws round a drawable, behind it or in front of
    /// it, as scene items: push the `front` ones after its sprite and the others
    /// before, with the same sort key, so they sort with it.
    void enchantSprites(const Anchor& a, std::uint16_t netCode, bool front, SpriteCache& sprites,
                        std::vector<MapView::SceneSprite>& out);
    /// The effects drawn over the scene: sparks (an additive glow, if `glow`, and a solid core),
    /// orbs, beams, tail links, the drawables' own effects, the particles; those the scene's sort
    /// (setSceneDraw) keeps. `sight`: the sentry rays' (their own pass).
    /// `puffs`: also the soft smoke puffs (not in the bloom pass: smoke doesn't shine).
    /// sorted: the particles drawn in the scene too (sceneSprites), for the bloom pass.
    void drawParticles(SDL_Renderer* renderer, const Camera& camera, const SightMap* sight, bool glow,
                       bool puffs = true, bool sorted = false);
    /// The scene's sort for the effects, once this frame's sight is built and before the scene is
    /// drawn (each time the scene is drawn in 2D): `sight` (null: everything's in sight), whether
    /// glows are drawn, and Fade Objects. Decides which of the effects drawn over the scene are
    /// drawn (sortOverScene); those sorted into it (sceneSprites) go by the scene's own sort.
    void setSceneDraw(const SightMap* sight, bool glow, bool fadeObjects);
    /// A drawable whose DRAW the effects draw (drawDrawables) was drawn by the scene's sort this
    /// frame (its scene item's draw calls this).
    void showDrawable(std::uint16_t netCode);
    /// The light the soft puffs are tinted by (the scene's sprites are lit; nullptr: unlit).
    void setLight(const LightMap* light) { light_ = light; }
    /// The light every effect gives: they're all LIGHT things. `glowLights` (per-pixel lighting):
    /// also a light under every glowing particle and spark and along lightning and energy bolts,
    /// which the game's glows don't give, so effects light what's round them.
    void lights(std::vector<LightSource>& out, bool glowLights = false) const;

    /// Screen shake (MSG_FX_JIGGLE): world units to add to the camera's y this game frame.
    int jiggle() const { return jiggle_; }
    /// Whether `frame` is a white-flash frame (MSG_FX_WHITE_FLASH): the game clears the whole
    /// screen to white instead of drawing it.
    bool whiteFlash(std::uint32_t frame) const { return whiteFlash_ && *whiteFlash_ == frame; }

    /// This client's gamedata.bin (its own copy, as the game's client reads its own) as a game
    /// with `gameFlags` reads it: ManaBombOutRadius, the ring the mana bomb's sparks fill
    /// (CliNet_OnPacket's MSG_FX_MANA_BOMB_CANCEL, updDrawManabombCharge, which read it each
    /// time). Looked up again only when the file or the flags change (call it each frame).
    void setGameData(const nox::things::GameData* data, std::uint32_t gameFlags);
    std::size_t size() const
    {
        return sparks_.size() + animations_.size() + particles_.size() + rays_.size() + orbs_.size() +
               killers_.size() + summons_.size() + bubbles_.size() + vortexOrbs_.size() + rainOrbs_.size();
    }

private:
    struct Palette {
        SDL_Color core{};
        SDL_Color glow{};
    };
    struct SparkType {
        Palette palette;
        const TypeArt* art = nullptr; ///< for its light
    };
    struct Spark {
        const SparkType* type = nullptr;
        std::int32_t fx = 0; ///< position in 1/4096 units
        std::int32_t fy = 0;
        std::uint8_t angle = 0; ///< 256 per turn, 0 east, 64 south
        std::int32_t speed = 0; ///< 1/256 units per frame
        std::int16_t z = 0;
        std::int8_t velZ = 0;
        std::uint32_t start = 0; ///< game frame spawned
        std::uint32_t death = 0; ///< game frame it's gone
        float prevX = 0, prevY = 0, prevZ = 0; ///< at the previous step, for blending
        std::int32_t remaining = 0, total = 1; ///< frames left and lifetime at the last step
        bool dead = false;
        /// Twinkles (PixieDustDraw, MagicSparkleDraw) don't move; each frame they pick
        /// a random size up to their remaining life, and one of two colour pairs.
        bool twinkle = false;
        Palette twinkleMain;
        Palette twinkleAlt;
        int twinkleSize = 0;
        bool twinkleUseAlt = false;
        std::int16_t zBase = 0; ///< the spawning drawable's Z (drawn this much higher)
        std::optional<std::array<std::uint8_t, 3>> lightColor; ///< instead of its type's
        /// BlueRainSparkDraw: on its first real bounce it becomes a white spark.
        bool splash = false;
        nox::client::FadeState fade; ///< its drawable's marks for the scene's sort (madeAt its start)
        bool shown = true;           ///< the sort draws it this frame (sortOverScene)
    };
    struct Animation {
        const TypeArt* art = nullptr;
        float x = 0;
        float y = 0;
        float z = 0;
        std::uint32_t start = 0;
        std::uint32_t end = 0; ///< first frame it isn't drawn
        std::uint16_t seed = 0;
        std::uint16_t follow = 0; ///< stays on this drawable (y + 3) and goes with it (0: none)
        int rise = 0;             ///< added to z every frame (a toxic cloud's puffs)
        /// Sorted with the scene (SceneSprite::fade): its marks, and the generic draw's last image.
        nox::client::FadeState fade;
        nox::client::DrawnImage drawn;
        bool sightDestroy = false; ///< the client makes it SIGHT_DESTROY (a toxic cloud's puffs)
    };
    /// A creature being summoned (Cli_StartSummonEffect, SummonEffectDraw):
    /// the SummonEffect animation at the spot and in a ring round it, and the creature's ghost
    /// fading in, until the summon lands or MSG_FX_SUMMON_CANCEL.
    struct Summon {
        std::uint16_t id = 0;
        int x = 0, y = 0;
        std::uint16_t typeId = 0;
        std::uint8_t direction = 0; ///< AnimDir
        std::uint32_t start = 0;
        std::uint32_t duration = 0;
        std::uint16_t seed = 0; ///< stands in for its drawable's net code in the ring's frames
        nox::client::FadeState fade;   ///< its SummonEffect drawable's marks (all its images share them)
        nox::client::DrawnImage drawn; ///< the generic draw's last image of it
    };

    /// A particle of the MSG_FX_PARTICLEFX engine (particleFxNew): a point in
    /// 16.16 fixed point with velocity, damping, gravity and bounce, drawn as a pixel blob
    /// and/or a glow disc.
    struct Particle {
        enum class Motion : std::uint8_t {
            Physics, ///< (the default)
            Sway,    ///< sparkles: a widening sideways sway while rising
            Swing,   ///< top sparks: swinging about a fixed point
        };
        Motion motion = Motion::Physics;
        std::int32_t x = 0, y = 0, z = 0; ///< 16.16
        std::int32_t vx = 0, vy = 0, vz = 0;
        std::int32_t dampX = 0, dampY = 0, dampZ = 0; ///< 16.16 multipliers, 0 = none
        std::int32_t gravity = 0;
        bool bounces = false;
        std::int32_t bounce = 0; ///< 16.16 speed kept on a bounce, 0 = all
        std::int32_t originX = 0, originY = 0; ///< Sway / Swing centre
        std::int32_t swing = 0, swingSpeed = 0, spread = 0;
        std::int32_t life = 0, delay = 0;
        std::uint16_t attached = 0; ///< follows this drawable's movement (0 none)
        SDL_Color color{255, 255, 255, 255};
        int size = 0; ///< pixel blob: 0 none, 1 dot, 2 square, 3 plus, more a disc
        SDL_Color glow{};
        float glowRadius = 0; ///< 0 no glow
        std::uint8_t glowIntensity = 0;
        float prevX = 0, prevY = 0; ///< drawn position (y less height) at the previous step
        /// It has a "Particle" drawable (converge, smoke embers): drawn by ParticleDraw
        /// in the sorted drawables at its ground y, not in the pass over the scene
        /// (skips it: flag 8).
        bool sorted = false;
        /// A sorted one's drawable (Particle, SIGHT_DESTROY): its marks, and whether the scene's
        /// sort drew it this frame (for the bloom pass).
        nox::client::FadeState fade;
        bool shown = false;
    };
    /// A periodic particle source: runs every `period` frames, `max` times.
    struct Spawner {
        std::uint16_t attached = 0;
        int period = 1, counter = 1, count = 0, max = 0;
    };

    /// One end of a beam: a fixed point, or a drawable plus an offset from it, so the beam
    /// follows the drawable's smooth (interpolated) motion between game frames.
    struct RayEnd {
        std::uint16_t netCode = 0; ///< 0: fixed at (x, y)
        float x = 0;
        float y = 0;
    };
    /// A beam: the ray drawables of Draw_CreateRayFromMsg (for one game frame, as the server
    /// sends them every frame) and Cli_AddDurationRay (until removed).
    struct Ray {
        enum class Kind : std::uint8_t {
            Lightning,      ///< LightningDraw
            ChainLightning, ///< ChainLightningBoltDraw
            EnergyBolt,     ///< EnergyBoltDraw
            GreenBolt,      ///< GreenBoltDraw
            DrainMana,      ///< DrainManaUpdateDraw: orbs, no beam
            Heal,           ///< HealUpdateDraw
            Charm,          ///< CharmUpdateDraw
            HarpoonRope,    ///< HarpoonRopeDraw
        };
        Kind kind = Kind::Lightning;
        RayEnd from;
        RayEnd to;
        bool duration = false;                    ///< until MSG_FX_DURATION_SPELL removes it
        std::uint16_t netCode1 = 0, netCode2 = 0; ///< a duration ray's drawables, as sent
        std::uint32_t until = 0;                  ///< message rays: the first frame it's gone
        std::uint8_t width = 0;                   ///< energy bolt: (level - 1) * 2
        std::uint32_t seed = 0;                   ///< this game frame's random path
        /// Its drawable stands where it was made, midway between its ends (each half chopped
        /// toward zero), and goes by the sight test there: the whole beam is drawn or none of it.
        std::optional<SDL_Point> mid;
        nox::client::FadeState fade;
        bool shown = true;
    };
    /// A glow orb flying to a point (LinearOrbUpdateDraw + GlowOrbDraw): drain mana, heal and
    /// charm orbs.
    struct Orb {
        const TypeArt* art = nullptr;
        Palette palette;
        std::int32_t x = 0, y = 0;         ///< as the game moves it
        std::int32_t prevX = 0, prevY = 0; ///< a step ago: the trail and the blend start
        std::int32_t targetX = 0, targetY = 0;
        int speed = 0;
        int radius = 0;
        int shrinkEvery = 0, shrinkCountdown = 0; ///< 0: keeps its size
        bool dead = false;
        /// A mana bomb charge's orb spirals into (targetX, targetY)
        /// instead: half a turn in 60 frames from `distance` out, starting at `angle`.
        bool spiral = false;
        bool clockwise = false;
        int angle = 0;
        int distance = 0;
        std::uint32_t start = 0;
        nox::client::FadeState fade;
        bool shown = true;
    };
    /// A sentry globe's ray (Draw_AddSentryRay), drawn for the frame it came in.
    struct SentryRay {
        std::int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        std::uint32_t frame = 0;
    };

    /// What the effects remember about a drawable between game frames (the game keeps it in
    /// the drawable): where it was, where its last tail link ended, a pixie's bob.
    struct DrawableFx {
        float prevX = 0, prevY = 0;   ///< at the previous step
        float fromX = 0, fromY = 0;   ///< where the last step started (a pixie's streak)
        float trailX = 0, trailY = 0; ///< the end of its last tail link
        float bob = 0;                ///< PixieDraw's height
        std::uint32_t firstSeen = 0;  ///< the step it appeared
        int magicSize = 0;            ///< MagicDraw / MagicMissileDraw's size this frame
        std::optional<LightSource> light; ///< set by its DRAW function each frame
        std::uint32_t seen = 0;           ///< the last step it was there
        /// A fist or meteor's fall (MSG_FX_DELTAZ_SPELL_START, then FistUpdateDraw /
        /// MeteorUpdateDraw): its height and speed, which replace the drawable's Z.
        float fallZ = 0, fallPrevZ = 0, fallSpeed = 0, fallBounce = 0;
        /// The scene's sort drew it this frame (showDrawable): its DRAW's effects are drawn.
        bool shown = false;
    };
    /// A missile's or arrow's trail segment (the *TailLinkDraw types): a half-transparent line
    /// whose colour runs down a table as it expires.
    struct TailLink {
        enum class Kind : std::uint8_t { Magic, MagicMissile, Arrow, WeakArrow };
        Kind kind = Kind::Magic;
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0; ///< its drawable stands at (x1, y1)
        std::uint32_t start = 0, end = 0;
        nox::client::FadeState fade;
        bool shown = true;
    };

    /// One of Turn Undead's ring of 43 (Draw_SpawnTurnUndeadRing): an UndeadKiller
    /// flying out from the caster at 4 units a frame, a blue glow that shrinks with
    /// distance and sheds white orbs back to the centre (UndeadKillerDraw).
    struct UndeadKiller {
        int originX = 0, originY = 0;
        std::uint8_t direction = 0;
        std::uint32_t start = 0;
        nox::client::FadeState fade;
        bool shown = true;
    };

    /// A *BubbleParticle (drawn by BubbleDraw): a glow and dot that
    /// pulses between sizes 0 and 4, drifts up or down by `rise` on three frames in four, and
    /// once its life is up shrinks away.
    struct Bubble {
        Palette palette;
        int x = 0, y = 0;
        int z = 0, prevZ = 0;
        int size = 0;
        std::uint8_t phase = 1; ///< 1 growing, 2 shrinking, 3 dying
        int counter = 0, period = 0;
        int rise = 0;
        std::uint32_t deadline = 0;
        const TypeArt* art = nullptr; ///< its type (the sight test's shape)
        nox::client::FadeState fade;
    };

    /// A vortex's orb (VortexSourceClientUpdate, VortexDraw): circling its
    /// source 2-3 steps of 256 a frame either way, rising a unit a frame while its circle
    /// shrinks from 50 by an eighth of its height, until it's gone or off the screen.
    struct VortexOrb {
        int cx = 0, cy = 0; ///< the source, when it was made
        int sortX = 0;      ///< where it was made: its drawable's position
        int sortY = 0;
        int angle = 0;
        int step = 0;
        int radius = 50;
        int z = 0;
        nox::client::FadeState fade;
    };
    /// A falling orb of the level-up rain (LevelUpDraw / OblivionUpDraw -> thing_falling_sparks_draw, drawn by
    /// RainOrbDraw): drops from the top of the view onto the player, then flies out
    /// 150 units as a move orb.
    struct RainOrb {
        bool blue = false;
        int x = 0, y = 0;
        int z = 0, prevZ = 0;
        int velZ = 0;
        int size = 0;
        int targetX = 0, targetY = 0;
        nox::client::FadeState fade;
    };

    /// A soft puff of gas or smoke (effects_puffs.cpp, drawn by fx_puff): not the game's; with
    /// smooth effects on, toxic clouds and smoke blasts are made of these instead of the game's
    /// GreenPuff / GreenSmoke / Smoke sprites, whose hard edges show.
    struct Puff {
        float x = 0, y = 0, z = 0;
        float prevX = 0, prevY = 0, prevZ = 0, prevRadius = 0;
        float vx = 0, vy = 0, vz = 0;
        float drag = 1;        ///< velocity kept each frame
        float radius = 0;
        float grow = 0;        ///< radius added each frame
        SDL_Color color{};     ///< alpha: opacity at its thickest
        std::uint32_t start = 0, end = 0;
        std::uint32_t seed = 0;
        /// Seen as the drawable it stands in for: a toxic cloud's GreenPuff (SIGHT_DESTROY) or a
        /// smoke blast's Smoke (lingering and fading as the generic draw fades it).
        const TypeArt* art = nullptr;
        bool sightDestroy = false;
        nox::client::FadeState fade;
        bool shown = true;
    };

    const SparkType* sparkType(std::string_view name);
    /// The game frame drawn now (the scene's sort and fade go by it).
    std::uint32_t drawnFrame() const { return static_cast<std::uint32_t>(std::max(0.0, time_)); }
    /// The scene's sort for one effect drawn over the scene (client/fade.hpp): the drawable sight
    /// test with its type `art` at (x, y), `z` high, then sortStep; whether it's drawn.
    bool sortEffect(nox::client::FadeState& fade, const TypeArt* art, float x, float y, float z);
    void sortOverScene();
    /// Deletes the SIGHT_DESTROY effects not drawn in sight in the last frame whose draw is over.
    void destroyUnseen();
    const TypeArt* tailArt(int kind);
    const TypeArt* rayArt(int kind);
    // Soft puffs (effects_puffs.cpp).
    void cloudPuff(int x, int y, int reach, std::uint32_t frame);
    void smokeBlast(int x, int y, std::uint32_t frame);
    void stepPuffs(std::uint32_t frame);
    void drawPuffs(SDL_Renderer* renderer, const Camera& camera, float t);
    // Toxic clouds, vortices and the rains (effects_rain.cpp).
    void stepRain(const Anchor& a, const TypeArt& art, std::uint32_t frame);
    void stepRainEffects(std::uint32_t frame);
    void rainSprites(std::vector<MapView::SceneSprite>& out);
    /// The bubbles' types (BubbleDraw things) addBubble makes, by their dot's colour.
    enum class BubbleKind : std::uint8_t { LightBlue, Red, White, Orange, Green, Violet, LightViolet, Yellow };
    void addBubble(BubbleKind kind, int x, int y, int z, int size, int period, int rise, int life, std::uint32_t frame);
    void stepEnchantments(std::uint32_t frame);
    void stepBubbles(std::uint32_t frame);
    int random(int lo, int hi); ///< inclusive, like Math_RandIntVisual
    int randomBelow(int lo, int hi);      ///< [lo, hi), like the game's own
    std::int32_t randomFixed(int lo, int hi); ///< 16.16 in [lo, hi), like the game's own
    Particle& addParticle(int x, int y, int z, int life);
    void particleFx(const nox::client::FxEvent& fx);
    void stepParticles(std::uint32_t frame);
    void drawParticle(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow, const Particle& p, float t);
    void spawn(const nox::client::FxEvent& fx, std::uint32_t frame);
    Spark& addSpark(const SparkType& type, int x, int y, std::uint32_t frame);
    void sparkBurst(std::string_view type, int count, int maxSpeed, int minLife, int x, int y, std::uint32_t frame);
    void explosionSparks(std::string_view type, int x, int y, int size, std::uint32_t frame);
    void addAnimation(std::string_view type, int x, int y, int z, std::uint32_t frame);
    void step(std::uint32_t frame);
    SDL_Texture* glowTexture(SDL_Renderer* renderer);

    // Beams and orbs (effects_beams.cpp).
    bool rayFx(const nox::client::FxEvent& fx, std::uint32_t frame);
    RayEnd rayEnd(int x, int y) const;
    std::optional<SDL_FPoint> endPoint(const RayEnd& end) const;
    std::optional<SDL_FPoint> drawablePoint(std::uint16_t netCode) const;
    void addPointRay(Ray::Kind kind, int x1, int y1, int x2, int y2, std::uint32_t until);
    void spawnOrb(std::string_view type, int x, int y, int targetX, int targetY, std::optional<int> speed = {});
    void lightningParticles(std::string_view spark, SDL_FPoint from, SDL_FPoint to, std::uint32_t frame);
    void stepRays(std::uint32_t frame);
    void stepOrbs(std::uint32_t frame);
    SDL_FPoint killerPosition(const UndeadKiller& k, double frame) const;
    void drawRays(SDL_Renderer* renderer, const Camera& camera, const SightMap* sight, SDL_Texture* glow);
    void drawHarpoonRope(SDL_Renderer* renderer, const Camera& camera, const Ray& r) const;
    void drawOrbs(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow, float t);

    // Effects drawables make on their own (effects_objects.cpp): their CLIENTUPDATE functions
    // and the DRAW functions that spawn or draw effects.
    void stepDrawables(std::uint32_t frame);
    void addTwinkle(const SparkType& type, int x, int y, int z, int zBase, int life, Palette main, Palette alt,
                    std::array<std::uint8_t, 3> light, std::uint32_t frame);
    void drawDrawables(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* glow);
    void drawTailLinks(SDL_Renderer* renderer, const Camera& camera, float t);

public:
    /// The light a drawable's DRAW function gives it this frame (a missile's flicker), if it
    /// sets one; use it instead of the type's.
    std::optional<LightSource> drawableLight(std::uint16_t netCode) const;
    /// The height a falling fist or meteor is drawn at (its client update sets it), blended
    /// between game frames; nullopt for other drawables.
    std::optional<float> drawableZ(std::uint16_t netCode, std::uint16_t typeId) const;

private:

    const nox::things::ThingDb& things_;
    ThingArt& art_;
    std::deque<nox::client::FxEvent> pending_;
    /// Fists and meteors whose fall (MSG_FX_DELTAZ_SPELL_START) has arrived but not yet played on
    /// the render clock, by net code: their start height. The drawable shows as soon as it
    /// arrives, so without this it sat on the ground until its fall began.
    std::map<std::uint16_t, float> fallPending_;
    std::vector<Spark> sparks_;
    std::vector<Animation> animations_;
    std::vector<Particle> particles_;
    const SightMap* sceneSight_ = nullptr;
    bool sceneGlow_ = true;
    bool fadeObjects_ = true;
    nox::client::SortedFrames sortedFrames_; ///< the frames the scene's sort ran for (setSceneDraw)
    std::array<const TypeArt*, 4> tailArts_{};  ///< by TailLink::Kind (tailArt)
    std::array<const TypeArt*, 8> rayArts_{};   ///< by Ray::Kind (rayArt)
    const TypeArt* particleArt_ = nullptr;      ///< Particle: a sorted particle's drawable
    // Types the effects name, looked up once each (ThingArt::Named).
    mutable ThingArt::Named killerArt_;         ///< UndeadKiller (the 3D view's draw too)
    mutable ThingArt::Named summonArt_;         ///< SummonEffect
    mutable std::array<ThingArt::Named, 2> rainOrbArts_; ///< RainOrbWhite, RainOrbBlue
    mutable ThingArt::Named vortexOrbArt_;      ///< WhiteVortexOrb
    std::array<ThingArt::Named, 9> reflectArts_; ///< the reflective shield's types, by AnimDir
    /// The bubbles' types (addBubble: BubbleKind), looked up once each.
    std::array<ThingArt::Named, 8> bubbleArts_;
    /// A named image's animation the enchantments draw (ConfusedBirdies, SphericalShieldAnim):
    /// the first image list entry of that name with frames, found once.
    struct NamedAnimation {
        const nox::things::Animation* animation = nullptr;
        bool looked = false;
    };
    NamedAnimation birdies_;
    NamedAnimation shieldAnim_;
    const nox::things::Animation* namedAnimation(NamedAnimation& slot, std::string_view name);
    std::vector<Puff> puffs_;
    const LightMap* light_ = nullptr;
    std::uint32_t puffSeed_ = 1;
    std::vector<Spawner> spawners_;
    std::vector<Ray> rays_;
    std::vector<Orb> orbs_;
    std::vector<SentryRay> sentryRays_;
    std::unordered_map<std::uint16_t, DrawableFx> drawableFx_;
    std::vector<TailLink> tailLinks_;
    std::vector<UndeadKiller> killers_;
    std::vector<Summon> summons_;
    std::vector<Bubble> bubbles_;
    std::vector<VortexOrb> vortexOrbs_;
    std::vector<RainOrb> rainOrbs_;
    SDL_FRect view_{}; ///< the world area on screen (setView)
    std::unordered_map<std::uint16_t, std::pair<int, int>> enchantPrev_; ///< positions a step ago
    std::minstd_rand drawRng_{1}; ///< randomness the game rolls while drawing, reseeded per frame
    int jiggle_ = 0;
    /// setGameData's file and flags, and the number it read for them.
    const nox::things::GameData* gameData_ = nullptr;
    std::uint32_t gameFlags_ = 0;
    bool numbersRead_ = false;
    int manaBombOutRadius_ = 0; ///< ManaBombOutRadius (whole units, as Math_FloatToInt chops it)
    std::optional<std::uint32_t> whiteFlash_;
    const Anchors* anchors_ = nullptr; ///< the latest update()'s (owned by the caller)
    std::function<std::optional<SDL_FPoint>(std::uint16_t)> staticPoint_;
    SpriteCache* sprites_ = nullptr;
    std::unordered_map<std::uint16_t, std::pair<float, float>> anchorsAtStep_; ///< for attached motion
    std::map<std::string, std::optional<SparkType>, std::less<>> sparkTypes_; ///< by thing name
    std::minstd_rand rng_{12345};
    std::optional<std::uint32_t> frame_; ///< the last frame stepped
    double time_ = 0;
    /// How far the render clock is from the last step towards the next, 0-1: what the drawing
    /// blends the steps' positions by.
    float stepBlend() const { return static_cast<float>(std::clamp(time_ - (frame_ ? *frame_ : 0), 0.0, 1.0)); }
    SDL_Texture* glow_ = nullptr;
    SDL_Renderer* glowRenderer_ = nullptr;
};

} // namespace noxsdl
