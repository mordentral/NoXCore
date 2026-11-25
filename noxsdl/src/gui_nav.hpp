// (noxcpp) The game controller in the window system: a navigation focus over a Gui's windows,
// moved by the D-pad and the left stick, separate from the typing focus (Gui::focus).
//
//   candidates  the windows a click at their middle would reach (hitTop's walk: the modal one's
//               alone when there is one; hidden ones and what's in them skipped, disabled ones
//               and what's in them skipped, NOINPUT ones skipped), of the kinds that take a click:
//               push, radio and check buttons, list boxes, horizontal sliders and entry fields
//               (upright sliders are list scroll bars: their list moves instead). A window can be
//               put in or left out (Window::nav), or take the pad's input itself (Window::padInput).
//   moving      the nearest candidate the way pressed (pickInDirection); the focus does what the
//               mouse's pointing does (Gui's hover, kLit, Hover / Unhover to the parent), so a
//               screen's sounds, tooltips and picks follow it as they follow the mouse.
//   in place    left / right step a horizontal slider (SliderChanged); up / down move a list's
//               line (Clicked, as a click on it) and keep it in view, and leave the list at its ends.
//   accept      activate(): what a click does to that kind of window, the mouse's release's own.
//   settling    a screen opened, or the focus gone (hidden, disabled, covered): a popup's windows
//               take it with the old focus kept to come back to once the popup goes; else the
//               screen's default (Gui::setNavDefault), the one nearest where it was, or the first
//               in reading order.
//
// Which Gui has the pad (the coordinator): the last one drawn with a candidate in the frame before
// (the game's windows draw bottom to top, so that's the one on top), from navFrame on. Only it
// takes the pad's input and draws the focus's outline, and only while the pad is the device in use.
// Without a pad nothing here runs.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace noxsdl::gui {

class Window;
class Gui;

/// What the pad asks of a menu.
enum class NavInput : std::uint8_t { Up, Down, Left, Right, Accept, TabPrevious, TabNext };

/// The SDL event the pad's menu input travels as (an SDL user event of its own, registered once):
/// through SDL_AppEvent's order like any other, to the Gui that takes it.
Uint32 navEventType();
SDL_Event navEvent(NavInput input);
/// The input an event carries, if it's a nav event.
std::optional<NavInput> navInput(const SDL_Event& event);
inline bool isNavEvent(const SDL_Event& event)
{
    return navInput(event).has_value();
}

/// The keyboard id on the Escape the pad's Back button sends (a made-up one): a screen that wants
/// the pad's Back to do less than Escape does (shut a drop-down, not the window) tells them apart.
inline constexpr SDL_KeyboardID kPadKeyboard = 0x7AD0'0B4Cu;
inline bool padEscape(const SDL_Event& event)
{
    return event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && event.key.which == kPadKeyboard;
}

/// The coordinator's frame: each frame (the pad layer's, only while a pad is connected) the Gui
/// that claimed last frame becomes the owner. `inUse`: the pad is the device in use (the outline shows).
void navFrame(bool connected, bool inUse);
/// The Gui that has the pad now (null: none, or no pad).
Gui* navOwner();
/// Whether a pad is connected / the device in use (navFrame's).
bool navConnected();
bool navPadInUse();
/// A Gui drawn this frame with a candidate claims the pad for the next.
void navClaim(Gui* gui);
/// A Gui going: never the owner after.
void navForget(const Gui* gui);

/// The pad's A gave an entry field the typing focus (Gui::navigate): noted for the on-screen
/// keyboard (pad/osk_game.cpp), which takes it once.
struct PadTyping {
    Gui* gui = nullptr;
    Window* field = nullptr;
};
void notePadTyping(Gui* gui, Window* field);
std::optional<PadTyping> takePadTyping();

/// The rectangle a window is on the 640 x 480 screen.
SDL_Rect screenRect(const Window& w);
/// The nearest of `candidates` from `from` the way `dir` (Up, Down, Left, Right) goes: its middle
/// past `from`'s middle that way; scored four times the gap that way, thirty times the gap across
/// (none when they overlap across: a window in line comes first) and the offset of their middles
/// across; the least wins.
/// -1 for none that way.
int pickInDirection(const SDL_Rect& from, std::span<const SDL_Rect> candidates, NavInput dir);

/// The top-level windows a Gui searches, as hitTop does.
struct Scene {
    std::span<const std::unique_ptr<Window>> tops;
    Window* modal = nullptr;
};
/// The window under (x, y) in `w` (press: ENABLED ones only, as a click), or null.
Window* hitWindow(Window& w, int x, int y, bool press);
/// The same over a scene: the modal window alone if set, else ABOVE ones, the rest, BELOW ones,
/// the newest first in each.
Window* hitScene(const Scene& scene, int x, int y, bool press);

/// Whether a window is the kind the pad goes to (its type, or Window::nav / padInput).
bool navKind(const Window& w);
/// Whether it and every window it's in are shown, and it's enabled.
bool shownAndEnabled(const Window& w);
/// The scene's candidates, in no particular order.
std::vector<Window*> navCandidates(const Scene& scene);

/// The mouse's pointing moved to `to` (null: nothing): the window left loses its light and its
/// parent hears Unhover (a pushed button left is let go), the one come to is lit and its parent
/// hears Hover, for push, radio and check buttons that track the mouse.
void hoverTo(Window*& hover, Window* to);
/// What a click does to `w` (the mouse's release on it, the pad's accept): a push button clicks
/// (let go first), a check box flips then clicks, a radio button not on goes on (its group's
/// others off) and clicks, a list box clicks on its selected line (none: nothing), an entry field
/// takes the typing focus and clicks, anything else clicks. A disabled one does nothing.
void activate(Window& w, Window*& typing);
/// A slider stepped (+1 towards its maximum, -1 towards its minimum) by a 20th of its range (at
/// least 1); its parent hears SliderChanged when it moved. False at its end.
bool stepSlider(Window& w, int dir);
/// A list's line moved down (+1) or up (-1): the line selected, kept in view (the parent hears
/// Scrolled when it scrolled), and clicked (Window::navLineClicks). False at its end.
bool stepList(Window& w, int dir);
/// Scrolls a list so its selected line shows; whether it scrolled.
bool keepLineInView(Window& w);

/// A Gui's navigation focus and what it comes back to.
class Navigator {
public:
    Window* focus() const { return focus_; }
    /// The window to start on when the focus is settled next (a screen's default; spent once used).
    void setDefault(Window* w) { default_ = w; }
    /// The focus on `w` (null: none), the pointing with it.
    void focusOn(Window* w, Window*& hover);
    /// Makes the focus one of `candidates`: kept if it is; a window still shown but out of reach (a
    /// popup over it) kept to come back to; then a kept one that's back in reach, the default, the
    /// one nearest where the focus was (hidden or disabled since), the first in reading order.
    /// False when there are no candidates. `passive` (the drawing's, not a press's): a focus only
    /// greyed for now (shown, disabled) is left where it is, to come back to once it's usable.
    bool settle(const std::vector<Window*>& candidates, Window*& hover, bool passive = false);
    /// One input from the pad over `scene` (settled first: the first press only shows the focus).
    /// `typing`: the Gui's typing focus; `tabs`: the screen's tabs (-1 / +1; true when it switched).
    /// Whether it took the input.
    bool input(NavInput in, const Scene& scene, Window*& hover, Window*& typing, const std::function<bool(int)>& tabs);
    /// Every window of `top` (a top-level window going) forgotten.
    void forget(const Window* top);
    void clear();

private:
    Window* focus_ = nullptr;
    Window* default_ = nullptr;
    std::vector<Window*> restore_; ///< the focus before each popup, the latest last
    std::optional<SDL_Rect> lost_; ///< where the focus was when it went (hidden or disabled)
};

} // namespace noxsdl::gui
