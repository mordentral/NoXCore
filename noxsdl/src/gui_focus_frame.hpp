// (noxcpp) How the game controller's focus is marked, one look everywhere (the menus' and the game
// windows' focus, the shop's and the trade's cells, the panels' snap cursor, the on-screen
// keyboard's key): an antique-gold frame about a layout pixel thick (two output pixels at least)
// with brighter corner pieces, a dark edge outside it and a faint one inside so it reads on dark
// art and bright alike, and a faint warm wash inside. A widget that lights itself when pointed at
// (a button's lit image) gets a softer frame and no wash: its own light leads.
#pragma once

#include <SDL3/SDL.h>

namespace noxsdl::gui {

enum class FocusLook : unsigned char {
    Normal,  ///< a list, a slider, a cell, a key: the frame and the wash
    Lit,     ///< a widget showing its own lit look: the frame fainter, no wash
    Holding, ///< something held to be put down (the panels' move): the same in pale steel
};

/// The focus frame round `box` (output pixels), `unit` output pixels to the 640 x 480 layout's unit
/// (the frame's thickness and corners follow it).
void drawFocusFrame(SDL_Renderer* renderer, const SDL_FRect& box, float unit, FocusLook look = FocusLook::Normal);

} // namespace noxsdl::gui
