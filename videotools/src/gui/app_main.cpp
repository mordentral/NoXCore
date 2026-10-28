// noxcontent, the NoX Content Editor: opens a game folder's files (thing.bin, modifier.bin,
// gamedata.bin, monster.bin, soundset.bin, nox.csf, video.bag and the HD image pack, Audio.bag,
// the windows, maps, characters and mods) in one docked window, laid out like the map editor's.
//
//   noxcontent [--game-dir DIR] [--pack FILE | --no-pack]
//
// The game folder is found as noxcore.exe finds it (--game-dir, NOX_DATA, this program's folder,
// the current folder, the installs); the HD pack defaults to video_hd.nxp beside this program,
// else in the game folder. Settings (folders, theme, zoom...) and the window layout are kept in
// SDL's per-user preferences folder (%APPDATA%\noxcpp\noxcontent), never in the game folder.
//
// Checking the UI without clicking through it: --screenshot out.png [--wait SECONDS] saves the
// window and quits, hidden (settings and layout untouched); --view images|catalog|record (the
// panel in front), --show LIST:NAME (thing:GruntAxe, monster:GruntAxe, string:QuitMenu.wnd:Quit...),
// --search TEXT (the Content search), --theme dark|medium|light, --index N, --hd-view
// side|sd|hd, --select Entry[:Animation[:Direction]] (the Sprites view), --frame N, --fill,
// --overlays, --tint set what it shows; --put FILE|DIR puts PNGs into their slots at start;
// --size WxH sets the window's size; --mods-dir DIR the mods folder; --content-pack ID the pack opened.

#include "gui/shell.hpp"
#include "image_source.hpp"
#include "png_write.hpp"
#include "window_icon.hpp" // (noxsdl/src/icon: nox_app_icon puts it on the include path)

#include "editor/imgui_host.hpp"
#include "editor/theme.hpp"

#include "noxcore/io/file.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace videotools;
using namespace videotools::gui;

namespace {

constexpr const char* kTitle = "NoX Content Editor";

fs::path exeDir()
{
    const char* base = SDL_GetBasePath();
    return base ? pathFromUtf8(base) : fs::path();
}

fs::path documentsDir()
{
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return docs ? pathFromUtf8(docs) / kTitle : fs::path();
}

fs::path prefDir(const char* app)
{
    char* pref = SDL_GetPrefPath("noxcpp", app);
    fs::path dir = pref ? pathFromUtf8(pref) : fs::current_path();
    SDL_free(pref);
    return dir;
}

bool holdsBag(const std::string& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::exists(pathFromUtf8(dir) / "video.bag", ec) && fs::exists(pathFromUtf8(dir) / "video.idx", ec);
}

void saveShot(SDL_Renderer* renderer, const fs::path& file)
{
    SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
    if (!shot) {
        return;
    }
    if (SDL_Surface* rgba = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32)) {
        std::vector<std::uint8_t> pixels(std::size_t(rgba->w) * std::size_t(rgba->h) * 4);
        for (int y = 0; y < rgba->h; ++y) {
            std::memcpy(&pixels[std::size_t(y) * std::size_t(rgba->w) * 4],
                        static_cast<const std::uint8_t*>(rgba->pixels) + std::size_t(y) * std::size_t(rgba->pitch), std::size_t(rgba->w) * 4);
        }
        nox::writeFileBytes(file, encodePng(rgba->w, rgba->h, 4, pixels));
        SDL_DestroySurface(rgba);
    }
    SDL_DestroySurface(shot);
}

} // namespace

int main(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, SDL_GetError(), nullptr);
        return 1;
    }
    const fs::path prefs = prefDir("noxcontent");
    {
        // The image editor's settings carried over once (it was noxvideo before).
        std::error_code ec;
        const fs::path old = prefDir("noxvideo") / "settings.txt";
        if (!fs::exists(prefs / "settings.txt", ec) && fs::exists(old, ec)) {
            fs::copy_file(old, prefs / "settings.txt", ec);
        }
    }
    Settings settings;
    loadSettings(prefs / "settings.txt", settings);
    std::string selectArg, gameArg, packArg, putArg, recordArg, searchArg, themeArg;
    fs::path screenshot;
    bool overlays = false, tintArg = false, noPack = false, fillArg = false;
    int screenshotFrame = 90;
    std::string view;
    int hdView = -1;
    std::int32_t indexArg = -1;
    int frameArg = -1;
    int sizeW = 0, sizeH = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--game-dir" && hasValue) {
            gameArg = argv[++i];
        } else if (arg == "--pack" && hasValue) {
            packArg = argv[++i];
        } else if (arg == "--no-pack") {
            noPack = true;
        } else if (arg == "--select" && hasValue) {
            selectArg = argv[++i];
        } else if (arg == "--screenshot" && hasValue) {
            screenshot = pathFromUtf8(argv[++i]);
        } else if (arg == "--overlays") {
            overlays = true;
        } else if (arg == "--wait" && hasValue) {
            screenshotFrame = std::max(1, int(std::atof(argv[++i]) * 60));
        } else if (arg == "--out-dir" && hasValue) {
            settings.outDir = argv[++i];
        } else if (arg == "--tint") {
            tintArg = true;
        } else if (arg == "--view" && hasValue) {
            view = argv[++i];
        } else if (arg == "--put" && hasValue) {
            putArg = argv[++i];
        } else if (arg == "--index" && hasValue) {
            indexArg = std::atoi(argv[++i]);
        } else if (arg == "--hd-view" && hasValue) {
            const std::string_view v = argv[++i];
            hdView = v == "sd" ? 1 : v == "hd" ? 2 : 0;
        } else if (arg == "--frame" && hasValue) {
            frameArg = std::atoi(argv[++i]);
        } else if (arg == "--fill") {
            fillArg = true;
        } else if (arg == "--show" && hasValue) {
            recordArg = argv[++i];
        } else if (arg == "--search" && hasValue) {
            searchArg = argv[++i];
        } else if (arg == "--theme" && hasValue) {
            themeArg = argv[++i];
        } else if (arg == "--mods-dir" && hasValue) {
            settings.modsDir = argv[++i];
        } else if (arg == "--content-pack" && hasValue) {
            settings.packId = argv[++i];
        } else if (arg == "--size" && hasValue) {
            // WxH: the window's size in pixels (a recording's), else 1600 x 950 scaled, maximised.
            if (std::sscanf(argv[++i], "%dx%d", &sizeW, &sizeH) != 2 || sizeW < 320 || sizeH < 240) {
                sizeW = sizeH = 0;
            }
        }
    }
    const bool shotRun = !screenshot.empty();
    if (!themeArg.empty()) {
        settings.theme = themeArg;
    }
    // The game folder: --game-dir, else the one used last if it still has the bag, else found as
    // noxcore.exe finds it.
    if (!gameArg.empty()) {
        settings.gameDir = gameArg;
    } else if (!holdsBag(settings.gameDir)) {
        settings.gameDir = utf8(locateGameDir(exeDir()));
    }
    // The HD pack: --pack, else the one used last, else video_hd.nxp beside this program or in the
    // game folder.
    fs::path pack;
    if (!noPack) {
        std::error_code ec;
        if (!packArg.empty()) {
            pack = pathFromUtf8(packArg);
        } else if (!settings.packPath.empty() && fs::exists(pathFromUtf8(settings.packPath), ec)) {
            pack = pathFromUtf8(settings.packPath);
        } else {
            for (const fs::path& p : {exeDir() / "video_hd.nxp", pathFromUtf8(settings.gameDir) / "video_hd.nxp"}) {
                if (!p.empty() && fs::exists(p, ec)) {
                    pack = p;
                    break;
                }
            }
        }
    }
    if (settings.outDir.empty()) {
        settings.outDir = utf8(documentsDir() / "export");
    }
    if (settings.extractDir.empty()) {
        settings.extractDir = utf8(documentsDir() / "extracted");
    }

    const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    SDL_Window* window = SDL_CreateWindow(kTitle, sizeW ? sizeW : int(1600 * scale), sizeH ? sizeH : int(950 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    noxsdl::setWindowIcon(window); // (res/noxcontent.ico; on Windows the exe's own icon is the class's already)
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!shotRun) {
        if (!sizeW) {
            SDL_MaximizeWindow(window);
        }
        SDL_ShowWindow(window);
    }
    // (A screenshot run stays hidden: the renderer draws and reads back without showing it.)

    {
        // The map editor's ImGui host: docking, the themes, the floating panels (none in a
        // screenshot run, whose layout isn't kept either).
        noxsdl::editor::Theme theme = noxsdl::editor::Theme::Dark;
        noxsdl::editor::themeFromKey(settings.theme, theme);
        const std::string iniFile = shotRun ? std::string() : utf8(prefs / "layout.ini");
        noxsdl::editor::ImGuiHost host(window, renderer, iniFile, float(settings.uiScalePercent) / 100.0f, theme, settings.floating && !shotRun,
                                       shotRun);
        // A screenshot run leaves the saved settings alone (it may point at other folders).
        Shell app(window, renderer, host, shotRun ? fs::path() : prefs / "settings.txt", settings, shotRun);
        app.openGame();
        if (!pack.empty()) {
            app.openPack(pack);
        }
        if (!selectArg.empty()) {
            // Entry[:Animation[:Direction]]
            const std::string_view arg = selectArg;
            const auto c1 = arg.find(':');
            const auto c2 = c1 == std::string_view::npos ? c1 : arg.find(':', c1 + 1);
            app.catalog().selectByName(arg.substr(0, c1), c1 == std::string_view::npos ? std::string_view() : arg.substr(c1 + 1, c2 - c1 - 1),
                                       c2 == std::string_view::npos ? std::string_view() : arg.substr(c2 + 1));
            app.focus(panel::kSprites);
        }
        if (!recordArg.empty() && !app.selectByText(recordArg)) {
            app.session().setStatus("--show: nothing called " + recordArg);
        }
        if (!searchArg.empty()) {
            app.setSearch(searchArg);
        }
        if (view == "images") {
            app.focus(panel::kImages);
        } else if (view == "catalog" || view == "sprites") {
            app.focus(panel::kSprites);
        } else if (view == "record") {
            app.focus(panel::kRecord);
        }
        if (indexArg >= 0) {
            app.images().show(indexArg);
        }
        if (hdView >= 0) {
            app.images().setView(hdView);
        }
        if (!putArg.empty()) {
            // --put FILE|DIR: PNGs into their slots at start (as Put in a folder of PNGs does).
            const fs::path put = pathFromUtf8(putArg);
            std::vector<InsertTask> tasks;
            for (auto& f : fs::is_directory(put) ? editor::findImageFiles(put) : std::vector{editor::findImageFile(put)}) {
                if (const auto slot = editor::fileSlot(f)) {
                    tasks.push_back({std::move(f), slot->first, slot->second});
                }
            }
            app.session().queueInserts(std::move(tasks));
        }
        app.catalog().setOverlays(overlays, overlays);
        app.catalog().setTint(tintArg);
        app.catalog().setFillFromBag(fillArg);
        if (frameArg >= 0) {
            app.catalog().setFrame(frameArg);
        }
        std::uint64_t last = SDL_GetTicksNS();
        int frames = 0;
        for (bool done = false; !done; ++frames) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                host.processEvent(event);
                const bool quit = event.type == SDL_EVENT_QUIT ||
                                  (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window));
                if (quit && (shotRun || app.mayQuit())) {
                    done = true;
                }
            }
            done = done || app.quitConfirmed();
            if (!shotRun && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
                SDL_Delay(10);
                continue;
            }
            const std::uint64_t now = SDL_GetTicksNS();
            const double dt = double(now - last) / 1e9;
            last = now;

            host.newFrame();
            app.frame(dt);
            SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
            SDL_RenderClear(renderer);
            host.render();
            if (shotRun && frames == screenshotFrame) {
                saveShot(renderer, screenshot);
                done = true;
            }
            SDL_RenderPresent(renderer);
            if (shotRun) {
                SDL_Delay(1); // (hidden: no vsync to pace it)
            }
        }
    } // the views' textures and the ImGui host go before the renderer

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
