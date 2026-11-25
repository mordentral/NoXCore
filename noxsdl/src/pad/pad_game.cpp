// (noxcpp) The game controller in the program (app.hpp's pad/pad_game.cpp group): the pads started
// with the window, noxcore_controller.cfg read and written, a pad's buttons routed into the game's
// controls (InputBindings, under pad codes) or the pad's own actions, the left stick walking, and
// --pad-script's made-up pad.
//
// Where a pad's event goes (main.cpp's SDL_AppEvent):
//  - first of all padEventFirst: the pads kept (GamepadManager), and, while a menu or a window that
//    owns Escape has the input (padHasMenus), MenuBack's button (East, Start) sent on as Escape;
//  - a release reaches padRelease with the keys' releases, whatever has the input;
//  - a press that nothing above the game's own controls took reaches padPress, which fires its
//    controls only while the game has the input (padGameInput: no chat, console, in-game menu,
//    Options or movie up, the window focused).
// What the pad holds is let go whenever InputBindings::releaseAll is (the pad's layer sees its
// releases() move on), as the keys' are.
#include "app.hpp"
#include "backend_hooks.hpp"
#include "cursor_pick.hpp"
#include "pad/pad_play.hpp"

#include "noxcore/io/file.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace noxsdl {

namespace {

using pad::PadAction;
using pad::PadButton;

std::filesystem::path padConfigPath(const App& app)
{
    return backend::settingsDirectory(app.gameDir) / "noxcore_controller.cfg";
}

/// The editor has the window's input (its panels, or the play-test's game in its Map panel without
/// the input): pad events are nobody's then.
bool editorHasInput(const App& app)
{
    if (!app.editor) {
        return false;
    }
    if (gameInPanel(app)) {
        return !app.playTestCaptured;
    }
    return !app.playTestInGame && !app.sessionInGame;
}

/// What the pad holds let go of, its game controls' codes released one by one.
void releasePadHeld(App& app)
{
    for (std::size_t i = 0; i < pad::kPadButtonCount; ++i) {
        const auto b = static_cast<PadButton>(i);
        if (!app.padRouter.pressed(b)) {
            continue;
        }
        const auto e = app.padRouter.release(b);
        if (e.code && app.input) {
            app.input->release(e.code);
        }
        for (std::size_t a = 0; a < pad::kPadActionCount; ++a) {
            if (e.actions.has(static_cast<PadAction>(a))) {
                padAction(app, static_cast<PadAction>(a), false);
            }
        }
    }
    app.padRouter.dropAll();
    app.padEscapes = 0;
    padActionsDropped(app);
}

/// InputBindings forgot what's held since we last looked: so does the pad.
void followReleases(App& app)
{
    if (app.input && app.input->releases() != app.padReleasesSeen) {
        app.padReleasesSeen = app.input->releases();
        app.padRouter.dropAll();
        app.padEscapes = 0;
        padActionsDropped(app);
    }
}

} // namespace

void startPads(App& app)
{
    const char* base = SDL_GetBasePath();
    if (!app.padScript.empty()) {
        // (A test's made-up pad: its events whether the window has the focus or not, a hidden
        // window's never has it.)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    if (!app.pads.init(base ? std::filesystem::path(reinterpret_cast<const char8_t*>(base)) / "gamecontrollerdb.txt"
                            : std::filesystem::path())) {
        return;
    }
    loadPadConfig(app);
    if (!app.padScript.empty()) {
        app.scriptPad = std::make_unique<pad::VirtualPad>("NoX Core script pad");
        if (!*app.scriptPad) {
            app.scriptPad.reset();
            app.padScript.clear();
        }
    }
}

void loadPadConfig(App& app)
{
    pad::PadConfig config = pad::defaultPadConfig();
    const auto path = padConfigPath(app);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        config = pad::parsePadConfig(text.str());
    }
    applyPadConfig(app, std::move(config));
}

void applyPadConfig(App& app, pad::PadConfig config)
{
    releasePadHeld(app);
    app.padConfig = std::move(config);
    app.padRouter.configure(app.padConfig);
    app.pads.setSettings(app.padConfig.settings);
    if (app.input) {
        app.input->setPadBindings(app.padRouter.gameBindings());
    }
}

bool savePadConfig(const App& app)
{
    const auto path = padConfigPath(app);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pad::formatPadConfig(app.padConfig);
    if (!out) {
        SDL_Log("%s: couldn't write it", nox::pathUtf8(path).c_str());
        return false;
    }
    return true;
}

bool padGameInput(const App& app)
{
    return app.live && !app.joinWaiting && app.padFocused && !editorHasInput(app) && !(app.movie && app.movie->active()) &&
           !(app.quitMenu && app.quitMenu->isOpen()) && !(app.chat && app.chat->isOpen()) &&
           !(app.console && app.console->isOpen()) && !(app.options && app.options->isOpen()) &&
           !(app.inputConfig && app.inputConfig->isOpen());
}

bool padHasMenus(const App& app)
{
    if (editorHasInput(app)) {
        return false;
    }
    if (!app.live || app.joinWaiting || (app.movie && app.movie->active())) {
        return true;
    }
    return (app.quitMenu && app.quitMenu->isOpen()) || (app.chat && app.chat->isOpen()) ||
           (app.console && app.console->isOpen()) || (app.options && app.options->isOpen()) ||
           (app.inputConfig && app.inputConfig->isOpen());
}

std::optional<pad::PadEdge> padEventFirst(App& app, SDL_Event& event)
{
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        app.padFocused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        if (!app.padFocused) {
            releasePadHeld(app);
        }
    }
    const auto edge = app.pads.handleEvent(event);
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        releasePadHeld(app); // (its releases won't come)
        return std::nullopt;
    }
    if (!edge) {
        return std::nullopt;
    }
    followReleases(app);
    const unsigned bit = 1u << static_cast<unsigned>(edge->button);
    // A menu, or a window that owns Escape (chat, the console), has the input: MenuBack's button is
    // Escape there (each screen's own Escape); the other menu actions are told to padMenuAction.
    if (edge->down && padHasMenus(app)) {
        const auto menu = app.padRouter.menuActions(edge->button);
        for (std::size_t a = 0; a < pad::kPadActionCount; ++a) {
            if (menu.has(static_cast<PadAction>(a)) && static_cast<PadAction>(a) != PadAction::MenuBack) {
                padMenuAction(app, static_cast<PadAction>(a));
            }
        }
        if (menu.has(PadAction::MenuBack)) {
            app.padEscapes |= bit;
            event = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
            return std::nullopt;
        }
    } else if (!edge->down && (app.padEscapes & bit)) {
        app.padEscapes &= ~bit;
        event = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
        return std::nullopt;
    }
    return edge;
}

void padRelease(App& app, const pad::PadEdge& edge)
{
    followReleases(app);
    if (padPlayRelease(app, edge)) {
        return; // (the free cursor's button)
    }
    const auto e = app.padRouter.release(edge.button);
    if (e.code && app.input) {
        app.input->release(e.code);
    }
    for (std::size_t a = 0; a < pad::kPadActionCount; ++a) {
        if (e.actions.has(static_cast<PadAction>(a))) {
            padAction(app, static_cast<PadAction>(a), false);
        }
    }
}

void padPress(App& app, const pad::PadEdge& edge)
{
    followReleases(app);
    if (!padGameInput(app) || !app.input) {
        return;
    }
    if (padPlayPress(app, edge)) {
        return; // (the free cursor's click)
    }
    const auto modifier = app.padRouter.modifier();
    const auto e = app.padRouter.press(edge.button, modifier && app.pads.isDown(*modifier));
    const std::vector<std::string> controls = e.code ? app.input->press(e.code) : std::vector<std::string>{};
    // (A spell waiting for its target fired instead; a slot's cast aimed first: pad_play.cpp.)
    if (padPlayControls(app, e.actions, controls)) {
        return;
    }
    for (std::size_t a = 0; a < pad::kPadActionCount; ++a) {
        if (e.actions.has(static_cast<PadAction>(a))) {
            padAction(app, static_cast<PadAction>(a), true);
        }
    }
    if (e.code) {
        // The game's controls, as a key's: the held ones (Action...) act each frame while it's down.
        liveControls(app, controls);
    }
}

void padAction(App& app, pad::PadAction action, bool down)
{
    // The pad's own game actions (Interact, TargetNext / TargetPrevious, FreeCursor: pad_play.cpp);
    // Modifier is the router's own (held: the second layer).
    padPlayAction(app, action, down);
}

void padActionsDropped(App& app)
{
    padPlayDropped(app); // (the free cursor's buttons)
}

void padMenuAction(App& /*app*/, pad::PadAction action)
{
    // The menus' MenuAccept and tabs (MenuBack goes on as Escape: padEventFirst). Not handled yet.
    (void)action;
}

PadSteer padSteer(App& app)
{
    if (!app.pads.connected()) {
        if (app.padPlay.hasPointer || app.padPlay.collecting) {
            padPlayIdle(app); // (the last pad gone)
        }
        return PadSteer::None;
    }
    app.pads.sample(SDL_GetTicksNS());
    followReleases(app);
    if (!app.pads.padInUse()) {
        app.padAim.reset();
        padPlayIdle(app);
        return PadSteer::None;
    }
    const auto at = app.live->client().position();
    if (!padGameInput(app) || app.view3d.active() || !at || (app.cinematics && app.cinematics->screenUp())) {
        padPlayIdle(app); // (a briefing's screen: the mouse's pointer, as it has the mouse)
        return PadSteer::None;
    }
    // The stick's walk (its cursor point out by its tilt), the aim and the target, the pointer
    // (pad_play.cpp).
    return padPlaySteer(app, static_cast<float>(at->first), static_cast<float>(at->second));
}

void runPadScript(App& app, std::uint64_t elapsedMs)
{
    while (app.scriptPad && !app.padScript.empty() && elapsedMs >= app.padScript.front().atMs) {
        const App::ScriptedPad s = app.padScript.front();
        app.padScript.erase(app.padScript.begin());
        switch (s.kind) {
        case App::ScriptedPad::Press:
        case App::ScriptedPad::Release:
            app.scriptPad->button(s.button, s.kind == App::ScriptedPad::Press);
            break;
        case App::ScriptedPad::LeftStick:
        case App::ScriptedPad::RightStick:
            app.scriptPad->stick(s.kind == App::ScriptedPad::RightStick, s.x, s.y);
            break;
        case App::ScriptedPad::LeftTrigger:
        case App::ScriptedPad::RightTrigger:
            app.scriptPad->trigger(s.kind == App::ScriptedPad::RightTrigger, s.x);
            break;
        }
        SDL_Log("pad script: %llu ms: step %d", static_cast<unsigned long long>(s.atMs), static_cast<int>(s.kind));
    }
}

std::vector<App::ScriptedPad> parsePadScript(std::string_view script)
{
    std::vector<App::ScriptedPad> out;
    std::string text(script);
    for (char& c : text) {
        if (c == ';') {
            c = '\n';
        }
    }
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream in(line);
        App::ScriptedPad s;
        std::string what;
        if (!(in >> s.atMs >> what)) {
            continue;
        }
        std::string arg;
        if (what == "press" || what == "release" || what == "tap") {
            in >> arg;
            const auto b = pad::padButtonFromName(arg);
            if (!b) {
                SDL_Log("--pad-script: no button %s", arg.c_str());
                continue;
            }
            s.button = *b;
            s.kind = what == "release" ? App::ScriptedPad::Release : App::ScriptedPad::Press;
            out.push_back(s);
            if (what == "tap") {
                // (let go a little later: a press and release between two of SDL's reads would be lost)
                s.kind = App::ScriptedPad::Release;
                s.atMs += 100;
                out.push_back(s);
            }
        } else if (what == "stick" || what == "trigger") {
            in >> arg >> s.x;
            const bool right = arg == "R" || arg == "r" || arg == "right";
            if (what == "stick") {
                in >> s.y;
                s.kind = right ? App::ScriptedPad::RightStick : App::ScriptedPad::LeftStick;
            } else {
                s.kind = right ? App::ScriptedPad::RightTrigger : App::ScriptedPad::LeftTrigger;
            }
            out.push_back(s);
        } else {
            SDL_Log("--pad-script: no step %s", what.c_str());
        }
    }
    std::ranges::stable_sort(out, {}, &App::ScriptedPad::atMs);
    return out;
}

} // namespace noxsdl
