// (noxcpp) The game controller in the program (app.hpp's pad/pad_game.cpp group): the pads started
// with the window, noxcore_controller.cfg read and written, a pad's buttons routed into the game's
// controls (InputBindings, under pad codes) or the pad's own actions, the left stick walking, and
// --pad-script's made-up pad.
//
// Where a pad's event goes (main.cpp's SDL_AppEvent):
//  - first of all padEventFirst: the pads kept (GamepadManager); while the on-screen keyboard is up
//    (pad/osk_game.cpp) every press is its own; while a menu or a window that
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
#include "pad/pad_panels.hpp"
#include "pad/pad_play.hpp"
#include "gui_nav.hpp"

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
}

/// InputBindings forgot what's held since we last looked: so does the pad.
void followReleases(App& app)
{
    if (app.input && app.input->releases() != app.padReleasesSeen) {
        app.padReleasesSeen = app.input->releases();
        app.padRouter.dropAll();
        app.padEscapes = 0;
    }
}

/// The rumble (Rumble on): a short pulse when our health drops, a longer one on a level up, while
/// the pad is in use. Off, nothing is kept (the next reading after it's switched on only takes note).
void padRumbleFrame(App& app)
{
    if (!app.padConfig.settings.rumble || !app.live) {
        app.padRumble.self = 0;
        return;
    }
    const auto& client = app.live->client();
    const auto& me = client.world().self();
    const auto pulse = app.padRumble.step(static_cast<std::uint16_t>(client.netCode()), me.health, me.maxHealth, me.level);
    SDL_Gamepad* g = app.pads.padInUse() ? app.pads.active() : nullptr;
    if (!g || pulse == pad::RumbleWatch::Pulse::None) {
        return;
    }
    if (pulse == pad::RumbleWatch::Pulse::LevelUp) {
        SDL_RumbleGamepad(g, 0x9000, 0x9000, 450);
    } else {
        SDL_RumbleGamepad(g, 0x7000, 0xB000, 140);
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
    // A test run never asks which to play with (unless --input-mode ask): --input-mode's mode, else
    // a --pad-script run plays with its pad and any other with the mouse.
    const bool testRun = !app.screenshot.empty() || !app.mouseScript.empty() || app.scriptPad;
    if (!app.padModeForced && testRun && !app.padModeAskForced) {
        app.padModeForced = app.scriptPad ? pad::InputDevice::Pad : pad::InputDevice::MouseKeyboard;
    }
    if (app.padModeForced) {
        setInputMode(app, *app.padModeForced == pad::InputDevice::Pad);
    }
}

pad::PadConfig readPadConfig(const App& app)
{
    const auto path = padConfigPath(app);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return pad::defaultPadConfig();
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return pad::parsePadConfig(text.str());
}

void loadPadConfig(App& app)
{
    applyPadConfig(app, readPadConfig(app));
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
    ++app.padPrompts.generation; // (the HUD's prompts made again)
}

pad::PromptLabels padPrompt(const App& app, std::string_view action)
{
    const pad::ActionButtons on = pad::actionButtons(app.padConfig, action);
    pad::PromptLabels out;
    const pad::PromptStyle style = app.padConfig.settings.prompts;
    if (on.button) {
        out.button = app.pads.label(*on.button, style);
    }
    if (on.modifier) {
        out.modifier = app.pads.label(*on.modifier, style);
    }
    return out;
}

const pad::HudPrompts* padHudPrompts(App& app)
{
    if (!app.pads.connected() || !app.pads.padInUse()) {
        return nullptr;
    }
    // Made again only when the config, the pad or its kind changed.
    const std::uint64_t key = (static_cast<std::uint64_t>(app.padPrompts.generation) << 32) ^
                              (static_cast<std::uint64_t>(app.pads.activeId()) << 8) ^ static_cast<std::uint64_t>(app.pads.activeType());
    if (key != app.padPrompts.key) {
        app.padPrompts.key = key;
        for (std::size_t i = 0; i < app.padPrompts.hud.slots.size(); ++i) {
            app.padPrompts.hud.slots[i] = padPrompt(app, "InvokeSlot" + std::to_string(i + 1));
        }
        app.padPrompts.hud.potions = {padPrompt(app, "QuickHealth"), padPrompt(app, "QuickMana"), padPrompt(app, "QuickCurePoison")};
        app.padPrompts.hud.book = padPrompt(app, "ToggleBook");
        app.padPrompts.hud.swap = padPrompt(app, "SwapWeapons");
    }
    return &app.padPrompts.hud;
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
    return app.live && !app.joinWaiting && app.padFocused && !oskOpen(app) && !editorHasInput(app) && !(app.movie && app.movie->active()) &&
           !(app.quitMenu && app.quitMenu->isOpen()) && !(app.chat && app.chat->isOpen()) &&
           !(app.console && app.console->isOpen()) && !(app.options && app.options->isOpen()) &&
           !(app.inputConfig && app.inputConfig->isOpen()) && !(app.controllerWindow && app.controllerWindow->isOpen());
}

bool padHasMenus(const App& app)
{
    if (editorHasInput(app)) {
        return false;
    }
    if (!app.live || app.joinWaiting || (app.movie && app.movie->active())) {
        return true;
    }
    // (and the game's windows that own Escape: the message of the day, the host's Server Settings,
    // the map download)
    return (app.quitMenu && app.quitMenu->isOpen()) || (app.chat && app.chat->isOpen()) ||
           (app.console && app.console->isOpen()) || (app.options && app.options->isOpen()) ||
           (app.inputConfig && app.inputConfig->isOpen()) || (app.controllerWindow && app.controllerWindow->isOpen()) ||
           (app.motdWindow && app.motdWindow->isOpen()) ||
           (app.serverOptions && app.serverOptions->isOpen()) || (app.mapDownload && app.live->client().downloadPercent());
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
    // The mouse's mode: the pad's presses are nobody's, as before there was pad support (the
    // question of which to use, and the Controller screen's wait for a button, aside).
    if (edge && !app.pads.padInUse() && !padModeHearsPad(app)) {
        return std::nullopt;
    }
    // The on-screen keyboard, while it's up, has the pad's presses (pad/osk_game.cpp).
    if (oskOpen(app) && oskEvent(app, event, edge)) {
        return std::nullopt;
    }
    if (!edge) {
        return std::nullopt;
    }
    followReleases(app);
    if (app.controllerWindow && app.controllerWindow->capturing()) {
        return edge; // (the Controller screen waits for a button: each as it is, none Escape or the menus')
    }
    if (edge->down && padPanelsPress(app, *edge)) {
        return std::nullopt; // (the panels' snap cursor's, pad_panels.cpp: nothing else sees it)
    }
    const unsigned bit = 1u << static_cast<unsigned>(edge->button);
    // A menu, or a window that owns Escape (chat, the console), has the input: MenuBack's button is
    // Escape there (each screen's own Escape; marked as the pad's: gui::padEscape); the D-pad, the
    // other menu actions go on as the menus' nav events (padMenuInput, pad_menu.cpp).
    if (edge->down && padHasMenus(app) && app.padRouter.menuActions(edge->button).has(PadAction::MenuBack)) {
        app.padEscapes |= bit;
        event = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
        event.key.which = gui::kPadKeyboard;
        return std::nullopt;
    }
    if (!edge->down && (app.padEscapes & bit)) {
        app.padEscapes &= ~bit;
        event = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
        event.key.which = gui::kPadKeyboard;
        return std::nullopt;
    }
    if (edge->down) {
        padMenuInput(app, *edge, event);
    }
    return edge;
}

void padRelease(App& app, const pad::PadEdge& edge)
{
    followReleases(app);
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
    const auto modifier = app.padRouter.modifier();
    const auto e = app.padRouter.press(edge.button, modifier && app.pads.isDown(*modifier));
    const std::vector<std::string> controls = e.code ? app.input->press(e.code) : std::vector<std::string>{};
    if (app.scriptPad && !e.isModifier) {
        // (--pad-script runs: the layer a press took and what it fired, for the test's log)
        std::string fired;
        for (const auto& c : controls) {
            fired += (fired.empty() ? "" : ", ") + c;
        }
        SDL_Log("pad: %s%s%s: %s", e.modifierLayer ? std::string(pad::padButtonName(*modifier)).c_str() : "", e.modifierLayer ? " + " : "",
                std::string(pad::padButtonName(edge.button)).c_str(), fired.empty() ? "no game control" : fired.c_str());
    }
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
    // The pad's own game actions (Interact, TargetNext / TargetPrevious: pad_play.cpp); Modifier is
    // the router's own (held: the second layer).
    padPlayAction(app, action, down);
}

PadSteer padSteer(App& app)
{
    if (!app.pads.connected()) {
        if (app.padPlay.hasPointer || app.padPlay.collecting) {
            padPlayIdle(app); // (the last pad gone)
        }
        padPanelsIdle(app);
        return PadSteer::None;
    }
    if (!app.pads.padInUse()) {
        // (The mouse's mode: the pad not even read.)
        app.padAim.reset();
        if (app.padPlay.hasPointer || app.padPlay.collecting) {
            padPlayIdle(app);
        }
        padPanelsIdle(app);
        return PadSteer::None;
    }
    app.pads.sample(SDL_GetTicksNS());
    followReleases(app);
    padRumbleFrame(app);
    const auto at = app.live->client().position();
    if (!padGameInput(app) || app.view3d.active() || !at || (app.cinematics && app.cinematics->screenUp())) {
        padPlayIdle(app); // (a briefing's screen, chat, a menu: the facing stays as it was)
        padPanelsIdle(app);
        return PadSteer::None;
    }
    // The panels' snap cursor (pad_panels.cpp): on the inventory or the book the player stands.
    const PanelFrame panels = padPanelsFrame(app);
    if (panels == PanelFrame::Still) {
        return PadSteer::Holds;
    }
    // The stick's walk (its cursor point out by its tilt), the aim and the target, the pointer
    // (pad_play.cpp).
    const PadSteer steer = padPlaySteer(app, static_cast<float>(at->first), static_cast<float>(at->second));
    if (panels == PanelFrame::Dpad) {
        padPanelsPointer(app); // (on the window's spot, over the aim)
    }
    if (app.scriptPad) {
        // (--pad-script runs: the facing and the move as they change, and our animation, for the
        // test's log)
        static std::uint32_t logged = 0xFFFFFFFFu;
        const auto& in = app.live->client().lastInput();
        const std::uint32_t now = in.facing | (in.move ? 0x100u : 0u) | (static_cast<std::uint32_t>(ownAnimState(app) & 0xFF) << 16);
        if (now != logged) {
            logged = now;
            SDL_Log("pad: facing %u%s, animation %d, tilt %.2f", static_cast<unsigned>(in.facing), in.move ? " moving" : "",
                    ownAnimState(app), app.pads.frame().left.length());
        }
    }
    return steer;
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
