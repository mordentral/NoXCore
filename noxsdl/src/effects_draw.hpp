// The game's drawing primitives for effects, in world units so they scale with the camera:
// one-pixel lines, the point shapes, and the additive glow disc stamped singly or along a line.
// Internal to the effects sources.
//
// Smooth drawing (the default with the GPU renderer's shaders, gpu_shaders.hpp): lines, dots
// and squares keep the game's sizes, colours and blend, but are drawn with anti-aliased edges at
// the screen's resolution and at their exact (unrounded) positions, instead of as world pixels
// scaled up into blocks. The game's round point shapes become discs of the same diameter; its
// square ones stay square.
#pragma once

#include "camera.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace noxsdl::fxdraw {

/// What smooth drawing needs: a white pixel and the fx_disc / fx_rect render states. All null
/// (the stock pixel shapes) unless main sets them.
struct Smooth {
    SDL_Texture* white = nullptr;
    SDL_GPURenderState* disc = nullptr;
    SDL_GPURenderState* rect = nullptr;
    SDL_GPURenderState* glow = nullptr; ///< fx_glow: glows worked out per pixel (may be null)
    SDL_GPURenderState* puff = nullptr; ///< fx_puff: the smoke blast's soft dust (may be null)
    SDL_GPURenderState* glowLine = nullptr; ///< fx_glowline: a line of glows in one quad (may be null)
};
/// The one shared set of smooth-drawing states.
inline Smooth& smooth()
{
    static Smooth s;
    return s;
}
/// Whether smooth drawing is on: the white pixel and the disc and rect states are all set.
inline bool smoothOn()
{
    const Smooth& s = smooth();
    return s.white && s.disc && s.rect;
}

/// HD Spell FX (Options, apart from Smooth FX): the spell effects drawn as noxsdl's own shader
/// effects in place of the game's sprites: the toxic cloud's puffs (fx_puff), the Force Field's
/// sphere (fx_barrier) and the Reflective Shield's water disc (fx_ripple). All null (the game's
/// sprites) unless main sets them.
struct SpellFx {
    SDL_Texture* white = nullptr;
    SDL_GPURenderState* puff = nullptr;
    SDL_GPURenderState* barrier = nullptr;
    SDL_GPURenderState* ripple = nullptr;
};
/// The one shared set of HD Spell FX states.
inline SpellFx& spellFx()
{
    static SpellFx s;
    return s;
}
/// Whether HD Spell FX is on (its white pixel set).
inline bool spellFxOn()
{
    return spellFx().white != nullptr;
}

namespace detail {

// World units to screen pixels.
inline SDL_FPoint toScreen(const Camera& camera, float x, float y)
{
    return {(x - camera.left()) * camera.zoom, (y - camera.top()) * camera.zoom};
}

/// A quad about `centre` (screen pixels) with half-extents `along` and `across` on the unit axes
/// `ax` / `perp`, through `state`: the shape's edge is at the half-extents, the quad a pixel and
/// a half beyond for the smoothed edge.
/// Quads gathered to go in one draw (one render state, one blend): many small shapes drawn one
/// call each stop the renderer batching them (a chain lightning's glow is thousands of discs).
struct QuadBatch {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    /// A quad's corners about `centre` (screen pixels) with half-extents `along` and `across` on
    /// the unit axes `ax` / `perp`: the shape's edge is at the half-extents, the quad a pixel and
    /// a half beyond for the smoothed edge.
    static std::array<SDL_Vertex, 4> corners(SDL_FPoint centre, SDL_FPoint ax, float along, float across, SDL_Color color)
    {
        constexpr float kMargin = 1.5f;
        along = std::max(along, 0.01f);
        across = std::max(across, 0.01f);
        const float ea = along + kMargin;
        const float ec = across + kMargin;
        const SDL_FPoint perp{-ax.y, ax.x};
        const float ua = 0.5f * ea / along;
        const float uc = 0.5f * ec / across;
        const SDL_FColor c{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
        const auto corner = [&](float sa, float sc) {
            return SDL_Vertex{{centre.x + ax.x * ea * sa + perp.x * ec * sc, centre.y + ax.y * ea * sa + perp.y * ec * sc},
                              c,
                              {0.5f + ua * sa, 0.5f + uc * sc}};
        };
        return {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)};
    }

    /// The quad `corners` makes, added.
    void add(SDL_FPoint centre, SDL_FPoint ax, float along, float across, SDL_Color color)
    {
        const auto v = corners(centre, ax, along, across, color);
        const int base = static_cast<int>(vertices.size());
        vertices.insert(vertices.end(), v.begin(), v.end());
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    /// `vertices` / `indices` through `state`, with the renderer's draw blend mode (or `forceBlend`).
    static void drawGeometry(SDL_Renderer* renderer, SDL_GPURenderState* state, SDL_BlendMode forceBlend,
                             const SDL_Vertex* vertices, int vertexCount, const int* indices, int indexCount)
    {
        const Smooth& s = smooth();
        SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
        SDL_GetRenderDrawBlendMode(renderer, &blend);
        if (forceBlend != SDL_BLENDMODE_INVALID) {
            blend = forceBlend;
        }
        SDL_SetTextureBlendMode(s.white, blend == SDL_BLENDMODE_NONE ? SDL_BLENDMODE_BLEND : blend);
        SDL_SetGPURenderState(renderer, state);
        SDL_RenderGeometry(renderer, s.white, vertices, vertexCount, indices, indexCount);
        SDL_SetGPURenderState(renderer, nullptr);
    }

    /// All of them through `state`, with the renderer's draw blend mode (or `forceBlend`).
    void draw(SDL_Renderer* renderer, SDL_GPURenderState* state, SDL_BlendMode forceBlend = SDL_BLENDMODE_INVALID)
    {
        if (vertices.empty()) {
            return;
        }
        drawGeometry(renderer, state, forceBlend, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
                     static_cast<int>(indices.size()));
        vertices.clear();
        indices.clear();
    }
};

// One quad drawn on its own (as a QuadBatch of one would, without its storage).
inline void quad(SDL_Renderer* renderer, SDL_GPURenderState* state, SDL_FPoint centre, SDL_FPoint ax, float along,
                 float across, SDL_Color color, SDL_BlendMode forceBlend = SDL_BLENDMODE_INVALID)
{
    const auto v = QuadBatch::corners(centre, ax, along, across, color);
    static constexpr int kIndices[6] = {0, 1, 2, 0, 2, 3};
    QuadBatch::drawGeometry(renderer, state, forceBlend, v.data(), 4, kIndices, 6);
}

/// A batch kept for the one-off draws below (cleared by its draw, its storage kept): drawing
/// is on the one thread, and no draw starts another while it fills.
template <class Batch>
Batch& scratch()
{
    static Batch b;
    return b;
}

// The renderer's current draw colour.
inline SDL_Color drawColor(SDL_Renderer* renderer)
{
    SDL_Color c{255, 255, 255, 255};
    SDL_GetRenderDrawColor(renderer, &c.r, &c.g, &c.b, &c.a);
    return c;
}

/// drawPointMB's shape of `size` at (x, y) as smooth drawing makes it: sizes 1 and 2 (squares)
/// stay squares, `square(cx, cy, side)`; the round ones become discs of the same diameter and
/// centre, `disc(cx, cy, diameter)`. The discs are centred where the classic blobs are: odd sizes
/// on the pixel's centre (+0.5), even ones on its far corner (+1).
template <class Square, class Disc>
void pointShape(float x, float y, int size, Square&& square, Disc&& disc)
{
    switch (size) {
    case 0:
    case 1:
        square(x + 0.5f, y + 0.5f, 1.0f);
        return;
    case 2:
        square(x + 1, y + 1, 2.0f);
        return;
    case 4:
    case 6:
        disc(x + 1, y + 1, static_cast<float>(size));
        return;
    case 3:
    case 5:
        disc(x + 0.5f, y + 0.5f, static_cast<float>(size));
        return;
    default:
        // Larger: the classic circle is 2 * (size / 2) + 1 pixels across.
        disc(x + 0.5f, y + 0.5f, static_cast<float>((size >> 1) * 2 + 1));
        return;
    }
}

} // namespace detail

/// A smooth square of side `side` world units centred on (cx, cy).
inline void smoothSquare(SDL_Renderer* renderer, const Camera& camera, float cx, float cy, float side, SDL_Color color)
{
    const float h = side / 2 * camera.zoom;
    detail::quad(renderer, smooth().rect, detail::toScreen(camera, cx, cy), {1, 0}, h, h, color);
}

/// A smooth disc of diameter `diameter` world units centred on (cx, cy).
inline void smoothDisc(SDL_Renderer* renderer, const Camera& camera, float cx, float cy, float diameter,
                       SDL_Color color)
{
    const float r = diameter / 2 * camera.zoom;
    detail::quad(renderer, smooth().disc, detail::toScreen(camera, cx, cy), {1, 0}, r, r, color);
}

/// A filled rectangle x, y, w, h in world units: smooth when smooth drawing is on, else the
/// renderer's plain fill (in the current draw colour either way).
inline void fillRect(SDL_Renderer* renderer, const Camera& camera, float x, float y, float w, float h)
{
    if (smoothOn()) {
        detail::quad(renderer, smooth().rect, detail::toScreen(camera, x + w / 2, y + h / 2), {1, 0},
                     w / 2 * camera.zoom, h / 2 * camera.zoom, detail::drawColor(renderer));
        return;
    }
    const SDL_FRect r = camera.toScreen(x, y, w, h);
    SDL_RenderFillRect(renderer, &r);
}

/// A line one world unit thick, stepped like the game's one-pixel lines: thick across the
/// minor axis. Uses the renderer's draw blend mode (alpha < 255 needs BLEND). Smooth drawing: the
/// same band, anti-aliased at any angle.
inline void line(SDL_Renderer* renderer, const Camera& camera, SDL_FPoint a, SDL_FPoint b, SDL_Color color)
{
    if (smoothOn()) {
        const bool flat = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
        const float ox = flat ? 0.0f : 0.5f;
        const float oy = flat ? 0.5f : 0.0f;
        const SDL_FPoint p = detail::toScreen(camera, a.x + ox, a.y + oy);
        const SDL_FPoint q = detail::toScreen(camera, b.x + ox, b.y + oy);
        const float dx = q.x - p.x;
        const float dy = q.y - p.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const SDL_FPoint ax = len > 0 ? SDL_FPoint{dx / len, dy / len} : SDL_FPoint{1, 0};
        detail::quad(renderer, smooth().rect, {(p.x + q.x) / 2, (p.y + q.y) / 2}, ax, len / 2, camera.zoom / 2,
                     color);
        return;
    }
    // Stock: a one-unit band as a quad, one unit down (flat lines) or right (steep ones).
    const bool flat = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
    const auto screen = [&](float x, float y) { return detail::toScreen(camera, x, y); };
    const float ox = flat ? 0.0f : 1.0f;
    const float oy = flat ? 1.0f : 0.0f;
    const SDL_FColor c{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    const SDL_FPoint p[4] = {screen(a.x, a.y), screen(b.x, b.y), screen(b.x + ox, b.y + oy), screen(a.x + ox, a.y + oy)};
    const SDL_Vertex v[4] = {{p[0], c, {}}, {p[1], c, {}}, {p[2], c, {}}, {p[3], c, {}}};
    const int index[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, v, 4, index, 6);
}

/// One glow disc: `color` x intensity / 256 at the centre, fading to
/// nothing at `radius`, added onto the screen.
inline void glow(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* texture, float x, float y,
                 SDL_Color color, float radius, int intensity)
{
    if (!texture || intensity <= 0 || radius <= 0) {
        return;
    }
    if (smoothOn() && smooth().glow) {
        // The same falloff worked out per pixel (fx_glow), added on.
        const SDL_Color c{color.r, color.g, color.b, static_cast<std::uint8_t>(std::min(intensity, 255))};
        const float r = radius * camera.zoom;
        detail::quad(renderer, smooth().glow, detail::toScreen(camera, x, y), {1, 0}, r, r, c, SDL_BLENDMODE_ADD);
        return;
    }
    SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(texture, static_cast<std::uint8_t>(std::min(intensity, 255)));
    const SDL_FRect dest = camera.toScreen(x - radius, y - radius, radius * 2, radius * 2);
    SDL_RenderTexture(renderer, texture, nullptr, &dest);
}

/// Whether a smoke blast is drawn as soft dust (fx_puff, Smooth FX) instead of the game's Smoke.
inline bool puffsOn()
{
    return smoothOn() && smooth().puff;
}

/// Whether a toxic cloud is drawn as soft puffs (fx_puff, HD Spell FX) instead of the game's
/// GreenPuff / GreenSmoke.
inline bool cloudPuffsOn()
{
    return spellFxOn() && spellFx().puff;
}

/// Whether any puffs are drawn (puffsOn or cloudPuffsOn): puff() has a state to draw with.
inline bool anyPuffsOn()
{
    return puffsOn() || cloudPuffsOn();
}

/// A soft puff (fx_puff) of `radius` world units at (x, y), alpha-blended: `color`'s alpha is its
/// opacity at the middle. `seed` picks its noise. Needs anyPuffsOn.
inline void puff(SDL_Renderer* renderer, const Camera& camera, float x, float y, float radius, SDL_Color color,
                 std::uint32_t seed)
{
    // (The one fx_puff state, whichever switch lent it.)
    const bool smoothPuff = puffsOn();
    SDL_Texture* white = smoothPuff ? smooth().white : spellFx().white;
    SDL_GPURenderState* state = smoothPuff ? smooth().puff : spellFx().puff;
    const SDL_FPoint c = detail::toScreen(camera, x, y);
    const float r = radius * camera.zoom;
    const SDL_FColor fc{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    // The seed as a whole-number offset of the texture coordinates (the shader takes floor()).
    const float su = static_cast<float>(seed % 997);
    const float sv = static_cast<float>((seed / 997) % 991);
    const SDL_Vertex v[4] = {{{c.x - r, c.y - r}, fc, {su, sv}},
                             {{c.x + r, c.y - r}, fc, {su + 0.999f, sv}},
                             {{c.x + r, c.y + r}, fc, {su + 0.999f, sv + 0.999f}},
                             {{c.x - r, c.y + r}, fc, {su, sv + 0.999f}}};
    const int index[6] = {0, 1, 2, 0, 2, 3};
    SDL_SetTextureBlendMode(white, SDL_BLENDMODE_BLEND);
    SDL_SetGPURenderState(renderer, state);
    SDL_RenderGeometry(renderer, white, v, 4, index, 6);
    SDL_SetGPURenderState(renderer, nullptr);
}

/// Whether the Force Field is drawn as the fx_barrier sphere (HD Spell FX) instead of the game's
/// animation.
inline bool barrierOn()
{
    return spellFxOn() && spellFx().barrier;
}

/// The Force Field's sphere (fx_barrier) of `radius` world units about (x, y), added onto the
/// screen: `color` its tint, its alpha the strength. `seconds` runs its animation; `seed` sets
/// each unit's out of step.
inline void barrier(SDL_Renderer* renderer, const Camera& camera, float x, float y, float radius, SDL_Color color,
                    double seconds, std::uint32_t seed)
{
    const SpellFx& s = spellFx();
    const SDL_FPoint c = detail::toScreen(camera, x, y);
    const float r = radius * camera.zoom;
    const SDL_FColor fc{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    // The seed as a whole-number offset of the texture coordinates (the shader takes floor()).
    const float su = static_cast<float>(seed % 97);
    const float sv = static_cast<float>((seed / 97) % 89);
    const SDL_Vertex v[4] = {{{c.x - r, c.y - r}, fc, {su, sv}},
                             {{c.x + r, c.y - r}, fc, {su + 0.999f, sv}},
                             {{c.x + r, c.y + r}, fc, {su + 0.999f, sv + 0.999f}},
                             {{c.x - r, c.y + r}, fc, {su, sv + 0.999f}}};
    const int index[6] = {0, 1, 2, 0, 2, 3};
    const float uniforms[4] = {static_cast<float>(std::fmod(seconds, 3600.0)), 0, 0, 0};
    SDL_SetGPURenderStateFragmentUniforms(s.barrier, 0, uniforms, sizeof(uniforms));
    SDL_SetTextureBlendMode(s.white, SDL_BLENDMODE_ADD);
    SDL_SetGPURenderState(renderer, s.barrier);
    SDL_RenderGeometry(renderer, s.white, v, 4, index, 6);
    SDL_SetGPURenderState(renderer, nullptr);
}

/// Whether the Reflective Shield is drawn as the fx_ripple water disc (HD Spell FX) instead of the
/// game's animation.
inline bool rippleOn()
{
    return spellFxOn() && spellFx().ripple;
}

/// The Reflective Shield's water disc (fx_ripple): an ellipse about (x, y) whose axes are `major`
/// and `minor` world units across, its major axis `tiltDegrees` off upright (its top to the right),
/// alpha-blended. `light` multiplies its colour (the scene's light there, as the sprite's); `loop`
/// (0-1) runs its rings, `seconds` its slow drift; `back`: seen from behind.
inline void ripple(SDL_Renderer* renderer, const Camera& camera, float x, float y, float major, float minor,
                   float tiltDegrees, SDL_Color light, float loop, double seconds, bool back)
{
    const SpellFx& s = spellFx();
    const float tilt = tiltDegrees * 3.14159265f / 180.0f;
    const SDL_FPoint down{-std::sin(tilt), std::cos(tilt)}; // (the major axis, downward on screen)
    const auto v = detail::QuadBatch::corners(detail::toScreen(camera, x, y), down, major / 2 * camera.zoom,
                                              minor / 2 * camera.zoom, light);
    static constexpr int kIndices[6] = {0, 1, 2, 0, 2, 3};
    const float uniforms[4] = {loop, static_cast<float>(std::fmod(seconds, 3600.0)), back ? 1.0f : 0.0f,
                               major > 0 ? minor / major : 1.0f};
    SDL_SetGPURenderStateFragmentUniforms(s.ripple, 0, uniforms, sizeof(uniforms));
    SDL_SetTextureBlendMode(s.white, SDL_BLENDMODE_BLEND);
    SDL_SetGPURenderState(renderer, s.ripple);
    SDL_RenderGeometry(renderer, s.white, v.data(), 4, kIndices, 6);
    SDL_SetGPURenderState(renderer, nullptr);
}

namespace detail {

/// Glow lines for fx_glowline: one quad per segment covering its discs, which the shader adds up
/// per pixel. The discs are where the game's glow line stamps them (glowLine).
struct GlowLineBatch {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;

    void add(const Camera& camera, SDL_FPoint a, SDL_FPoint b, float radius)
    {
        const float steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
        const float every = std::max(1.0f, std::floor(radius / 4));
        // Disc spacing as glowLine stamps them: every radius / 4 units, at least every unit.
        // The discs at s = 0, every, 2 every ... <= steps along the line.
        const int count = steps > 0 ? static_cast<int>(std::floor(steps / every)) + 1 : 1;
        const SDL_FPoint step = steps > 0 ? SDL_FPoint{(b.x - a.x) * every / steps, (b.y - a.y) * every / steps}
                                          : SDL_FPoint{0, 0};
        const SDL_FPoint first = toScreen(camera, a.x, a.y);
        const SDL_FPoint last = toScreen(camera, a.x + step.x * (count - 1), a.y + step.y * (count - 1));
        const float r = radius * camera.zoom;
        const float dx = last.x - first.x;
        const float dy = last.y - first.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const SDL_FPoint ax = len > 0 ? SDL_FPoint{dx / len, dy / len} : SDL_FPoint{1, 0};
        const SDL_FPoint perp{-ax.y, ax.x};
        const SDL_FPoint centre{(first.x + last.x) / 2, (first.y + last.y) / 2};
        const float along = len / 2 + r + 1;
        const float across = r + 1;
        // The colour carries the step between discs in radii and their count (see the shader).
        const SDL_FColor c{step.x / radius, step.y / radius, static_cast<float>(count), 1};
        const auto corner = [&](float sa, float sc) {
            const SDL_FPoint p{centre.x + ax.x * along * sa + perp.x * across * sc,
                               centre.y + ax.y * along * sa + perp.y * across * sc};
            return SDL_Vertex{p, c, {(p.x - first.x) / r, (p.y - first.y) / r}};
        };
        const int base = static_cast<int>(vertices.size());
        vertices.insert(vertices.end(), {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    /// All of them in `color` x intensity / 256, added onto the screen.
    void draw(SDL_Renderer* renderer, SDL_Color color, int intensity)
    {
        if (vertices.empty()) {
            return;
        }
        const Smooth& s = smooth();
        const float uniforms[4] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                                   static_cast<float>(std::min(intensity, 255)) / 255.0f};
        SDL_SetGPURenderStateFragmentUniforms(s.glowLine, 0, uniforms, sizeof(uniforms));
        SDL_SetTextureBlendMode(s.white, SDL_BLENDMODE_ADD);
        SDL_SetGPURenderState(renderer, s.glowLine);
        SDL_RenderGeometry(renderer, s.white, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
                           static_cast<int>(indices.size()));
        SDL_SetGPURenderState(renderer, nullptr);
        vertices.clear();
        indices.clear();
    }
};

} // namespace detail

/// Whether glow lines go through fx_glowline (needs fx_glow too).
inline bool glowLinesOn()
{
    return smoothOn() && smooth().glow && smooth().glowLine;
}

/// The game's glow line: glow discs of `radius` stamped along the line every
/// radius / 4 pixels (every pixel for small ones). Smooth drawing: the discs summed in one quad
/// (fx_glowline).
inline void glowLine(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* texture, SDL_FPoint a, SDL_FPoint b,
                     SDL_Color color, float radius, int intensity)
{
    if (!texture || intensity <= 0) {
        return;
    }
    if (glowLinesOn() && radius > 0) {
        auto& batch = detail::scratch<detail::GlowLineBatch>();
        batch.add(camera, a, b, radius);
        batch.draw(renderer, color, intensity);
        return;
    }
    SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(texture, static_cast<std::uint8_t>(std::min(intensity, 255)));
    const float steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
    const float every = std::max(1.0f, std::floor(radius / 4));
    for (float s = 0; s <= steps; s += every) {
        const float t = steps > 0 ? s / steps : 0;
        const float x = a.x + (b.x - a.x) * t;
        const float y = a.y + (b.y - a.y) * t;
        if (smoothOn() && smooth().glow) {
            glow(renderer, camera, texture, x, y, color, radius, intensity);
            continue;
        }
        const SDL_FRect dest = camera.toScreen(x - radius, y - radius, radius * 2, radius * 2);
        SDL_RenderTexture(renderer, texture, nullptr, &dest);
    }
}

using Segment = std::pair<SDL_FPoint, SDL_FPoint>;

/// glowLine along each segment of a path (a lightning bolt's pass): with smooth glows in one draw
/// (a quad per segment with fx_glowline, else every disc), else glowLine each.
inline void glowPath(SDL_Renderer* renderer, const Camera& camera, SDL_Texture* texture, std::span<const Segment> path,
                     SDL_Color color, float radius, int intensity)
{
    if (!texture || intensity <= 0 || radius <= 0) {
        return;
    }
    if (glowLinesOn()) {
        auto& batch = detail::scratch<detail::GlowLineBatch>();
        for (const auto& [a, b] : path) {
            batch.add(camera, a, b, radius);
        }
        batch.draw(renderer, color, intensity);
        return;
    }
    if (!(smoothOn() && smooth().glow)) {
        for (const auto& [a, b] : path) {
            glowLine(renderer, camera, texture, a, b, color, radius, intensity);
        }
        return;
    }
    auto& batch = detail::scratch<detail::QuadBatch>();
    const SDL_Color c{color.r, color.g, color.b, static_cast<std::uint8_t>(std::min(intensity, 255))};
    const float r = radius * camera.zoom;
    const float every = std::max(1.0f, std::floor(radius / 4));
    for (const auto& [a, b] : path) {
        const float steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
        for (float s = 0; s <= steps; s += every) {
            const float t = steps > 0 ? s / steps : 0;
            batch.add(detail::toScreen(camera, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t), {1, 0}, r, r, c);
        }
    }
    batch.draw(renderer, smooth().glow, SDL_BLENDMODE_ADD);
}

/// line along each segment of a path: smooth lines in one draw, else line each.
inline void linePath(SDL_Renderer* renderer, const Camera& camera, std::span<const Segment> path, SDL_Color color)
{
    if (!smoothOn()) {
        for (const auto& [a, b] : path) {
            line(renderer, camera, a, b, color);
        }
        return;
    }
    auto& batch = detail::scratch<detail::QuadBatch>();
    for (const auto& [a, b] : path) {
        const bool flat = std::abs(b.x - a.x) >= std::abs(b.y - a.y);
        const float ox = flat ? 0.0f : 0.5f;
        const float oy = flat ? 0.5f : 0.0f;
        const SDL_FPoint p = detail::toScreen(camera, a.x + ox, a.y + oy);
        const SDL_FPoint q = detail::toScreen(camera, b.x + ox, b.y + oy);
        const float dx = q.x - p.x;
        const float dy = q.y - p.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        const SDL_FPoint ax = len > 0 ? SDL_FPoint{dx / len, dy / len} : SDL_FPoint{1, 0};
        batch.add({(p.x + q.x) / 2, (p.y + q.y) / 2}, ax, len / 2, camera.zoom / 2, color);
    }
    batch.draw(renderer, smooth().rect);
}

/// Many small glows and dots (sparks) gathered into three draws (the glows, then the square dots,
/// then the round ones) when smooth drawing has its glow shader; batched() says whether it does
/// (else draw them one by one with glow / point).
struct DotBatch {
    detail::QuadBatch glows;
    detail::QuadBatch squares;
    detail::QuadBatch discs;

    static bool batched() { return smoothOn() && smooth().glow; }

    void glow(const Camera& camera, float x, float y, SDL_Color color, float radius, int intensity)
    {
        if (intensity <= 0 || radius <= 0) {
            return;
        }
        const float r = radius * camera.zoom;
        glows.add(detail::toScreen(camera, x, y), {1, 0}, r, r,
                  {color.r, color.g, color.b, static_cast<std::uint8_t>(std::min(intensity, 255))});
    }
    /// smoothSquare / smoothDisc.
    void square(const Camera& camera, float cx, float cy, float side, SDL_Color c)
    {
        const float h = side / 2 * camera.zoom;
        squares.add(detail::toScreen(camera, cx, cy), {1, 0}, h, h, c);
    }
    void disc(const Camera& camera, float cx, float cy, float diameter, SDL_Color c)
    {
        const float r = diameter / 2 * camera.zoom;
        discs.add(detail::toScreen(camera, cx, cy), {1, 0}, r, r, c);
    }
    /// point()'s smooth shapes.
    void point(const Camera& camera, float x, float y, int size, SDL_Color c)
    {
        detail::pointShape(
            x, y, size, [&](float cx, float cy, float side) { square(camera, cx, cy, side, c); },
            [&](float cx, float cy, float diameter) { disc(camera, cx, cy, diameter, c); });
    }
    void draw(SDL_Renderer* renderer)
    {
        glows.draw(renderer, smooth().glow, SDL_BLENDMODE_ADD);
        squares.draw(renderer, smooth().rect);
        discs.draw(renderer, smooth().disc);
    }
};

/// drawPointMB's shapes in the current draw colour. Smooth drawing: sizes 1 and 2
/// (squares) stay squares, the round ones become discs of the same diameter and centre.
inline void point(SDL_Renderer* renderer, const Camera& camera, float x, float y, int size)
{
    if (smoothOn()) {
        const SDL_Color c = detail::drawColor(renderer);
        detail::pointShape(
            x, y, size, [&](float cx, float cy, float side) { smoothSquare(renderer, camera, cx, cy, side, c); },
            [&](float cx, float cy, float diameter) { smoothDisc(renderer, camera, cx, cy, diameter, c); });
        return;
    }
    const auto rect = [&](float rx, float ry, float w, float h) {
        const SDL_FRect r = camera.toScreen(rx, ry, w, h);
        SDL_RenderFillRect(renderer, &r);
    };
    // Classic: pixel-art blobs built from filled rows, round the pixel at (x, y).
    x = std::floor(x);
    y = std::floor(y);
    switch (size) {
    case 0:
    case 1:
        rect(x, y, 1, 1);
        break;
    case 2:
        rect(x, y, 2, 2);
        break;
    case 3:
        rect(x, y - 1, 1, 1);  // a plus
        rect(x - 1, y, 3, 1);
        rect(x, y + 1, 1, 1);
        break;
    case 4:
        rect(x, y - 1, 2, 1);  // 4 and 6: rounded blobs, one pixel right and down of centre
        rect(x - 1, y, 4, 2);
        rect(x, y + 2, 2, 1);
        break;
    case 5:
        rect(x - 1, y - 2, 3, 1);
        rect(x - 2, y - 1, 5, 3);
        rect(x - 1, y + 2, 3, 1);
        break;
    case 6:
        rect(x, y - 2, 2, 1);
        rect(x - 1, y - 1, 4, 1);
        rect(x - 2, y, 6, 2);
        rect(x - 1, y + 2, 4, 1);
        rect(x, y + 3, 2, 1);
        break;
    default: {
        // Bigger: a filled circle of radius size / 2, one row at a time.
        const float r = static_cast<float>(size >> 1);
        for (float dy = -r; dy <= r; ++dy) {
            const float half = std::floor(std::sqrt(r * r - dy * dy));
            rect(x - half, y + dy, half * 2 + 1, 1);
        }
        break;
    }
    }
}

} // namespace noxsdl::fxdraw
