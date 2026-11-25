// (noxcpp) The game controller's focus frame (gui_focus_frame.hpp).
#include "gui_focus_frame.hpp"

#include <algorithm>
#include <cmath>

namespace noxsdl::gui {

namespace {

void fillRect(SDL_Renderer* r, float x, float y, float w, float h, SDL_Color c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    const SDL_FRect f{x, y, w, h};
    SDL_RenderFillRect(r, &f);
}

/// A frame `t` thick just inside the rectangle (x, y, w, h).
void frame(SDL_Renderer* r, float x, float y, float w, float h, float t, SDL_Color c)
{
    fillRect(r, x, y, w, t, c);
    fillRect(r, x, y + h - t, w, t, c);
    fillRect(r, x, y + t, t, h - 2 * t, c);
    fillRect(r, x + w - t, y + t, t, h - 2 * t, c);
}

} // namespace

void drawFocusFrame(SDL_Renderer* renderer, const SDL_FRect& box, float unit, FocusLook look)
{
    if (!renderer || box.w <= 0 || box.h <= 0) {
        return;
    }
    // Colours from the game's own: its gold text (230, 165, 65) as antique gold, the corners a
    // little brighter, the wash a breath of it; holding, a pale steel. Never white: the art's own
    // highlights are.
    const bool holding = look == FocusLook::Holding;
    const bool lit = look == FocusLook::Lit;
    const SDL_Color line = holding ? SDL_Color{168, 188, 204, 225} : SDL_Color{196, 146, 66, static_cast<Uint8>(lit ? 175 : 225)};
    const SDL_Color corner = holding ? SDL_Color{212, 226, 238, 255} : SDL_Color{236, 190, 104, static_cast<Uint8>(lit ? 225 : 255)};
    const SDL_Color shadow{12, 8, 3, static_cast<Uint8>(lit ? 150 : 185)};
    const SDL_Color inner{12, 8, 3, static_cast<Uint8>(lit ? 60 : 90)};
    const SDL_Color wash = holding ? SDL_Color{150, 175, 200, 24} : SDL_Color{205, 150, 70, 20};

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    // About a layout pixel thick (never under 2 output pixels), the dark edge outside it about
    // half that, a faint dark line inside it: it reads on dark art and on bright alike. The corner
    // pieces a little bolder (inwards), so it reads as the focus beside the art's own frames (a
    // worn item's).
    const float t = std::max(2.0f, std::round(unit * 1.2f));
    const float edge = std::max(1.0f, std::round(unit * 0.6f));
    const float ct = t + std::max(1.0f, std::round(unit * 0.5f));
    // (snapped to whole pixels: crisp lines at any scale)
    const float x = std::floor(box.x) - t;
    const float y = std::floor(box.y) - t;
    const float w = std::ceil(box.x + box.w) + t - x;
    const float h = std::ceil(box.y + box.h) + t - y;
    if (!lit) {
        fillRect(renderer, x + t, y + t, w - 2 * t, h - 2 * t, wash);
    }
    frame(renderer, x - edge, y - edge, w + 2 * edge, h + 2 * edge, edge, shadow);
    frame(renderer, x + t, y + t, w - 2 * t, h - 2 * t, 1, inner);
    frame(renderer, x, y, w, h, t, line);
    // The corners: short pieces along each edge from each corner.
    const float len = std::clamp(std::min(w, h) * 0.28f, 3 * ct, 12 * std::max(unit, 1.0f));
    for (const float cx : {x, x + w - len}) {
        fillRect(renderer, cx, y, len, ct, corner);
        fillRect(renderer, cx, y + h - ct, len, ct, corner);
    }
    for (const float cy : {y + ct, y + h - len}) {
        fillRect(renderer, x, cy, ct, len - ct, corner);
        fillRect(renderer, x + w - ct, cy, ct, len - ct, corner);
    }
}

} // namespace noxsdl::gui
