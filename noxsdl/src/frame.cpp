// The frame's way to the screen: drawn into a target of the game's area (letterboxed) or put
// through the gamma ramp (beginFrame / endFrame), presented with --frame-stats' counts, a movie's
// frame, and the screenshots (F12's, --screenshot's).

#include "app.hpp"
#include "backend_hooks.hpp"
#if NOX_PRIVATE_TOOLS
#include "dev_hooks.hpp"
#endif

#include "noxcore/io/file.hpp"
#include "noxcore/io/png.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace noxsdl {

/// Reads back the frame just rendered and writes it to --screenshot's path as a PNG; false if SDL can't read it.
bool saveScreenshot(App& app)
{
    SDL_Surface* shot = SDL_RenderReadPixels(app.renderer, nullptr);
    if (!shot) {
        SDL_Log("SDL_RenderReadPixels failed: %s", SDL_GetError());
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(shot);
    if (!rgba) {
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rgba->w) * rgba->h * 4);
    // Row by row: the surface's pitch can be wider than its pixels.
    for (int y = 0; y < rgba->h; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(rgba->pixels) + static_cast<std::size_t>(y) * rgba->pitch;
        std::copy(row, row + static_cast<std::size_t>(rgba->w) * 4,
                  pixels.begin() + static_cast<std::ptrdiff_t>(y) * rgba->w * 4);
    }
    nox::writeFileBytes(app.screenshot, nox::encodePng(rgba->w, rgba->h, pixels));
    SDL_Log("saved %s (%dx%d) at %.0f,%.0f", app.screenshot.c_str(), rgba->w, rgba->h, app.camera.x, app.camera.y);
    SDL_DestroySurface(rgba);
    return true;
}

namespace {

/// Options' gamma above 1, with the GPU renderer's gamma shader to put it on.
bool gammaRamp(const App& app)
{
    return app.gamma > 1 && app.shaders && app.shaders->gamma();
}

/// The gamma shader set for the next draws: each channel to the power 1 / (1 + (gamma - 1) / 6).
void setGammaShader(App& app)
{
    const struct {
        float params[4];
    } uniforms{{1.0f / (1.0f + static_cast<float>(app.gamma - 1) / 6.0f), 0, 0, 0}};
    SDL_SetGPURenderStateFragmentUniforms(app.shaders->gamma(), 0, &uniforms, sizeof(uniforms));
    SDL_SetGPURenderState(app.renderer, app.shaders->gamma());
}

/// `target` a render target of w x h (opaque: no blending), made again when it's another size;
/// false if it can't be made.
bool fitTarget(SDL_Renderer* renderer, SDL_Texture*& target, int w, int h)
{
    float tw = 0, th = 0;
    if (target && SDL_GetTextureSize(target, &tw, &th) && (static_cast<int>(tw) != w || static_cast<int>(th) != h)) {
        SDL_DestroyTexture(target);
        target = nullptr;
    }
    if (!target) {
        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!target) {
            return false;
        }
        SDL_SetTextureBlendMode(target, SDL_BLENDMODE_NONE);
    }
    return true;
}

} // namespace

/// The frame starts: with gamma above 1 (and the GPU renderer's gamma shader), drawn into a
/// texture the size of the output, for endFrame to put on the screen through the ramp.
void beginFrame(App& app)
{
    if (!gammaRamp(app) && !app.letterboxed) {
        return;
    }
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(app.renderer, &w, &h);
    if (app.letterboxed) { // (the game's area alone: everything sizes itself to the target)
        w = static_cast<int>(app.gameArea.w);
        h = static_cast<int>(app.gameArea.h);
    }
    if (fitTarget(app.renderer, app.gammaTarget, w, h)) {
        SDL_SetRenderTarget(app.renderer, app.gammaTarget);
    }
}

namespace {

/// ScreenShot (F12): the frame just drawn (the render target's) as nox<n>.bmp in the game folder, n
/// counting from 1. The exe starts again at 1 each run and writes over what's there; here the next
/// free number.
void saveF12Screenshot(App& app)
{
    SDL_Surface* shot = SDL_RenderReadPixels(app.renderer, nullptr);
    if (!shot) {
        return;
    }
    fs::path path;
    do {
        path = app.gameDir / ("nox" + std::to_string(++app.screenshotNumber) + ".bmp");
    } while (fs::exists(path) && app.screenshotNumber < 100000);
    const bool saved = SDL_SaveBMP(shot, path.string().c_str());
    SDL_DestroySurface(shot);
    if (app.console) {
        app.console->print(saved ? "screenshot: " + path.string() : "screenshot: couldn't write " + path.string(),
                           saved ? Yellow : Red);
    }
}

} // namespace

/// The frame ends: through the gamma ramp onto the screen (each channel to the power
/// 1 / (1 + (gamma - 1) / 6)), then an F12 screenshot of it. A play-test in the editor's Map
/// panel keeps it in its texture for the panel (through the ramp into one of its own: playTestGamma).
void endFrame(App& app)
{
    if (gameInPanel(app) && app.gammaTarget && SDL_GetRenderTarget(app.renderer) == app.gammaTarget) {
        app.playTestFrame = app.gammaTarget;
        if (gammaRamp(app)) {
            // Through the ramp into a texture of its own for the panel.
            float tw = 0, th = 0;
            SDL_GetTextureSize(app.gammaTarget, &tw, &th);
            if (fitTarget(app.renderer, app.playTestGamma, static_cast<int>(tw), static_cast<int>(th))) {
                SDL_SetRenderTarget(app.renderer, app.playTestGamma);
                setGammaShader(app);
                SDL_RenderTexture(app.renderer, app.gammaTarget, nullptr, nullptr);
                SDL_SetGPURenderState(app.renderer, nullptr);
                app.playTestFrame = app.playTestGamma;
            }
        }
        if (std::exchange(app.screenshotWanted, false)) {
            saveF12Screenshot(app); // (the game's frame alone)
        }
        SDL_SetRenderTarget(app.renderer, nullptr);
        return;
    }
    if (SDL_GetRenderTarget(app.renderer) == app.gammaTarget && app.gammaTarget) {
        SDL_SetRenderTarget(app.renderer, nullptr);
        const bool gamma = gammaRamp(app);
        if (app.letterboxed) { // the bars
            SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
            SDL_RenderClear(app.renderer);
        }
        if (gamma) {
            setGammaShader(app);
        }
        SDL_RenderTexture(app.renderer, app.gammaTarget, nullptr, app.letterboxed ? &app.gameArea : nullptr);
        if (gamma) {
            SDL_SetGPURenderState(app.renderer, nullptr);
        }
    }
    if (std::exchange(app.screenshotWanted, false)) {
        saveF12Screenshot(app);
    }
}

namespace {

/// --frame-stats: this frame's time noted; every 5 s the spread logged.
void noteFrameStats(App& app)
{
    const std::uint64_t now = SDL_GetTicksNS();
    if (app.lastPresentNs != 0) {
        app.frameTimes.push_back(static_cast<float>(now - app.lastPresentNs) / 1e6f);
    }
    app.lastPresentNs = now;
    const std::uint64_t ms = now / 1000000;
    if (app.frameStatsSince == 0) {
        app.frameStatsSince = ms;
        app.madeBefore = app.sprites ? app.sprites->made() : 0;
        return;
    }
    if (ms - app.frameStatsSince < 5000 || app.frameTimes.empty()) {
        return;
    }
    auto t = app.frameTimes;
    std::sort(t.begin(), t.end());
    double sum = 0;
    for (const float f : t) {
        sum += f;
    }
    const auto over = std::count_if(t.begin(), t.end(), [](float f) { return f > 33.4f; });
    const std::size_t made = app.sprites ? app.sprites->made() : 0;
    const SpriteCache::LoadStats loads = app.sprites ? app.sprites->takeLoadStats() : SpriteCache::LoadStats{};
    SDL_Log("frames: %zu in %.1f s, avg %.2f ms, p99 %.2f ms, worst %.2f ms, %d over 33 ms; %zu images made (%zu HD in "
            "the frame wanting them, %.1f ms; %zu floor waits, %.1f ms), %zu MB of textures, %zu sprites held, %zu effects",
            t.size(), static_cast<double>(ms - app.frameStatsSince) / 1000.0, sum / static_cast<double>(t.size()),
            static_cast<double>(t[t.size() * 99 / 100]), static_cast<double>(t.back()), static_cast<int>(over), made - app.madeBefore,
            loads.misses, loads.missMs, loads.waits, loads.waitMs, app.sprites ? app.sprites->textureBytes() >> 20 : 0,
            app.sprites ? app.sprites->loaded() : 0, app.effects ? app.effects->size() : 0);
    if (!app.worldDrawTimes.empty()) {
        // The 2D world's draw on this thread (floor, scene, a session's pending edits), less the GPU's own time.
        auto w = app.worldDrawTimes;
        std::sort(w.begin(), w.end());
        double total = 0;
        for (const float v : w) total += v;
        SDL_Log("world draw: avg %.3f ms, median %.3f ms, p99 %.3f ms over %zu frames%s", total / static_cast<double>(w.size()),
                static_cast<double>(w[w.size() / 2]), static_cast<double>(w[w.size() * 99 / 100]), w.size(),
                app.view && app.view->hasPending() ? " (pending edits drawn)" : "");
        app.worldDrawTimes.clear();
    }
    app.frameTimes.clear();
    app.frameStatsSince = ms;
    app.madeBefore = made;
}

} // namespace

/// The frame shown (the game we host runs on its own thread meanwhile: LocalHost), then the
/// sprite cache puts in the HD images decoded meanwhile.
void present(App& app)
{
    if (app.host) {
        app.host->publishClient(); // (our client as this frame leaves it, for the host's reads)
    }
#if NOX_PRIVATE_TOOLS
    if (app.live) {
        const auto& world = app.live->client().world();
        if (const auto ours = world.drawables().find(app.live->client().netCode()); ours != world.drawables().end()) {
            devhooks::gameView(static_cast<float>(ours->second.x), static_cast<float>(ours->second.y), app.camera.left(), app.camera.top(), app.camera.zoom,
                               app.letterboxed ? app.gameArea.x : 0.0f, app.letterboxed ? app.gameArea.y : 0.0f);
        }
    }
    devhooks::beforePresent(app.renderer);
#endif
    SDL_RenderPresent(app.renderer);
    if (app.sprites) {
        // (HD decodes put in place, what wasn't drawn for a while let go: SpriteCache.)
        app.sprites->endFrame(SDL_GetTicks());
    }
    if (app.frameStats) {
        noteFrameStats(app);
    }
}

/// A frame of the movie playing, over the whole window (no gamma ramp, no pointer); a
/// --screenshot run's shot once it's due.
SDL_AppResult showMovie(App& app, bool shotReady)
{
    int w = 0;
    int h = 0;
    SDL_GetCurrentRenderOutputSize(app.renderer, &w, &h);
    const auto area = backend::usableRenderArea(app.renderer);
    if (area) {
        SDL_SetRenderViewport(app.renderer, &*area);
        w = area->w;
        h = area->h;
    }
    app.movie->draw(w, h);
    if (area) SDL_SetRenderViewport(app.renderer, nullptr);
    if (shotReady && SDL_GetTicks() >= static_cast<std::uint64_t>(app.shotAfterSeconds * 1000)) {
        app.shotTaken = true;
        SDL_Log("movie: %s frame %d", app.movie->hd() ? "HD" : "VQA", app.movie->frame());
        return saveScreenshot(app) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    present(app);
    return SDL_APP_CONTINUE;
}

} // namespace noxsdl
