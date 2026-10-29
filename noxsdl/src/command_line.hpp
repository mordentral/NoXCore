// The command line (main.cpp's header has the flags): read into the App's switches and the
// start-up's choices (StartOptions), which SDL_AppInit acts on.
#pragma once

#include "app.hpp"

#include "noxcore/lobby/browser.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace noxsdl {

/// What the command line chose that SDL_AppInit acts on; the switches that go straight into the
/// App (tests, the viewer's camera, the 3D view...) are set there.
struct StartOptions {
    bool gameDirGiven = false; ///< --game-dir; else the installed game is looked for
    std::string mapArg = "Estate"; ///< --map: the viewer's map
    std::string connect;           ///< --connect HOST[:PORT] (or the game --host / --solo hosts here)
    nox::client::PlayerProfile profile; ///< --name, --class, --hair... (the menus' till one is chosen)
    std::filesystem::path mapCache = "noxsdl_maps"; ///< --map-cache: where downloaded maps go
    std::string mapPicture;                ///< --map-picture MAP OUT.png: the map vote's picture written out
    std::filesystem::path mapPictureOut;
    int width = 1280; ///< --size WxH: the window
    int height = 800;
    bool haveAt = false;   ///< --at X Y gave the viewer's camera
    bool mapGiven = false; ///< --map given (else the menus)
    bool browse = false;   ///< --browse: the menus' game list straight away
    std::string editorMap; ///< --map-editor MAP
    std::string hostEditMap; ///< --host-edit MAP
    std::string hostEditMod; ///< --host-edit-mod ID: the session carries mods\ID
    bool hostEditFromMenus = false; ///< --host-edit-menus (tests): the menus behind the session's editor
    std::vector<std::string> menuClicks; ///< --menu-clicks (tests)
    bool noMovies = false;   ///< --no-movies
    bool noHdMovies = false; ///< --no-hd-movies
    std::string movieArg;    ///< --movie NAME
    bool forceStartUp = false; ///< --startup
    std::filesystem::path hdDirArg;       ///< --hd-dir DIR
    std::filesystem::path saveDir; ///< --save-dir
    bool useGpu = true;       ///< SDL's GPU renderer and our shaders (--no-gpu: SDL's usual renderer)
    std::string gpuDriver;    ///< --gpu-driver: its API (direct3d12 by default on Windows; SDL's choice, Vulkan, elsewhere)
    bool stockEffects = false; ///< --stock-effects: the effects' pixel shapes and sprites (Smooth FX and HD Spell FX off)
    bool noBloom = false;      ///< --no-bloom: no halo round the bright effects
    std::optional<int> smoothing; ///< --smoothing off|movement|network|lock30
    std::optional<int> display;   ///< --display windowed|borderless|fullscreen
    std::optional<bool> vsync;    ///< --vsync / --no-vsync
    bool noSmoothMotion = false; ///< --no-smooth-motion: no drawing between snapshots
    std::optional<bool> predictMovement; ///< --predict-movement / --no-predict-movement
    bool noSmoothScrolling = false;      ///< --no-smooth-scrolling
    std::optional<int> otherUnits;       ///< --other-units smooth|newest|ahead
    bool hdImages = false;       ///< --hd-images: Use HD Images for this run
    bool noAudio = false;        ///< --no-audio: no sound at all
    std::string audioCapture;    ///< --audio-capture FILE: what's played, recorded
    bool audioLog = false;       ///< --audio-log: log the sounds asked for
    bool gridLight = false;    ///< --grid-light: the game's light grid instead of per-pixel lighting
    float bloomStrength = 0.2f; ///< --bloom N: how much of it is added (0-1)
    std::optional<std::pair<int, int>> videoMode; ///< --video-mode WxH: the view in a game (tests)
    std::optional<int> videoSize;                 ///< --video-size N: the game window's size (tests)
    nox::lobby::ServerBrowser::Options browserOptions;
    std::optional<HostSettings> hostSettings; ///< --host MAP: host a game here and join it
    int hostBots = 0;                         ///< --host-bots N: bots in it (the Bots tab's), the classes in turn
    std::optional<nox::mapedit::PendingView> pendingViewArg; ///< --pending-view
};

/// Reads the command line (argv[1] on), and the account: --account, else the saved online login.
StartOptions parseCommandLine(App& app, int argc, char* argv[]);

} // namespace noxsdl
