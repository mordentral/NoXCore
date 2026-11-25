// (noxcpp) The input mode in the program (app.hpp's pad/pad_mode.cpp group): which the player plays
// with, the game controller or the mouse and keyboard, chosen once a pad is connected
// (pad/input_mode.hpp says when it's asked; input_mode_ask.hpp is the question).
//
//   the pad's      the mouse is nobody's: its moves, clicks and wheel are dropped before anything
//                  sees them (the pad's own made-up clicks, GamepadManager::kPadMouse, go on; the
//                  map editor keeps the mouse), the mouse's pointer is never drawn and no hover
//                  follows where it was left. The keyboard still types (chat, names, Escape).
//   the mouse's    the pad is nobody's: no presses, no prompts, no navigation (padEventFirst drops
//                  its presses), as before there was pad support.
//
// Asked: at start-up with a pad there, before the movies (a moment's wait for a pad SDL is still
// finding); later, when a pad comes while the mouse's mode is in use, once something may ask (a
// menu, never in a game's fight: in a game the game's menu, Escape, brings it up). The last pad
// going goes back to the mouse's by itself. Not kept between runs: each start asks again (the
// Controller screen's Use switches without restarting). With no pad nothing here runs a frame.
#include "app.hpp"
#include "gui_nav.hpp"

#include "noxcore/strings/l10n.hpp"

#include <SDL3/SDL.h>

namespace noxsdl {

namespace {

/// A pad SDL may still be finding at start-up is waited for this long before the movies start.
constexpr std::uint64_t kStartWaitMs = 300;

/// Whether the question may come up now: not over a movie, the legal screen or the editor; in a
/// game only over its menus (never in the middle of a fight).
bool mayAsk(const App& app)
{
    if (app.editor || (app.movie && app.movie->active()) || app.legal) {
        return false;
    }
    if (!app.live || app.joinWaiting) {
        return true;
    }
    return menuWindowUp(app);
}

void runThen(App& app)
{
    if (auto then = std::exchange(app.padModeThen, {})) {
        then();
    }
}

void openAsk(App& app)
{
    if (!app.padModeAsk) {
        app.padModeAsk = std::make_unique<InputModeAsk>(guiResources(app), app.gameDir / "window");
    }
    app.padModeChoice.opened();
    // (The pad heard while it asks: its D-pad, stick and A on the choices, the focus outlined.)
    app.pads.setMode(pad::InputDevice::Pad);
    SDL_Log("input mode: asked which to play with");
    app.padModeAsk->open([&app](bool pad) {
        app.padModeChoice.answered(pad);
        setInputMode(app, pad);
        runThen(app);
    });
}

} // namespace

void setInputMode(App& app, bool pad)
{
    const pad::InputDevice mode = pad ? pad::InputDevice::Pad : pad::InputDevice::MouseKeyboard;
    app.padModeChoice.chose(pad);
    if (app.pads.padInUse() != (pad && app.pads.connected()) && app.input) {
        app.input->releaseAll(); // (what either held let go: the pad's layer follows, padEventFirst)
    }
    app.pads.setMode(mode);
    if (!pad && oskOpen(app)) {
        app.osk.osk.close();
    }
    if (pad && !app.editor) {
        SDL_HideCursor(); // (the system's pointer too: none shows in the pad's mode)
    }
    SDL_Log("input mode: %s", pad ? "the game controller" : "the mouse and keyboard");
}

void padModeStart(App& app, std::function<void()> start)
{
    app.padModeThen = std::move(start);
    if (app.padModeAskForced) {
        openAsk(app); // (--input-mode ask: a test of the question)
        return;
    }
    if (app.padModeForced) {
        runThen(app); // (set by startPads: --input-mode, or a test run's)
        return;
    }
    if (SDL_HasGamepad()) {
        openAsk(app);
        return;
    }
    // (SDL may find a pad a moment after its start: the start-up waits that long for one.)
    app.padModeWaitUntilMs = SDL_GetTicks() + kStartWaitMs;
}

bool padModeAsking(const App& app)
{
    return app.padModeAsk && app.padModeAsk->isOpen();
}

bool padModeHearsPad(const App& app)
{
    return padModeAsking(app) || (app.controllerWindow && app.controllerWindow->isOpen() && app.controllerWindow->capturing());
}

bool padModeEvent(App& app, const SDL_Event& event)
{
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        // (GamepadManager has opened it; once it's open the mode in use is known.)
        app.padModeChoice.padArrived(app.pads.padInUse(), app.pads.fellBack());
        if (app.padModeChoice.due() && app.live && !mayAsk(app)) {
            centredMessage(app, NOX_TR("input_mode.later", "A game controller is connected: open the game's menu to choose it."));
        }
        return false;
    }
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && !app.pads.connected()) {
        app.padModeChoice.lastPadGone();
        if (padModeAsking(app)) {
            app.padModeAsk->close();
            setInputMode(app, false);
            runThen(app);
        }
        return false;
    }
    if (padModeAsking(app)) {
        // The question has everything (but the window's own events and the close).
        if (event.type == SDL_EVENT_QUIT || (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)) {
            return false;
        }
        SDL_Event e = event;
        if (mapsPointer(app)) {
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                intoGameArea(app, e.motion.x, e.motion.y);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                intoGameArea(app, e.button.x, e.button.y);
            }
        }
        app.padModeAsk->handleEvent(e, app.camera.viewWidth, app.camera.viewHeight);
        return true;
    }
    // The pad's mode: the mouse is nobody's (the pad's own clicks aside; the editor keeps it).
    if (app.pads.padInUse() && !app.editor) {
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION: return event.motion.which != pad::GamepadManager::kPadMouse;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: return event.button.which != pad::GamepadManager::kPadMouse;
        case SDL_EVENT_MOUSE_WHEEL: return event.wheel.which != pad::GamepadManager::kPadMouse;
        default: break;
        }
    }
    return false;
}

void padModeFrame(App& app)
{
    if (app.padModeWaitUntilMs != 0) {
        // The start-up's wait for a pad SDL is still finding: asked once one is open, else on.
        if (app.pads.connected()) {
            app.padModeWaitUntilMs = 0;
            openAsk(app);
        } else if (SDL_GetTicks() >= app.padModeWaitUntilMs) {
            app.padModeWaitUntilMs = 0;
            runThen(app);
        }
        return;
    }
    if (padModeAsking(app)) {
        app.padModeAsk->update(SDL_GetTicks());
        return;
    }
    if (app.padModeChoice.due() && app.padModeChoice.askNow(app.pads.connected() && mayAsk(app))) {
        openAsk(app);
    }
}

void drawPadModeAsk(App& app)
{
    if (padModeAsking(app)) {
        app.padModeAsk->draw(app.renderer, app.camera.viewWidth, app.camera.viewHeight);
    }
}

} // namespace noxsdl
