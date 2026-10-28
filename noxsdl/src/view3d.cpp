// The first-person 3D view (see view3d.hpp for the design): building each frame's floor, walls,
// things and effects as camera-space triangles, the crosshair's pick, and the held weapon. The
// triangles go to the depth pass (view3d_gpu.cpp) when the GPU allows, else drawSorted.
#include "view3d.hpp"

#include "effects.hpp"
#include "effects_draw.hpp"
#include "player_art.hpp"

#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <string>

namespace noxsdl {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kFloorRadius = 800;   // the floor texture reaches this far each way from the eye
constexpr float kDrawDistance = 760;  // nothing further is drawn (the server sends about a screen's worth)
constexpr float kFogStart = 340;      // from here to kDrawDistance the world fades to black
constexpr float kNear = 2;            // the near plane, world units in front of the eye
constexpr float kEyeHeight = 44;      // a Nox character's eyes (ZSIZE ~ 50-60)
constexpr float kWallThickness = 6;
constexpr float kHalfFov = 0.7853982f; // 90 degrees across
constexpr float kMouseTurn = 0.0035f;  // radians per mouse unit (noxsdl scales the raw counts to game pixels)
constexpr float kMaxPitch = 1.1f;
constexpr float kWalkSpeed = 320;      // the viewer's free flight, units a second
constexpr float kCell = 23;            // the wall grid
constexpr double kHeldAngle = 270;     // the held weapon's turn (clockwise degrees) from its ground picture

/// NOXSDL_VIEW3D_DEBUG set: the debug log's lines (read once).
bool debugLog()
{
    static const bool on = SDL_getenv("NOXSDL_VIEW3D_DEBUG") != nullptr;
    return on;
}

// A wall cell's arms from its centre to its corners (sight.cpp's table): 1 NE, 2 NW,
// 4 SE, 8 SW. Directions 0 and 1 are the full diagonals.
constexpr std::uint8_t kEdgeFlags[] = {0x09, 0x06, 0x0F, 0x0B, 0x07, 0x0D, 0x0E, 0x03, 0x05, 0x0C, 0x0A};
// The base line of directions with no arms (map_view.cpp's table).
constexpr std::array<std::array<float, 4>, 15> kWallBase = {{
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 11, 23, 11}, {0, 23, 23, 0}, {0, 0, 23, 23},
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 0, 10, 10}, {13, 10, 23, 0}, {0, 23, 10, 13},
    {0, 0, 10, 10}, {13, 10, 23, 0}, {13, 13, 23, 23}, {13, 13, 23, 23}, {0, 23, 10, 13},
}};

/// A wall cell's arm: from its centre (or a base line's start) to a corner.
struct WallArm {
    float ax, ay, bx, by;
    bool capA, capB; ///< ends that may be free (closed when no other wall meets them)
};

// Appends a wall cell's arms (world units) to `out`: for the diagonal directions, an arm from the
// cell's centre to each corner its kEdgeFlags name; else the one base line from kWallBase.
void wallArms(const nox::map::Wall& w, std::vector<WallArm>& out)
{
    const float x0 = static_cast<float>(w.x) * kCell;
    const float y0 = static_cast<float>(w.y) * kCell;
    const std::size_t dir = w.direction & 0x7F;
    if (dir < std::size(kEdgeFlags)) {
        const float cx = x0 + 11.5f;
        const float cy = y0 + 11.5f;
        const std::uint8_t flags = kEdgeFlags[dir];
        struct Corner {
            std::uint8_t flag;
            float x, y;
        };
        const Corner corners[4] = {{1, x0 + kCell, y0}, {2, x0, y0}, {4, x0 + kCell, y0 + kCell}, {8, x0, y0 + kCell}};
        for (const auto& c : corners) {
            if (flags & c.flag) {
                out.push_back({cx, cy, c.x, c.y, false, true});
            }
        }
    } else if (dir < kWallBase.size()) {
        const auto& b = kWallBase[dir];
        out.push_back({x0 + b[0], y0 + b[1], x0 + b[2], y0 + b[3], true, true});
    }
}

// Radians (0 east, growing toward south as screen y does) to a Nox facing: 256 steps a turn.
std::uint8_t toFacing(float radians)
{
    const float turns = radians / (2 * kPi);
    return static_cast<std::uint8_t>(static_cast<int>(std::lround(turns * 256)) & 0xFF);
}

// A light colour times `f` (0-1).
Rgb scaled(Rgb c, float f)
{
    return {static_cast<std::uint8_t>(static_cast<float>(c[0]) * f), static_cast<std::uint8_t>(static_cast<float>(c[1]) * f),
            static_cast<std::uint8_t>(static_cast<float>(c[2]) * f)};
}

} // namespace

View3D::~View3D()
{
    releaseGpu();
}

void View3D::releaseGpu()
{
    depth_.reset(); // (its pipelines, shaders, buffers and textures, on the renderer's GPU device)
    if (floorTexture_) {
        SDL_DestroyTexture(floorTexture_);
        floorTexture_ = nullptr;
    }
    if (heldTarget_) {
        SDL_DestroyTexture(heldTarget_);
        heldTarget_ = nullptr;
    }
    floorTextureSize_ = 0;
    floorSprite_.texture = nullptr;
}

void View3D::setLook(float yawDegrees, float pitchDegrees)
{
    yaw_ = yawDegrees * kPi / 180;
    pitch_ = std::clamp(pitchDegrees * kPi / 180, -kMaxPitch, kMaxPitch);
}

bool View3D::handleEvent(SDL_Event& event, std::string& control)
{
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const SDL_Scancode code = event.key.scancode;
        if (code == SDL_SCANCODE_LALT) {
            altHeld_ = down;
            return false;
        }
        // FPS keys: 1-5 the spell bar's slots, E / R the previous / next spell set (the game's
        // controls, run as its own bindings would run them).
        const char* fpsControl = nullptr;
        switch (code) {
        case SDL_SCANCODE_1: fpsControl = "InvokeSlot1"; break;
        case SDL_SCANCODE_2: fpsControl = "InvokeSlot2"; break;
        case SDL_SCANCODE_3: fpsControl = "InvokeSlot3"; break;
        case SDL_SCANCODE_4: fpsControl = "InvokeSlot4"; break;
        case SDL_SCANCODE_5: fpsControl = "InvokeSlot5"; break;
        case SDL_SCANCODE_E: fpsControl = "PreviousSpellSet"; break;
        case SDL_SCANCODE_R: fpsControl = "NextSpellSet"; break;
        default: break;
        }
        if (fpsControl) {
            if (!active() || windowWantsMouse_) {
                return false; // (typing, or the 2D view's own keys)
            }
            if (down && !event.key.repeat) {
                control = fpsControl;
                if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_5) {
                    castMs_ = SDL_GetTicks(); // (facing the crosshair while it's cast)
                }
            }
            return true;
        }
        // W A S D (and the arrows) walk.
        bool* key = code == SDL_SCANCODE_UP || code == SDL_SCANCODE_W       ? &keyUp_
                    : code == SDL_SCANCODE_DOWN || code == SDL_SCANCODE_S   ? &keyDown_
                    : code == SDL_SCANCODE_LEFT || code == SDL_SCANCODE_A   ? &keyLeft_
                    : code == SDL_SCANCODE_RIGHT || code == SDL_SCANCODE_D  ? &keyRight_
                                                                            : nullptr;
        if (!key) {
            return false;
        }
        if (!down) {
            *key = false;
            return active();
        }
        if (!active() || windowWantsMouse_) {
            return false; // (typing, or the 2D view's own arrows)
        }
        *key = true;
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        keyUp_ = keyDown_ = keyLeft_ = keyRight_ = altHeld_ = false;
        return false;
    case SDL_EVENT_MOUSE_MOTION:
        if (!looking()) {
            return false;
        }
        yaw_ = std::remainder(yaw_ + event.motion.xrel * kMouseTurn, 2 * kPi);
        pitch_ = std::clamp(pitch_ - event.motion.yrel * kMouseTurn, -kMaxPitch, kMaxPitch);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (looking()) {
            event.button.x = -10000; // on no window: the crosshair is on the world
            event.button.y = -10000;
        }
        return false;
    case SDL_EVENT_MOUSE_WHEEL:
        if (looking()) {
            event.wheel.mouse_x = -10000;
            event.wheel.mouse_y = -10000;
        }
        return false;
    default:
        return false;
    }
}

std::optional<SDL_FPoint> View3D::cursorMouse(const Camera& camera, bool running) const
{
    if (!looking() || !target_) {
        return std::nullopt;
    }
    SDL_FPoint p = *target_;
    if (running && !targetThing_) {
        constexpr float kRunReach = 160;
        if (std::hypot(p.x - playerX_, p.y - playerY_) < kRunReach) {
            p = SDL_FPoint{playerX_ + cos_ * kRunReach, playerY_ + sin_ * kRunReach};
        }
    }
    return SDL_FPoint{(p.x - camera.left()) * camera.zoom, (p.y - camera.top()) * camera.zoom};
}

std::optional<std::uint8_t> View3D::facing() const
{
    if (!active()) {
        return std::nullopt;
    }
    return toFacing(yaw_);
}

bool View3D::aiming() const
{
    return castMs_ != 0 && SDL_GetTicks() - castMs_ < 500;
}

bool View3D::walking(bool attacking) const
{
    const int forward = (keyUp_ ? 1 : 0) - (keyDown_ ? 1 : 0);
    const int side = (keyRight_ ? 1 : 0) - (keyLeft_ ? 1 : 0);
    if (!active() || windowWantsMouse_) {
        return false;
    }
    if (attacking || aiming()) {
        return forward > 0; // (facing the crosshair: forward only)
    }
    return forward != 0 || side != 0;
}

std::uint8_t View3D::walkFacing(std::uint8_t fallback, bool attacking) const
{
    if (!walking(attacking)) {
        return fallback;
    }
    if (attacking || aiming()) {
        return toFacing(yaw_);
    }
    const float forward = static_cast<float>((keyUp_ ? 1 : 0) - (keyDown_ ? 1 : 0));
    const float side = static_cast<float>((keyRight_ ? 1 : 0) - (keyLeft_ ? 1 : 0));
    return toFacing(yaw_ + std::atan2(side, forward));
}

std::uint8_t View3D::seenDirection(std::uint8_t direction, float x, float y) const
{
    if (!active() || direction == 4 || direction > 8) {
        return direction;
    }
    // The AnimDir grid's column and row, each -1 to 1.
    const int dx = direction % 3 - 1;
    const int dy = direction / 3 - 1;
    // Turned so our line of sight to it is the 2D view's (north): its facing less that turn.
    const float facingAngle = std::atan2(static_cast<float>(dy), static_cast<float>(dx));
    const float sight = std::atan2(y - eyeY_, x - eyeX_);
    const float seen = facingAngle - (sight + kPi / 2);
    // Snapped to the nearest of the 8 directions.
    const float step = std::round(seen / (kPi / 4)) * (kPi / 4);
    const int ndx = static_cast<int>(std::lround(std::cos(step)));
    const int ndy = static_cast<int>(std::lround(std::sin(step)));
    return static_cast<std::uint8_t>((ndy + 1) * 3 + (ndx + 1));
}

Camera View3D::worldCamera(const Camera& camera) const
{
    if (!active()) {
        return camera;
    }
    Camera c = camera;
    c.zoom = 1;
    c.viewWidth = static_cast<int>(2 * kFloorRadius);
    c.viewHeight = static_cast<int>(2 * kFloorRadius);
    return c;
}

void View3D::drawCrosshair(SDL_Renderer* renderer, Reticle reticle) const
{
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    SDL_Color c{235, 235, 235, 230};
    switch (reticle) {
    case Reticle::Plain:
        break;
    case Reticle::Enemy:
        c = {255, 60, 40, 255};
        break;
    case Reticle::Friend:
        c = {60, 170, 255, 255};
        break;
    case Reticle::Pickup:
        c = {255, 210, 60, 255};
        break;
    case Reticle::Use:
        c = {80, 255, 110, 255};
        break;
    case Reticle::Talk:
        c = {90, 240, 240, 255};
        break;
    case Reticle::Caution:
        c = {255, 140, 30, 255};
        break;
    case Reticle::Target:
        c = {230, 90, 255, 255};
        break;
    }
    if (debugLog()) {
        static std::uint64_t lastLog = 0;
        if (SDL_GetTicks() - lastLog > 1000) {
            lastLog = SDL_GetTicks();
            SDL_Log("view3d: crosshair %d at %dx%d, target %s", static_cast<int>(reticle), w, h, targetThing_ ? "thing" : "ground");
        }
    }
    // Sized to the output (1 at 600 high): four arms `len` long, `gap` from the centre, `t` thick,
    // each over a dark outline.
    const float s = std::max(1.0f, static_cast<float>(h) / 600.0f);
    const float cx = std::floor(static_cast<float>(w) / 2);
    const float cy = std::floor(static_cast<float>(h) / 2);
    const float gap = std::round(4 * s);
    const float len = std::round(7 * s);
    const float t = std::max(2.0f, std::round(2 * s));
    const SDL_FRect arms[4] = {{cx - gap - len, cy - t / 2, len, t},
                               {cx + gap, cy - t / 2, len, t},
                               {cx - t / 2, cy - gap - len, t, len},
                               {cx - t / 2, cy + gap, t, len}};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    for (const auto& a : arms) {
        const SDL_FRect o{a.x - 1, a.y - 1, a.w + 2, a.h + 2};
        SDL_RenderFillRect(renderer, &o);
    }
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRects(renderer, arms, 4);
    if (reticle != Reticle::Plain) {
        // Something to do there: a dot in the middle too.
        const SDL_FRect dot{cx - t / 2, cy - t / 2, t, t};
        SDL_RenderFillRect(renderer, &dot);
    }
}

// World to camera space: depth along the view's flat forward (cos_, sin_), x along its right
// (-sin_, cos_), up from the eye's height.
View3D::Cam View3D::toCam(float x, float y, float z) const
{
    const float dx = x - eyeX_;
    const float dy = y - eyeY_;
    return {-dx * sin_ + dy * cos_, dx * cos_ + dy * sin_, z - eyeZ_};
}

// Perspective: focal_ pixels per unit at depth 1; up counts from the horizon line, which the
// pitch moves (shear look), so verticals stay vertical.
SDL_FPoint View3D::project(const Cam& p) const
{
    const float k = focal_ / p.depth;
    return {centreX_ + p.x * k, horizon_ - p.up * k};
}

// 1 out to kFogStart, falling to 0 at kDrawDistance.
float View3D::fog(float distance) const
{
    return std::clamp((kDrawDistance - distance) / (kDrawDistance - kFogStart), 0.0f, 1.0f);
}

// A vertex colour (0-1): the light (0-255) fogged by distance.
SDL_FColor View3D::shade(Rgb light, float distance) const
{
    const float f = fog(distance) / 255.0f;
    return {static_cast<float>(light[0]) * f, static_cast<float>(light[1]) * f, static_cast<float>(light[2]) * f, 1};
}

void View3D::emit(Batch& batch, const ClipVertex* poly, int n)
{
    // As triangles (a fan) in camera space: the depth pass clips them itself, the fallback
    // (drawSorted) to the near plane before projecting them.
    for (int i = 1; i + 1 < n; ++i) {
        tris_.push_back(poly[0]);
        tris_.push_back(poly[i]);
        tris_.push_back(poly[i + 1]);
        batch.count += 3;
    }
}

int View3D::clipTriangle(const ClipVertex* tri, ClipVertex* out) const
{
    // Sutherland-Hodgman against the near plane: up to 4 corners.
    int m = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVertex& a = tri[i];
        const ClipVertex& b = tri[(i + 1) % 3];
        const bool inA = a.p.depth >= kNear;
        const bool inB = b.p.depth >= kNear;
        if (inA) {
            out[m++] = a;
        }
        if (inA != inB) {
            const float t = (kNear - a.p.depth) / (b.p.depth - a.p.depth);
            ClipVertex v;
            v.p = {a.p.x + (b.p.x - a.p.x) * t, kNear, a.p.up + (b.p.up - a.p.up) * t};
            v.u = a.u + (b.u - a.u) * t;
            v.v = a.v + (b.v - a.v) * t;
            v.c = {a.c.r + (b.c.r - a.c.r) * t, a.c.g + (b.c.g - a.c.g) * t, a.c.b + (b.c.b - a.c.b) * t,
                   a.c.a + (b.c.a - a.c.a) * t};
            out[m++] = v;
        }
    }
    return m;
}

void View3D::addFace(const Sprite& s, float sx0, float sy0, float ax, float ay, float bx, float by, float mx0, float my0,
                     float mx1, float my1, float z0, float z1, const LightMap* light, bool twoSided, float zScale,
                     float zShift)
{
    const float midX = (ax + bx) / 2;
    const float midY = (ay + by) / 2;
    if (!twoSided) {
        // Only the side facing the eye: the face runs a -> b with its outside on the left
        // (Nox's y is down, so (dy, -dx) of the edge points out).
        const float nx = by - ay;
        const float ny = -(bx - ax);
        if (nx * (eyeX_ - midX) + ny * (eyeY_ - midY) <= 0) {
            return;
        }
    }
    const Cam ca = toCam(ax, ay, 0);
    const Cam cb = toCam(bx, by, 0);
    // Wholly behind the eye, or too far off: nothing.
    if (ca.depth < kNear && cb.depth < kNear) {
        return;
    }
    const float distance = std::hypot(midX - eyeX_, midY - eyeY_);
    if (distance > kDrawDistance + 40) {
        return;
    }
    if (ca.depth > kNear && cb.depth > kNear) {
        // Off the sides of the view.
        const float pa = project(ca).x;
        const float pb = project(cb).x;
        if ((pa < 0 && pb < 0) || (pa > static_cast<float>(viewW_) && pb > static_cast<float>(viewW_))) {
            return;
        }
    }
    // The texture's size, for 0-1 coordinates; uvOf maps the sprite's logical pixels onto its texels
    // (HD art has more of them).
    float tw = 0, th = 0;
    SDL_GetTextureSize(s.texture, &tw, &th);
    if (tw <= 0 || th <= 0 || s.width <= 0 || s.height <= 0) {
        return;
    }
    // The sprite's own pixels in the texture (a material sprite's image is the left half).
    const float rw = s.imageWidth();
    const float rh = s.rect.h;
    const auto uvOf = [&](float t, float z) {
        const float px = std::clamp(mx0 + (mx1 - mx0) * t - sx0, 0.5f, s.width - 0.5f);
        const float py = std::clamp(my0 + (my1 - my0) * t - (z - zShift) * zScale - sy0, 0.5f, s.height - 0.5f);
        return SDL_FPoint{(s.rect.x + px * rw / s.width) / tw, (s.rect.y + py * rh / s.height) / th};
    };
    // Only the part of the face that's inside its sprite: across, the columns whose pixels are
    // in it; up, at each column, the heights between its bottom and top rows there (the sprite
    // pixel of (x, y, z) is (x, y - z), so the bounds slope along the face). Outside it nothing
    // would show but the edge pixels smeared.
    float tA = 0;
    float tB = 1;
    {
        const float p0 = mx0 - sx0;
        const float dp = mx1 - mx0;
        if (std::abs(dp) < 1e-4f) {
            if (p0 < 0 || p0 > s.width) {
                return;
            }
        } else {
            const float ta = (0 - p0) / dp;
            const float tb = (s.width - p0) / dp;
            tA = std::max(tA, std::min(ta, tb));
            tB = std::min(tB, std::max(ta, tb));
        }
        if (tB <= tA) {
            return;
        }
    }
    // The face's top and bottom at t: the heights where the sprite's top and bottom rows land there
    // (sprite row = y - z, times zScale), kept within z0-z1.
    const auto zTop = [&](float t) { return std::min(z1, (my0 + (my1 - my0) * t - sy0) / zScale + zShift); };
    const auto zBottom = [&](float t) { return std::max(z0, (my0 + (my1 - my0) * t - sy0 - s.height) / zScale + zShift); };

    // A grid fine enough that the affine mapping of each piece doesn't show.
    const float near = std::max(std::min(ca.depth, cb.depth), kNear);
    const float length = std::hypot(bx - ax, by - ay) * (tB - tA);
    const int columns = std::clamp(static_cast<int>(length * focal_ / near / 40) + 1, 1, 10);
    const int rows = std::clamp(static_cast<int>((z1 - z0) * focal_ / near / 60) + 1, 1, 8);

    Batch batch{s.texture, &s, static_cast<int>(tris_.size()), 0, 255, s.hd};  // opaque; filtered for HD art
    std::array<SDL_FColor, 11> colours{};
    // Each column edge's light and fog, shared by the quads either side of it.
    for (int i = 0; i <= columns; ++i) {
        const float t = tA + (tB - tA) * static_cast<float>(i) / static_cast<float>(columns);
        const float x = ax + (bx - ax) * t;
        const float y = ay + (by - ay) * t;
        colours[static_cast<std::size_t>(i)] =
            shade(light ? light->sample(x, y) : Rgb{255, 255, 255}, std::hypot(x - eyeX_, y - eyeY_));
    }
    // Each column a stack of `rows` quads between its bottom and top heights.
    for (int i = 0; i < columns; ++i) {
        const float t0 = tA + (tB - tA) * static_cast<float>(i) / static_cast<float>(columns);
        const float t1 = tA + (tB - tA) * static_cast<float>(i + 1) / static_cast<float>(columns);
        const float bottom0 = zBottom(t0);
        const float bottom1 = zBottom(t1);
        const float top0 = std::max(zTop(t0), bottom0);
        const float top1 = std::max(zTop(t1), bottom1);
        if (top0 - bottom0 < 0.01f && top1 - bottom1 < 0.01f) {
            continue;
        }
        for (int j = 0; j < rows; ++j) {
            const float fa = static_cast<float>(j) / static_cast<float>(rows);
            const float fb = static_cast<float>(j + 1) / static_cast<float>(rows);
            const auto corner = [&](float t, float z, const SDL_FColor& c) {
                ClipVertex v;
                v.p = toCam(ax + (bx - ax) * t, ay + (by - ay) * t, z);
                const SDL_FPoint uv = uvOf(t, z);
                v.u = uv.x;
                v.v = uv.y;
                v.c = c;
                return v;
            };
            const SDL_FColor& c0 = colours[static_cast<std::size_t>(i)];
            const SDL_FColor& c1 = colours[static_cast<std::size_t>(i + 1)];
            const ClipVertex quad[4] = {corner(t0, bottom0 + (top0 - bottom0) * fa, c0),
                                        corner(t1, bottom1 + (top1 - bottom1) * fa, c1),
                                        corner(t1, bottom1 + (top1 - bottom1) * fb, c1),
                                        corner(t0, bottom0 + (top0 - bottom0) * fb, c0)};
            emit(batch, quad, 4);
        }
    }
    if (batch.count > 0) {
        // Sorted with everything else by the distance to the face's middle.
        items_.push_back({distance, false, static_cast<int>(batches_.size())});
        batches_.push_back(batch);
    }
}

// The height (world units) a wall material stands, measured once from its sprite and kept in
// wallHeights_; 60 when it can't be measured.
float View3D::wallHeight(const Frame& f, std::uint8_t material)
{
    if (const auto it = wallHeights_.find(material); it != wallHeights_.end()) {
        return it->second;
    }
    // Measured from the material's "\" piece: in the middle column of its cell the wall's base
    // is at y 11.5, and its top edge is the column's first opaque pixel (y - z there).
    float height = 60;
    const auto& walls = f.view.things_.walls;
    if (material < walls.size() && !walls[material].directions[1].empty()) {
        const auto& image = walls[material].directions[1][0][0];
        if (const auto* d = f.sprites.decoded(f.sprites.indexOf(image.image)); d && d->width > 0) {
            const float sx0 = -50.0f + static_cast<float>(image.x) + static_cast<float>(d->offsetX);
            const float sy0 = -72.0f - static_cast<float>(image.y) + static_cast<float>(d->offsetY);
            const int column = static_cast<int>(11.5f - sx0);
            if (column >= 0 && column < d->width) {
                for (int row = 0; row < d->height; ++row) {
                    if (d->rgba[(static_cast<std::size_t>(row) * d->width + column) * 4 + 3] > 0) {
                        // (A few units under it: the sprite's top edge is a staircase of pixels
                        // whose transparent steps would show as a saw along the box's top.)
                        height = std::clamp(11.5f - (sy0 + static_cast<float>(row)) - 4, 24.0f, 160.0f);
                        break;
                    }
                }
            }
        }
        SDL_Log("view3d: wall %s stands %.0f high", walls[material].name.c_str(), height);
    }
    wallHeights_[material] = height;
    return height;
}

// Redraws floorTexture_ (made or resized as needed): the floor within kFloorRadius of the eye, from
// above, with the things that lie on it. Sets floorLeft_, floorTop_ and floorTexelsPerUnit_, which
// addFloor and addFacades map the texture by.
void View3D::drawFloorTexture(const Frame& f)
{
    // The floor round the eye as the 2D view draws it, seen from straight above.
    // Twice the texels per world unit with the HD pack.
    const float texels = f.sprites.usingPack() ? 2.0f : 1.0f;
    const int size = static_cast<int>(2 * kFloorRadius * texels);
    if (!floorTexture_ || floorTextureSize_ != size) {
        if (floorTexture_) {
            SDL_DestroyTexture(floorTexture_);
        }
        floorTexture_ = SDL_CreateTexture(f.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, size, size);
        floorTextureSize_ = size;
        if (!floorTexture_) {
            SDL_Log("view3d: floor texture %dx%d: %s", size, size, SDL_GetError());
            return;
        }
    }
    // A camera centred on the eye (whole units) whose view is exactly the texture.
    Camera above;
    above.x = std::round(eyeX_);
    above.y = std::round(eyeY_);
    above.zoom = texels;
    above.viewWidth = size;
    above.viewHeight = size;
    floorLeft_ = above.left();
    floorTop_ = above.top();
    floorTexelsPerUnit_ = texels;

    // Drawn into the texture, then the previous render target put back.
    SDL_Texture* const previous = SDL_GetRenderTarget(f.renderer);
    SDL_SetRenderTarget(f.renderer, floorTexture_);
    SDL_SetRenderDrawColor(f.renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(f.renderer);
    f.view.drawFloor(f.renderer, above, f.sprites, f.light, nullptr);

    // (Debug: lists the floor types in use, once.)
    if (debugLog()) {
        static bool listed = false;
        if (!listed) {
            listed = true;
            std::map<std::uint8_t, std::pair<int, SDL_FPoint>> used;
            for (const auto& t : f.view.tiles_) {
                auto& u = used[t.floor];
                if (u.first++ == 0) {
                    u.second = {t.x, t.y};
                }
            }
            for (const auto& [floor, u] : used) {
                if (floor < f.view.things_.floors.size()) {
                    const auto& def = f.view.things_.floors[floor];
                    SDL_Log("view3d: floor type %s: %dx%d cells, %d frames, %d tiles, e.g. at %.0f,%.0f", def.name.c_str(), def.cols,
                            def.rows, def.frames, u.first, u.second.x, u.second.y);
                }
            }
        }
    }
    // What lies on the floor goes into it, drawn as in 2D (BELOW things, pressure plates).
    const auto lightAt = [&](float x, float y) { return f.light ? f.light->sample(x, y) : Rgb{255, 255, 255}; };
    for (const auto& o : f.view.objects_) {
        // Skipped: no art, neither a floor type nor a plate, drawn from the server's copy instead (its
        // extent is in liveCodes), or past the texture's reach.
        if (!o.art || !(floorType(*o.art) || o.plate) || (f.liveCodes && f.liveCodes->contains(o.extent)) ||
            std::hypot(o.x - eyeX_, o.y - eyeY_) > kFloorRadius) {
            continue;
        }
        if (o.plate) {
            f.view.drawPlate(f.renderer, above, o);
            continue;
        }
        const auto image = mapObjectImage(*o.art, o.extent, o.doorAngle, o.frame, o.enabled, o.xStatus, o.xStatusSince, f.time);
        if (!image) {
            continue;
        }
        if (const Sprite* s = o.colours ? f.sprites.tinted(*image, *o.colours) : f.sprites.get(*image)) {
            if (debugLog() && (s->width > 100 || s->height > 100)) {
                static std::set<std::string> logged;
                if (logged.insert(o.art->name).second) {
                    SDL_Log("view3d: floor %s at %.0f,%.0f: %.0fx%.0f below %d box %.0fx%.0f r%.0f z %.0f-%.0f Z %.0f classes %08X",
                            o.art->name.c_str(), o.x, o.y, s->width, s->height, o.art->below ? 1 : 0, o.art->extentWidth,
                            o.art->extentHeight, o.art->extentRadius, o.art->zSizeMin, o.art->zSizeMax, o.art->z, o.art->classes);
                }
            }
            f.view.drawSprite(f.renderer, f.sprites, above, *s, o.x - o.art->halfWidth, o.y - o.art->halfHeight - o.art->z,
                              lightAt(o.x, o.y));
        }
    }
    // The live drawables MapView sorts as lying on the floor (their sort y pushed back by kBelow).
    for (const auto& e : f.extra) {
        if (e.sprite && e.sortY < e.lightY - MapView::kBelow / 2 && !unseen(e)) {
            f.view.drawSprite(f.renderer, f.sprites, above, *e.sprite, e.x, e.y, e.light ? *e.light : lightAt(e.lightX, e.lightY),
                              -1e9f, 1e9f, e.alpha);
        }
    }
    SDL_SetRenderTarget(f.renderer, previous);
}

// Lays floorTexture_ on the ground (z 0) as fogged, textured quads, queued as the farthest batch
// so it's under everything.
void View3D::addFloor(const Frame& f)
{
    (void)f;
    if (!floorTexture_) {
        return;
    }
    Batch batch{floorTexture_, nullptr, static_cast<int>(tris_.size()), 0, 255, true};
    // The ground as squares of kGrid units within the draw distance, each culled against the view.
    constexpr float kGrid = 32;
    const float size = static_cast<float>(floorTextureSize_);
    const int i0 = static_cast<int>(std::floor((eyeX_ - kDrawDistance) / kGrid));
    const int i1 = static_cast<int>(std::floor((eyeX_ + kDrawDistance) / kGrid));
    const int j0 = static_cast<int>(std::floor((eyeY_ - kDrawDistance) / kGrid));
    const int j1 = static_cast<int>(std::floor((eyeY_ + kDrawDistance) / kGrid));
    const SDL_FColor white{1, 1, 1, 1};
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const float x0 = static_cast<float>(i) * kGrid;
            const float y0 = static_cast<float>(j) * kGrid;
            const float d = std::hypot(x0 + kGrid / 2 - eyeX_, y0 + kGrid / 2 - eyeY_);
            if (d > kDrawDistance + kGrid) {
                continue;
            }
            const Cam corners[4] = {toCam(x0, y0, 0), toCam(x0 + kGrid, y0, 0), toCam(x0 + kGrid, y0 + kGrid, 0),
                                    toCam(x0, y0 + kGrid, 0)};
            // Culled: squares wholly behind the near plane, or (all in front) wholly off one side.
            bool anyFront = false;
            bool allFront = true;
            for (const auto& c : corners) {
                anyFront = anyFront || c.depth >= kNear;
                allFront = allFront && c.depth >= kNear;
            }
            if (!anyFront) {
                continue;
            }
            if (allFront) {
                bool left = true;
                bool right = true;
                for (const auto& c : corners) {
                    const float x = project(c).x;
                    left = left && x < 0;
                    right = right && x > static_cast<float>(viewW_);
                }
                if (left || right) {
                    continue;
                }
            }
            // Split finer near the eye, so the affine texture mapping of each piece doesn't show.
            const int split = d < 90 ? 4 : d < 260 ? 2 : 1;
            const float g = kGrid / static_cast<float>(split);
            for (int b = 0; b < split; ++b) {
                for (int a = 0; a < split; ++a) {
                    const auto corner = [&](float x, float y) {
                        ClipVertex v;
                        v.p = toCam(x, y, 0);
                        v.u = std::clamp((x - floorLeft_) * floorTexelsPerUnit_ / size, 0.0f, 1.0f);
                        v.v = std::clamp((y - floorTop_) * floorTexelsPerUnit_ / size, 0.0f, 1.0f);
                        const float f2 = fog(std::hypot(x - eyeX_, y - eyeY_));
                        v.c = {white.r * f2, white.g * f2, white.b * f2, 1};
                        return v;
                    };
                    const float xa = x0 + static_cast<float>(a) * g;
                    const float ya = y0 + static_cast<float>(b) * g;
                    const ClipVertex quad[4] = {corner(xa, ya), corner(xa + g, ya), corner(xa + g, ya + g), corner(xa, ya + g)};
                    emit(batch, quad, 4);
                }
            }
        }
    }
    items_.push_back({1e9f, false, static_cast<int>(batches_.size())}); // under everything
    batches_.push_back(batch);
}

void View3D::addFacades(const Frame& f)
{
    if (!floorTexture_) {
        return;
    }
    const auto& floors = f.view.things_.floors;
    // The ground facade tiles cover, on a grid of kRes units over the floor texture's area (a
    // tile is a 46-unit diamond from its top-left).
    constexpr float kRes = 4;
    constexpr float kTile = 46;
    const float span = 2 * kFloorRadius;
    const int n = static_cast<int>(span / kRes);
    facadeCover_.assign(static_cast<std::size_t>(n) * n, 0);
    if (debugLog()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::map<int, float> south; // by 100-unit x band: the southernmost facade tile bottom
            for (const auto& t : f.view.tiles_) {
                if (t.floor < floors.size() && floors[t.floor].name.find("Facade") != std::string::npos) {
                    auto& s = south[static_cast<int>(t.x) / 100];
                    s = std::max(s, t.y + kTile);
                }
            }
            for (const auto& [band, y] : south) {
                SDL_Log("view3d: facade x %d-%d: base y %.0f", band * 100, band * 100 + 99, y);
            }
        }
    }
    if (facadeFloorsOf_ != &floors || facadeFloors_.size() != floors.size()) {
        facadeFloorsOf_ = &floors;
        facadeFloors_.clear();
        for (const auto& floor : floors) {
            facadeFloors_.push_back(floor.name.find("Facade") != std::string::npos ? 1 : 0);
        }
    }
    // Mark the cells each facade tile's diamond covers.
    bool any = false;
    for (const auto& t : f.view.tiles_) {
        if (t.floor >= floors.size() || !facadeFloors_[t.floor]) {
            continue;
        }
        // The grid cells under the tile: a cell counts when its centre is inside the diamond
        // (|dx| + |dy| within half the tile from the tile's middle).
        const int c0 = static_cast<int>((t.x - floorLeft_) / kRes);
        const int r0 = static_cast<int>((t.y - floorTop_) / kRes);
        const int cells = static_cast<int>(kTile / kRes) + 1;
        for (int r = std::max(r0, 0); r < std::min(r0 + cells, n); ++r) {
            for (int c = std::max(c0, 0); c < std::min(c0 + cells, n); ++c) {
                const float px = floorLeft_ + (static_cast<float>(c) + 0.5f) * kRes - t.x;
                const float py = floorTop_ + (static_cast<float>(r) + 0.5f) * kRes - t.y;
                if (std::abs(px - kTile / 2) + std::abs(py - kTile / 2) <= kTile / 2) {
                    facadeCover_[static_cast<std::size_t>(r) * n + c] = 1;
                    any = true;
                }
            }
        }
    }
    if (!any) {
        return;
    }
    // The picture includes the ground in front of what it shows (a courtyard before a cliff), so
    // where the building stands is where the map stops you: its invisible blockers (Extent*:
    // ExtentStoneCylinderLarge, ExtentBoxLarge...) line the foot of a facade. Per column, the
    // southern edge of those inside the facade (smoothed between them) is the base, and the
    // facade's run of rows north of it the height. South of it the facade stays on the floor;
    // a column with no blocker over it, too.
    const auto covered = [&](int c, float y) {
        const int r = static_cast<int>((y - floorTop_) / kRes);
        return c >= 0 && c < n && r >= 0 && r < n && facadeCover_[static_cast<std::size_t>(r) * n + c];
    };
    std::vector<float> base(static_cast<std::size_t>(n), -1), height(static_cast<std::size_t>(n), 0);
    // Each column's base: the southern edge of the Extent blockers standing in the facade.
    for (const auto& o : f.view.objects_) {
        if (!o.art || o.art->name.rfind("Extent", 0) != 0) {
            continue;
        }
        // Its reach: half the box's longer side, or its radius, plus 12 of slack.
        const float r = (o.art->extentBox ? std::max(o.art->extentWidth, o.art->extentHeight) * 0.5f : o.art->extentRadius) + 12;
        const int c0 = static_cast<int>((o.x - r - floorLeft_) / kRes);
        const int c1 = static_cast<int>((o.x + r - floorLeft_) / kRes);
        for (int c = std::max(c0, 0); c <= std::min(c1, n - 1); ++c) {
            const float dx = floorLeft_ + (static_cast<float>(c) + 0.5f) * kRes - o.x;
            if (std::abs(dx) > r) {
                continue;
            }
            const float y = o.y + std::sqrt(r * r - dx * dx) * 0.5f; // (between centre and edge)
            if (covered(c, o.y) && y > base[static_cast<std::size_t>(c)]) {
                base[static_cast<std::size_t>(c)] = y;
            }
        }
    }
    {
        // Smoothed along the facade (a 9-column average of the columns that have one).
        std::vector<float> smooth(base);
        for (int c = 0; c < n; ++c) {
            if (base[static_cast<std::size_t>(c)] < 0) {
                continue;
            }
            float sum = 0;
            int k = 0;
            for (int d = -4; d <= 4; ++d) {
                if (c + d >= 0 && c + d < n && base[static_cast<std::size_t>(c + d)] >= 0) {
                    sum += base[static_cast<std::size_t>(c + d)];
                    ++k;
                }
            }
            smooth[static_cast<std::size_t>(c)] = sum / static_cast<float>(k);
        }
        base.swap(smooth);
    }
    // Each column's height: the run of covered rows north of its base.
    for (int c = 0; c < n; ++c) {
        if (base[static_cast<std::size_t>(c)] < 0) {
            continue;
        }
        float top = base[static_cast<std::size_t>(c)] - kRes;
        while (covered(c, top - kRes)) {
            top -= kRes;
        }
        // (At most what the floor texture holds above the base, and no taller than a cliff's art.)
        height[static_cast<std::size_t>(c)] =
            std::min({base[static_cast<std::size_t>(c)] - top, base[static_cast<std::size_t>(c)] - floorTop_ - 8, 450.0f});
    }
    // The floor texture as a sprite, for addFace's mapping (logical size: world units).
    const float size = static_cast<float>(floorTextureSize_);
    floorSprite_ = Sprite{};
    floorSprite_.texture = floorTexture_;
    floorSprite_.width = span;
    floorSprite_.height = span;
    floorSprite_.rect = {0, 0, size, size};
    floorSprite_.hd = true;
    // Each column's surface, its base meeting the next column's (no steps), both sides.
    const auto edgeBase = [&](int c) {
        const float a = c > 0 ? base[static_cast<std::size_t>(c - 1)] : -1;
        const float b = c < n ? base[static_cast<std::size_t>(c)] : -1;
        return a < 0 ? b : b < 0 ? a : (a + b) / 2;
    };
    // The standing surfaces, one per kRes column.
    for (int c = 0; c < n; ++c) {
        if (base[static_cast<std::size_t>(c)] < 0) {
            continue;
        }
        const float x0 = floorLeft_ + static_cast<float>(c) * kRes;
        const float x1 = x0 + kRes;
        const float y0 = edgeBase(c);
        const float y1 = edgeBase(c + 1);
        const float mid = (x0 + x1) / 2;
        if (std::hypot(mid - eyeX_, (y0 + y1) / 2 - eyeY_) > kDrawDistance + 40) {
            continue;
        }
        // Not where the base runs nearly north-south (the oblique picture has nothing for a
        // surface turned so: its columns smear), nor a sliver of a column or two.
        if (std::abs(y1 - y0) > 2.5f * kRes) {
            continue;
        }
        // Only columns in a run of at least 4 with a base (counting stops at 4).
        int run = 1;
        for (int d = c - 1; d >= 0 && base[static_cast<std::size_t>(d)] >= 0 && run < 4; --d) {
            ++run;
        }
        for (int d = c + 1; d < n && base[static_cast<std::size_t>(d)] >= 0 && run < 4; ++d) {
            ++run;
        }
        if (run < 4) {
            continue;
        }
        addFace(floorSprite_, floorLeft_, floorTop_, x0, y0, x1, y1, x0, y0, x1, y1, 0, height[static_cast<std::size_t>(c)],
                nullptr, true);
    }
}
void View3D::addWallFace(const Sprite& s, float sx0, float sy0, float ax, float ay, float bx, float by, float mx0, float my0,
                         float mx1, float my1, float height, const LightMap* light)
{
    // Taller than pictured (the pictures alone make walls barely over a head), the picture
    // stretched up evenly: one piece, no seam.
    addFace(s, sx0, sy0, ax, ay, bx, by, mx0, my0, mx1, my1, 0, height * (1 + wallRaise_), light, false, 1 / (1 + wallRaise_));
}

const nox::things::WallImage* View3D::wallImage(const Frame& f, const nox::map::Wall& w)
{
    // As drawScene picks it: the windowed pieces for window walls; the back image always (there's
    // no front in 3D).
    const auto& walls = f.view.things_.walls;
    if (w.material >= walls.size()) {
        return nullptr;
    }
    const auto& dirs = walls[w.material].directions;
    std::size_t dir = w.direction & 0x7F;
    std::size_t variant = w.variant;
    if (f.view.windows_.contains(MapView::wallKey(w.x, w.y)) && dir <= 1) {
        dir += 11;  // directions 11 and 12: the windowed pieces of 0 and 1
        if (dir < dirs.size() && variant >= dirs[dir].size()) {
            variant = 0;  // (fewer windowed variants: the first)
        }
    }
    if (dir >= dirs.size() || variant >= dirs[dir].size()) {
        return nullptr;
    }
    return &dirs[dir][variant][0];
}

const Sprite* View3D::wallSprite(const Frame& f, const nox::map::Wall& w,
                                 const std::unordered_map<std::uint16_t, const nox::map::Wall*>& cells, float& sx0,
                                 float& sy0)
{
    const auto* image = wallImage(f, w);
    if (!image) {
        return nullptr;
    }
    const std::int32_t index = f.sprites.indexOf(image->image);
    const auto* own = f.sprites.decoded(index);
    // A picture's top-left in the world: a wall image sits at (23x - 50 + dx, 23y - 72 - dy) plus the
    // image's own offset, as map_view.hpp describes.
    const auto origin = [](const nox::map::Wall& c, const nox::things::WallImage& im, const nox::video::DecodedImage& d) {
        return SDL_Point{c.x * static_cast<int>(kCell) - 50 + im.x + d.offsetX, c.y * static_cast<int>(kCell) - 72 - im.y + d.offsetY};
    };
    // Not decoded (no pixels to combine): the plain sprite at its world spot.
    if (!own || own->width <= 0) {
        const Sprite* s = f.sprites.get(image->image);
        if (s) {
            sx0 = static_cast<float>(w.x) * kCell - 50 + static_cast<float>(image->x) + s->offsetX;
            sy0 = static_cast<float>(w.y) * kCell - 72 - static_cast<float>(image->y) + s->offsetY;
        }
        return s;
    }
    // The neighbours it carries straight on into, across the corners its arms reach: in the 2D
    // view their pictures overlap its own and fill each other's clear ends.
    struct Part {
        const nox::video::DecodedImage* d;
        SDL_Point at;
        std::int32_t index;
        int row, column; ///< its cell: the 2D view draws the walls row by row (byRow)
    };
    std::vector<Part> parts{{own, origin(w, *image, *own), index, w.y, w.x}};
    const std::size_t dir = w.direction & 0x7F;
    if (dir < std::size(kEdgeFlags)) {
        constexpr int kCorner[4][3] = {{1, 1, -1}, {2, -1, -1}, {4, 1, 1}, {8, -1, 1}}; // flag, dx, dy
        constexpr std::uint8_t kOpposite[4] = {8, 4, 2, 1};  // the neighbour's arm back into the shared corner
        for (int k = 0; k < 4; ++k) {
            if (!(kEdgeFlags[dir] & kCorner[k][0])) {
                continue;
            }
            const auto it = cells.find(MapView::wallKey(w.x + kCorner[k][1], w.y + kCorner[k][2]));
            if (it == cells.end()) {
                continue;
            }
            const nox::map::Wall& n = *it->second;
            const std::size_t nd = n.direction & 0x7F;
            if (nd >= std::size(kEdgeFlags) || !(kEdgeFlags[nd] & kOpposite[k])) {
                continue; // (not carrying on into this corner)
            }
            if (const auto* ni = wallImage(f, n)) {
                const std::int32_t nIndex = f.sprites.indexOf(ni->image);
                if (const auto* nd2 = f.sprites.decoded(nIndex); nd2 && nd2->width > 0) {
                    parts.push_back({nd2, origin(n, *ni, *nd2), nIndex, n.y, n.x});
                }
            }
        }
    }
    // No neighbour carries on: its own picture.
    if (parts.size() == 1) {
        const Sprite* s = f.sprites.get(image->image);
        sx0 = static_cast<float>(parts[0].at.x);
        sy0 = static_cast<float>(parts[0].at.y);
        return s;
    }
    // Over all their area, in the 2D view's order (row by row, west to east), each over the ones
    // before: its picture as the 2D view shows it where they meet.
    const SDL_Point ownAt = parts[0].at;
    int x0 = ownAt.x, y0 = ownAt.y, x1 = x0 + own->width, y1 = y0 + own->height;
    std::string key = "view3d-wall:" + std::to_string(index);
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const auto& p = parts[i];
        x0 = std::min(x0, p.at.x);
        y0 = std::min(y0, p.at.y);
        x1 = std::max(x1, p.at.x + p.d->width);
        y1 = std::max(y1, p.at.y + p.d->height);
        key += ":" + std::to_string(p.index) + "@" + std::to_string(p.at.x - ownAt.x) + "," + std::to_string(p.at.y - ownAt.y);
    }
    std::ranges::sort(parts, [](const Part& a, const Part& b) { return a.row != b.row ? a.row < b.row : a.column < b.column; });
    const int width = x1 - x0;
    const int height = y1 - y0;
    sx0 = static_cast<float>(x0);
    sy0 = static_cast<float>(y0);
    // Built once per combination of pieces (the key) and cached by SpriteCache::composite.
    return f.sprites.composite(key, width, height, [&] {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4, 0);
        // Each part's opaque pixels copied in, later parts over earlier ones.
        for (const auto& p : parts) {
            for (int y = 0; y < p.d->height; ++y) {
                for (int x = 0; x < p.d->width; ++x) {
                    const std::uint8_t* src = &p.d->rgba[(static_cast<std::size_t>(y) * p.d->width + x) * 4];
                    if (src[3] == 0) {
                        continue;
                    }
                    std::uint8_t* dst =
                        &out[(static_cast<std::size_t>(p.at.y - y0 + y) * width + static_cast<std::size_t>(p.at.x - x0 + x)) * 4];
                    std::copy(src, src + 4, dst);
                }
            }
        }
        return out;
    });
}

void View3D::addWalls(const Frame& f)
{
    const auto& things = f.view.things_;
    const float tanPitch = std::tan(pitch_);  // the crosshair ray's rise per unit of depth
    // Which corners walls end at, to close the free ends only.
    std::map<std::pair<int, int>, int> ends;
    const auto inRange = [&](const nox::map::Wall& w) {
        return std::hypot(static_cast<float>(w.x) * kCell + 11.5f - eyeX_, static_cast<float>(w.y) * kCell + 11.5f - eyeY_) <=
               kDrawDistance + 40;
    };
    const auto key = [](float x, float y) { return std::pair{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}; };
    // The ways arms leave each point (centres and corners), as a bit per compass octant: an arm
    // is run on past a point only where nothing carries straight on from it (an angle), else
    // the two would overlap in one plane.
    const auto octant = [](float dx, float dy) {
        const int a = static_cast<int>(std::lround(std::atan2(dy, dx) / (kPi / 4))) & 7;
        return static_cast<std::uint8_t>(1u << a);
    };
    std::map<std::pair<int, int>, std::uint8_t> ways;
    std::unordered_map<std::uint16_t, const nox::map::Wall*> cells; // (wallSprite's neighbours)
    std::vector<WallArm> arms;
    // First pass, over the walls in range: count the arms ending at each point and the ways they
    // leave it.
    for (const auto& w : f.view.walls()) {
        if (!inRange(w)) {
            continue;
        }
        cells[MapView::wallKey(w.x, w.y)] = &w;
        arms.clear();
        wallArms(w, arms);
        for (const auto& a : arms) {
            ++ends[key(a.bx, a.by)];
            if (a.capA) {
                ++ends[key(a.ax, a.ay)];
            }
            ways[key(a.ax, a.ay)] |= octant(a.bx - a.ax, a.by - a.ay);
            ways[key(a.bx, a.by)] |= octant(a.ax - a.bx, a.ay - a.by);
        }
    }

    wallLines_.clear();
    const float h = kWallThickness / 2;
    // Second pass: each arm stood up as a box kWallThickness thick.
    for (const auto& w : f.view.walls()) {
        if (!inRange(w) || w.material >= things.walls.size()) {
            continue;
        }
        float sx0 = 0;
        float sy0 = 0;
        const Sprite* s = wallSprite(f, w, cells, sx0, sy0);
        if (!s || !s->texture) {
            continue;
        }
        const float pictured = wallHeight(f, w.material);
        const float height = pictured * (1 + wallRaise_);
        // Its centre in front of (or just behind) the eye counts as seen.
        if (toCam(static_cast<float>(w.x) * kCell + 11.5f, static_cast<float>(w.y) * kCell + 11.5f, 0).depth > -30) {
            f.view.seenWalls_.set(MapView::wallKey(w.x, w.y)); // (the minimap's seen walls)
        }
        arms.clear();
        wallArms(w, arms);
        for (const auto& arm : arms) {
            const float len = std::hypot(arm.bx - arm.ax, arm.by - arm.ay);
            if (len < 0.5f) {
                continue;
            }
            const float dx = (arm.bx - arm.ax) / len;
            const float dy = (arm.by - arm.ay) / len;
            // From a centre, back by the half thickness, so arms at an angle close their corner;
            // and on through a corner another wall meets at an angle, for the same reason (else
            // a wedge between them shows what's behind). Where the next piece carries straight
            // on the two meet exactly at the corner (no overlap to fight over in the depth
            // buffer): its picture's clear ends are filled from the neighbour's (wallSprite).
            const bool straightOnA = ways[key(arm.ax, arm.ay)] & octant(-dx, -dy);
            const bool straightOnB = ways[key(arm.bx, arm.by)] & octant(dx, dy);
            const bool sharedEnd = ends[key(arm.bx, arm.by)] > 1;
            const float backA = arm.capA || straightOnA ? 0.0f : h;
            const float onB = !sharedEnd || straightOnB ? 0.0f : h;
            const float ax = arm.ax - dx * backA;
            const float ay = arm.ay - dy * backA;
            const float bx = arm.bx + dx * onB;
            const float by = arm.by + dy * onB;
            const float nx = -dy * h; // a side's offset
            const float ny = dx * h;
            wallLines_.push_back({ax, ay, bx, by, height});
            // The two sides, textured from the centre line; the free ends closed.
            addWallFace(*s, sx0, sy0, ax - nx, ay - ny, bx - nx, by - ny, ax, ay, bx, by, pictured, f.light);
            addWallFace(*s, sx0, sy0, bx + nx, by + ny, ax + nx, ay + ny, bx, by, ax, ay, pictured, f.light);
            // End caps only where no other arm meets the end.
            if (arm.capB && !sharedEnd) {
                addWallFace(*s, sx0, sy0, bx - nx, by - ny, bx + nx, by + ny, bx, by, bx, by, pictured, f.light);
            }
            if (arm.capA && ends[key(arm.ax, arm.ay)] == 1) {
                addWallFace(*s, sx0, sy0, ax + nx, ay + ny, ax - nx, ay - ny, ax, ay, ax, ay, pictured, f.light);
            }
            // The crosshair's ray (flat forward, rising by the pitch) against the wall's line.
            if (const auto hit = crosshairCross(ax, ay, bx, by)) {
                const auto [t, u] = *hit;
                const float z = eyeZ_ + t * tanPitch;
                // A hit: in front, nearer than any so far, within the wall's length and its height.
                if (t > kNear && t < pickDepth_ && u >= 0 && u <= 1 && z >= 0 && z <= height) {
                    pickDepth_ = t;
                    pick_ = SDL_FPoint{ax + (bx - ax) * u, ay + (by - ay) * u}; // the wall's foot there
                    pickThing_ = false;
                }
            }
        }
    }
}

std::optional<std::pair<float, float>> View3D::crosshairCross(float ax, float ay, float bx, float by) const
{
    const float ex = bx - ax;
    const float ey = by - ay;
    const float denom = cos_ * ey - sin_ * ex;  // cross(forward, edge): 0 when they're parallel
    if (std::abs(denom) <= 1e-5f) {
        return std::nullopt;
    }
    const float qx = ax - eyeX_;
    const float qy = ay - eyeY_;
    return std::pair{(qx * ey - qy * ex) / denom, (qx * sin_ - qy * cos_) / denom};
}

bool View3D::addOnWall(const Frame& f, const Sprite& s, float sx, float sy, const MapView::Object& o, const LightMap* light)
{
    (void)f;
    // The nearest wall line to it, within reach of a thing hung on it.
    constexpr float kReach = 26;
    const WallLine* best = nullptr;
    float bestDistance = kReach;
    for (const auto& l : wallLines_) {
        const float ex = l.bx - l.ax;
        const float ey = l.by - l.ay;
        const float len2 = ex * ex + ey * ey;
        if (len2 < 1e-3f) {
            continue;
        }
        // The nearest point of the line (u clamped to the segment).
        const float u = std::clamp(((o.x - l.ax) * ex + (o.y - l.ay) * ey) / len2, 0.0f, 1.0f);
        const float d = std::hypot(o.x - (l.ax + ex * u), o.y - (l.ay + ey * u));
        if (d < bestDistance) {
            bestDistance = d;
            best = &l;
        }
    }
    if (!best) {
        return false;
    }
    // A plane along the wall, just off its face on the thing's side, long enough for the whole
    // picture (addFace keeps only the part inside it); seen from that side only.
    const float ex = best->bx - best->ax;
    const float ey = best->by - best->ay;
    const float len = std::hypot(ex, ey);
    const float dx = ex / len;
    const float dy = ey / len;
    float nx = -dy;
    float ny = dx;
    if (nx * (o.x - best->ax) + ny * (o.y - best->ay) < 0) {
        nx = -nx;
        ny = -ny;
    }
    const float off = kWallThickness / 2 + 0.75f;
    const float px = best->ax + nx * off - dx * 60;
    const float py = best->ay + ny * off - dy * 60;
    const float qx = best->bx + nx * off + dx * 60;
    const float qy = best->by + ny * off + dy * 60;
    // (addFace shows the side (dy, -dx) of a -> b points to: ours is n.)
    if (dy * nx - dx * ny > 0) {
        addFace(s, sx, sy, px, py, qx, qy, px, py, qx, qy, 0, 250, light, false);  // (250 tall: addFace keeps only the picture's rows)
    } else {
        addFace(s, sx, sy, qx, qy, px, py, qx, qy, px, py, 0, 250, light, false);
    }
    // The crosshair on its picture: its foot (a lever's use, as the 2D cursor on it).
    if (const auto hit = crosshairCross(px, py, qx, qy)) {
        const auto [t, u] = *hit;
        if (t > kNear && t < pickDepth_ && u >= 0 && u <= 1) {
            const float hx = px + (qx - px) * u;
            const float hy = py + (qy - py) * u;
            const float z = eyeZ_ + t * std::tan(pitch_);
            // A hit only on the picture's own rectangle (sprite row = y - z, as in 2D).
            const float col = hx - sx;
            const float row = hy - z - sy;
            if (col >= 0 && col < s.width && row >= 0 && row < s.height) {
                pickDepth_ = t;
                pick_ = SDL_FPoint{o.x, o.y};
                pickThing_ = true;
            }
        }
    }
    return true;
}

void View3D::addCap(const Sprite& s, float sx0, float sy0, const SDL_FPoint* corners, int n, float z, const LightMap* light,
                    float zScale)
{
    float tw = 0, th = 0;
    SDL_GetTextureSize(s.texture, &tw, &th);
    if (tw <= 0 || th <= 0 || s.width <= 0 || s.height <= 0 || n < 3 || n > 8) {
        return;
    }
    float cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) {
        cx += corners[i].x / static_cast<float>(n);
        cy += corners[i].y / static_cast<float>(n);
    }
    const float distance = std::hypot(cx - eyeX_, cy - eyeY_);
    const float rw = s.imageWidth();  // (a material sprite's image is the left half when its material bytes are beside it)
    const SDL_FColor colour = shade(light ? light->sample(cx, cy) : Rgb{255, 255, 255}, distance);
    // A fan from the centre, each triangle split once more, so the affine mapping holds.
    Batch batch{s.texture, &s, static_cast<int>(tris_.size()), 0, 255, s.hd};
    const auto vertex = [&](float x, float y) {
        ClipVertex v;
        v.p = toCam(x, y, z);
        const float px = std::clamp(x - sx0, 0.5f, s.width - 0.5f);
        const float py = std::clamp(y - z * zScale - sy0, 0.5f, s.height - 0.5f);
        v.u = (s.rect.x + px * rw / s.width) / tw;
        v.v = (s.rect.y + py * s.rect.h / s.height) / th;
        v.c = colour;
        return v;
    };
    for (int i = 0; i < n; ++i) {
        const SDL_FPoint& a = corners[i];
        const SDL_FPoint& b = corners[(i + 1) % n];
        const float mx = (a.x + b.x) / 2;
        const float my = (a.y + b.y) / 2;
        const ClipVertex t1[3] = {vertex(cx, cy), vertex(a.x, a.y), vertex(mx, my)};
        const ClipVertex t2[3] = {vertex(cx, cy), vertex(mx, my), vertex(b.x, b.y)};
        emit(batch, t1, 3);
        emit(batch, t2, 3);
    }
    if (batch.count > 0) {
        items_.push_back({distance - 0.5f, false, static_cast<int>(batches_.size())}); // over its own sides
        batches_.push_back(batch);
    }
}

// The bounding box of an image's pixels with alpha over 16 (the whole image if none); cached by
// image.
SDL_Rect View3D::opaqueBounds(const Frame& f, std::int32_t image)
{
    if (const auto it = opaque_.find(image); it != opaque_.end()) {
        return it->second;
    }
    SDL_Rect r{0, 0, 0, 0};
    if (const auto* d = image >= 0 ? f.sprites.decoded(image) : nullptr; d && d->width > 0 && d->height > 0) {
        int x0 = d->width, y0 = d->height, x1 = -1, y1 = -1;
        for (int y = 0; y < d->height; ++y) {
            for (int x = 0; x < d->width; ++x) {
                if (d->rgba[(static_cast<std::size_t>(y) * d->width + x) * 4 + 3] > 16) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
        }
        if (x1 >= x0) {
            r = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        } else {
            r = {0, 0, d->width, d->height};
        }
    }
    opaque_[image] = r;
    return r;
}

void View3D::addSolid(const Frame& f, const Sprite& s, std::int32_t image, float sx, float sy, const TypeArt& art,
                      float ox, float oy, const LightMap* light)
{
    // The extent's box as the game lays it (physics.cpp boxCorners): its width along "\", its
    // height along "/". Clockwise on the screen: top, right, bottom, left.
    constexpr float kBoxHalf = 0.35355339f; // 1 / (2 sqrt 2)
    // (A round one as the square of its radius.)
    float a = art.extentBox ? art.extentWidth * kBoxHalf : std::max(art.extentRadius, 3.0f) / 2;
    float b = art.extentBox ? art.extentHeight * kBoxHalf : std::max(art.extentRadius, 3.0f) / 2;
    const SDL_Rect seen = opaqueBounds(f, image);
    const bool measured = seen.w > 0 && seen.h > 0;
    // Widened a little where its picture is wider than its extent (an obelisk's crystal over a
    // small base), so less of it is cut off at the sides; not much, or we'd stand inside it.
    const float pictureWidth = measured ? static_cast<float>(seen.w) : s.width;
    const float grow = std::clamp(pictureWidth / std::max(2 * (a + b), 1.0f), 1.0f, 1.5f);
    a *= grow;
    b *= grow;
    const SDL_FPoint c[4] = {{ox + b - a, oy - a - b}, {ox + a + b, oy + a - b}, {ox + a - b, oy + a + b}, {ox - a - b, oy + b - a}};
    // The picture is turned to face us, as a billboard is, and wrapped round the sides we see:
    // in a frame turned toward the eye (across: our right; out: toward us), a side's column is
    // its place across, and its rows rise from the front edge of the footprint there (what's
    // nearest us at that column): an obelisk's drawn base (a V) sits on the box's front edges, from
    // any side. (Only obelisks are drawn so: on a wide thing it tilts the picture up at one end.)
    float outX = eyeX_ - ox;
    float outY = eyeY_ - oy;
    const float outLen = std::max(std::hypot(outX, outY), 1e-3f);
    outX /= outLen;
    outY /= outLen;
    const float acrossX = outY; // (out (0, 1), south: across (1, 0), east, as the 2D view)
    const float acrossY = -outX;
    float ax_[4], ay_[4];
    float nearest = -1e9f;
    for (int i = 0; i < 4; ++i) {
        ax_[i] = (c[i].x - ox) * acrossX + (c[i].y - oy) * acrossY;
        ay_[i] = (c[i].x - ox) * outX + (c[i].y - oy) * outY;
        nearest = std::max(nearest, ay_[i]);
    }
    // The footprint's front (nearest us, largest `out`) at a place across: over the edges spanning
    // it; `nearest` where none does.
    const auto frontAt = [&](float across) {
        float best = -1e9f;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            const float lo = std::min(ax_[i], ax_[j]);
            const float hi = std::max(ax_[i], ax_[j]);
            if (across < lo - 1e-3f || across > hi + 1e-3f) {
                continue;
            }
            const float t = hi - lo > 1e-3f ? (across - ax_[i]) / (ax_[j] - ax_[i]) : 0.0f;
            best = std::max(best, ay_[i] + (ay_[j] - ay_[i]) * std::clamp(t, 0.0f, 1.0f));
        }
        return best > -1e8f ? best : nearest;
    };
    const float top = sy + (measured ? static_cast<float>(seen.y) : 0.0f);
    // As tall as it's pictured standing (the 2D view's front corner up to the top row), the same
    // from any side; the whole picture fitted up its sides, nothing laid back as a top.
    const float pictured = std::clamp(oy + nearest - top, 8.0f, 250.0f); // (from this side's front)
    float height = std::clamp(c[2].y - top, 8.0f, 250.0f);
    if (art.nameObelisk) {
        height = kEyeHeight + 10; // obelisks: a little over our eyes
    }
    const float zScale = pictured / height;  // sprite rows per unit of height up the sides
    // The crosshair on a side: its foot (the thing itself).
    const auto pickSide = [&](const SDL_FPoint& p, const SDL_FPoint& q, float tall) {
        const auto hit = crosshairCross(p.x, p.y, q.x, q.y);
        if (!hit) {
            return;
        }
        const auto [t, u] = *hit;
        const float z = eyeZ_ + t * std::tan(pitch_);
        if (t > kNear && t < pickDepth_ && u >= 0 && u <= 1 && z >= 0 && z <= tall) {
            pickDepth_ = t;
            pick_ = SDL_FPoint{ox, oy};
            pickThing_ = true;
        }
    };
    if (debugLog()) {
        static std::set<std::string> logged;
        if (logged.insert(art.name).second) {
            SDL_Log("view3d: solid %s: sprite %.0fx%.0f at %.0f,%.0f (object %.0f,%.0f), opaque %d,%d %dx%d, grow %.2f, "
                    "front y %.0f, pictured %.0f, height %.0f",
                    art.name.c_str(), s.width, s.height, sx, sy, ox, oy, seen.x, seen.y, seen.w, seen.h, grow, c[2].y,
                    pictured, height);
        }
    }
    for (int i = 0; i < 4; ++i) {
        const SDL_FPoint& p = c[i];
        const SDL_FPoint& q = c[(i + 1) % 4];
        const int ip = i;
        const int iq = (i + 1) % 4;
        addFace(s, sx, sy, p.x, p.y, q.x, q.y, ox + ax_[ip], oy + frontAt(ax_[ip]), ox + ax_[iq], oy + frontAt(ax_[iq]), 0, height,
                light, false, zScale);
        pickSide(p, q, height);
    }
    // Its top only when the eye is above it.
    if (eyeZ_ > height) {
        addCap(s, sx, sy, c, 4, height, light, zScale);
    }
    solidSpots_.push_back({ox, oy});
}

void View3D::addBoard(const Sprite& s, float sx, float sy, float bx, float by, Rgb light, std::uint8_t alpha, float bias)
{
    const Cam c = toCam(bx, by, 0);
    if (c.depth < kNear + 4) {
        return;
    }
    const float distance = std::hypot(bx - eyeX_, by - eyeY_);
    if (distance > kDrawDistance) {
        return;
    }
    // Each pixel row of the 2D sprite at the height it stands for (row y: height by - y), its
    // columns across from the base as they are in 2D.
    const float k = focal_ / c.depth;
    const SDL_FRect dst{centreX_ + (c.x + (sx - bx)) * k, horizon_ + (sy - by + eyeZ_) * k, s.width * k, s.height * k};
    if (dst.x > static_cast<float>(viewW_) || dst.x + dst.w < 0 || dst.y > static_cast<float>(viewH_) || dst.y + dst.h < 0) {
        return;
    }
    const Rgb lit = scaled(light, fog(distance));
    if (depth_) {
        // With the depth buffer: a quad facing us at the base's depth (its rectangle kept for
        // the crosshair's pick).
        float tw = 0, th = 0;
        SDL_GetTextureSize(s.texture, &tw, &th);
        if (tw > 0 && th > 0) {
            Batch batch{s.texture, &s, static_cast<int>(tris_.size()), 0, alpha, s.hd};
            const float u0 = s.rect.x / tw;
            const float u1 = (s.rect.x + s.imageWidth()) / tw;
            const float v0 = s.rect.y / th;
            const float v1 = (s.rect.y + s.rect.h) / th;
            const float left = c.x + (sx - bx);
            const float top = by - sy - eyeZ_;
            const SDL_FColor colour{lit[0] / 255.0f, lit[1] / 255.0f, lit[2] / 255.0f, alpha / 255.0f};
            const auto corner = [&](float x, float up, float u, float v, float back = 0) {
                ClipVertex cv;
                cv.p = {x, c.depth - bias * 0.01f + back, up};
                cv.u = u;
                cv.v = v;
                cv.c = colour;
                return cv;
            };
            // A bent card: the rows above its base row stand up facing us, leaning back (setTilt:
            // the art is drawn from above, its tops seen from above); the rows below it are where
            // the 2D view shows them, on the floor in front of the base (feet, shadows, the near
            // half of what lies on the ground), so laid flat toward us rather than sunk under it.
            const float baseRow = std::clamp(by - sy, 0.0f, s.height); // (row of height 0)
            const float vBase = v0 + (v1 - v0) * baseRow / s.height;
            if (baseRow > 0) {
                const float back = baseRow * tiltTan_; // (the top row's; the bottom stands)
                const ClipVertex quad[4] = {corner(left, top, u0, v0, back), corner(left + s.width, top, u1, v0, back),
                                            corner(left + s.width, top - baseRow, u1, vBase),
                                            corner(left, top - baseRow, u0, vBase)};
                emit(batch, quad, 4);
            }
            if (baseRow < s.height) {
                // On the floor: a row r below the base lies r units nearer us, a hair above it.
                const auto flat = [&](float x, float rows, float u, float v) {
                    ClipVertex cv;
                    cv.p = {x, c.depth - bias * 0.01f - rows, 0.05f - eyeZ_};
                    cv.u = u;
                    cv.v = v;
                    cv.c = colour;
                    return cv;
                };
                const float below = s.height - baseRow;
                const ClipVertex quad[4] = {flat(left, 0, u0, vBase), flat(left + s.width, 0, u1, vBase),
                                            flat(left + s.width, below, u1, v1), flat(left, below, u0, v1)};
                emit(batch, quad, 4);
            }
            items_.push_back({distance - bias, false, static_cast<int>(batches_.size())});
            batches_.push_back(batch);
        }
        boards_.push_back({&s, dst, lit, alpha, k});
        return;
    }
    // Without the depth pass: a plain screen rectangle, sorted with the batches by distance.
    items_.push_back({distance - bias, true, static_cast<int>(boards_.size())});
    boards_.push_back({&s, dst, lit, alpha, k});
}

void View3D::addParticle(float x, float y, float z, float radius, SDL_Color colour, float alpha, int mode, int seed)
{
    if (radius <= 0 || alpha <= 0 || unseen(x, y)) {
        return;
    }
    const Cam c = toCam(x, y, z);
    if (c.depth < kNear) {
        return;
    }
    const float distance = std::hypot(x - eyeX_, y - eyeY_);
    if (distance > kDrawDistance) {
        return;
    }
    const float f = fog(distance) / 255.0f;
    // (Fading out as it comes right up to the eye, where it'd fill the view.)
    const float nearFade = std::clamp((c.depth - kNear) / 30.0f, 0.0f, 1.0f);
    const SDL_FColor fc{static_cast<float>(colour.r) * f, static_cast<float>(colour.g) * f, static_cast<float>(colour.b) * f,
                        std::min(alpha, 1.0f) * nearFade};
    // The look in the coordinates' whole part (view3d_particle.frag.hlsl): u / 4 the mode, v / 4 a seed.
    const float u0 = static_cast<float>(mode * 4);
    const float v0 = static_cast<float>((seed & 255) * 4);
    const auto corner = [&](float dx, float dy, float u, float v) {
        ClipVertex cv;
        cv.p = {c.x + dx, c.depth, c.up + dy};
        cv.u = u0 + u;
        cv.v = v0 + v;
        cv.c = fc;
        return cv;
    };
    ParticleQuad q;
    q.depth = c.depth;
    q.v[0] = corner(-radius, radius, 0, 0);
    q.v[1] = corner(radius, radius, 1, 0);
    q.v[2] = corner(radius, -radius, 1, 1);
    q.v[3] = corner(-radius, -radius, 0, 1);
    (mode == 0 ? addedQuads_ : blendedQuads_).push_back(q);
}

void View3D::addRibbon(float ax, float ay, float az, float bx, float by, float bz, float width, SDL_Color colour, float alpha,
                       bool added)
{
    const Cam a = toCam(ax, ay, az);
    const Cam b = toCam(bx, by, bz);
    if ((a.depth < kNear && b.depth < kNear) || alpha <= 0 || (unseen(ax, ay) && unseen(bx, by))) {
        return;
    }
    const float distance = std::hypot((ax + bx) / 2 - eyeX_, (ay + by) / 2 - eyeY_);
    if (distance > kDrawDistance) {
        return;
    }
    // Across the line as it's seen (on the screen), `width` world units each side at each end.
    float dx = b.x - a.x;
    float dy = b.up - a.up;
    // Both in front: the direction on the screen (x and up over depth); else the camera-space one.
    if (a.depth > kNear && b.depth > kNear) {
        dx = b.x / b.depth - a.x / a.depth;
        dy = b.up / b.depth - a.up / a.depth;
    }
    const float len = std::hypot(dx, dy);
    if (len < 1e-6f) {
        dx = 1;
        dy = 0;
    } else {
        dx /= len;
        dy /= len;
    }
    const float nx = -dy * width;
    const float ny = dx * width;
    const float f = fog(distance) / 255.0f;
    const float nearFade = std::clamp((std::min(a.depth, b.depth) - kNear) / 20.0f, 0.0f, 1.0f);
    const SDL_FColor fc{static_cast<float>(colour.r) * f, static_cast<float>(colour.g) * f, static_cast<float>(colour.b) * f,
                        std::min(alpha, 1.0f) * nearFade};
    const auto corner = [&](const Cam& p, float sign, float u, float v) {
        ClipVertex cv;
        cv.p = {p.x + nx * sign, p.depth, p.up + ny * sign};
        cv.u = 12 + u; // (mode 3, the ribbon)
        cv.v = v;
        cv.c = fc;
        return cv;
    };
    ParticleQuad q;
    q.depth = (a.depth + b.depth) / 2;
    q.v[0] = corner(a, 1, 0, 0);
    q.v[1] = corner(b, 1, 1, 0);
    q.v[2] = corner(b, -1, 1, 1);
    q.v[3] = corner(a, -1, 0, 1);
    (added ? addedQuads_ : blendedQuads_).push_back(q);
}

void View3D::addRing()
{
    if (!ring_ || ring_->radius <= 0) {
        return;
    }
    const Ring& r = *ring_;
    const float distance = std::hypot(r.x - eyeX_, r.y - eyeY_);
    if (distance > kDrawDistance) {
        return;
    }
    // The 2D view's band (3 game pixels: a solid middle between half see-through edges) as a
    // ribbon on the floor, a hair above it, bright down its middle; segments round the circle.
    constexpr float kHalfWidth = 2.5f;
    constexpr float kLift = 1;
    const float f = fog(distance) / 255.0f;
    const SDL_FColor fc{static_cast<float>(r.colour.r) * f, static_cast<float>(r.colour.g) * f,
                        static_cast<float>(r.colour.b) * f, static_cast<float>(r.colour.a) / 255.0f};
    const int n = std::clamp(static_cast<int>(r.radius * 0.75f), 24, 96);
    const auto corner = [&](float angle, float radius, float u, float v) {
        ClipVertex cv;
        cv.p = toCam(r.x + std::cos(angle) * radius, r.y + std::sin(angle) * radius, kLift);
        cv.u = 12 + u; // (mode 3, the ribbon: v across it)
        cv.v = v;
        cv.c = fc;
        return cv;
    };
    for (int i = 0; i < n; ++i) {
        const float a0 = 2 * kPi * static_cast<float>(i) / static_cast<float>(n);
        const float a1 = 2 * kPi * static_cast<float>(i + 1) / static_cast<float>(n);
        ParticleQuad q;
        q.v[0] = corner(a0, r.radius - kHalfWidth, 0, 0);
        q.v[1] = corner(a1, r.radius - kHalfWidth, 1, 0);
        q.v[2] = corner(a1, r.radius + kHalfWidth, 1, 1);
        q.v[3] = corner(a0, r.radius + kHalfWidth, 0, 1);
        if (q.v[0].p.depth < kNear && q.v[1].p.depth < kNear && q.v[2].p.depth < kNear && q.v[3].p.depth < kNear) {
            continue;
        }
        q.depth = (q.v[0].p.depth + q.v[2].p.depth) / 2;
        blendedQuads_.push_back(q);
    }
}

void View3D::addEffects(const Frame& f)
{
    // What Effects::drawParticles and its parts draw in 2D (sizes, colours and intensities as
    // there), each at its real height instead of up the screen.
    const Effects& fx = *f.effects;
    // How far into the current game frame we are (0-1): positions are drawn between their last and
    // current ones.
    const float t = fx.stepBlend();
    const auto intensity = [](int i) { return static_cast<float>(std::min(i, 255)) / 255.0f; };

    // MSG_FX_PARTICLEFX's particles (drawParticle): a glow and a core.
    for (const auto& p : fx.particles_) {
        // 16.16 fixed point; the 2D view draws it at y - z, so its height is its ground y less that.
        const float x = p.prevX + (static_cast<float>(p.x) / 0x10000 - p.prevX) * t;
        const float drawn = p.prevY + (static_cast<float>(p.y - p.z) / 0x10000 - p.prevY) * t;
        const float ground = static_cast<float>(p.y) / 0x10000;
        const float z = ground - drawn;
        if (p.glowRadius > 0 && p.glowIntensity > 0) {
            addParticle(x, ground, z, p.glowRadius, p.glow, intensity(p.glowIntensity), 0);
        }
        if (p.size > 0) {
            addParticle(x, ground, z, std::max(0.75f, static_cast<float>(p.size) * 0.5f), p.color, 1, 1);
        }
    }
    // The sparks: shrinking from 3 to a point and fading over their lives; twinkles.
    for (const auto& s : fx.sparks_) {
        const float x = s.prevX + (static_cast<float>(s.fx >> 12) - s.prevX) * t;
        const float ground = s.prevY + (static_cast<float>(s.fy >> 12) - s.prevY) * t;
        const float z = s.prevZ + (static_cast<float>(s.z) - s.prevZ) * t + s.zBase;
        if (s.twinkle) {
            const auto& pal = s.twinkleUseAlt ? s.twinkleAlt : s.twinkleMain;
            if (s.twinkleSize > 0) {
                addParticle(x, ground, z, static_cast<float>(s.twinkleSize * 2 + 1 + 4), pal.glow,
                            intensity((s.twinkleSize + 1) << 5), 0);
                addParticle(x, ground, z, std::max(0.75f, static_cast<float>(s.twinkleSize) * 0.5f), pal.core, 1, 1);
            }
            continue;
        }
        if (!s.type) {
            continue;
        }
        // Its size (0-4) and glow (0-128, in steps of 32) shrink with the life left.
        const int total = std::max(s.total, 1);
        const int size = s.remaining * 4 / total;
        const int glow = std::min(s.remaining * 5 / total, 4) << 5;
        if (glow > 0) {
            addParticle(x, ground, z, static_cast<float>(size * 2 + 1 + 4), s.type->palette.glow, intensity(glow), 0);
        }
        addParticle(x, ground, z, static_cast<float>(std::clamp(size, 1, 3)) * 0.5f + 0.25f, s.type->palette.core, 1, 1);
    }
    // Soft puffs of smoke and gas, lit where they are.
    if (fxdraw::puffsOn()) {
        for (const auto& p : fx.puffs_) {
            const float x = p.prevX + (p.x - p.prevX) * t;
            const float y = p.prevY + (p.y - p.prevY) * t;
            const float z = p.prevZ + (p.z - p.prevZ) * t;
            const float radius = p.prevRadius + (p.radius - p.prevRadius) * t;
            const float life = static_cast<float>(p.end - p.start);
            const float age = (static_cast<float>(fx.time_) - static_cast<float>(p.start)) / std::max(life, 1.0f);
            const float fade = age < 0.2f ? std::max(age, 0.0f) / 0.2f : std::max(0.0f, (1.0f - age) / 0.8f);
            SDL_Color c = p.color;
            if (f.light) {
                const Rgb l = f.light->sample(x, y);
                c.r = static_cast<std::uint8_t>(c.r * l[0] / 255);
                c.g = static_cast<std::uint8_t>(c.g * l[1] / 255);
                c.b = static_cast<std::uint8_t>(c.b * l[2] / 255);
            }
            addParticle(x, y, z, radius, c, static_cast<float>(p.color.a) / 255.0f * fade, 2, static_cast<int>(p.seed));
        }
    }
    // Glow orbs (drain mana, heal, charm), 22 up: a glow, a core and a trail.
    for (const auto& o : fx.orbs_) {
        const float x = static_cast<float>(o.prevX) + static_cast<float>(o.x - o.prevX) * t;
        const float ground = static_cast<float>(o.prevY) + static_cast<float>(o.y - o.prevY) * t;
        constexpr float kOrbHeight = 22;
        addParticle(x, ground, kOrbHeight, static_cast<float>(o.radius + 4), o.palette.glow, intensity(5 << 5), 0);
        addParticle(x, ground, kOrbHeight, std::max(0.75f, static_cast<float>(o.radius >> 1) * 0.5f), o.palette.core, 1, 1);
        addRibbon(x, ground, kOrbHeight, x - static_cast<float>(o.x - o.prevX), ground - static_cast<float>(o.y - o.prevY),
                  kOrbHeight, 0.5f, o.palette.core, 1);
    }
    // Turn Undead's ring: blue glows shrinking as they fly out.
    if (!fx.killers_.empty()) {
        if (const TypeArt* killer = fx.art_.get(fx.killerArt_, "UndeadKiller")) {
            for (const auto& k : fx.killers_) {
                const SDL_FPoint p = fx.killerPosition(k, fx.time_);
                const int distance = static_cast<int>(std::hypot(p.x - static_cast<float>(k.originX), p.y - static_cast<float>(k.originY)));
                int size = 8 - distance / 40;
                if (size < 0) {
                    size = 1;
                }
                addParticle(p.x, p.y, killer->z, static_cast<float>(size + 4), {100, 100, 255, 255}, intensity(12 << 5), 0);
            }
        }
    }
    // What drawables draw of their own: magic and missile glows, pixies.
    if (fx.anchors_) {
        for (const auto& [netCode, d] : fx.drawableFx_) {
            const auto a = fx.anchors_->find(netCode);
            if (a == fx.anchors_->end()) {
                continue;
            }
            const TypeArt* art = fx.art_.get(a->second.typeId);
            if (!art) {
                continue;
            }
            const Anchor& at = a->second;
            if (art->drawFn == DrawFn::Magic || art->drawFn == DrawFn::MagicMissile) {
                const float z = art->z + at.z;
                const SDL_Color c = art->drawFn == DrawFn::Magic ? SDL_Color{0, 200, 255, 255} : SDL_Color{255, 255, 50, 255};
                addParticle(at.x, at.y, z, static_cast<float>(d.magicSize * 2 + 1 + 4), c, intensity(((d.magicSize >> 1) + 3) << 5), 0);
                addParticle(at.x, at.y, z, std::max(0.75f, static_cast<float>(d.magicSize) * 0.5f), {255, 255, 255, 255}, 1, 1);
            } else if (art->drawFn == DrawFn::Pixie) {
                const float z = art->z + d.bob;
                addParticle(at.x, at.y, z, 14, {255, 200, 0, 255}, intensity(4 << 5), 0);
                addRibbon(at.x, at.y, z, d.fromX, d.fromY, z, 0.5f, {255, 255, 100, 255}, 1);
            }
        }
    }
    // Beams, 20 up between their ends: lightning (blue, green) and energy bolts, a jagged path
    // drawn afresh each game frame, a wide glow under a bright core.
    for (const auto& r : fx.rays_) {
        SDL_Color core{};
        SDL_Color glow{};
        switch (r.kind) {
        case Effects::Ray::Kind::Lightning:
        case Effects::Ray::Kind::ChainLightning:
            core = {255, 255, 255, 255};
            glow = {128, 128, 255, 255};
            break;
        case Effects::Ray::Kind::GreenBolt:
            core = {40, 225, 40, 255};
            glow = {150, 220, 150, 255};
            break;
        case Effects::Ray::Kind::EnergyBolt:
            core = {255, 255, 255, 255};
            glow = {255, 255, 0, 255};
            break;
        default:
            continue; // (drain, heal, charm: their orbs; the harpoon's rope: not yet)
        }
        const auto a = fx.endPoint(r.from);
        const auto b = fx.endPoint(r.to);
        if (!a || !b) {
            continue;
        }
        constexpr float kRayHeight = 20;
        const float len = std::hypot(b->x - a->x, b->y - a->y);
        // A piece every 14 units or so (2-24), the inner points jittered up to 6 each way; seeded from
        // the ray, so the path is the same until its seed changes.
        const int pieces = std::clamp(static_cast<int>(len / 14), 2, 24);
        std::minstd_rand rng(r.seed ? r.seed : 1);
        nox::util::uniform_real_distribution<float> jitter(-6, 6);
        float px = a->x, py = a->y, pz = kRayHeight;
        for (int i = 1; i <= pieces; ++i) {
            const float s = static_cast<float>(i) / static_cast<float>(pieces);
            float qx = a->x + (b->x - a->x) * s;
            float qy = a->y + (b->y - a->y) * s;
            float qz = kRayHeight;
            if (i < pieces) {
                qx += jitter(rng);
                qy += jitter(rng);
                qz += jitter(rng);
            }
            addRibbon(px, py, pz, qx, qy, qz, r.kind == Effects::Ray::Kind::EnergyBolt ? 3.0f + r.width : 4.0f, glow, 0.55f);
            addRibbon(px, py, pz, qx, qy, qz, 0.9f, core, 1);
            px = qx;
            py = qy;
            pz = qz;
        }
    }
    // Missiles' and arrows' trails, 28 up (32 for arrows), fading as they expire.
    for (const auto& l : fx.tailLinks_) {
        const double left = static_cast<double>(l.end) - fx.time_;
        if (left <= 0) {
            continue;
        }
        const bool arrow = l.kind == Effects::TailLink::Kind::Arrow || l.kind == Effects::TailLink::Kind::WeakArrow;
        const float lift = 28 + (arrow ? 4.0f : 0.0f);
        const float fade = static_cast<float>(left / static_cast<double>(std::max<std::uint32_t>(l.end - l.start, 1)));
        const SDL_Color c = arrow ? SDL_Color{220, 220, 220, 255}
                            : l.kind == Effects::TailLink::Kind::MagicMissile ? SDL_Color{255, 255, 120, 255}
                                                                              : SDL_Color{120, 200, 255, 255};
        addRibbon(l.x1, l.y1, lift, l.x2, l.y2, lift, 0.6f, c, 0.6f * fade, false);
    }
}

float View3D::standingBase(const Frame& f, std::int32_t image, float sy, float height)
{
    // The row under its lowest solid pixel: standing there, the whole picture is upright.
    const SDL_Rect seen = opaqueBounds(f, image);
    return sy + (seen.h > 0 ? static_cast<float>(seen.y + seen.h) : height);
}

bool View3D::itemType(const TypeArt& art)
{
    namespace oc = nox::things::ObjectClass;
    return (art.classes & oc::Pickup) && !(art.classes & (oc::Monster | oc::Player | oc::Immobile)) && art.zSizeMax <= 30;
}

bool View3D::solidType(const TypeArt& art)
{
    // (Not what animates, a brazier's flames: its frames differ in size, and a box cut them.)
    const bool animated = art.draw.kind == nox::things::DrawKind::Animate ||
                          art.draw.kind == nox::things::DrawKind::Conditional ||
                          art.draw.kind == nox::things::DrawKind::AnimateState;
    return art.extentBox && (art.classes & nox::things::ObjectClass::Immobile) && !art.hanging && !animated &&
           art.extentWidth >= 8 && art.extentHeight >= 8 && !floorType(art);
}

bool View3D::floorType(const TypeArt& art)
{
    // BELOW things, and what only looks raised from above: shadows (a tent's, a rock's) and a
    // bridge's raised pieces, whose deck is the floor's own tiles: drawn into the floor where the
    // 2D view shows them.
    return art.below || art.nameShadowOrBridge;
}

void View3D::addCanopy(const Sprite& s, float sx, float sy, float raised, const LightMap* light)
{
    // A roof (a trader's tent top, drawn `raised` up): its picture laid on a level at canopy
    // height over the ground it covers in the 2D view (a pixel at row y over ground y + raised).
    constexpr float kCanopyHeight = 58;
    const SDL_FPoint corners[4] = {{sx, sy + raised}, {sx + s.width, sy + raised}, {sx + s.width, sy + s.height + raised},
                                   {sx, sy + s.height + raised}};
    addCap(s, sx, sy, corners, 4, kCanopyHeight, light, raised / kCanopyHeight);
}

void View3D::addThings(const Frame& f)
{
    namespace oc = nox::things::ObjectClass;
    const auto lightAt = [&](float x, float y) { return f.light ? f.light->sample(x, y) : Rgb{255, 255, 255}; };
    const float tanPitch = std::tan(pitch_);
    const float cx = centreX_;
    const float cy = static_cast<float>(viewH_) / 2;
    // The crosshair on a billboard: the thing's foot is the cursor (as clicking its foot circle).
    // Hit on its picture's solid pixels (a clear corner of a tree or a creature doesn't hide what's
    // behind it). An item on a box round its solid pixels instead, padded (small things are hard
    // to put a crosshair on): the padding in world units, so the box shrinks with distance as the
    // item does, never under a few pixels each way; and down to the floor.
    const auto pickBoard = [&](std::size_t board, float bx, float by, std::int32_t image = -1, bool item = false) {
        if (board >= boards_.size()) {
            return;
        }
        const auto& b = boards_[board];
        const float depth = toCam(bx, by, 0).depth;
        // See-through boards (alpha under 128) aren't hit, nor anything behind a nearer hit.
        if (b.alpha < 128 || depth >= pickDepth_ || b.scale <= 0) {
            return;
        }
        if (item) {
            constexpr float kItemPad = 12; // world units each way
            const SDL_Rect seen = image >= 0 ? opaqueBounds(f, image) : SDL_Rect{0, 0, 0, 0};
            SDL_FRect box = b.dst;
            if (seen.w > 0 && seen.h > 0) {
                box = {b.dst.x + static_cast<float>(seen.x) * b.scale, b.dst.y + static_cast<float>(seen.y) * b.scale,
                       static_cast<float>(seen.w) * b.scale, static_cast<float>(seen.h) * b.scale};
            }
            const float pad = std::max(kItemPad * b.scale, 6.0f);
            const float floorY = horizon_ + eyeZ_ * focal_ / depth; // (the floor under its foot, on screen)
            const float bottom = std::max(box.y + box.h, floorY);
            if (cx < box.x - pad || cx >= box.x + box.w + pad || cy < box.y - pad || cy >= bottom + pad) {
                return;
            }
        } else {
            if (cx < b.dst.x || cx >= b.dst.x + b.dst.w || cy < b.dst.y || cy >= b.dst.y + b.dst.h) {
                return;
            }
            if (const auto* d = image >= 0 ? f.sprites.decoded(image) : nullptr; d && d->width > 0) {
                const int x = static_cast<int>((cx - b.dst.x) / b.scale);
                const int y = static_cast<int>((cy - b.dst.y) / b.scale);
                if (x < 0 || y < 0 || x >= d->width || y >= d->height ||
                    d->rgba[(static_cast<std::size_t>(y) * d->width + x) * 4 + 3] <= 16) {
                    return; // (a clear pixel)
                }
            }
        }
        pickDepth_ = depth;
        pick_ = SDL_FPoint{bx, by};
        pickThing_ = true;
    };

    static bool listed = false;
    if (!listed && debugLog()) {
        listed = true;
        std::set<std::string> seen;
        for (const auto& o : f.view.objects_) {
            if (o.art && (std::hypot(o.x - eyeX_, o.y - eyeY_) < 400 || o.art->z >= 8) && seen.insert(o.art->name).second) {
                SDL_Log("view3d: object %s at %.0f,%.0f: %s %.0fx%.0f r%.0f z %.0f-%.0f Z %.0f classes %08X hanging %d",
                        o.art->name.c_str(), o.x, o.y, o.art->extentBox ? "box" : "circle", o.art->extentWidth,
                        o.art->extentHeight, o.art->extentRadius, o.art->zSizeMin, o.art->zSizeMax, o.art->z, o.art->classes,
                        o.art->hanging ? 1 : 0);
            }
        }
    }
    for (const auto& o : f.view.objects_) {
        if (!o.art || floorType(*o.art) || o.plate) {
            continue; // (in the floor)
        }
        if ((o.art->classes & oc::Fire) && !o.enabled) {
            continue; // an unlit candle
        }
        // Solid things (IMMOBILE with a box extent) are boxes, from the map's copy even when
        // the server sends one too (its image kept current by setStatics); the live one isn't drawn.
        const bool solid = solidType(*o.art) && !o.doorAngle;
        // Things on walls (hangings: shields, banners, torches; switches, levers, buttons:
        // IMMOBILE USEABLE) lie on the wall's face, likewise from the map's copy.
        constexpr std::uint32_t kUseable = 0x80;
        const bool onWall = o.art->hanging || ((o.art->classes & oc::Immobile) && (o.art->subclasses & kUseable));
        // What doesn't move or turn (not a creature, not a door) is drawn from the map's copy,
        // a box with its picture wrapped toward us (addSolid); the server's copy, if it sends one, isn't drawn.
        const bool still = !o.doorAngle && !(o.art->classes & (oc::Monster | oc::Player));
        if (f.liveCodes && f.liveCodes->contains(o.extent) && !solid && !onWall && !still) {
            continue; // the server's copy is in `extra`
        }
        if (std::hypot(o.x - eyeX_, o.y - eyeY_) > kDrawDistance) {
            continue;
        }
        const auto image = mapObjectImage(*o.art, o.extent, o.doorAngle, o.frame, o.enabled, o.xStatus, o.xStatusSince, f.time);
        if (!image) {
            continue;
        }
        const Sprite* s = o.colours ? f.sprites.tinted(*image, *o.colours) : f.sprites.get(*image);
        if (!s) {
            continue;
        }
        const bool selfLit = (o.art->classes & oc::Light) && o.enabled;
        const Rgb light = selfLit ? Rgb{255, 255, 255} : lightAt(o.x, o.y);
        const float sx = o.x - o.art->halfWidth + s->offsetX;
        const float sy = o.y - o.art->halfHeight - o.art->z + s->offsetY;
        if (solid && o.art->z >= 60) {
            addCanopy(*s, sx, sy, o.art->z, selfLit ? nullptr : f.light); // (a roof: a tent's top)
            solidSpots_.push_back({o.x, o.y});
            continue;
        }
        if (o.doorAngle) {
            // A door: a slab along its leaf like a wall's piece (both sides and its ends), textured
            // from its sprite as walls are and raised with them, so it fills its doorway. A leaf
            // running nearly north-south shows as a line in the oblique view: a billboard.
            const SDL_Point leaf = doorLeaf(*o.doorAngle);
            if (std::abs(leaf.x) >= 10) {  // (a leaf under 10 across: a billboard, below)
                const float bx = o.x + static_cast<float>(leaf.x);
                const float by = o.y + static_cast<float>(leaf.y);
                // Every door as tall as the walls (raised with them), its picture fitted to that
                // from the leaf's back end up to its top opaque row: so a pair's two leaves (their
                // images' margins differ) always match.
                const SDL_Rect seen = opaqueBounds(f, f.sprites.indexOf(*image));
                const float opaqueTop = sy + static_cast<float>(seen.h > 0 ? seen.y : 0);
                const float pictured = std::clamp(std::min(o.y, by) - opaqueTop, 20.0f, 150.0f);
                constexpr float kDoorPictured = 58; // (as the walls about it)
                const float height = kDoorPictured * (1 + wallRaise_);
                const float fit = pictured / height;
                const float len = std::hypot(bx - o.x, by - o.y);
                const float half = 2; // (a door's thickness, half)
                const float nx = -(by - o.y) / len * half;
                const float ny = (bx - o.x) / len * half;
                const LightMap* lit = f.light;
                addFace(*s, sx, sy, o.x - nx, o.y - ny, bx - nx, by - ny, o.x, o.y, bx, by, 0, height, lit, false, fit);
                addFace(*s, sx, sy, bx + nx, by + ny, o.x + nx, o.y + ny, bx, by, o.x, o.y, 0, height, lit, false, fit);
                addFace(*s, sx, sy, bx - nx, by - ny, bx + nx, by + ny, bx, by, bx, by, 0, height, lit, false, fit);
                addFace(*s, sx, sy, o.x + nx, o.y + ny, o.x - nx, o.y - ny, o.x, o.y, o.x, o.y, 0, height, lit, false, fit);
                // The crosshair on the leaf: like a wall, a ground point there, not a thing.
                if (const auto hit = crosshairCross(o.x, o.y, bx, by)) {
                    const auto [t, u] = *hit;
                    const float z = eyeZ_ + t * tanPitch;
                    if (t > kNear && t < pickDepth_ && u >= 0 && u <= 1 && z >= 0 && z <= height) {
                        pickDepth_ = t;
                        pick_ = SDL_FPoint{o.x + (bx - o.x) * u, o.y + (by - o.y) * u};
                        pickThing_ = false;
                    }
                }
                continue;
            }
        }
        if (onWall && addOnWall(f, *s, sx, sy, o, selfLit ? nullptr : f.light)) {
            solidSpots_.push_back({o.x, o.y}); // (the server's copy not drawn too)
            continue;
        }
        if (still && o.art->nameObelisk) {
            // Obelisks: a box, their picture wrapped toward us. Everything else still is a
            // billboard (made shapes didn't fit how the art lays things out).
            addSolid(f, *s, f.sprites.indexOf(*image), sx, sy, *o.art, o.x, o.y, selfLit ? nullptr : f.light);
            solidSpots_.push_back({o.x, o.y}); // (the server's copy not drawn too)
            continue;
        }
        // Else a billboard (a hanging with no wall by it drawn just in front of where it'd be); an
        // item on the ground stands upright on its lowest solid pixel, unbent (as Doom's pickups).
        const std::size_t before = boards_.size();
        addBoard(*s, sx, sy, o.x, itemType(*o.art) ? standingBase(f, f.sprites.indexOf(*image), sy, s->height) : o.y, light, 255,
                 o.art->hanging ? 8.0f : 0.0f);
        if (boards_.size() > before) {
            pickBoard(before, o.x, o.y, f.sprites.indexOf(*image), itemType(*o.art));
        }
        if (f.liveCodes && f.liveCodes->contains(o.extent)) {
            solidSpots_.push_back({o.x, o.y});
        }
    }

    // Solid things the server sends (an obelisk the map's copy isn't drawn for) as boxes too,
    // from the drawable: its image as the 2D view picks it (an obelisk's charge, SlaveDraw).
    if (f.client && f.art) {
        for (const auto& [netCode, d] : f.client->world().drawables()) {
            const TypeArt* a = f.art->get(d.typeId);
            if (!a || !d.enabled || a->editorOnly || !solidType(*a)) {
                continue;
            }
            if (a->z < 60 && !a->nameObelisk) {
                continue; // (a billboard, from its scene sprite)
            }
            const auto x = static_cast<float>(d.x);
            const auto y = static_cast<float>(d.y);
            if (std::hypot(x - eyeX_, y - eyeY_) > kDrawDistance ||
                std::ranges::any_of(solidSpots_, [&](const SDL_FPoint& p) { return std::abs(p.x - x) < 3 && std::abs(p.y - y) < 3; })) {
                continue; // (far, or the map's copy is drawn)
            }
            if (sight_ && (offView(x, y) || // (Frame::sight: where the 2D view doesn't show it)
                           !sight_->drawableSeen(*a, SightMap::liveKey(netCode), x, y, static_cast<float>(d.z), 0, {}))) {
                continue;
            }
            Pose pose;
            pose.direction = d.direction;
            pose.animState = d.animState;
            pose.variant = d.frame.value_or(static_cast<std::uint8_t>(netCode));
            pose.netCode = netCode;
            pose.time = f.time;
            pose.enabled = d.enabled;
            pose.xStatus = d.xStatus;
            const auto image = pickImage(*a, pose);
            const Sprite* s = image ? f.sprites.get(*image) : nullptr;
            if (!s) {
                continue;
            }
            const bool selfLit = (a->classes & oc::Light) != 0;
            const float sx = x - a->halfWidth + s->offsetX;
            const float sy = y - a->halfHeight - a->z + s->offsetY;
            if (a->z >= 60) {
                addCanopy(*s, sx, sy, a->z, selfLit ? nullptr : f.light);
                solidSpots_.push_back({x, y});
                continue;
            }
            addSolid(f, *s, f.sprites.indexOf(*image), sx, sy, *a, x, y, selfLit ? nullptr : f.light);
        }
    }
    // The items the server sends (their scene sprites stand like the map's items): where they
    // lie, and their image's bottom.
    struct LiveItem {
        float x, y;
        std::int32_t image;
    };
    std::vector<LiveItem> items;
    if (f.client && f.art) {
        for (const auto& [netCode, d] : f.client->world().drawables()) {
            const TypeArt* a = f.art->get(d.typeId);
            if (!a || !d.enabled || !itemType(*a)) {
                continue;
            }
            Pose pose;
            pose.direction = d.direction;
            pose.variant = d.frame.value_or(static_cast<std::uint8_t>(netCode));
            pose.netCode = netCode;
            pose.time = f.time;
            if (const auto image = pickImage(*a, pose)) {
                items.push_back({static_cast<float>(d.x), static_cast<float>(d.y), f.sprites.indexOf(*image)});
            }
        }
    }
    for (const auto& e : f.extra) {
        if (!e.sprite || e.sortY < e.lightY - MapView::kBelow / 2) {
            continue; // (drawn otherwise, or in the floor)
        }
        if (unseen(e)) {
            continue; // (Frame::sight: the 2D view hides it)
        }
        if (f.client && std::hypot(e.lightX - eyeX_, e.lightY - eyeY_) < 12) {
            continue; // ourselves (and what's drawn on us): we're the eye
        }
        if (std::ranges::any_of(solidSpots_, [&](const SDL_FPoint& p) {
                return std::abs(p.x - e.lightX) < 3 && std::abs(p.y - e.lightY) < 3;
            })) {
            continue; // the server's copy of a solid drawn as a box
        }
        const float sy = e.y + e.sprite->offsetY;
        float base = e.lightY;
        std::int32_t itemImage = -1;
        // A live item (its drawable within 2 units) stands on its image's lowest solid row.
        if (const auto item = std::ranges::find_if(items, [&](const LiveItem& i) {
                return std::abs(i.x - e.lightX) < 2 && std::abs(i.y - e.lightY) < 2;
            });
            item != items.end()) {
            base = standingBase(f, item->image, sy, e.sprite->height);
            itemImage = item->image;
        }
        const std::size_t before = boards_.size();
        addBoard(*e.sprite, e.x + e.sprite->offsetX, sy, e.lightX, base, e.light ? *e.light : lightAt(e.lightX, e.lightY),
                 e.alpha, 0);
        if (boards_.size() > before && !e.bloom) {
            pickBoard(before, e.lightX, e.lightY, itemImage, itemImage >= 0); // (not missiles and other lights)
        }
    }
}

void View3D::drawHeld(const Frame& f)
{
    // What we hold, in the corners of the view (Minecraft style): the weapon on the right, a
    // shield on the left, as their pictures on the ground; bobbing as we walk, the weapon
    // swinging while Action is held.
    if (!f.client || !f.art || !f.modifiers) {
        return;
    }
    const nox::client::InventoryItem* weapon = nullptr;
    const nox::client::InventoryItem* shield = nullptr;
    const TypeArt* weaponArt = nullptr;
    const TypeArt* shieldArt = nullptr;
    // The first equipped weapon, and a shield, from our inventory.
    for (const auto& [netCode, item] : f.client->world().self().inventory) {
        if (!item.equipped) {
            continue;
        }
        const TypeArt* a = f.art->get(item.typeId);
        if (!a || !a->still) {
            continue;
        }
        if (a->itemColours == ItemColours::Weapon && !weapon) {
            weapon = &item;
            weaponArt = a;
        } else if (a->itemColours == ItemColours::Armor && a->nameShield) {
            shield = &item;
            shieldArt = a;
        }
    }
    // The swing: half a sine over kSwing seconds, restarted while Action stays held.
    const double now = static_cast<double>(SDL_GetTicksNS()) / 1e9;
    constexpr double kSwing = 0.42;
    if (f.attacking && (swingStart_ < 0 || now - swingStart_ > kSwing)) {
        swingStart_ = now;
    } else if (!f.attacking && swingStart_ >= 0 && now - swingStart_ > kSwing) {
        swingStart_ = -1;
    }
    const float swing = swingStart_ >= 0 ? static_cast<float>(std::sin((now - swingStart_) / kSwing * std::numbers::pi)) : 0.0f;
    const float w = static_cast<float>(viewW_);
    const float h = static_cast<float>(viewH_);
    // The walk bob: side to side and up, by the distance walked (walkPhase_).
    const float bobX = std::cos(walkPhase_) * w * 0.008f;
    const float bobY = std::abs(std::sin(walkPhase_)) * h * 0.018f;
    // Lit as the ground at the eye, never under 70 a channel.
    Rgb light = f.light ? f.light->sample(eyeX_, eyeY_) : Rgb{255, 255, 255};
    for (auto& c : light) {
        c = std::max<std::uint8_t>(c, 70);
    }

    // Each is drawn into a square of its own (its colours through SpriteCache::render, the
    // material shader's), then onto the screen turned: the ground pictures lie at an angle.
    constexpr int kHeld = 256;
    if (!heldTarget_) {
        heldTarget_ = SDL_CreateTexture(f.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, kHeld, kHeld);
        if (!heldTarget_) {
            return;
        }
        SDL_SetTextureBlendMode(heldTarget_, SDL_BLENDMODE_BLEND);
    }
    const auto draw = [&](const nox::client::InventoryItem& item, const TypeArt& a, float cxFrac, float cyFrac, float size,
                          float dx, float dy, double angle) {
        const auto materials = itemMaterials(a, &item.modifiers, *f.modifiers);
        const Sprite* s = materials ? f.sprites.tinted(*a.still, *materials) : f.sprites.get(*a.still);
        if (!s || s->width <= 0 || s->height <= 0) {
            return;
        }
        // Fitted and centred in the square (its colours through SpriteCache::render).
        const float fit = static_cast<float>(kHeld) / std::max(s->width, s->height);
        SDL_Texture* const previous = SDL_GetRenderTarget(f.renderer);
        SDL_SetRenderTarget(f.renderer, heldTarget_);
        SDL_SetRenderDrawColor(f.renderer, 0, 0, 0, 0);
        SDL_RenderClear(f.renderer);
        s->scale = SDL_SCALEMODE_LINEAR;
        const SDL_FRect inside{(kHeld - s->width * fit) / 2, (kHeld - s->height * fit) / 2, s->width * fit, s->height * fit};
        const SDL_FRect src{0, 0, s->imageWidth(), s->rect.h};
        f.sprites.render(*s, &src, inside);
        SDL_SetRenderTarget(f.renderer, previous);

        // On the screen: a square `size` of the view's height, centred at the given fractions of the view.
        const float side = h * size;
        const SDL_FRect dst{w * cxFrac - side / 2 + dx, h * cyFrac - side / 2 + dy, side, side};
        SDL_SetTextureColorMod(heldTarget_, light[0], light[1], light[2]);
        SDL_SetTextureScaleMode(heldTarget_, SDL_SCALEMODE_LINEAR);
        SDL_RenderTextureRotated(f.renderer, heldTarget_, nullptr, &dst, angle, nullptr, SDL_FLIP_NONE);
    };
    if (shield) {
        draw(*shield, *shieldArt, 0.15f, 0.86f, 0.40f, -bobX, bobY, 0);
    }
    if (weapon) {
        // Turned upright (the ground picture lies blade down-right); a swing sweeps it across.
        draw(*weapon, *weaponArt, 0.80f, 0.80f, 0.55f, bobX - swing * w * 0.16f, bobY - swing * h * 0.06f,
             kHeldAngle - swing * 70.0);
    }
}

bool View3D::drawDepth(const Frame& f)
{
    // Camera space to clip space: across by the focal length; up sheared by the horizon (the
    // look up and down); depth into 0-1 between the near plane and far.
    constexpr float kFar = 4000;
    const float xs = focal_ / (static_cast<float>(viewW_) / 2);
    const float ya = 1 - 2 * horizon_ / static_cast<float>(viewH_);
    const float yb = 2 * focal_ / static_cast<float>(viewH_);
    const float za = kFar / (kFar - kNear);
    const float zb = -kNear * kFar / (kFar - kNear);
    gpuVerts_.resize(tris_.size());
    // Each vertex: clip x and y, depth mapped into 0-1, w the camera depth (for the perspective
    // divide), then its texture coordinates and colour.
    for (std::size_t i = 0; i < tris_.size(); ++i) {
        const ClipVertex& v = tris_[i];
        gpuVerts_[i] = {v.p.x * xs, v.p.depth * ya + v.p.up * yb, v.p.depth * za + zb, v.p.depth, v.u, v.v,
                        v.c.r, v.c.g, v.c.b, v.c.a};
    }
    gpuDraws_.clear();
    for (const auto& item : items_) {
        if (item.board) {
            continue; // (not made with the depth pass up)
        }
        const Batch& b = batches_[static_cast<std::size_t>(item.index)];
        if (b.count == 0 || (!b.texture && b.kind == DepthPass::Kind::Scene)) {
            continue;
        }
        if (b.sprite) {
            b.sprite->ready(); // (an atlas page's new images uploaded before our pass)
        }
        gpuDraws_.push_back({b.texture, b.sprite, b.first, b.count, b.alpha < 255, b.linear, item.depth, b.kind});
    }
    SDL_Texture* scene = depth_->render(f.renderer, viewW_, viewH_, gpuVerts_, gpuDraws_);
    if (!scene) {
        return false;
    }
    const SDL_FRect all{0, 0, static_cast<float>(viewW_), static_cast<float>(viewH_)};
    SDL_RenderTexture(f.renderer, scene, nullptr, &all);
    return true;
}

void View3D::drawSorted(const Frame& f)
{
    // Back to front (painter's: no depth buffer); runs of geometry with one texture in one call.
    std::ranges::stable_sort(items_, [](const Item& a, const Item& b) { return a.depth > b.depth; });
    SDL_Texture* runTexture = nullptr;
    const auto flushRun = [&] {
        if (!runVerts_.empty()) {
            SDL_RenderGeometry(f.renderer, runTexture, runVerts_.data(), static_cast<int>(runVerts_.size()), nullptr, 0);
        }
        runVerts_.clear();
    };
    for (const auto& item : items_) {
        if (item.board) {
            flushRun();
            runTexture = nullptr;
            const Board& b = boards_[static_cast<std::size_t>(item.index)];
            const Sprite& s = *b.sprite;
            s.scale = s.hd ? SDL_SCALEMODE_LINEAR : (b.scale >= 1 ? SDL_SCALEMODE_PIXELART : SDL_SCALEMODE_LINEAR);
            SDL_SetTextureColorMod(s.texture, b.light[0], b.light[1], b.light[2]);
            SDL_SetTextureAlphaMod(s.texture, b.alpha);
            const SDL_FRect src{0, 0, s.imageWidth(), s.rect.h};
            f.sprites.render(s, &src, b.dst);
            SDL_SetTextureColorMod(s.texture, 255, 255, 255);
            SDL_SetTextureAlphaMod(s.texture, 255);
            continue;
        }
        const Batch& b = batches_[static_cast<std::size_t>(item.index)];
        if (b.count == 0 || b.kind != DepthPass::Kind::Scene) {
            continue; // (particles: the depth pass's only)
        }
        if (b.texture != runTexture) {
            flushRun();
            runTexture = b.texture;
            if (b.sprite) {
                b.sprite->ready(); // (an atlas page's new images uploaded)
            }
            SDL_SetTextureScaleMode(runTexture, SDL_SCALEMODE_PIXELART);
        }
        // Each triangle clipped to the near plane, then projected; a clipped one (4 corners) as a fan.
        for (int i = 0; i < b.count; i += 3) {
            ClipVertex out[4];
            const int m = clipTriangle(&tris_[static_cast<std::size_t>(b.first + i)], out);
            for (int k = 1; k + 1 < m; ++k) {
                for (const int j : {0, k, k + 1}) {
                    runVerts_.push_back({project(out[j].p), out[j].c, {out[j].u, out[j].v}});
                }
            }
        }
    }
    flushRun();
}

bool View3D::offView(float x, float y) const
{
    constexpr float kSide = 16;   // a picture's half width past the sides
    constexpr float kBelow = 64;  // a picture rising from below the bottom edge
    return x < seenLeft_ - kSide || x > seenRight_ + kSide || y < seenTop_ || y > seenBottom_ + kBelow;
}

bool View3D::unseen(float x, float y) const
{
    return sight_ && (offView(x, y) || !sight_->visible(x, y));
}

bool View3D::unseen(const MapView::SceneSprite& e) const
{
    if (!sight_ || e.fadeSelf) {
        return false; // (our own unit's: always in sight)
    }
    if (offView(e.lightX, e.lightY)) {
        return true;
    }
    if (e.sightArt) {
        return !sight_->drawableSeen(*e.sightArt, e.sightKey, e.lightX, e.lightY, e.sightZ, e.sightDoorAngle,
                                     e.drawnImage ? *e.drawnImage : nox::client::DrawnImage{});
    }
    return !sight_->visible(e.lightX, e.lightY);
}

void View3D::draw(const Frame& f)
{
    viewW_ = f.camera.viewWidth;
    viewH_ = f.camera.viewHeight;
    // A game's sight (Frame::sight): what the 2D view shows, its rectangle and its sight test.
    sight_ = f.client ? f.sight : nullptr;
    seenLeft_ = f.camera.left();
    seenTop_ = f.camera.top();
    seenRight_ = f.camera.right();
    seenBottom_ = f.camera.bottom();
    // The frame's time (capped at 0.1 s), for the viewer's flight.
    const std::uint64_t nowNs = SDL_GetTicksNS();
    const float dt = lastNs_ ? std::min(static_cast<float>(nowNs - lastNs_) / 1e9f, 0.1f) : 0.0f;
    lastNs_ = nowNs;
    if (!f.client) {
        // The map viewer: the arrows fly the eye about.
        const float forward = static_cast<float>((keyUp_ ? 1 : 0) - (keyDown_ ? 1 : 0));
        const float side = static_cast<float>((keyRight_ ? 1 : 0) - (keyLeft_ ? 1 : 0));
        f.camera.x += (std::cos(yaw_) * forward - std::sin(yaw_) * side) * kWalkSpeed * dt;
        f.camera.y += (std::sin(yaw_) * forward + std::cos(yaw_) * side) * kWalkSpeed * dt;
    }
    // The held weapon bobs by how far we moved (a jump of 50 or more is a teleport, not a walk).
    const float moved = std::hypot(f.camera.x - eyeX_, f.camera.y - eyeY_);
    if (moved < 50) {
        walkPhase_ += moved * 0.07f;
    }
    eyeX_ = f.camera.x;
    eyeY_ = f.camera.y;
    eyeZ_ = kEyeHeight;
    playerX_ = eyeX_;
    playerY_ = eyeY_;
    if (f.client) {
        if (const auto at = f.client->position()) {
            playerX_ = static_cast<float>(at->first);
            playerY_ = static_cast<float>(at->second);
        }
    }
    // This frame's view: its rotation, the focal length for the field of view, and the horizon moved
    // by the pitch.
    cos_ = std::cos(yaw_);
    sin_ = std::sin(yaw_);
    focal_ = static_cast<float>(viewW_) / 2 / std::tan(kHalfFov);
    centreX_ = static_cast<float>(viewW_) / 2;
    horizon_ = static_cast<float>(viewH_) / 2 + std::tan(pitch_) * focal_;

    f.view.setTime(f.time);
    f.view.setCollectBloom(false);
    // The depth pass made once, on the first frame (NOXSDL_VIEW3D_NO_DEPTH keeps the fallback).
    if (!depthTried_) {
        depthTried_ = true;
        if (!SDL_getenv("NOXSDL_VIEW3D_NO_DEPTH")) {
            depth_ = DepthPass::create(f.renderer);
        }
    }
    // This frame's geometry, built afresh.
    tris_.clear();
    batches_.clear();
    boards_.clear();
    items_.clear();
    solidSpots_.clear();

    // The pick: the nearest wall, door or billboard on the crosshair. Not the floor: everything
    // standing on it is drawn over it, and a wall's hit is in front of the floor's by its height.
    pickDepth_ = kDrawDistance;
    pick_.reset();
    pickThing_ = false;

    drawFloorTexture(f);
    addFloor(f);
    addWalls(f);
    addFacades(f); // (stood up along the walls' lines)
    addThings(f);
    addedQuads_.clear();
    blendedQuads_.clear();
    if (depth_) {
        if (f.effects) {
            addEffects(f);
        }
        addRing(); // (with them: drawn over the world's floor, behind what stands in front of it)
        // Two batches: the blended far to near, the added in any order (the depth pass draws
        // them after the world).
        std::ranges::sort(blendedQuads_, [](const ParticleQuad& a, const ParticleQuad& b) { return a.depth > b.depth; });
        for (const auto& [quads, kind] : {std::pair{&blendedQuads_, DepthPass::Kind::ParticlesBlended},
                                         std::pair{&addedQuads_, DepthPass::Kind::ParticlesAdded}}) {
            Batch batch{nullptr, nullptr, static_cast<int>(tris_.size()), 0, 255, false, kind};
            for (const auto& q : *quads) {
                emit(batch, q.v, 4);
            }
            if (batch.count > 0) {
                items_.push_back({0, false, static_cast<int>(batches_.size())});
                batches_.push_back(batch);
            }
        }
    }

    // The depth pass, else (no GPU pass, or it failed) the painter's fallback.
    if (!depth_ || !drawDepth(f)) {
        drawSorted(f);
    }

    // The mouse's target: a thing the pick is on (its foot).
    // Else the ground straight ahead where the crosshair meets it (looking down), else straight
    // ahead, flat, at a run's distance. A ground point is laid along our facing from where the
    // server has us, at its distance from the eye, so the cursor's way from us is exactly the
    // crosshair's (the game turns us to the cursor as the 2D view's mouse does).
    targetThing_ = pick_ && pickThing_;
    if (!targetThing_) {
        float t = 300;
        if (pick_) {
            t = std::hypot(pick_->x - eyeX_, pick_->y - eyeY_);
        } else if (const float tanPitch = std::tan(pitch_); tanPitch < -1e-3f) {
            t = std::min(eyeZ_ / -tanPitch, kDrawDistance);
        }
        pick_ = SDL_FPoint{playerX_ + cos_ * t, playerY_ + sin_ * t};
    }
    target_ = pick_;
    if (debugLog()) {
        static std::uint64_t lastLog = 0;
        if (SDL_GetTicks() - lastLog > 1000) {
            lastLog = SDL_GetTicks();
            SDL_Log("view3d: pick %s at %.0f,%.0f", targetThing_ ? "thing" : "ground", pick_->x, pick_->y);
        }
    }

    drawHeld(f);
}

} // namespace noxsdl
