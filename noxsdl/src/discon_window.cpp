// The disconnect icon and the lag window (discon_window.hpp).
#include "discon_window.hpp"

#include "hud.hpp"

#include <array>

namespace noxsdl {

namespace {

// discon.wnd
constexpr std::int32_t kWait = 576, kExit = 577, kMessage = 578;
constexpr int kIconSize = 50;
// The leaving notice: discon.wnd's window cut to the text's height, without its buttons.
constexpr int kNoticeHeight = 100;

} // namespace

DisconWindow::DisconWindow(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> onWait,
                           std::function<void()> onExit)
    : gui_(res), iconGui_(res), noticeGui_(res), onWait_(std::move(onWait)), onExit_(std::move(onExit))
{
    notice_ = noticeGui_.load(windowDir / "discon.wnd", [](gui::Window&, int, gui::Window&) { return true; });
    if (notice_) {
        for (const std::int32_t id : {kWait, kExit}) {
            if (gui::Window* b = notice_->find(id)) {
                b->show(false);
            }
        }
        notice_->rect.h = kNoticeHeight;
        gui::centreOnScreen(*notice_);
        noticeText_ = notice_->find(kMessage);
        if (noticeText_) {
            noticeText_->rect = SDL_Rect{8, 8, notice_->rect.w - 16, kNoticeHeight - 16};
            noticeText_->text.clear();
        }
        notice_->show(false);
    }
    window_ = gui_.load(windowDir / "discon.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        if (from.id == kWait) {
            deferred_.push_back([this] {
                if (onWait_) {
                    onWait_();
                }
            });
            return true;
        }
        if (from.id == kExit) {
            deferred_.push_back([this] {
                if (onExit_) {
                    onExit_();
                }
            });
            return true;
        }
        return false;
    });
    if (window_) {
        // Centred on the screen (its 150 x 140 at the middle of the layout).
        gui::centreOnScreen(*window_);
        window_->show(false);
    }
    // The icon: a window of its own at the screen's right edge, half way down
    // (win_width - 50, win_height / 2 + 3), drawing DisconnectIcon; it takes no input.
    nox::gui::WindowDef def;
    def.x = 640 - kIconSize;
    def.y = 480 / 2 + 3;
    def.width = kIconSize;
    def.height = kIconSize;
    def.typeName = "USER";
    def.type = nox::gui::windowType("USER");
    def.status = nox::gui::status::Enabled;
    icon_ = iconGui_.load(def, {});
    if (icon_) {
        icon_->draw = [](gui::Window& self, gui::Painter& p) {
            const SDL_Point at = self.screenPos();
            p.image("DisconnectIcon", at.x, at.y);
        };
        icon_->show(false);
    }
    // At the game's scale, its right edge the screen's, the layout's middle the screen's.
    iconGui_.setPlacement([](int w, int h) {
        const float s = Hud::scale(w, h);
        return std::array<float, 3>{s, static_cast<float>(w) - 640 * s, (static_cast<float>(h) - 480 * s) / 2};
    });
}

void DisconWindow::update(bool iconShown, bool windowShown, std::uint64_t nowMs)
{
    if (icon_ && iconShown != iconShown_) {
        icon_->show(iconShown);
    }
    iconShown_ = iconShown;
    if (window_ && windowShown != this->windowShown()) {
        // Shown modal with the focus (wndShowModalMB); hidden, the input back to the game.
        window_->show(windowShown);
        gui_.setModal(windowShown ? window_ : nullptr);
    }
    gui_.update(nowMs);
    gui::runQueued(deferred_);
}

void DisconWindow::setNotice(const std::optional<std::string>& text)
{
    if (!notice_) {
        return;
    }
    if (noticeText_ && text) {
        noticeText_->text = *text;
    }
    if (text.has_value() == notice_->hidden()) {
        notice_->show(text.has_value());
    }
}

void DisconWindow::draw(SDL_Renderer* renderer, int width, int height)
{
    if (iconShown_) {
        iconGui_.draw(renderer, width, height);
    }
    if (windowShown()) {
        gui_.draw(renderer, width, height);
    }
    if (noticeShown()) {
        noticeGui_.draw(renderer, width, height);
    }
}

bool DisconWindow::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!windowShown()) {
        return false;
    }
    gui_.handleEvent(event, width, height);
    // (modal: the game gets nothing but the quit and the releases)
    return event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_KEY_UP && event.type != SDL_EVENT_MOUSE_BUTTON_UP;
}

} // namespace noxsdl
