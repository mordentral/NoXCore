// The map download screen (map_download.hpp): mapdnld.wnd with its progress bar and Cancel.
#include "map_download.hpp"

namespace noxsdl {

namespace {

constexpr std::int32_t kCancel = 1601;
constexpr std::int32_t kBar = 1603;

} // namespace

MapDownload::MapDownload(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> cancel)
    : gui_(std::move(res)), cancel_(std::move(cancel))
{
    window_ = gui_.load(windowDir / "mapdnld.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        gui_.resources().sound(0x2FE); // ButtonPress
        if (from.id == kCancel && cancel_) {
            cancel_();
            return true;
        }
        return false;
    });
    if (window_) {
        // The 1024 x 768 window centred on the screen, here the menus' 640 x 480.
        gui::centreOnScreen(*window_);
        bar_ = window_->find(kBar);
        gui_.setModal(window_);
    }
}

void MapDownload::draw(SDL_Renderer* renderer, int width, int height, int percent)
{
    if (!window_) {
        return;
    }
    if (bar_) {
        bar_->progress = percent;
    }
    gui_.draw(renderer, width, height);
}

bool MapDownload::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!window_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        gui_.resources().sound(0xE7); // PermanentFizzle
        if (cancel_) {
            cancel_();
        }
        return true;
    }
    gui_.handleEvent(event, width, height);
    return event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
           event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_MOTION;
}

} // namespace noxsdl
