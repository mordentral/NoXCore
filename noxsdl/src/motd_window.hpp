// The message of the day (motd.wnd: guiMotdLoad), shown as it comes from the
// server (motd, called each frame while one waits): the window
// taking the input (wndShowModalMB), the text a list line a line (
// split at its line ends; an empty line a space), its slider and arrows; OK closes it
// (0x2FE). Not in a quest's chat map, nor while another dialog is
// up.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace noxsdl {

class MotdWindow {
public:
    MotdWindow(gui::Resources res, std::filesystem::path windowDir);

    bool isOpen() const { return window_ != nullptr; }
    void show(const std::string& text);
    void close();

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// While it's open it takes the mouse and keys (Escape closes it too).
    bool handleEvent(const SDL_Event& event, int width, int height);

private:
    void scrollTo(int first);

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    gui::Window* window_ = nullptr;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
