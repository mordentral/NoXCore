// The map download screen (mapdnld.wnd): shown while the server sends us a map we
// don't have, over a black screen, the 1024 x 768 window centred. Its progress bar (1603) shows the
// share of the map received (Cli_MapDownload_OnBlock); Cancel (1601) or
// Escape gives up the download, and the game with it.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>

namespace noxsdl {

class MapDownload {
public:
    MapDownload(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> cancel);

    /// Shown at `percent` (0-100).
    void draw(SDL_Renderer* renderer, int width, int height, int percent);
    /// Cancel and Escape; it has all input while shown. Returns whether it took the event.
    bool handleEvent(const SDL_Event& event, int width, int height);

private:
    gui::Gui gui_;
    gui::Window* window_ = nullptr;
    gui::Window* bar_ = nullptr;
    std::function<void()> cancel_;
};

} // namespace noxsdl
