// Settings (app.hpp): nox.cfg's and noxcore.cfg's into the App and back, the Options' switches
// applied, the window's display mode, the mouse as the game read it and who has it, and the
// game view's size.
#include "app.hpp"
#include "effects_draw.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace fs = std::filesystem;

namespace noxsdl {

/// "1024x768" or "1024 768 16" (nox.cfg's VideoMode) as a width and height.
std::optional<std::pair<int, int>> parseSize(std::string_view s)
{
    const auto number = [&](std::size_t& at) {
        int v = 0;
        bool any = false;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
            v = v * 10 + (s[at++] - '0');
            any = true;
        }
        return any ? v : -1;
    };
    std::size_t at = 0;
    const int w = number(at);
    while (at < s.size() && (s[at] == 'x' || s[at] == ' ')) {
        ++at;
    }
    const int h = number(at);
    if (w <= 0 || h <= 0) {
        return std::nullopt;
    }
    return std::pair{w, h};
}

/// Takes on nox.cfg's settings: the bindings, a game's view (VideoMode, VideoSize) and what the
/// Options' advanced panel switches (RenderGUI, RenderBubbles, RenderGlow, SoftShadowEdge,
/// FadeObjects).
void applySettings(App& app, const nox::config::Config& config)
{
    if (app.input) {
        app.input->setConfig(config);
    } else {
        app.input = std::make_unique<InputBindings>(config);
    }
    if (const auto mode = config.setting("VideoMode")) {
        if (const auto wh = parseSize(*mode)) {
            // One of the game's resolutions: never more of the world than its largest.
            app.viewWidth = std::clamp(wh->first, 320, 1024);
            app.viewHeight = std::clamp(wh->second, 240, 768);
        }
    }
    if (const auto size = config.setting("VideoSize")) {
        app.windowSize = std::clamp(std::atoi(size->c_str()), 40, 100);
    }
    // A switch takes a value given (atoi, non-zero on); a key missing or with no value leaves it
    // as it is (the game's readers take the value only when there is one).
    const auto flag = [&](std::string_view name, bool& to) {
        if (const auto v = config.setting(name); v && !v->empty()) {
            to = std::atoi(v->c_str()) != 0;
        }
    };
    flag("RenderGUI", app.renderGui);
    flag("RenderBubbles", app.renderBubbles);
    flag("RenderGlow", app.renderGlow);
    flag("SoftShadowEdge", app.softShadowEdge);
    flag("FadeObjects", app.fadeObjects);

    if (app.audio) {
        app.audio->setVolumes(config);
    }
}

/// Writes nox.cfg in the game folder, as the game does when its Options close.
void saveSettings(App& app)
{
    if (!app.input) {
        return;
    }
    try {
        nox::config::saveConfig(app.gameDir, app.input->config());
        SDL_Log("saved %s", (app.gameDir / "nox.cfg").string().c_str());
    } catch (const std::exception& ex) {
        SDL_Log("nox.cfg: %s", ex.what());
    }
}

/// `c` made the settings in use and nox.cfg written, if its settings differ from those.
void storeConfig(App& app, const nox::config::Config& c)
{
    if (c.settings != app.input->config().settings) {
        app.input->setConfig(c);
        saveSettings(app);
    }
}

/// noxsdl's own settings (the Options' added switches), in noxcore.cfg beside nox.cfg:
/// "Name = 0 / 1" lines. The game would reject nox.cfg with them in it. The exe was noxsdl.exe
/// and the file noxsdl.cfg: with no noxcore.cfg yet, noxsdl.cfg is copied to it once (and left
/// as it was), so the settings carry over.
OptionsWindow::Extras loadExtras(const fs::path& gameDir)
{
    OptionsWindow::Extras e;
    fs::path file = gameDir / "noxcore.cfg";
    std::error_code ec;
    if (const fs::path old = gameDir / "noxsdl.cfg"; !fs::exists(file, ec) && fs::exists(old, ec)) {
        if (fs::copy_file(old, file, ec)) {
            SDL_Log("noxcore.cfg: the settings carried over from noxsdl.cfg (left as it was)");
        } else {
            SDL_Log("noxcore.cfg: couldn't copy noxsdl.cfg to it (%s); read from noxsdl.cfg", ec.message().c_str());
            file = old;
        }
    }
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, eq);
        std::erase_if(name, [](char c) { return c == ' ' || c == '\t'; });
        const bool on = std::atoi(line.c_str() + eq + 1) != 0;
        if (name == "FrameRate") {
            e.frameRate = on;
        } else if (name == "Bloom") {
            e.bloom = on;
        } else if (name == "PerPixelLight") {
            e.perPixelLight = on;
        } else if (name == "SmoothEffects") {
            e.smoothEffects = on;
        } else if (name == "Smoothing") {
            e.smoothing = std::clamp(std::atoi(line.c_str() + eq + 1), 0, 3);
        } else if (name == "HDImages") {
            e.hdImages = on;
        } else if (name == "Gamma") {
            e.gamma = std::clamp(std::atoi(line.c_str() + eq + 1), 1, 10);
        } else if (name == "MouseAccel") {
            e.mouseAccel = on;
        } else if (name == "KillFeed") {
            e.killFeed = on;
        } else if (name == "Display") {
            e.display = std::clamp(std::atoi(line.c_str() + eq + 1), 0, 2);
        } else if (name == "VSync") {
            e.vsync = on;
        } else if (name == "ServerUpdateRate") {
            e.serverUpdateRate = std::clamp(std::atoi(line.c_str() + eq + 1) / 30, 1, 4) * 30;  // (rounded down to 30, 60, 90 or 120)
        } else if (name == "ServerMapVote") {
            e.serverMapVote = on;
        } else if (name == "ExactMaths") {
            e.exactMaths = on;
        } else if (name == "MouseSpeed") {
            e.mouseSpeed = std::clamp(std::atoi(line.c_str() + eq + 1), OptionsWindow::kMouseSpeedMin,
                                      OptionsWindow::kMouseSpeedMax);
        } else if (name == "MouseSpeed3D") {
            e.mouseSpeed3D = std::clamp(std::atoi(line.c_str() + eq + 1), OptionsWindow::kMouseSpeedMin,
                                        OptionsWindow::kMouseSpeedMax);
        } else if (name == "PendingChanges") {
            e.pendingView = std::clamp(std::atoi(line.c_str() + eq + 1), 0, 2);
        } else if (name == "WeatherEffects") {
            e.weather = std::clamp(std::atoi(line.c_str() + eq + 1), 0, 2);
        }
    }
    return e;
}

/// Writes noxcore.cfg from the switches (what loadExtras reads back).
void saveExtras(const App& app, const OptionsWindow::Extras& e)
{
    std::ofstream out(app.gameDir / "noxcore.cfg", std::ios::trunc);
    out << "FrameRate = " << e.frameRate << "\nBloom = " << e.bloom << "\nPerPixelLight = " << e.perPixelLight
        << "\nSmoothEffects = " << e.smoothEffects << "\nSmoothing = " << e.smoothing
        << "\nHDImages = " << e.hdImages << "\nGamma = " << e.gamma << "\nMouseAccel = " << e.mouseAccel
        << "\nMouseSpeed = " << e.mouseSpeed << "\nMouseSpeed3D = " << e.mouseSpeed3D << "\nKillFeed = " << e.killFeed << "\nDisplay = " << e.display
        << "\nVSync = " << e.vsync << "\nServerUpdateRate = " << e.serverUpdateRate
        << "\nServerMapVote = " << e.serverMapVote << "\nExactMaths = " << e.exactMaths << "\nPendingChanges = " << e.pendingView
        << "\nWeatherEffects = " << e.weather << "\n";
    if (!out) {
        SDL_Log("noxcore.cfg: couldn't write it");
    }
}

/// The switches as they stand now.
OptionsWindow::Extras currentExtras(const App& app)
{
    OptionsWindow::Extras e;
    e.frameRate = app.showFps;
    e.bloom = app.bloom != nullptr;
    e.perPixelLight = app.light.perPixel();
    e.smoothEffects = fxdraw::smoothOn();
    e.smoothing = app.smoothing;
    e.hdImages = app.sprites && app.sprites->usingPack();
    e.hdAvailable = !app.imagePack.empty();
    e.gamma = app.gamma;
    e.gpu = app.shaders != nullptr && app.white != nullptr;
    e.mouseAccel = app.mouseAccel;
    e.mouseSpeed = app.mouseSpeed;
    e.mouseSpeed3D = app.mouseSpeed3D;
    e.view3d = app.view3d.enabled();
    e.killFeed = app.killFeed;
    e.display = app.display;
    e.vsync = app.vsync;
    e.serverUpdateRate = app.serverUpdateRate;
    e.serverMapVote = app.serverMapVote;
    e.exactMaths = app.exactMaths;
    e.pendingView = static_cast<int>(app.pendingView);
    e.weather = app.weatherEffects;
    return e;
}

namespace {

/// Options' Display and VSync onto the window and renderer: a window (bordered, resizable),
/// fullscreen windowed (SDL's desktop fullscreen: borderless at the desktop's size) or fullscreen
/// (exclusive, in the desktop's display mode); frames waiting for the refresh, or not. Not for a
/// hidden screenshot run.
void applyDisplay(App& app)
{
    if (!app.window || !app.renderer) {
        return;
    }
    if (!SDL_SetRenderVSync(app.renderer, app.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED)) {
        SDL_Log("vsync: %s", SDL_GetError());
    }
    if (!app.screenshot.empty()) {
        return;
    }
    bool ok = true;
    if (app.display == 0) {
        ok = SDL_SetWindowFullscreen(app.window, false);
    } else {
        const SDL_DisplayMode* mode = nullptr;
        if (app.display == 2) {
            mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(app.window));
        }
        ok = SDL_SetWindowFullscreenMode(app.window, mode) && SDL_SetWindowFullscreen(app.window, true);
    }
    if (!ok) {
        SDL_Log("display: %s", SDL_GetError());
    }
    SDL_Log("display: %s, vsync %s", app.display == 2 ? "fullscreen" : app.display == 1 ? "fullscreen windowed" : "windowed",
            app.vsync ? "on" : "off");
}

} // namespace

/// The mouse as the original read it: raw movement counts (DirectInput's DIMOFS_X / DIMOFS_Y,
/// client_nextMouseEvent), each moving the cursor one pixel of the game's screen (added and clamped
/// by client_changeMousePos, no scaling). Here that's SDL's relative mode (raw input) with each count moving
/// the cursor one game pixel: in a game a world unit (camera.zoom output pixels), in the menus
/// one of their 640 x 480. Then Win Mouse Accel (on by default) puts Windows' pointer speed and
/// acceleration curve on the counts, and Mouse's speed multiplies them. SDL keeps the cursor's
/// place in the window from that, so everything reading the mouse reads it as before. The
/// pointer stays in the window while it has the focus, as the original's did.
/// Called every frame: the game pixel's size changes with the window and the resolution.
void applyMouse(App& app)
{
    if (!app.window) {
        return;
    }
    int outW = 0, outH = 0;
    SDL_GetCurrentRenderOutputSize(app.renderer, &outW, &outH);
    const float gamePixel = app.live ? app.camera.zoom // (viewZoom, set this frame)
                                     : std::min(static_cast<float>(outW) / 640.0f, static_cast<float>(outH) / 480.0f);
    const float density = std::max(SDL_GetWindowPixelDensity(app.window), 0.01f); // (events are in window points)
    // The 3D view's mouse look turns by the same counts at its own speed (Options' Mouse slider
    // sets that one while the 3D view is on); a window open there gets the cursor's speed back.
    const bool looking = app.view3d.looking();
    const int speed = looking ? app.mouseSpeed3D : app.mouseSpeed;
    if (looking != app.mouseLooking) {
        app.mouseLooking = looking;
        SDL_Log("mouse: %s, speed x%.2f", looking ? "3D mouse look" : "cursor", speed / 100.0);
    }
    const float scale = std::max(gamePixel, 0.01f) / density * static_cast<float>(speed) / 100.0f;
    if (!app.mouseApplied && !app.screenshot.empty()) {
        app.mouseApplied = true; // (a --screenshot run's hidden window leaves the desktop's mouse alone)
    }
    if (!app.mouseApplied) {
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "0"); // (the cursor stays where it is)
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, app.mouseAccel ? "1" : "0");
        if (!SDL_SetWindowRelativeMouseMode(app.window, true)) {
            SDL_Log("mouse: relative mode: %s", SDL_GetError());
        }
        SDL_Log("mouse: raw input, speed x%.2f%s", app.mouseSpeed / 100.0,
                app.mouseAccel ? " with Windows' acceleration" : " (1:1 with the original at x1)");
        app.mouseApplied = true;
        app.mouseScale = 0; // (set below)
    }
    if (std::abs(scale - app.mouseScale) > 0.001f) {
        app.mouseScale = scale;
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, std::to_string(scale).c_str());
    }
}

/// The window's mouse handed to the menus or a game over the whole window (mouse_handover.hpp),
/// whole: every way out of the editor and of a play view comes through here. Nothing of the
/// editor's or the panel's game's left behind: no held pointer place (the drawn pointer follows the
/// mouse again), no confinement, grab or capture, SDL's pointer hidden (the game's own is drawn), and
/// relative mode on again by applyMouse at the next frame (never in a hidden run), whatever the
/// editor or a play view turned off.
void handMouseTo(App& app, MouseOwner owner)
{
    const WindowMouse want = windowMouse(owner, !app.screenshot.empty());
    app.playTestCaptured = false;
    const bool dropped = !want.heldPointer && app.playTestMouse.has_value();
    if (!want.heldPointer) {
        app.playTestMouse.reset();
    }
    if (app.input) {
        app.input->releaseAll(); // (what a press began in the editor or the panel waits for no release)
    }
    if (!app.window) {
        return;
    }
    if (!want.confined) {
        app.playTestMouseRect = {};
        SDL_SetWindowMouseRect(app.window, nullptr);
    }
    if (!want.grabbed) {
        SDL_SetWindowMouseGrab(app.window, false);
    }
    SDL_CaptureMouse(false); // (ImGui's backend captures it while a button is held)
    if (want.systemPointer) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
    if (want.relative) {
        app.mouseApplied = false; // (applyMouse: relative mode with its speed, at the next frame)
    } else if (SDL_GetWindowRelativeMouseMode(app.window)) {
        SDL_SetWindowRelativeMouseMode(app.window, false);
    }
    SDL_Log("mouse: to the %s (relative %s, pointer %s, grab %s, confined %s, drawn pointer from %s%s)",
            owner == MouseOwner::Menus ? "menus" : "game", want.relative ? "on next frame" : SDL_GetWindowRelativeMouseMode(app.window) ? "on" : "off",
            SDL_CursorVisible() ? "shown" : "hidden", SDL_GetWindowMouseGrab(app.window) ? "on" : "off",
            SDL_GetWindowMouseRect(app.window) ? "yes" : "no", pointerSourceName(pointerSource(app.testMouse.has_value(), app.playTestMouse.has_value(), false)),
            dropped ? "; the panel's held place dropped" : "");
}

/// Options' Other Units into the interpolator: Ahead moves players and monsters on, and the
/// harpoon: its bolt sits on whoever it struck (HarpoonUpdate puts it at their
/// position each frame), so it goes on as they do and stays on them, at the rope's end.
void setOtherUnits(App& app)
{
    app.interpolator.setOthers(static_cast<Interpolator::Others>(app.otherUnits));
    app.interpolator.setAheadFilter([&app](const nox::client::Drawable& d) {
        const TypeArt* a = app.art ? app.art->get(d.typeId) : nullptr;
        return a && ((a->classes & (nox::things::ObjectClass::Player | nox::things::ObjectClass::Monster)) != 0 ||
                     a->drawFn == DrawFn::Harpoon);
    });
}

/// The map's own images queued for the HD decoders and kept till the next map (SpriteCache::beginMap):
/// as a map loads, and when the HD images are turned on in one.
void preloadMap(App& app, const std::string& name)
{
    if (app.sprites && app.sprites->usingPack() && app.view && app.art) {
        app.sprites->beginMap(app.view->preloadList(app.map, *app.art, *app.sprites, name));
    } else if (app.sprites) {
        app.sprites->beginMap({}); // (no pack: only the last map's decoded pixels go)
    }
}

/// The images any map can need queued for the HD decoders and kept for the session
/// (SpriteCache::beginSession): at start-up, and when the HD images are turned on.
void preloadSession(App& app)
{
    if (app.sprites && app.sprites->usingPack() && app.art) {
        const std::uint64_t start = SDL_GetTicksNS();
        std::string counts;
        const auto images = sessionImages(app.things, *app.art, *app.sprites, &counts);
        SDL_Log("session images: %zu HD (%s), listed in %.1f ms", images.images.size(), counts.c_str(),
                static_cast<double>(SDL_GetTicksNS() - start) / 1e6);
        app.sprites->beginSession(images);
    }
}

/// Switches the renderer's extras on or off at once (they need the GPU renderer's shaders).
void applyExtras(App& app, const OptionsWindow::Extras& e)
{
    app.showFps = e.frameRate;
    app.killFeed = e.killFeed;
    app.serverUpdateRate = e.serverUpdateRate;
    app.serverMapVote = e.serverMapVote;
    app.exactMaths = e.exactMaths;
    app.pendingView = static_cast<nox::mapedit::PendingView>(std::clamp(e.pendingView, 0, 2));
    app.weatherEffects = std::clamp(e.weather, 0, 2);
    if (e.display != app.display || e.vsync != app.vsync) {
        app.display = std::clamp(e.display, 0, 2);
        app.vsync = e.vsync;
        applyDisplay(app);
    }
    // Options' Smoothing: Off, Smooth Movement (eased), Smooth Network (the clock), Lock to 30 FPS.
    app.smoothing = std::clamp(e.smoothing, 0, 3);
    app.smoothMotion = app.smoothing == 2;
    app.smoothScrolling = app.smoothing == 2;
    app.interpolator.setEasing(app.smoothing == 1);
    app.gamma = std::clamp(e.gamma, 1, 10);
    if (e.mouseAccel != app.mouseAccel || e.mouseSpeed != app.mouseSpeed) {
        app.mouseAccel = e.mouseAccel;
        app.mouseSpeed = e.mouseSpeed;
        app.mouseApplied = false; // (applyMouse, next frame)
    }
    app.mouseSpeed3D = e.mouseSpeed3D; // (applyMouse picks it up while the 3D view looks)
    app.interpolator.setSmoothing(app.smoothMotion);
    app.interpolator.setScrolling(app.smoothScrolling);
    app.interpolator.setPrediction(app.predictMovement);
    setOtherUnits(app);
    // Use HD Images: the pack's images (upscaled ones and the originals), or video.bag's.
    if (app.sprites && e.hdImages != app.sprites->usingPack()) {
        if (e.hdImages && !app.imagePack.empty()) {
            if (auto pack = SpriteCache::openPack(app.imagePack, app.gameDir / "video.idx")) {
                app.sprites->setPack(std::move(pack), app.hdDecodeSync ? fs::path() : app.imagePack);
                preloadSession(app);
                preloadMap(app, app.viewMap);
            }
        } else if (!e.hdImages) {
            app.sprites->setPack(nullptr);
            SDL_Log("images: video.bag");
        }
    }
    if (!app.shaders || !app.white) {
        return;
    }
    if (e.bloom && !app.bloom) {
        app.bloom = std::make_unique<Bloom>(app.shaders->blur(), app.shaders->bright(), app.bloomStrength);
    } else if (!e.bloom) {
        app.bloom.reset();
    }
    app.light.setPerPixel(e.perPixelLight ? app.shaders->light() : nullptr, app.white);
    fxdraw::smooth() = e.smoothEffects
                           ? fxdraw::Smooth{app.white, app.shaders->disc(), app.shaders->rect(), app.shaders->glow(),
                                            app.shaders->puff(), app.shaders->glowLine(), app.shaders->barrier()}
                           : fxdraw::Smooth{};
}

/// Opens the Options window on the current settings (in a game the resolution stays).
void openOptions(App& app)
{
    if (app.options && app.input) {
        OptionsWindow::SoundOn sound{true, true, true};
        if (app.audio) {
            const auto s = app.audio->switches();
            sound = {s.effects, s.dialog, s.music};
        }
        app.options->open(app.input->config(), app.live != nullptr, currentExtras(app), sound);
    }
}

/// nox.cfg's TrackData: the classes whose ending was seen, 0x80 the intro; a
/// byte, as the exe reads it.
int trackData(App& app)
{
    const auto v = app.input ? app.input->config().setting("TrackData") : std::nullopt;
    return nox::client::movies::trackDataByte(v ? std::atoi(v->c_str()) : 0);
}

/// TrackData set (a byte), written to nox.cfg at once (the exe writes it with nox.cfg's next save).
void setTrackData(App& app, int mask)
{
    if (!app.input) {
        return;
    }
    auto config = app.input->config();
    config.set("TrackData", std::to_string(nox::client::movies::trackDataByte(mask)));
    app.input->setConfig(config);
    saveSettings(app);
}

/// The zoom in a game: the game's area (the window, less the bars that keep the chosen
/// resolution's shape) filled with that resolution's area of the world.
float viewZoom(const App& app)
{
    const float w = static_cast<float>(app.camera.viewWidth) / static_cast<float>(std::max(app.viewWidth, 1));
    const float h = static_cast<float>(app.camera.viewHeight) / static_cast<float>(std::max(app.viewHeight, 1));
    return std::max({w, h, 0.125f});
}

/// The game window's size (VideoSize) up or down by `by` percent, 40-100 (draw_setCutSize).
void stepWindowSize(App& app, int by)
{
    app.windowSize = std::clamp(app.windowSize + by, 40, 100);
    if (app.input) {
        app.input->setSetting("VideoSize", std::to_string(app.windowSize)); // (saved with the Options)
    }
}

/// The game window at its size (VideoSize, draw_setCutSize) in the view, in its pixels: the middle
/// windowSize percent each way, on whole pixels.
SDL_FRect gameWindowBox(const App& app)
{
    const auto w = static_cast<float>(app.camera.viewWidth);
    const auto h = static_cast<float>(app.camera.viewHeight);
    if (app.windowSize >= 100) {
        return {0, 0, w, h};
    }
    const float cw = std::floor(w * static_cast<float>(app.windowSize) / 100);
    const float ch = std::floor(h * static_cast<float>(app.windowSize) / 100);
    return {std::floor((w - cw) / 2), std::floor((h - ch) / 2), cw, ch};
}

} // namespace noxsdl
