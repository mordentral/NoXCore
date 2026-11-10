// Opt-in Mac safe-area integration. Other builds retain the full-window behavior.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace noxsdl::displayarea {

#if defined(SDL_PLATFORM_MACOS) && defined(NOXSDL_USE_DISPLAY_SAFE_AREA) && NOXSDL_USE_DISPLAY_SAFE_AREA
inline constexpr bool enabled = true;
#else
inline constexpr bool enabled = false;
#endif

inline std::optional<SDL_Rect> window(SDL_Window* window)
{
    if constexpr (enabled) {
        SDL_Rect area{};
        if (SDL_GetWindowSafeArea(window, &area) && area.w > 0 && area.h > 0) return area;
    }
    return std::nullopt;
}

inline std::optional<SDL_Rect> render(SDL_Renderer* renderer)
{
    if constexpr (enabled) {
        SDL_Rect area{};
        if (SDL_GetRenderSafeArea(renderer, &area) && area.w > 0 && area.h > 0) return area;
    }
    return std::nullopt;
}

// SDL confines relative motion in window points, not framebuffer pixels. Round
// inward so the pointer never accumulates hidden travel in a letterbox or notch.
inline SDL_Rect pointerBounds(const SDL_FRect& area, float density)
{
    const int x = static_cast<int>(std::ceil(area.x / density));
    const int y = static_cast<int>(std::ceil(area.y / density));
    return {x, y, std::max(0, static_cast<int>(std::floor((area.x + area.w) / density)) - x),
                  std::max(0, static_cast<int>(std::floor((area.y + area.h) / density)) - y)};
}

inline void clampPointer(const SDL_Rect& area, float& x, float& y)
{
    x = std::clamp(x, static_cast<float>(area.x), static_cast<float>(area.x + area.w - 1));
    y = std::clamp(y, static_cast<float>(area.y), static_cast<float>(area.y + area.h - 1));
}

inline void confinePointer(SDL_Window* window, const SDL_Rect* area)
{
    if constexpr (enabled) {
        const SDL_Rect* current = SDL_GetWindowMouseRect(window);
        if ((area == nullptr) != (current == nullptr) ||
            (area && current && !SDL_RectsEqual(area, current))) {
            if (!SDL_SetWindowMouseRect(window, area)) SDL_Log("mouse confinement: %s", SDL_GetError());
        }
        if (area && SDL_GetMouseFocus() == window && SDL_GetKeyboardFocus() == window) {
            // SDL preserves an existing position beyond the right/bottom edge.
            // Rebase it on resize/ownership changes before the next relative count.
            float x, y;
            SDL_GetMouseState(&x, &y);
            float cx = x, cy = y;
            clampPointer(*area, cx, cy);
            if (cx != x || cy != y) SDL_WarpMouseInWindow(window, cx, cy);
        }
    }
}

inline void confineEvent(SDL_Window* window, SDL_Event& event)
{
    if constexpr (enabled) {
        if (!SDL_GetWindowRelativeMouseMode(window)) return;
        const SDL_Rect* area = SDL_GetWindowMouseRect(window);
        if (!area) return;
        // Events already queued when the bounds changed still carry the old
        // coordinates. Use the same bounds as SDL's position and the drawn cursor.
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: clampPointer(*area, event.motion.x, event.motion.y); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: clampPointer(*area, event.button.x, event.button.y); break;
        case SDL_EVENT_MOUSE_WHEEL: clampPointer(*area, event.wheel.mouse_x, event.wheel.mouse_y); break;
        default: break;
        }
    }
}

inline bool acceptsPointer(SDL_Window* window, const SDL_Event& event)
{
    // Releases must still reach the client if a drag leaves the safe area.
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_WHEEL) return true;
    if (const auto area = displayarea::window(window)) {
        const float x = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? event.button.x : event.wheel.mouse_x;
        const float y = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? event.button.y : event.wheel.mouse_y;
        return x >= area->x && y >= area->y && x < area->x + area->w && y < area->y + area->h;
    }
    return true;
}

} // namespace noxsdl::displayarea
