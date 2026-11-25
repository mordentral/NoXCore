// (noxcpp) The game controller in the menus and the game's windows (app.hpp's pad/pad_menu.cpp
// group): the D-pad, the left stick (in the menus), MenuAccept and the tabs as the window system's
// nav events (gui_nav.hpp), which go through SDL_AppEvent's order to the Gui that has the pad.
//
// Which Gui has it is gui::navOwner(): the one drawn last with somewhere for the pad to go
// (padMenuFrame turns the frame over). A press goes on as a nav event while a menu has the input
// (padHasMenus) or a game window has the pad (a shop's, a quest's, the briefing's load menu...);
// its edge stays padPress's, so a press no window takes is still the game's (a game window that
// leaves the game playing under it). A held D-pad direction repeats, as the left stick does in the
// menus (in a game window the stick still walks).
#include "app.hpp"
#include "gui_nav.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <optional>
#include <utility>

namespace noxsdl {

namespace {

using gui::NavInput;
using pad::PadAction;
using pad::PadButton;

/// A held direction's repeat: the first after kFirstMs, then every kEveryMs.
constexpr std::uint64_t kFirstMs = 380;
constexpr std::uint64_t kEveryMs = 110;
/// The left stick's tilt (after the dead zone) that counts as a press in the menus, and under
/// which it's let go.
constexpr float kStickPress = 0.55f;
constexpr float kStickLetGo = 0.35f;

struct Repeat {
    std::optional<PadButton> button;   ///< the D-pad's held direction
    std::optional<NavInput> stick;     ///< the stick's held direction
    NavInput input = NavInput::Up;
    std::uint64_t nextMs = 0;
    std::optional<NavInput> right;     ///< the right stick's held way (its jumps out of the focus's group)
    std::uint64_t rightNextMs = 0;
};
Repeat repeat;
bool wasConnected = false;

std::optional<NavInput> dpadInput(PadButton b)
{
    switch (b) {
    case PadButton::DpadUp: return NavInput::Up;
    case PadButton::DpadDown: return NavInput::Down;
    case PadButton::DpadLeft: return NavInput::Left;
    case PadButton::DpadRight: return NavInput::Right;
    default: return std::nullopt;
    }
}

void push(NavInput in)
{
    SDL_Event e = gui::navEvent(in);
    SDL_PushEvent(&e);
}

/// A stick's way in the menus (the larger of its two leans), with hysteresis.
std::optional<NavInput> stickWay(const App& app, std::optional<NavInput> held, bool right = false)
{
    SDL_Gamepad* g = app.pads.active();
    if (!g) {
        return std::nullopt;
    }
    const pad::PadSettings& s = app.pads.settings();
    const pad::StickVec v =
        pad::radialDeadZone({pad::axisUnit(SDL_GetGamepadAxis(g, right ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX)),
                             pad::axisUnit(SDL_GetGamepadAxis(g, right ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY))},
                            right ? s.rightDeadZone : s.deadZone, s.outerDeadZone);
    const float tilt = v.length();
    if (tilt < (held ? kStickLetGo : kStickPress)) {
        return std::nullopt;
    }
    if (std::fabs(v.x) > std::fabs(v.y)) {
        return v.x > 0 ? NavInput::Right : NavInput::Left;
    }
    return v.y > 0 ? NavInput::Down : NavInput::Up;
}

} // namespace

bool padMenusUp(const App& app)
{
    return padHasMenus(app) || gui::navOwner() != nullptr;
}

void padMenuInput(App& app, const pad::PadEdge& edge, SDL_Event& event)
{
    if (!padMenusUp(app)) {
        return;
    }
    std::optional<NavInput> in = dpadInput(edge.button);
    const pad::PadActionSet menu = app.padRouter.menuActions(edge.button);
    if (menu.has(PadAction::MenuAccept)) {
        in = NavInput::Accept;
    } else if (menu.has(PadAction::TabPrevious)) {
        in = NavInput::TabPrevious;
    } else if (menu.has(PadAction::TabNext)) {
        in = NavInput::TabNext;
    }
    if (!in) {
        return;
    }
    event = gui::navEvent(*in);
    if (dpadInput(edge.button)) {
        repeat.button = edge.button;
        repeat.input = *in;
        repeat.nextMs = SDL_GetTicks() + kFirstMs;
    }
}

void padMenuFrame(App& app)
{
    const bool connected = app.pads.connected();
    if (!connected) {
        if (std::exchange(wasConnected, false)) {
            gui::navFrame(false, false);
            repeat = {};
            oskFrame(app); // (the on-screen keyboard closes)
        }
        return;
    }
    wasConnected = true;
    gui::navFrame(true, app.pads.padInUse());
    oskFrame(app);
    const std::uint64_t now = SDL_GetTicks();
    if (oskOpen(app) || !padMenusUp(app)) { // (the on-screen keyboard has the pad's directions)
        repeat = {};
        return;
    }
    // The right stick: a push jumps out of the focus's group that way (repeating while held), in
    // the menus and the game's windows that have the pad.
    {
        const std::optional<NavInput> way = stickWay(app, repeat.right, true);
        if (way != repeat.right) {
            repeat.right = way;
            if (way) {
                push(gui::jumpOf(*way));
                repeat.rightNextMs = now + kFirstMs;
            }
        } else if (way && now >= repeat.rightNextMs) {
            push(gui::jumpOf(*way));
            repeat.rightNextMs = now + kEveryMs;
        }
    }
    // A held D-pad direction.
    if (repeat.button) {
        if (!app.pads.isDown(*repeat.button)) {
            repeat.button.reset();
        } else if (now >= repeat.nextMs) {
            push(repeat.input);
            repeat.nextMs = now + kEveryMs;
        }
    }
    // The left stick, in the menus (in a game window it walks).
    if (!padHasMenus(app) || repeat.button) {
        repeat.stick.reset();
        return;
    }
    const std::optional<NavInput> way = stickWay(app, repeat.stick);
    if (way != repeat.stick) {
        repeat.stick = way;
        if (way) {
            push(*way);
            repeat.nextMs = now + kFirstMs;
        }
    } else if (way && now >= repeat.nextMs) {
        push(*way);
        repeat.nextMs = now + kEveryMs;
    }
}

} // namespace noxsdl
