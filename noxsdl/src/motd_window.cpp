// The message of the day window (motd_window.hpp): motd.wnd filled with the text, modal till closed.
#include "motd_window.hpp"

#include <algorithm>

namespace noxsdl {

namespace {

// motd.wnd
constexpr std::int32_t kList = 4203, kSlider = 4204, kUp = 4205, kDown = 4206;
constexpr int kLinesShown = 20; // (230 high, 11 a line)

} // namespace

MotdWindow::MotdWindow(gui::Resources res, std::filesystem::path windowDir) : gui_(std::move(res)), windowDir_(std::move(windowDir)) {}

void MotdWindow::show(const std::string& text)
{
    close();
    window_ = gui_.load(windowDir_ / "motd.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message == gui::SliderChanged && from.id == kSlider) {
            gui::Window* list = window_->find(kList);
            const auto* sl = std::get_if<nox::gui::SliderData>(&from.data);
            if (list && sl) {
                const int most = std::max(0, static_cast<int>(list->lines.size()) - kLinesShown);
                list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
            }
            return true;
        }
        if (message != gui::Clicked) {
            return false;
        }
        if (from.id == kUp || from.id == kDown) {
            if (gui::Window* list = window_->find(kList)) {
                scrollTo(list->scroll + (from.id == kUp ? -1 : 1));
            }
            return true;
        }
        if (from.type == nox::gui::WindowType::PushButton) {
            // Any button (OK) closes it.
            gui_.resources().sound(0x2FE);
            deferred_.push_back([this] { close(); });
            return true;
        }
        return false;
    });
    gui::useSliderThumb(window_->find(kSlider));
    // The MOTD's lines: up to a line end ('\r' dropped), an empty one a space.
    if (gui::Window* list = window_->find(kList)) {
        list->lines.clear();
        std::string line;
        const auto add = [&] { list->lines.push_back({line.empty() ? " " : line, list->textColor}); };
        for (const char c : text) {
            if (c == '\n') {
                add();
                line.clear();
            } else if (c != '\r' && c != '\0') {
                line += c;
            }
        }
        if (!line.empty()) {
            add();
        }
    }
    scrollTo(0);
    window_->show(true); // (hidden in the file: wndShowModalMB shows it)
    gui_.setModal(window_);
}

void MotdWindow::scrollTo(int first)
{
    gui::Window* list = window_ ? window_->find(kList) : nullptr;
    if (!list) {
        return;
    }
    const int most = std::max(0, static_cast<int>(list->lines.size()) - kLinesShown);
    list->scroll = std::clamp(first, 0, most);
    if (gui::Window* slider = window_->find(kSlider)) {
        if (const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data)) {
            slider->sliderValue = gui::sliderFromScroll(*sl, list->scroll, most);
        }
    }
}

void MotdWindow::close()
{
    if (window_) {
        gui_.setModal(nullptr);
        gui_.destroy(window_);
        window_ = nullptr;
    }
}

void MotdWindow::update(std::uint64_t nowMs)
{
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void MotdWindow::draw(SDL_Renderer* renderer, int width, int height)
{
    if (window_) {
        gui_.draw(renderer, width, height);
    }
}

bool MotdWindow::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!window_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
        close();
        return true;
    }
    gui_.handleEvent(event, width, height);
    return event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_KEY_UP && event.type != SDL_EVENT_MOUSE_BUTTON_UP;
}

} // namespace noxsdl
