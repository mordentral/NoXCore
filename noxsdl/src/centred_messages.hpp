// The messages in the middle of the screen (guimsg.c): Gui_PrintCenteredMessage puts
// a line in the next of three slots, shown till 5 s of game frames on (frame + 5 * FPS), and
// echoes it to the console in red ("GuiMsg.c:SystemMsg", "system> %s"; the caller here);
// Gui_ClearCenteredMessages empties them (MSG_MESSAGES_KILL, a game's start).
// drawMessageLines draws them after the minimap (drawMinimapAndLines): the newest at three
// quarters of the view's height less 15, centred across the screen, each older one above it by the
// font's height and 4, stopping at the first that has run out; a black shadow at (+-1, +-1), the
// newest white, the older ones light grey (0xD4).
#pragma once

#include "text.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

namespace noxsdl {

class CentredMessages {
public:
    struct Slot {
        std::string text;
        std::uint32_t until = 0; ///< the last game frame it shows
    };

    /// Gui_PrintCenteredMessage at game frame `frame`; empty text is ignored (the exe's null).
    void print(std::string text, std::uint32_t frame);
    void clear();

    /// `view` is the game's view on screen (the windowSize box), `scale` the HUD's.
    void draw(SDL_Renderer* renderer, Font* font, int screenWidth, const SDL_FRect& view, float scale,
              std::uint32_t frame) const;

    const std::array<Slot, 3>& slots() const { return slots_; }
    int newest() const { return index_; }

    static constexpr std::uint32_t kShowFrames = 5 * 30; ///< 5 * gameFPS__

private:
    std::array<Slot, 3> slots_{};
    int index_ = 0; ///< g_guiCenterMsgIndex
};

} // namespace noxsdl
