// A window's pixel density as noxsdl counts it: its output pixels to a point of SDL's window
// coordinates (the mouse's, ImGui's layout), for a window made with SDL_WINDOW_HIGH_PIXEL_DENSITY;
// 1 for any other. noxsdl makes none, so it's 1 here on every platform whatever the display's
// scale, and the pointer mapping (pointer_map.hpp) and the editor's scaling (imgui_host.cpp) are
// left as they were; a build that makes its window with the flag gets them.
#pragma once

#include <SDL3/SDL.h>

namespace noxsdl {

inline float windowPixelDensity(SDL_Window* window)
{
    if (!window || !(SDL_GetWindowFlags(window) & SDL_WINDOW_HIGH_PIXEL_DENSITY)) {
        return 1.0f;
    }
    const float density = SDL_GetWindowPixelDensity(window);
    return density > 0 ? density : 1.0f;
}

} // namespace noxsdl
