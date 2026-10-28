// The server gone quiet, as the exe's client shows it (the client side: server_silence.hpp): the
// disconnect icon (DisconnectIcon, a 50 x 50 window at the screen's right edge, half way down) after
// 2 s without a datagram, and the lag window (discon.wnd, centred, modal: "The server is not
// responding. Do you want to wait or exit now?") after 22 s, its Wait (576) closing it for another
// 20 s and its Exit (577) leaving the game.
//
// And ours alone: the leaving notice (leaving_notice.hpp) while a leave waits for the server's ack,
// drawn as discon.wnd's window without its buttons (the exe's window stands frozen then; VD-33).
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class DisconWindow {
public:
    DisconWindow(gui::Resources res, const std::filesystem::path& windowDir, std::function<void()> onWait,
                 std::function<void()> onExit);

    /// Each frame: the icon and the window as the client's watch has them.
    void update(bool iconShown, bool windowShown, std::uint64_t nowMs);
    bool iconShown() const { return iconShown_; }
    bool windowShown() const { return window_ != nullptr && !window_->hidden(); }
    /// The leaving notice's text, or nothing to hide it (it takes no input).
    void setNotice(const std::optional<std::string>& text);
    bool noticeShown() const { return notice_ != nullptr && !notice_->hidden(); }
    void draw(SDL_Renderer* renderer, int width, int height);
    /// While the window is up it takes the mouse and keys.
    bool handleEvent(const SDL_Event& event, int width, int height);

private:
    gui::Gui gui_;
    gui::Gui iconGui_;
    gui::Gui noticeGui_;
    gui::Window* window_ = nullptr;
    gui::Window* icon_ = nullptr;
    gui::Window* notice_ = nullptr;
    gui::Window* noticeText_ = nullptr;
    bool iconShown_ = false;
    std::function<void()> onWait_, onExit_;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
