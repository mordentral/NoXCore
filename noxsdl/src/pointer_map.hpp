// The pointer from SDL's window coordinates to the game's (app.hpp's intoGameArea): SDL gives the
// mouse in window points, the game draws in output pixels. They're the same unless the window
// has more pixels than points (its pixel density over 1: made with SDL_WINDOW_HIGH_PIXEL_DENSITY
// on a display that has them; noxsdl's windows aren't, so it's 1 here: window_density.hpp). No
// SDL in it, so the tests drive it.
#pragma once

#include <algorithm>

namespace noxsdl::pointermap {

/// The game's area in the output, in pixels (App::gameArea).
struct Area {
    float x = 0, y = 0, w = 0, h = 0;
};

/// Whether a window point needs mapping at all: a letterboxed game, or a window whose points
/// aren't its pixels.
inline bool needed(float density, bool letterboxed)
{
    return letterboxed || density != 1.0f;
}

/// A window point in output pixels (times the density), then, letterboxed, from the game area's
/// corner and kept on it, as the game kept the mouse on its screen.
inline void toGame(float& x, float& y, float density, const Area& area, bool letterboxed)
{
    if (density != 1.0f) {
        x *= density;
        y *= density;
    }
    if (letterboxed) {
        x = std::clamp(x - area.x, 0.0f, std::max(area.w - 1, 0.0f));
        y = std::clamp(y - area.y, 0.0f, std::max(area.h - 1, 0.0f));
    }
}

} // namespace noxsdl::pointermap
