// A script mode UI's pointer in noxcore.exe (script_mode_ui.cpp), with no SDL in it so the tests
// drive it:
//  - which of the UI's windows a point goes to, as they're drawn: the others newest first, then
//    the HUD pieces (drawn under them); never a click-through window or a hidden HUD piece, nor a
//    frameless window's point outside the hit areas it declared (API 4); and for a UI of API 3
//    never a point a game window drawn over that kind of window holds (`covered`);
//  - a press's life (UI API 3): a drag once it has moved kDragStart units, its moves told once a
//    frame, its end at the button's release whoever has the pointer then (the program hands every
//    release here first: ModeUi::release), or when the pointer is lost (focus) or the UI stops.
#pragma once

#include "noxcore/modes/script/script_ui.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace noxsdl::modeui {

inline constexpr float kTitleH = 13;  ///< a framed window's title bar (UI units)
inline constexpr float kDragStart = 3; ///< (UI units a press moves before it's a drag)

/// A window a point goes to: its id, its inside's top left, and whether the point is inside it
/// (else on its frame or title bar).
struct Hit {
    std::string window;
    float x = 0, y = 0;
    bool inside = false;
};

/// The window of `ui` at (x, y) (UI units). `frame`: a framed window's frame and title bar count.
/// `covered(hud)`: a game window drawn over a mod window of that kind holds the point (asked only
/// for a UI of API 3: an API 1 or 2 UI's windows take what they always took).
template <class Covered>
std::optional<Hit> hitWindow(const nox::modes::script::ScriptUiClient& ui, float x, float y, bool frame, Covered&& covered)
{
    const bool api3 = ui.api() >= 3;
    for (const bool hud : {false, true}) {
        const auto& windows = ui.windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            const nox::modes::script::UiWindow& w = *it;
            if (w.hud != hud || w.clickThrough || (w.hud && !ui.hudShown())) {
                continue;
            }
            const float edge = frame && w.frame ? 2.0f : 0.0f;
            const float title = frame && w.frame ? kTitleH : 0.0f;
            const float wx = static_cast<float>(w.x), wy = static_cast<float>(w.y);
            if (!(x >= wx - edge && x < wx + static_cast<float>(w.w) + edge && y >= wy - title - edge && y < wy + static_cast<float>(w.h) + edge)) {
                continue;
            }
            if (!w.frame && !w.hitAreas.empty() && !nox::modes::script::windowTakes(w, x, y)) {
                continue; // (API 4: a frameless window with hit areas, outside them: click-through)
            }
            if (api3 && covered(w.hud)) {
                return std::nullopt; // (a game window over it has the point: the game's)
            }
            return Hit{w.id, wx, wy, x >= wx && x < wx + static_cast<float>(w.w) && y >= wy && y < wy + static_cast<float>(w.h)};
        }
    }
    return std::nullopt;
}

/// (API 3) A press in one of the UI's windows and its drag.
class Pointer {
public:
    /// `button` (1 left, 2 right) pressed at (x, y) in `window` of `ui`, whose inside was at (wx, wy).
    void press(const nox::modes::script::ScriptUiClient& ui, std::string window, int button, float x, float y, float wx, float wy)
    {
        press_ = Press{std::move(window), button, x, y, x, y, x, y, wx, wy, false, ui.runs()};
    }
    /// The mouse moved to (x, y): told to the UI at the next frame().
    void moved(float x, float y)
    {
        if (press_) {
            press_->atX = x;
            press_->atY = y;
        }
    }
    /// Once a frame: the drag's start (once it has moved kDragStart units, at the press's place)
    /// and its move to where the mouse went. A UI no longer of API 3: the press let go.
    void frame(nox::modes::script::ScriptUiClient& ui)
    {
        if (!press_ || stale(ui) || (press_->atX == press_->toldX && press_->atY == press_->toldY)) {
            return;
        }
        if (!press_->dragging) {
            if (std::hypot(press_->atX - press_->fromX, press_->atY - press_->fromY) < kDragStart) {
                return;
            }
            press_->dragging = true;
            ui.drag(press_->window, nox::modes::script::UiDrag::Start, press_->fromX - press_->wx, press_->fromY - press_->wy, press_->button);
        }
        if (press_) {
            press_->toldX = press_->atX;
            press_->toldY = press_->atY;
            ui.drag(press_->window, nox::modes::script::UiDrag::Move, press_->atX - press_->wx, press_->atY - press_->wy, press_->button);
        }
    }
    /// `button` let go at (x, y), `over` the window there (none: none, or a menu up). False: not
    /// the press's button (or no press): the release goes on to the game. True: the press ended
    /// (the drag's end, then mouseUp over a window) and the release is the UI's, whatever is on
    /// top now (nothing else saw its press).
    bool release(nox::modes::script::ScriptUiClient& ui, int button, float x, float y, const std::optional<Hit>& over)
    {
        if (!press_ || stale(ui) || button != press_->button) {
            return false;
        }
        const Press p = std::move(*press_);
        press_.reset();
        if (p.dragging) {
            ui.drag(p.window, nox::modes::script::UiDrag::End, x - p.wx, y - p.wy, p.button);
        }
        if (over && over->inside) {
            ui.mouseUp(over->window, x - over->x, y - over->y, p.button);
        }
        return true;
    }
    /// The pointer lost (the window's focus gone, its release going elsewhere): a drag ends where
    /// the mouse last was; no mouseUp.
    void lost(nox::modes::script::ScriptUiClient* ui)
    {
        if (press_ && ui && !stale(*ui) && press_->dragging) {
            const Press p = std::move(*press_);
            press_.reset();
            ui->drag(p.window, nox::modes::script::UiDrag::End, p.atX - p.wx, p.atY - p.wy, p.button);
        }
        press_.reset();
    }
    void reset() { press_.reset(); }
    bool pressed() const noexcept { return press_.has_value(); }
    bool dragging() const noexcept { return press_ && press_->dragging; }

private:
    struct Press {
        std::string window;
        int button = 1;
        float fromX = 0, fromY = 0, atX = 0, atY = 0, toldX = 0, toldY = 0;
        float wx = 0, wy = 0;
        bool dragging = false;
        std::uint32_t runs = 0; ///< (the UI's run it was made in)
    };
    /// The UI stopped or replaced since the press (none of API 3 now, or another run): it's let
    /// go, nothing told.
    bool stale(const nox::modes::script::ScriptUiClient& ui)
    {
        if (ui.api() >= 3 && ui.runs() == press_->runs) {
            return false;
        }
        press_.reset();
        return true;
    }
    std::optional<Press> press_;
};

} // namespace noxsdl::modeui
