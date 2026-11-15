// The boxes the windows drawn over the HUD share (the map vote, the modes' maps, windows,
// strips and cards): a point-in-box test, and a box filled or outlined in one colour, in screen
// pixels.
#pragma once

#include <SDL3/SDL.h>

namespace noxsdl::box {

/// (x, y) in `r`, its right and bottom edges outside.
inline bool inside(const SDL_FRect& r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/// (x, y) in the rw x rh box at (rx, ry), its right and bottom edges outside (the HUD's and the
/// spellbook's tests, in game pixels).
inline bool inside(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

inline void fill(SDL_Renderer* renderer, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(renderer, &r);
}

inline void outline(SDL_Renderer* renderer, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderRect(renderer, &r);
}

} // namespace noxsdl::box
