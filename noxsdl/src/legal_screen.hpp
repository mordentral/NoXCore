// The legal screen after the start-up movies (game_showLegal, legal.wnd): the
// CopyrightScreen picture, the legal text at the bottom (9980, set so its last line ends at 477),
// the version (9999, "V:%S"), the trademark sign ("TM" at 500, 150 in grey) and
// the title music (Snd_SetMusic(0xE)). A count of 300 frames runs down: below 270
// the click message (9910) shows; from 30 frames on, a click on the screen (9901,
// windowMainBGProc) or a key goes on to the main menu, as does the
// count's end. The exe counts the frames it draws (its 30 a second); here 30 a second of time.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace noxsdl {

class LegalScreen {
public:
    /// `done` runs once it's over (the main menu, game_showMainMenu).
    LegalScreen(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> done);

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    void handleEvent(const SDL_Event& event, int width, int height);
    bool over() const { return over_; }

private:
    void leave();

    gui::Gui gui_;
    gui::Window* screen_ = nullptr;
    std::function<void()> done_;
    int count_ = 300; ///<
    std::uint64_t stepMs_ = 0;
    bool placed_ = false;
    bool over_ = false;
};

} // namespace noxsdl
