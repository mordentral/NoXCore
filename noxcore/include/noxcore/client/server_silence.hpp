// The client's watch on a server gone quiet, as the exe's client runs it each frame (its network
// pump after the frame's receive):
//   - every datagram from the server notes the time (the client's packet handler);
//   - over 2000 ms since the last: the disconnect icon (DisconnectIcon, at the screen's right edge)
//     shown, and the time noted as the lag window's base;
//   - over 20000 ms since the last datagram and over 20000 ms since that base: the lag window
//     (discon.wnd: "The server is not responding. Do you want to wait or exit now?", Wait / Exit),
//     modal, and MSG_NEED_TIMESTAMP (0x29) sent to the server, reliably;
//   - a datagram hides the icon, and closes the lag window with its base reset to then;
//   - Wait closes the window with its base reset to then: it comes back 20 s later if the server
//     is still quiet. Exit leaves the game (Game_FinishQuit) and then shows the Timeout notice.
// The exe's client never leaves on its own: the player chooses Exit.
#pragma once

#include <cstdint>

namespace nox::client {

class ServerSilence {
public:
    static constexpr std::uint32_t kIconAfterMs = 2000;
    static constexpr std::uint32_t kWindowAfterMs = 20000;

    /// A datagram came from the server at `nowMs`.
    void heard(std::uint32_t nowMs) noexcept
    {
        lastHeard_ = nowMs;
        heardAny_ = true;
        icon_ = false;
        if (window_) {
            window_ = false;
            base_ = nowMs;
        }
    }

    /// The frame's check. Returns true when the lag window has just opened (the caller sends
    /// MSG_NEED_TIMESTAMP).
    bool tick(std::uint32_t nowMs) noexcept
    {
        if (!heardAny_) {
            return false;
        }
        const std::uint32_t quiet = nowMs - lastHeard_;
        if (quiet > kIconAfterMs && !icon_) {
            icon_ = true;
            base_ = nowMs;
        }
        if (quiet > kWindowAfterMs && !window_ && nowMs - base_ > kWindowAfterMs) {
            window_ = true;
            return true;
        }
        return false;
    }

    /// The lag window's Wait: closed, shown again 20 s on if nothing comes.
    void wait(std::uint32_t nowMs) noexcept
    {
        base_ = nowMs;
        window_ = false;
    }

    /// Forgets everything (a new connection, or the watch turned off).
    void reset() noexcept { *this = ServerSilence{}; }

    bool iconShown() const noexcept { return icon_; }
    bool windowShown() const noexcept { return window_; }
    /// Milliseconds since the last datagram (0 before the first).
    std::uint32_t quietFor(std::uint32_t nowMs) const noexcept { return heardAny_ ? nowMs - lastHeard_ : 0; }

private:
    std::uint32_t lastHeard_ = 0;
    std::uint32_t base_ = 0;
    bool heardAny_ = false;
    bool icon_ = false;
    bool window_ = false;
};

} // namespace nox::client
