// The world in first person, Doom style (the console's view3d, not listed by help), over the
// unchanged game logic.
//
// Only the drawing and the mouse change. The game still runs as noxsdl runs it: the client sends
// the same facing, moves and cursor point it would from the 2D view, worked out from the 3D one.
//
// Drawing (draw):
//   floor     drawn by MapView::drawFloor (tiles, edges, the light map) into a texture of the
//             area round the eye, seen from above, with the things that lie on it (BELOW:
//             plates, blood, scorch marks); then that texture laid on the ground plane
//   walls     boxes along each wall cell's arms (the sight code's half-diagonals), their faces
//             textured by the wall's own sprite. Nox's projection is oblique: world (x, y, z) is
//             drawn at screen (x, y - z), so every point of a face has its pixel in the sprite
//             exactly where the 2D view draws it
//   things    billboards facing the eye: the map's objects and the live drawables (noxsdl's
//             scene sprites), each pixel row of the sprite at the height it stands for in the 2D
//             view (row y is height base - y); doors are panels along their leaf
//   order     back to front (painter's), no depth buffer: SDL_Renderer has none
//   look      up and down by shearing (Build engine style), so upright things stay upright and a
//             billboard is a plain screen rectangle drawn through SpriteCache::render (tints,
//             mips, the HD pack all as in 2D)
//
// The mouse (mouse look: no window open, Left Alt not held):
//   facing    the camera's forward, flat (XY only), for every facing the client sends
//   cursor    the crosshair's ray traced into the scene: on a thing's billboard, the thing's foot
//             (as if its foot circle were clicked in 2D); else the ground straight ahead along
//             the flat forward (the floor's hit, a wall's base). Never a height: the game's
//             missiles fly flat. noxsdl gets it as mousePos(), in the 2D view's pixels, so its
//             hover, pickup, use, talk and aiming all work on it unchanged; what it's on gets the
//             2D view's ring at its feet (setRing). An item is hit on a padded box round it
//   speed     its own (Options' Mouse slider while the 3D view is on; noxcore.cfg MouseSpeed3D)
//   clicks    kept off the HUD's windows (they'd be under the 2D cursor point)
//   windows   with any window open (inventory, book, menus, shops, chat) the cursor is back and
//             the windows work as in 2D
//
// Keys: W A S D or the arrows walk (forward, back, sideways); 1-5 the spell slots, E / R the spell
// sets; Left Alt held frees the cursor. Command line (tests): --3d starts in it, --3d-look YAW PITCH
// (degrees) aims it.
#pragma once

#include "camera.hpp"
#include "lighting.hpp"
#include "map_view.hpp"
#include "sight.hpp"
#include "sprite_cache.hpp"
#include "thing_art.hpp"
#include "view3d_gpu.hpp"

#include "noxcore/client/headless_client.hpp"
#include "noxcore/things/modifier.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace noxsdl {

class Effects;

class View3D {
public:
    View3D() = default;
    ~View3D();
    /// Its GPU objects and textures let go (the depth pass, the floor and held-item targets): before
    /// the renderer, whose GPU device they're on, is destroyed (App's view3d outlives it).
    void releaseGpu();
    View3D(const View3D&) = delete;
    View3D& operator=(const View3D&) = delete;

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    /// How far upright pictures lean back (degrees, 0-60): the art is drawn from above, so a
    /// sprite's rows go back by their height above its bottom times tan(tilt), its tops (a
    /// barrel's lid, a sign's edge, a head) receding as seen from above. The console's view3d tilt.
    void setTilt(float degrees) { tiltTan_ = std::tan(std::clamp(degrees, 0.0f, 60.0f) * 0.017453292f); }
    float tilt() const { return std::atan(tiltTan_) * 57.29578f; }
    /// How much taller walls (and doors) stand than pictured, their pictures stretched up evenly
    /// (0-1). The console's view3d walls (percent).
    void setWallRaise(float fraction) { wallRaise_ = std::clamp(fraction, 0.0f, 1.0f); }
    float wallRaise() const { return wallRaise_; }
    /// Degrees: yaw 0 east, 90 south (Nox's facing); pitch up positive.
    void setLook(float yawDegrees, float pitchDegrees);

    /// Each frame before anything else: whether the world is up (a game, or the map viewer), and
    /// whether a window or menu wants the mouse (the cursor is back while one is).
    void beginFrame(bool inWorld, bool windowWantsMouse)
    {
        inWorld_ = inWorld;
        windowWantsMouse_ = windowWantsMouse;
    }
    /// The 3D view draws the world.
    bool active() const { return enabled_ && inWorld_; }
    /// The mouse turns the view and the crosshair aims.
    bool looking() const { return active() && !windowWantsMouse_ && !altHeld_; }

    /// Left Alt, W A S D and the arrows, the mouse look; true when the event is used up.
    /// 1-5, E and R are used up too, setting `control` to the game control to run (the spell
    /// slots, previous / next spell set). In mouse look, button and wheel events are moved off
    /// every window (the crosshair is on the world).
    bool handleEvent(SDL_Event& event, std::string& control);

    /// In mouse look: the crosshair's point in the world as the 2D view's pixel showing it.
    /// `running`: the ground ahead (not a thing) at least a run's distance out along the same
    /// line, so the game runs us there rather than walks (it walks to a cursor within 100).
    std::optional<SDL_FPoint> cursorMouse(const Camera& camera, bool running = false) const;
    /// In mouse look with the crosshair on a thing (cursorMouse is its foot), not the ground.
    bool onThing() const { return looking() && target_ && targetThing_; }
    /// While the 3D view is up: the camera's flat forward as a Nox facing (0 east, 64 south).
    std::optional<std::uint8_t> facing() const;
    /// A movement key held (with no window having the keys). The game moves a player only the
    /// way it faces, so while we aim (`attacking`: Action held, or just after a cast from 1-5)
    /// only forward counts: we face the crosshair's way, as the 2D view faces the cursor.
    bool walking(bool attacking) const;
    /// The facing to run at: the keys' direction from the view's (the view's own while aiming),
    /// or `fallback`.
    std::uint8_t walkFacing(std::uint8_t fallback, bool attacking) const;
    /// The area the light map is built over: the 3D view's, else `camera`.
    Camera worldCamera(const Camera& camera) const;
    /// A drawable's AnimDir (3 x 3 grid, 4 unused) for its picture as the eye sees it: the art is
    /// drawn for an eye to the south looking north, so in 3D the facing is turned by our line of
    /// sight to it (a creature running at us shows its front from any side). As it is in 2D.
    std::uint8_t seenDirection(std::uint8_t direction, float x, float y) const;

    /// The ring at the feet of what's under the crosshair, as the 2D view draws it under what's
    /// under the cursor (noxsdl's drawHoverRing works it out): its centre, radius and colour.
    struct Ring {
        float x = 0, y = 0, radius = 0;
        SDL_Color colour{255, 255, 255, 255};
    };
    /// This frame's ring (none: nothing under the crosshair), drawn on the floor by draw().
    void setRing(std::optional<Ring> ring) { ring_ = ring; }

    enum class Reticle { Plain, Enemy, Friend, Pickup, Use, Talk, Caution, Target };
    /// The crosshair at the view's centre.
    void drawCrosshair(SDL_Renderer* renderer, Reticle reticle = Reticle::Plain) const;

    struct Frame {
        SDL_Renderer* renderer = nullptr;
        MapView& view;
        SpriteCache& sprites;
        Camera& camera; ///< noxsdl's 2D camera: its centre is the eye; the viewer's arrows move it
        LightMap* light = nullptr;
        std::span<const MapView::SceneSprite> extra; ///< the live drawables and effects (noxsdl's)
        const std::set<std::uint32_t>* liveCodes = nullptr;
        const nox::client::HeadlessClient* client = nullptr; ///< live: our equipment, for the held weapon
        ThingArt* art = nullptr;
        const nox::things::ModifierDb* modifiers = nullptr;
        double time = 0;        ///< game frames
        bool attacking = false; ///< Action held: the held weapon swings
        const Effects* effects = nullptr; ///< the particles, beams and glows, drawn in 3D (depth pass only)
        /// In a game: the 2D view's line of sight. Given, what the server sends (its drawables and
        /// the effects) is drawn only where the 2D view would show it: in the 2D view's rectangle
        /// (camera) and seen by its sight test. Release builds always give it, so the 3D view never
        /// shows what the 2D view hides (behind sight blockers, past the screen's edge).
        const SightMap* sight = nullptr;
    };
    /// The world, in the frame's target (camera.viewWidth x viewHeight).
    void draw(const Frame& f);

private:
    /// A point in camera space: across (right +), depth (forward), up (from the eye).
    struct Cam {
        float x = 0;
        float depth = 0;
        float up = 0;
    };
    /// A vertex being clipped: camera space, texture coordinates, colour.
    struct ClipVertex {
        Cam p;
        float u = 0;
        float v = 0;
        SDL_FColor c{1, 1, 1, 1};
    };
    /// Triangles with one texture (`count` vertices of tris_ from `first`), at one depth in the
    /// drawing order.
    struct Batch {
        SDL_Texture* texture = nullptr;
        const Sprite* sprite = nullptr; ///< made ready() before it's drawn; its tint, if any
        int first = 0;
        int count = 0;
        std::uint8_t alpha = 255; ///< under 255: translucent (drawn after the opaque, blended)
        bool linear = false;      ///< filtered (HD art, the floor)
        DepthPass::Kind kind = DepthPass::Kind::Scene; ///< particles: no texture (view3d_particle)
    };
    /// A particle's quad (addParticle, addRibbon): its corners and how far it is.
    struct ParticleQuad {
        ClipVertex v[4];
        float depth = 0;
    };
    /// A billboard: a sprite in a screen rectangle.
    struct Board {
        const Sprite* sprite = nullptr;
        SDL_FRect dst{};
        Rgb light{};
        std::uint8_t alpha = 255;
        float scale = 1; ///< screen pixels per sprite pixel
    };
    struct Item {
        float depth = 0;
        bool board = false;
        int index = 0;
    };

    /// With Frame::sight: a point outside the 2D view's rectangle (a drawable's foot: some room
    /// for its picture past the sides and the bottom, which rises into the view).
    bool offView(float x, float y) const;
    /// Frame::sight's test for this frame (false without one): a point the 2D view doesn't show.
    bool unseen(float x, float y) const;
    /// And for a live drawable's sprite, as the 2D scene tests it (SightMap::drawableSeen).
    bool unseen(const MapView::SceneSprite& e) const;

    Cam toCam(float x, float y, float z) const;
    SDL_FPoint project(const Cam& p) const;
    float fog(float distance) const;
    SDL_FColor shade(Rgb light, float distance) const;
    /// A convex polygon added to `batch` as triangles (tris_).
    void emit(Batch& batch, const ClipVertex* poly, int n);
    /// A triangle clipped to the near plane: its corners in `out` (up to 4), how many.
    int clipTriangle(const ClipVertex* tri, ClipVertex* out) const;
    /// This frame's batches and billboards, back to front through SDL_Renderer (no depth pass).
    void drawSorted(const Frame& f);
    /// This frame's batches through the depth pass; false if it couldn't.
    bool drawDepth(const Frame& f);
    /// A vertical face from (ax, ay) to (bx, by), z0 to z1, split into a grid; the texture
    /// coordinates from the sprite at the line (mx0, my0) - (mx1, my1) (the face's own line, or
    /// the wall's centre line), by the oblique projection; `zScale` sprite rows per unit of height
    /// (below 1: the picture stretched taller), the rows taken from `zShift` lower down (a wall's
    /// repeated band).
    void addFace(const Sprite& s, float sx0, float sy0, float ax, float ay, float bx, float by, float mx0, float my0,
                 float mx1, float my1, float z0, float z1, const LightMap* light, bool twoSided, float zScale = 1,
                 float zShift = 0);
    /// A wall's face (addFace) as tall as kWallRaise makes it: the band in the middle repeated.
    void addWallFace(const Sprite& s, float sx0, float sy0, float ax, float ay, float bx, float by, float mx0, float my0,
                     float mx1, float my1, float height, const LightMap* light);
    /// A thing on a wall (a switch, a banner, a torch): its picture on the face of the wall it's
    /// on, as the 2D view places it there. False if there's no wall by it.
    bool addOnWall(const Frame& f, const Sprite& s, float sx, float sy, const MapView::Object& o, const LightMap* light);
    /// Where the crosshair's ray (flat forward from the eye) crosses the line through a and b:
    /// {t, u}, t the distance along the flat forward, u the point along a -> b (0 to 1 on it);
    /// nullopt when they're parallel. (The callers test the height it's at, and what it hits.)
    std::optional<std::pair<float, float>> crosshairCross(float ax, float ay, float bx, float by) const;
    /// A horizontal polygon at height z (a solid's top), textured from the sprite as addFace.
    void addCap(const Sprite& s, float sx0, float sy0, const SDL_FPoint* corners, int n, float z, const LightMap* light,
                float zScale = 1);
    /// A solid thing (IMMOBILE with a BOX extent: obelisks, fireplaces, furniture): its extent's
    /// diamond (rotated 45 degrees, as the game's boxes are) stood up to the sprite's height.
    void addSolid(const Frame& f, const Sprite& s, std::int32_t image, float sx, float sy, const TypeArt& art, float ox,
                  float oy, const LightMap* light);
    /// Whether a type is drawn as a solid (IMMOBILE with a box extent, not hung on a wall, not
    /// animated, not a floor thing).
    static bool solidType(const TypeArt& art);
    /// Whether a type is drawn into the floor (BELOW, shadows, a bridge's raised pieces).
    static bool floorType(const TypeArt& art);
    /// A roof drawn `raised` up in 2D (a tent's top), as a level at canopy height.
    void addCanopy(const Sprite& s, float sx, float sy, float raised, const LightMap* light);
    /// Whether a type is an item lying on the ground (PICKUP, not a creature or fixture, low).
    static bool itemType(const TypeArt& art);
    /// The world y an item's billboard stands on so all of it is upright (its lowest solid row).
    float standingBase(const Frame& f, std::int32_t image, float sy, float height);
    /// An image's opaque pixels' bounds (its own pixels), measured once.
    SDL_Rect opaqueBounds(const Frame& f, std::int32_t image);
    void addFloor(const Frame& f);
    /// Facades painted on the floor (floor types named *Facade*: Dun Mir's castle front), stood
    /// up: in each column of the ground they cover, their southernmost edge is where the building
    /// meets the ground, and a point a distance north of it on the floor is that far up it (the
    /// oblique projection). A standing surface there, from the floor texture (lit as drawn).
    void addFacades(const Frame& f);
    void addWalls(const Frame& f);
    void addThings(const Frame& f);
    /// The effects (Effects' particles, sparks, puffs, orbs, beams and glows) as particles: at
    /// their real heights, facing the eye, lit and fogged, through the depth pass.
    void addEffects(const Frame& f);
    /// A particle at world (x, y, z) of `radius`: mode 0 glow (added), 1 core, 2 puff (`seed`).
    void addParticle(float x, float y, float z, float radius, SDL_Color colour, float alpha, int mode, int seed = 0);
    /// A glowing line from (a) to (b), `width` each side, facing the eye (added; else blended).
    void addRibbon(float ax, float ay, float az, float bx, float by, float bz, float width, SDL_Color colour, float alpha,
                   bool added = true);
    /// setRing's ring lying on the floor (a ribbon bent round its circle).
    void addRing();
    /// A billboard for a sprite drawn in 2D at world top-left (sx, sy), standing at (bx, by).
    void addBoard(const Sprite& s, float sx, float sy, float bx, float by, Rgb light, std::uint8_t alpha, float bias);
    void drawFloorTexture(const Frame& f);
    void drawHeld(const Frame& f);
    float wallHeight(const Frame& f, std::uint8_t material);
    /// A wall's image as drawScene picks it, or nullptr.
    static const nox::things::WallImage* wallImage(const Frame& f, const nox::map::Wall& w);
    /// A wall's picture with the pictures of the neighbours it carries straight on into under
    /// its clear pixels (cells: the walls by wallKey), and where its top-left is in the world
    /// (sx0, sy0); nullptr without one.
    const Sprite* wallSprite(const Frame& f, const nox::map::Wall& w,
                             const std::unordered_map<std::uint16_t, const nox::map::Wall*>& cells, float& sx0,
                             float& sy0);

    bool enabled_ = false;
    bool inWorld_ = false;
    bool windowWantsMouse_ = false;
    bool altHeld_ = false;
    bool keyUp_ = false, keyDown_ = false, keyLeft_ = false, keyRight_ = false;
    std::uint64_t castMs_ = 0; ///< SDL ticks of the last cast from 1-5 (aiming for a moment after)
    float yaw_ = -1.5707964f; ///< radians, Nox's screen angles (0 east, +pi/2 south)
    /// Where the server has us (the eye trails it a little, smoothed): the crosshair's ground
    /// point is laid along the view's facing from here, so the cursor's way is exactly ours.
    float playerX_ = 0, playerY_ = 0;
    bool aiming() const;
    float pitch_ = 0;         ///< radians, up positive
    float tiltTan_ = 0;       ///< setTilt's (off)
    float wallRaise_ = 0.25f;     ///< setWallRaise's

    // This frame's view.
    const SightMap* sight_ = nullptr; ///< Frame::sight in a game, else null
    float seenLeft_ = 0, seenTop_ = 0, seenRight_ = 0, seenBottom_ = 0; ///< the 2D view's rectangle, with sight_
    float eyeX_ = 0, eyeY_ = 0, eyeZ_ = 44;
    float cos_ = 1, sin_ = 0;
    float focal_ = 1;
    float centreX_ = 0;
    float horizon_ = 0;
    int viewW_ = 1024;
    int viewH_ = 768;

    std::vector<ClipVertex> tris_; ///< this frame's triangles, camera space
    std::vector<Batch> batches_;
    std::vector<Board> boards_;
    std::vector<Item> items_;
    std::vector<SDL_FPoint> solidSpots_; ///< this frame's solids, whose live copies aren't drawn too
    Sprite floorSprite_;                 ///< the floor texture as a sprite (addFacades' addFace)
    std::vector<std::uint8_t> facadeCover_; ///< addFacades' grid: the ground facade tiles cover
    /// addFacades: by floor type, whether its name has "Facade" in it (worked out once a thing.bin).
    std::vector<std::uint8_t> facadeFloors_;
    const void* facadeFloorsOf_ = nullptr;
    std::vector<ParticleQuad> addedQuads_;   ///< this frame's added particles (glows, beams)
    std::vector<ParticleQuad> blendedQuads_; ///< and blended ones (cores, puffs), sorted far to near
    /// This frame's wall arms as built (their centre lines), for the things hung on walls.
    struct WallLine {
        float ax, ay, bx, by;
        float height;
    };
    std::vector<WallLine> wallLines_;
    std::vector<SDL_Vertex> runVerts_; ///< drawSorted's merged batches

    std::unique_ptr<DepthPass> depth_; ///< the depth-buffered drawing, when the renderer's GPU allows it
    bool depthTried_ = false;
    std::vector<DepthPass::Vertex> gpuVerts_;
    std::vector<DepthPass::Draw> gpuDraws_;

    /// The crosshair's pick: the nearest hit along the ray, and its point on the ground.
    float pickDepth_ = 0;
    std::optional<SDL_FPoint> pick_;
    bool pickThing_ = false;           ///< the pick is a thing's foot
    std::optional<SDL_FPoint> target_; ///< last frame's pick, for the mouse
    bool targetThing_ = false;
    std::optional<Ring> ring_; ///< setRing's

    SDL_Texture* floorTexture_ = nullptr;
    int floorTextureSize_ = 0;
    float floorLeft_ = 0;
    float floorTop_ = 0;
    float floorTexelsPerUnit_ = 1;

    std::map<std::uint8_t, float> wallHeights_; ///< by WALL material, measured from its sprite
    std::map<std::int32_t, SDL_Rect> opaque_;   ///< opaqueBounds' by image

    SDL_Texture* heldTarget_ = nullptr; ///< a held item drawn before it's turned onto the screen
    std::uint64_t lastNs_ = 0;
    float walkPhase_ = 0;   ///< the held weapon's bob
    double swingStart_ = -1; ///< SDL ticks (s) the current swing began
};

} // namespace noxsdl
