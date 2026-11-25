// (noxcpp) The question which the player plays with, the game controller or the mouse and keyboard,
// asked when a pad is connected (pad/pad_mode.cpp says when): the game's own message box (dlg.wnd,
// its border, title and text) with two of the in-game menu's large buttons (QuitMenu.wnd's art),
// "Controller" over "Mouse and Keyboard". Answered by a click or by the pad (the D-pad or the stick
// between the two, A on the one focused: Controller first). Over everything, in the menus or a
// game, modal while it's up.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace noxsdl {

class InputModeAsk {
public:
    InputModeAsk(gui::Resources res, std::filesystem::path windowDir);

    bool isOpen() const { return dialog_ != nullptr; }
    /// Up, the controller's choice focused; `onAnswer(pad)` once one is chosen (the window gone first).
    void open(std::function<void(bool pad)> onAnswer);
    /// Gone with no answer (the last pad went).
    void close();

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Every event while it's up (the mouse's in the game's output pixels, the pad's as nav events).
    void handleEvent(const SDL_Event& event, int width, int height);

private:
    gui::Gui gui_;
    std::filesystem::path windowDir_;
    gui::Window* dialog_ = nullptr;
    std::function<void(bool)> onAnswer_;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
