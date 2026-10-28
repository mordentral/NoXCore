// The map editor session's pencil: the game list's mark for an edit session, and the "who's
// editing" mark over an editor's character (in the game, the editor's live view and the Session
// panel). One shape for every place: slanted up to the right, a graphite point at the lower left,
// the wooden cone, the body in the editor's colour, a pink cap.
//
// pencilShape gives the outline's points (no SDL, so the editor's ImGui draw and the tests share
// it); drawPencil draws it with SDL. The editors' colours (by peer id, as their lock outlines and
// cursors) are here too, so the game's pencil matches the editor's outlines.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace noxsdl {

/// An editor's colour by peer id (the lock outlines, cursors and pencils: one palette).
struct PeerRgb {
    std::uint8_t r = 0, g = 0, b = 0;
};
inline constexpr std::array<PeerRgb, 6> kPeerColours{{{240, 200, 60}, {90, 200, 255}, {255, 110, 140}, {120, 230, 120}, {200, 140, 255}, {255, 160, 80}}};
inline PeerRgb peerRgb(std::uint32_t peer)
{
    return kPeerColours[peer % kPeerColours.size()];
}
/// An idle editor's pencil: the colour half way to grey.
inline PeerRgb idleRgb(PeerRgb c)
{
    const int l = (c.r * 30 + c.g * 59 + c.b * 11) / 100;
    return {static_cast<std::uint8_t>((c.r + l) / 2 * 3 / 4), static_cast<std::uint8_t>((c.g + l) / 2 * 3 / 4),
            static_cast<std::uint8_t>((c.b + l) / 2 * 3 / 4)};
}

/// The pencil's parts for one of `size` pixels (its length) centred at (cx, cy).
struct PencilShape {
    struct P {
        float x = 0, y = 0;
    };
    std::array<P, 3> lead;   ///< the graphite point
    std::array<P, 4> cone;   ///< the sharpened wood (lead's base to the body)
    std::array<P, 4> body;   ///< the painted body
    std::array<P, 4> cap;    ///< the eraser
    std::array<P, 7> outline; ///< round the whole shape, closed (the last point back to the first)
};

inline PencilShape pencilShape(float cx, float cy, float size)
{
    // Along the pencil (up and right) and across it.
    constexpr float k = 0.70710678f;
    const float ux = k, uy = -k, nx = k, ny = k;
    const float half = size / 2;
    const float t = std::max(1.5f, size * 0.15f); // half its width
    const auto at = [&](float along, float across) {
        return PencilShape::P{cx + ux * along + nx * across, cy + uy * along + ny * across};
    };
    const float tip = -half, leadEnd = -half + size * 0.12f, coneEnd = -half + size * 0.30f, bodyEnd = half - size * 0.16f;
    PencilShape s;
    s.lead = {at(tip, 0), at(leadEnd, -t * 0.40f), at(leadEnd, t * 0.40f)};
    s.cone = {at(leadEnd, -t * 0.40f), at(coneEnd, -t), at(coneEnd, t), at(leadEnd, t * 0.40f)};
    s.body = {at(coneEnd, -t), at(bodyEnd, -t), at(bodyEnd, t), at(coneEnd, t)};
    s.cap = {at(bodyEnd, -t), at(half, -t), at(half, t), at(bodyEnd, t)};
    s.outline = {at(tip, 0), at(coneEnd, -t), at(half, -t), at(half, t), at(coneEnd, t), at(tip, 0), at(tip, 0)};
    return s;
}

/// Draws the pencil with SDL: `body` its paint, `alpha` the whole (0-255), a dark edge round it.
inline void drawPencil(SDL_Renderer* renderer, float cx, float cy, float size, PeerRgb body, std::uint8_t alpha = 255)
{
    const PencilShape s = pencilShape(cx, cy, size);
    const float a = static_cast<float>(alpha) / 255.0f;
    const auto fill = [&](const auto& pts, SDL_FColor c) {
        std::array<SDL_Vertex, 4> v{};
        for (std::size_t i = 0; i < pts.size(); ++i) {
            v[i] = {{pts[i].x, pts[i].y}, c, {0, 0}};
        }
        static constexpr int kQuad[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(renderer, nullptr, v.data(), static_cast<int>(pts.size()), pts.size() == 4 ? kQuad : nullptr, pts.size() == 4 ? 6 : 0);
    };
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    // The dark edge first, a little wider than the shape (lines a pixel out on each side).
    SDL_SetRenderDrawColor(renderer, 10, 10, 12, static_cast<std::uint8_t>(alpha * 0.85f));
    for (const float d : {-1.0f, 1.0f}) {
        std::array<SDL_FPoint, 7> line{};
        for (std::size_t i = 0; i < s.outline.size(); ++i) {
            line[i] = {s.outline[i].x + d * 0.5f, s.outline[i].y + d * 0.5f};
        }
        SDL_RenderLines(renderer, line.data(), static_cast<int>(line.size()));
    }
    fill(s.cone, {0.95f, 0.80f, 0.55f, a});
    fill(s.lead, {0.18f, 0.17f, 0.17f, a});
    fill(s.body, {body.r / 255.0f, body.g / 255.0f, body.b / 255.0f, a});
    fill(s.cap, {0.95f, 0.58f, 0.62f, a});
    SDL_SetRenderDrawBlendMode(renderer, blend);
}

} // namespace noxsdl
