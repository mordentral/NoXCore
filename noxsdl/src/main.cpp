// noxsdl: the 2D front end. A map viewer, or a live view of a game.
//
//   noxsdl --map <name|path.map> [--game-dir DIR] [--at X Y] [--zoom Z]
//          [--size WxH] [--screenshot out.png [--shot-after SECONDS]] [--frames N] [--fx-demo]
//   noxsdl --connect HOST[:PORT] [--name NAME] [--class warrior|wizard|conjurer] [--hair RRGGBB] [--skin RRGGBB] [--team ID]
//          [--map-cache DIR] [--zoom Z] [--size WxH] [--screenshot out.png --shot-after SECONDS]
//   noxsdl --host <map> [--host-port 18590] [--host-name NAME] [--host-mode arena|ctf|flagball|kotr|elimination]
//          [--host-lessons N] [--host-time MINUTES] [--host-teams] [--host-sysop PW] [--host-bots N]
//          [the --connect options]
//   noxsdl --map-editor [map] [--editor-maps-dir DIR] [--editor-user-dir DIR] [--editor-run "cmd;cmd"] [--playtest-return S]: the map editor alone
//          (editor/map_editor.hpp); the main menu's Game Modes and Host Custom open it too
//   noxsdl --solo [--class warrior|wizard|conjurer]: the campaign, its class's first map, hosted here
//          hosts the game here (noxcore's server in this process, on that UDP port: others can join)
//          and joins it; the console (F1) runs the host's commands and noxcpp's "nx" test ones
//
// Viewer keys: arrows/WASD pan, +/- or the mouse wheel zoom, right-drag pans, Escape quits.
// Live: the camera follows us; hold the right mouse button to run toward the cursor, Space
// leaves observer mode, +/- zoom, Escape leaves the game. L toggles lighting, V line of sight,
// N the name plates over players.
// --screenshot renders into a PNG and exits (in a live game, --shot-after seconds after
// joining, then leaves) for checking the renderer headless.
// Movies (movie_player.hpp): the menus start with the start-up's movies and legal screen, but
// for --menu-clicks, --mouse-script and --screenshot (--startup: anyway); --no-movies plays
// none, --movie NAME plays movies\NAME first, --no-hd-movies keeps to the VQAs, --hd-dir DIR
// looks for the HD movies and FFmpeg's DLLs there instead of HDVideo\ beside the exe.
// The log (log_output.hpp) goes to noxcore-log.txt beside the exe and to stderr. On Windows
// noxsdl has no console window: -cmd (or --cmd) gives the log one (the terminal it was started
// from, else a new window); a redirected stderr gets the lines either way.
// The game folder: --game-dir DIR, else NOX_DATA, noxsdl's own folder, the current folder, the
// installed game, then the build's NOX_GAME_DIR if it was set (io/game_dir.hpp).
//
// This file: SDL's callbacks, the frame, the input's way to the windows and the game, and the
// start-up's movies. The frame's way to the screen is in frame.cpp, the game's cursor (its choice
// and draw) in game_cursor.cpp, the map editor's glue (its play-test and edit sessions) in
// editor_glue.cpp, the command line's reading in command_line.cpp;
// the App and the files that share it are listed in app.hpp.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app.hpp"
#include "game_data_swap.hpp"
#include "backend_hooks.hpp"
#include "character_play.hpp"
#include "client_level_gate.hpp"
#include "command_line.hpp"
#include "editor/editor_settings.hpp"
#include "editor/editor_text.hpp"
#include "effects_draw.hpp"
#include "icon/window_icon.hpp"
#include "identify_wait.hpp"
#include "leaving_notice.hpp"
#include "log_output.hpp"
#include "map_cache_dir.hpp"
#include "menu_rules.hpp"
#include "mods_folder.hpp"
#include "new_game.hpp"
#include "predicted_drop.hpp"
#include "snapshot_pacing.hpp"
#include "window_density.hpp"
#if NOX_PRIVATE_TOOLS
#include "dev_hooks.hpp"
#endif

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/bot/bot_squad.hpp"
#include "noxcore/client/inform_text.hpp"
#include "noxcore/client/solo_menu.hpp"
#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/io/game_dir.hpp"
#include "noxcore/io/png.hpp"
#include "noxcore/lobby/listing.hpp"
#include "noxcore/modes/builtin.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/modes/script/script_ui.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/version.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace noxsdl;

namespace {

/// The viewer's zoom, kept between 1/8 and 16 output pixels a world unit.
void zoomBy(Camera& cam, float factor)
{
    cam.zoom = std::clamp(cam.zoom * factor, 0.125f, 16.0f);
}

/// Nox facing units (0 east, 64 south, clockwise on screen) from us to the mouse: from where the
/// server last put us, not the view centre, which smooth scrolling keeps a little behind us (so
/// the facing sent, a swing's direction, isn't off by it). While the mouse is on a drawable (the
/// front pick), to the mouse's x at that drawable's own y, the y MSG_MOUSE carries for it
/// (Cli_ProcessControlEvents): a fireball sent up a creature's body flies at its feet.
std::uint8_t facingToMouse(const App& app)
{
    if (const auto f = app.view3d.facing()) {
        return *f; // the 3D view's facing
    }
    const SDL_FPoint m = mousePos(app);
    float fromX = app.camera.viewWidth / 2.0f;
    float fromY = app.camera.viewHeight / 2.0f;
    float toX = m.x;
    float toY = m.y;
    std::optional<int> frontY;
    if (app.live) {
        if (const auto at = app.live->client().position()) {
            fromX = static_cast<float>(at->first);
            fromY = static_cast<float>(at->second);
            toX = app.camera.left() + m.x / app.camera.zoom;
            toY = app.camera.top() + m.y / app.camera.zoom;
            if (app.hovered) {
                if (const auto* d = app.live->client().world().drawable(*app.hovered)) {
                    frontY = std::clamp(static_cast<int>(d->y), 0, 0xFFFF); // (as cursorToSend sends it)
                }
            }
        }
    }
    return pick::facing(fromX, fromY, toX, toY, frontY);
}
/// gameChangeMap after a map it had (not one just downloaded) is read: on a chat
/// map (MapInfo's 0x80000000) cdecode.c:EnterChat and cdecode.c:KeyToChat with the first key
/// bound to Chat (the keybind title lookup (8, 1); none, empty), centred.
void chatMapNotice(App& app)
{
    try {
        if (!(nox::map::decodeSection<nox::map::MapInfo>(app.map).gameFlags & 0x80000000u)) {
            return;
        }
    } catch (const std::exception&) {
        return;
    }
    centredMessage(app, gameString(app, "cdecode.c:EnterChat"));
    std::string key;
    if (app.input) {
        if (const auto bound = app.input->config().bindingsFor("Chat"); !bound.empty() && !bound.front()->keys.empty()) {
            key = InputBindings::layoutName(bound.front()->keys.front()); // (as this keyboard prints it: VD-66)
            if (const auto* k = nox::config::findKey(bound.front()->keys.front()); key.empty() && k) {
                key = gameString(app, k->title.empty() ? k->name : k->title);
            }
        }
    }
    centredMessage(app, nox::client::inform::format(gameString(app, "cdecode.c:KeyToChat"), {key}));
}
/// Live: load the server's map once it's known and on disk.
void syncLiveMap(App& app)
{
    // Each map the client loads (the same name again too: a saved game of this map), from the
    // file it loaded (the maps folder's of USE_MAP's CRC, a download, or a solo game's saved copy).
    const auto& client = app.live->client();
    if (client.mapName().empty() || client.mapLoads() == app.liveMapLoads) {
        return;
    }
    const auto path = app.live->mapPath();
    if (!path) {
        return; // still downloading
    }
    app.liveMapLoads = client.mapLoads();
    try {
        app.map = nox::map::loadMapFile(*path);
        app.view = std::make_unique<MapView>(app.map, app.things, *app.art, app.modifiers, MapView::ObjectSet::ClientLoaded);
        app.liveMap = client.mapName();
        app.viewMap = path->filename().string();
        preloadMap(app, app.viewMap);
        // Not shown till its HD images (and the session's) are in: no original stands in for them.
        // Without the pack, till the originals its first view draws are made, a few a frame
        // (holdMap): made all in its first frame they froze the window for seconds under load.
        const bool hold = app.sprites && ((app.sprites->preloading() && !app.hdPlaceholders) || !app.sprites->usingPack());
        app.mapHeldSinceMs = hold ? std::max<std::uint64_t>(1, SDL_GetTicks()) : 0;
        app.effects->clear();
        app.wallsApplied.reset();
        if (app.minimap) {
            app.minimap->mapLoaded(app.map);
        }
        const auto remembered = std::ranges::count_if(app.view->walls(), [](const nox::map::Wall& w) { return w.unknown != 0; });
        SDL_Log("map %s (the minimap's memory: %d walls, %d obelisks)", editor::pathText(*path).c_str(), static_cast<int>(remembered),
                app.minimap ? static_cast<int>(app.minimap->obelisksListed()) : 0);
        if (!std::exchange(app.mapDownloaded, false)) {
            chatMapNotice(app);
        }
    } catch (const std::exception& ex) {
        SDL_Log("loading %s failed: %s", editor::pathText(*path).c_str(), ex.what());
        app.liveMap = client.mapName(); // don't retry every frame
    }
}

/// Whether the map just loaded is still held back this frame (App::mapHeld): till the session's
/// and its own HD images are in (SpriteCache::preloading) and the HD floor in view is composited,
/// at most kMapHoldMs (then the rest are made as they're first drawn, still in HD). Without the
/// pack, till the originals the first view draws are made, kOriginalsMs of them a frame
/// (MapView::prefetchOriginals). Logged when it ends.
void holdMap(App& app)
{
    constexpr std::uint64_t kMapHoldMs = 20000;
    constexpr std::uint64_t kOriginalsMs = 8;
    app.mapHeld = false;
    if (app.mapHeldSinceMs == 0 || !app.sprites) {
        return;
    }
    const std::uint64_t held = SDL_GetTicks() - app.mapHeldSinceMs;
    const bool pack = app.sprites->usingPack();
    bool waiting = pack && app.sprites->preloading();
    if (app.view) {
        // (Asked for each frame: where the view is once we're in.)
        Camera at = app.camera;
        if (const auto pos = app.interpolator.camera()) {
            at.x = pos->x;
            at.y = pos->y;
        }
        waiting = (pack ? app.view->prefetchFloor(at, *app.sprites)
                        : app.view->prefetchOriginals(at, *app.sprites, SDL_GetTicksNS() + kOriginalsMs * 1000000)) ||
                  waiting;
    }
    if (!waiting || held >= kMapHoldMs) {
        SDL_Log("map %s: shown after %llu ms of %s%s; textures %s", app.viewMap.c_str(), static_cast<unsigned long long>(held),
                pack ? "loading its HD images" : "making its first view's images",
                waiting ? " (timed out: the rest are made as they're drawn)" : "", app.sprites->textureSummary().c_str());
        app.mapHeldSinceMs = 0;
    } else {
        app.mapHeld = true;
    }
    app.sprites->setHolding(app.mapHeld);
}

/// The heartbeat under 40% health (the health tube's draw): every (3/4 s x health / that)
/// + 1/3 s, louder the lower it is (33 to 99).
void heartBeat(App& app)
{
    if (!app.audio || !app.live) {
        return;
    }
    const auto& self = app.live->client().world().self();
    const int limit = self.maxHealth * 2 / 5;
    const int health = self.health;
    if (health <= 0 || health >= limit) {
        return;
    }
    constexpr int kFps = 30;
    const double period = static_cast<double>((kFps * 3 / 4) * health / limit + kFps / 3);
    const double now = app.interpolator.gameTime();
    if (now - app.lastHeartBeat > period - 1) {
        app.lastHeartBeat = now;
        app.audio->play(0x380, (limit - health) * 66 / limit + 33); // HeartBeat
    }
}

/// An NPC's line or a shop's greeting spoken as it opens (Gui_NpcDialog_Open via its opener,
/// Gui_Shop_Open): Str_GetString picks one of the string's variants at random, and its text and
/// speech file go together; the pick is kept in speakingVariant for
/// the dialog window. Closing it fades the speech out.
void speech(App& app)
{
    if (!app.audio || !app.live || !app.strings) {
        return;
    }
    const auto& world = app.live->client().world();
    std::string id;
    if (world.dialog().open) {
        id = "dialog:" + world.dialog().textId;
    } else if (world.trade().kind == nox::client::TradeState::Kind::Shop) {
        id = "shop:" + world.trade().greeting;
    }
    if (id == app.speaking) {
        return;
    }
    app.speaking = id;
    if (id.empty()) {
        app.audio->stopDialog();
        return;
    }
    const auto label = id.substr(id.find(':') + 1);
    if (const auto* entry = app.strings->find(label); entry && !entry->variants.empty()) {
        app.speakingVariant = pickVariant(app, *entry);
        if (const auto& voice = entry->variants[app.speakingVariant].extra; !voice.empty()) {
            app.audio->dialog(voice);
        }
    }
}

} // namespace

/// The main menu (game_showMainMenu, with its music "shell").
void startMainMenu(App& app)
{
    if (app.audio) {
        app.audio->music(25);
    }
    app.menus->showMainMenu();
}

/// game_showLegal (its music, 0xE "title"), then the main menu.
void showLegal(App& app)
{
    const gui::Resources res = guiResources(app);
    try {
        app.legal = std::make_unique<LegalScreen>(res, app.gameDir / "window", [&app] { startMainMenu(app); });
    } catch (const std::exception& ex) {
        SDL_Log("legal.wnd: %s", ex.what());
        startMainMenu(app);
        return;
    }
    if (app.audio) {
        app.audio->music(14);
    }
}

/// The start-up (state 10, game_switchStates): game_rollLogoAndStart's
/// WWLogo, game_rollIntroAndStart's Intro (once: TrackData 0x80), then
/// game_rollNoxLogoAndStart's NoxLogo and the legal screen. (Game flag 0x2000000,
/// which skips each, is never set in noxsdl's menus.)
void rollNoxLogo(App& app)
{
    playMovie(app, "NoxLogo.vqa", movies::Skip::AnyInput, [&app] { showLegal(app); });
}

void rollIntro(App& app)
{
    if (movies::introAtStartup(trackData(app)) && app.movie && app.movie->available("Intro.vqa")) {
        app.movie->play("Intro.vqa", movies::Skip::EscapeOnly, [&app] {
            rollNoxLogo(app);
            setTrackData(app, movies::trackDataAfterIntro(trackData(app)));
        });
        return;
    }
    rollNoxLogo(app);
}

void startUp(App& app)
{
    playMovie(app, "WWLogo.vqa", movies::Skip::AnyInput, [&app] { rollIntro(app); });
}

namespace {

/// The menus' callbacks (Menus::Callbacks): a game joined or hosted from the game list or Host
/// Custom (the editor's session among them), the main menu's Solo, a mode started through the solo
/// screens, the music, the Intro, Quit, the editor and Options. `opts`: the online list's (XWIS) and
/// the saved login's account, as the command line left them.
Menus::Callbacks menusCallbacks(App* app, const StartOptions& opts)
{
    Menus::Callbacks callbacks;
    callbacks.join = [app, xwis = opts.browserOptions.xwis, account = opts.profile.account](
                         const std::string& host, const nox::client::PlayerProfile& chosen) {
        // (Westwood Online's list; a game joined from Network's, LAN play, isn't online)
        app->online = xwis && !(app->menus && app->menus->lanOnly());
        app->profile = chosen;
        if (app->menus && app->menus->skippedLogin()) {
            app->profile.account.clear(); // (Skip Login: no account name at all)
        } else if (app->profile.account.empty()) {
            app->profile.account = account; // (the saved online login's, as the game sends it)
        }
        app->characterFile = app->menus ? app->menus->characterFile() : fs::path{};
        if (host == Menus::kHostEditGame) {
            // Host Custom > Map Editor: the editor with its session dialog (the map, who
            // may edit, the sandbox...); Host hosts the session and joins it, Cancel goes
            // back to the game list.
            openEditor(*app, {});
            app->editorFromList = true; // (its close back to this game list, Network's or XWIS's)
            showEditSessionSetup(*app, true);
            return;
        }
        std::string address = host;
        const std::optional<Menus::ModeHost> modeHost = Menus::modeOfHostKey(host);
        const bool hostGame = host == Menus::kHostGame || host == Menus::kHostQuestGame || (modeHost && !modeHost->local);
        // Game Modes' NoX Core modes: as Host Custom's, but on this computer only.
        const bool soloMode = modeHost && modeHost->local;
        if (hostGame || host == Menus::kSoloQuestHost || soloMode) {
            // The Host button: our own game on So_Druid, the chat map (noxsdl has no world
            // map to click one on yet), joined like any other; the Server Settings come
            // up there for the game type and map. Host Quest (0x2713) and the main menu's
            // Solo Quest: a quest chosen (setQuest(1)) on So_Druid, the
            // chat map Solo Quest's world map click picks (menus.cpp onSoloQuest); in it,
            // OTQuest.rul begins the quest (questStartRul). Host Quest's game is a network
            // one others join (posted to XWIS when logged in); Solo Quest's a LAN one.
            HostSettings settings;
            if (hostGame) {
                hostSettingsFromConfig(*app, settings);
                settings.wolGame = app->online && app->menus && app->menus->hostLogin();
                settings.map = "So_Druid";
            }
            if (host == Menus::kHostQuestGame) {
                settings.quest = true;
                app->hostQuestRul = true;
            }
            if (modeHost) {
                // Host Custom's NoX Core mode (built in or a mod's: its own map or
                // generated lobby), with the settings its row had.
                settings.modeId = modeHost->id;
                settings.modeOptions = app->menus ? app->menus->pendingModeOptions() : nox::modes::OptionValues{};
            }
            if (soloMode) {
                // Local and unlisted: no router port, no XWIS post; its end back to the main menu.
                hostSettingsFromConfig(*app, settings);
                settings.map = "So_Druid";
                app->online = false;
                app->soloFromMenus = true;
            }
            if (host == Menus::kSoloQuestHost) {
                settings.quest = true;
                settings.map = "So_Druid";
                settings.updateRate = app->serverUpdateRate; // (noxcore.cfg's)
                // The server option flags are nox.cfg's whatever the game
                // (g_serverOptionFlags), so the Server
                // Settings here don't write them back to nox.cfg as off.
                HostSettings fromConfig;
                hostSettingsFromConfig(*app, fromConfig);
                settings.itemRespawn = fromConfig.itemRespawn;
                settings.camperAlarm = fromConfig.camperAlarm;
                app->hostQuestRul = true;
                // The exe presses Network on the Online screen for it (a LAN game): no
                // WOL login, so no account name, and its end back to
                // the main menu as a solo game's, not to the game list.
                app->online = false;
                app->profile.account.clear();
                app->soloFromMenus = true;
            }
            {
                // (Local tests: --menu-host-port, --host-loopback; normally the game's port. Game
                // Modes' own modes and its Quest too, so a test run never needs 18590.)
                const MenuHostSocket socket = menuHostSocket({settings.port, settings.loopbackOnly}, app->menuHostPort, app->hostLoopback);
                settings.port = socket.port;
                settings.loopbackOnly = socket.loopbackOnly;
            }
            try {
                app->host = makeHost(*app, settings);
                address = app->host->address();
                SDL_Log("hosting at %s", address.c_str());
                startPortMapping(*app, hostGame); // (Solo Quest: a LAN game)
                // Westwood Online's list (logged in, or Skip Login's generated login): posted
                // to XWIS, as GAME.EXE's host's game is. (Solo Quest is a LAN game: not posted.)
                app->xwisHost.reset();
                if (hostGame && app->online && app->menus && app->menus->hostLogin()) {
                    startXwisPost(*app, *app->menus->hostLogin());
                }
                SDL_Log("%s", hostListingLog(menusHostListing(*app, hostGame), "hosting", app->host->port()).c_str());
            } catch (const nox::server::MapRefused& ex) {
                SDL_Log("hosting failed: %s", ex.what());
                app->menus->memoryTricksNotice(ex.map); // (noxcpp: told why, not only logged)
                return;
            } catch (const std::exception& ex) {
                SDL_Log("hosting failed: %s", ex.what());
                return;
            }
        }
        SDL_Log("joining %s, account: %s", address.c_str(), app->profile.account.empty() ? "(none)" : app->profile.account.c_str());
        if (!app->host && app->browser && joinWaitsForPing(app->browser->entryFor(address))) {
            // An XWIS game whose ping is still out ("Checking..."): its session starts once
            // the ping has answered or given up (joinWaitFrame, PingWait), so how it's
            // joined (stock-shaped or asked first) is known; the join's dialog up meanwhile.
            app->pingWaitHost = address;
            app->pingWaitStartedAt = static_cast<std::uint32_t>(SDL_GetTicks());
            SDL_Log("join: %s's ping is still out: the join waits for it (at most %u ms)", address.c_str(),
                    PingWait::kMaxMs);
            app->joinWaiting = true;
            app->joinWaitShow = true;
            return;
        }
        startLive(*app, address);
        notePortMoved(*app);
        if (!app->host) {
            // Another's game (listed, saved or LAN): the menus stay up with the 1.2b
            // client's "Attempting to connect" and its Cancel till the host lets us in
            // (joinWaitFrame). Our own game is joined at once, as the exe's host's is.
            gui::setShellUp(true);
            app->joinWaiting = true;
            app->joinWaitShow = true;
        }
    };
    callbacks.solo = [app](const Menus::SoloStart& start) {
        // The main menu's Solo: our own solo game (as --solo), its character the one the
        // screens made or loaded (clientCurSave), its file loaded onto our
        // player as it's made (LocalHost, GameHost::Options::hostPlayerFile); continuing a
        // save, started on its map's copy (GameHost::Options::startFromSave).
        app->profile = start.profile;
        app->characterFile = start.characterFile;
        app->heldFileInfo = start.info;
        HostSettings settings;
        settings.solo = true;
        settings.updateRate = app->serverUpdateRate; // (noxcore.cfg's)
        settings.map = start.map.string();
        settings.saveRoot = app->saveRoot;
        settings.continueSlot = start.continueSlot;
        settings.characterFile = start.characterFile;
        // The exe's solo game opens no network socket at all (its server's listening
        // socket only without game flag 0x800); ours is joined by our own client alone:
        // on the loopback, any free port, so another program's game here (a server on
        // 18590, say) is never the one joined.
        settings.loopbackOnly = true;
        settings.port = 0;
        try {
            app->host = makeHost(*app, settings);
        } catch (const std::exception& ex) {
            SDL_Log("solo: %s", ex.what());
            app->menus->showMainMenu();
            if (const auto* refused = dynamic_cast<const nox::server::MapRefused*>(&ex)) {
                app->menus->memoryTricksNotice(refused->map); // (noxcpp: told why instead of a hang)
            }
            return;
        }
        app->soloFromMenus = true;
        SDL_Log("solo: hosting %s%s", settings.map.c_str(),
                start.continueSlot.empty() ? "" : (", continuing " + start.continueSlot).c_str());
        startLive(*app, app->host->address());
    };
    callbacks.soloScreensMode = [app, xwis = opts.browserOptions.xwis](const std::string& modeId, const Menus::SoloStart& start) {
        // Host Custom > a mode started through the solo screens (a campaign mode): hosted
        // here as a solo game with room for guests (the mode's server side, its saves its
        // own: mode_ui.hpp's SoloScreensStart), listed as the other hosted modes are (the
        // LAN, XWIS when logged in, the router's port), then joined. (Online as join's:
        // Westwood Online's game list, not Network's LAN play. The game stays a solo one,
        // not a WOL game's rules, wherever it's listed.)
        // (Game Modes' campaign row: the same game, local and unlisted as Game Modes' other modes
        // are: no router port, no XWIS post, its end back to the main menu.)
        const bool local = app->menus && app->menus->soloScreensLocal();
        const auto backToMenus = [app, local] {
            if (local) {
                app->menus->showMainMenu();
            } else {
                app->menus->showGameBrowser();
            }
        };
        const ModeUiEntry* entry = modeUiEntry(modeId);
        if (!entry || !entry->soloScreens) {
            SDL_Log("no game mode %s", modeId.c_str());
            backToMenus();
            return;
        }
        const SoloScreensStart& how = *entry->soloScreens;
        app->online = !local && xwis && !(app->menus && app->menus->lanOnly());
        app->profile = start.profile;
        app->characterFile = start.characterFile;
        app->heldFileInfo = start.info;
        HostSettings settings;
        settings.solo = true;
        settings.updateRate = app->serverUpdateRate; // (noxcore.cfg's)
        settings.map = start.map.string();
        // (A mod's campaign, the row's Campaign choice: its saves its own, under the mode's.)
        std::string campaign;
        if (app->menus) {
            if (const auto c = nox::modes::option(app->menus->pendingModeOptions(), "campaign")) {
                campaign = std::string(*c);
            }
        }
        beginSoloScreensHost(*app, modeId, start.profile.playerClass, settings, campaign);
        if (app->menus) {
            // (Host Custom's row's settings for it, the class the screens chose kept.)
            for (const auto& [key, value] : app->menus->pendingModeOptions()) {
                if (key != "class") {
                    settings.modeOptions[key] = value;
                }
            }
        }
        settings.continueSlot = start.continueSlot;
        settings.characterFile = start.characterFile;
        settings.loopbackOnly = app->hostLoopback;
        if (app->menuHostPort != 0) {
            settings.port = app->menuHostPort; // (--menu-host-port: tests, recordings)
        }
        const bool listed = !local && !app->hostLoopback;
        try {
            app->host = makeHost(*app, settings);
            startPortMapping(*app, listed);
            app->xwisHost.reset();
            if (app->online && app->menus && app->menus->hostLogin()) {
                startXwisPost(*app, *app->menus->hostLogin());
            }
        } catch (const std::exception& ex) {
            SDL_Log("%s: %s", how.logTag.c_str(), ex.what());
            endSoloScreensHost(*app);
            backToMenus();
            if (const auto* refused = dynamic_cast<const nox::server::MapRefused*>(&ex)) {
                app->menus->memoryTricksNotice(refused->map);
            }
            return;
        }
        // (Its end back to the game list it was hosted from, as the other hosted modes'.)
        const nox::modes::Mode* campaignMode = campaign.empty() ? nullptr : nox::modes::registry().find(campaign);
        const std::string what = campaignMode && campaignMode->info.campaign ? campaignMode->info.campaign->title
                                 : how.what                                 ? how.what(start.profile.playerClass)
                                                                            : modeId;
        SDL_Log("%s: hosting %s at %s%s", how.logTag.c_str(), what.c_str(),
                settings.map.c_str(), start.continueSlot.empty() ? "" : (", continuing " + start.continueSlot).c_str());
        SDL_Log("%s", hostListingLog(menusHostListing(*app, listed), how.logTag, app->host->port()).c_str());
        if (local) {
            app->soloFromMenus = true; // (its end back to the main menu, as Game Modes' other modes')
        }
        startLive(*app, app->host->address());
        notePortMoved(*app);
    };
    callbacks.music = [app] { return soloMusic(*app); };
    callbacks.introAvailable = [app] { return app->movie && app->movie->available("Intro.vqa"); };
    callbacks.intro = [app](std::function<void()> after) {
        app->movie->play("Intro.vqa", movies::Skip::AnyInput, std::move(after)); // (the player with 0)
    };
    callbacks.quit = [app] { app->quitRequested = true; };
    callbacks.editor = [app] {
        app->editorFromList = false; // (Game Modes': its close back to the main menu)
        openEditor(*app, {});
    };
    callbacks.options = [app] { openOptions(*app); };
    callbacks.closeOptions = [app] {
        if (!app->options || !app->options->isOpen()) {
            return false;
        }
        app->options->close();
        return true;
    };
    callbacks.loggedIn = [](const std::string& nick) { SDL_Log("account: %s (XWIS login)", nick.c_str()); };
    callbacks.characterListModeOnly = [app](const std::string& screen, bool on) {
        app->charactersModeOnly[screen] = on; // (kept in noxcore.cfg with the Options' switches)
        saveExtras(*app, currentExtras(*app));
    };
    return callbacks;
}

} // namespace

/// SDL's start-up callback: reads the command line, finds the game folder, reads nox.cfg, makes the window
/// and renderer, loads the game's data and builds the windows, then starts what was asked for: a game we
/// host (--host, --solo), the menus, a --connect join, or else the map viewer.
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    startLog(argc, argv); // (noxcore-log.txt; stderr if redirected; -cmd's console: log_output.hpp)
    nox::modes::registerBuiltinModes(); // (before any game is hosted)
    registerBuiltinModeUis();           // (and their UIs: before the command line names a fake screen)
    registerScriptModeUi(modeUiEntries()); // (and the script modes' UI, as their servers send it)
    // --mods-dir (a session's private test of its mod): known before the mods load, so its modes
    // and its content packs are the ones read, never those of mods\ beside noxcore.exe.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--mods-dir") {
            setModsFolder(fs::path(reinterpret_cast<const char8_t*>(argv[i + 1])));
        }
    }
    loadModsAtStart(); // (the mods put beside noxcore.exe: mods\<id>\, script modes, never downloaded)
#if NOX_PRIVATE_TOOLS
    devhooks::init(argc, argv);
#endif
    auto* app = new App;
    *appstate = app;
    if (argc > 0 && argv[0]) {
        app->exePath = fs::path(reinterpret_cast<const char8_t*>(argv[0]));
    }

    StartOptions opts = parseCommandLine(*app, argc, argv);
    // Without --game-dir: NOX_DATA, noxsdl's own folder, the current one, then the installed game
    // (the registry, then the usual GOG / Program Files / Origin folders; io/game_dir.hpp); the
    // folder the build was set up with (NOX_GAME_DIR), if any, is the last resort.
    std::string gameDirReason = "--game-dir";
    if (!opts.gameDirGiven) {
        const char* base = SDL_GetBasePath();
        if (auto found = nox::io::locateGameDir(base ? fs::path(base) : fs::path(), fs::path(NOXSDL_DEFAULT_GAME_DIR),
                                                [](const std::string& line) { SDL_Log("%s", line.c_str()); })) {
            app->gameDir = std::move(found->dir);
            gameDirReason = std::move(found->reason);
        } else {
            app->gameDir.clear();
        }
    }
    if (app->gameDir.empty()) {
        SDL_Log("game folder: none found");
        SDL_Log("loading failed: no folder with the game's data (thing.bin, gamedata.bin...) found");
        if (app->screenshot.empty() && opts.mapPicture.empty()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOX_TRC("game.load_failed.title", "NoX Core: loading failed"),
                                     NOX_TRC("game.load_failed.no_game",
                                             "Nox wasn't found.\n\nPut noxcore.exe (with its files) in your Nox folder, the one "
                                             "with GAME.EXE and thing.bin, or pass --game-dir (or set NOX_DATA) to that folder."),
                                     nullptr);
        }
        return SDL_APP_FAILURE;
    }
    SDL_Log("game folder: %s (%s)", nox::pathUtf8(app->gameDir).c_str(), gameDirReason.c_str());
    if (!opts.mapPicture.empty()) {
        try {
            const auto things = nox::things::loadThingDb(app->gameDir / "thing.bin");
            const std::uint64_t start = SDL_GetTicks();
            const auto p = MapThumbnails::draw(resolveMap(app->gameDir, opts.mapPicture), things, app->gameDir, 512);
            if (p.width == 0) {
                SDL_Log("map picture: %s can't be drawn", opts.mapPicture.c_str());
                return SDL_APP_FAILURE;
            }
            const auto png = nox::encodePng(p.width, p.height, p.rgba);
            // Report success only after the PNG has been written and closed.
            nox::writeFileBytes(opts.mapPictureOut, png);
            SDL_Log("map picture: %s %dx%d in %llu ms", nox::pathUtf8(opts.mapPictureOut).c_str(), p.width, p.height,
                    static_cast<unsigned long long>(SDL_GetTicks() - start));
        } catch (const std::exception& ex) {
            SDL_Log("map picture: %s", ex.what());
            return SDL_APP_FAILURE;
        }
        return SDL_APP_SUCCESS;
    }
    app->saveRoot = app->gameDir;
    if (!opts.saveDir.empty()) {
        // (--save-dir: the Save folder; a solo game's slots are in the Save\ of the folder above
        // one named Save, else in DIR\Save.)
        std::string leaf = opts.saveDir.filename().string();
        std::ranges::transform(leaf, leaf.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        app->saveRoot = leaf == "save" ? opts.saveDir.parent_path() : opts.saveDir;
    }

    // Saved nox.cfg from user settings, else the installed default.cfg: the key bindings,
    // and the view a game shows (VideoMode's resolution, VideoSize's window size).
    {
        bool fromDefault = false;
        const auto& settingsDir = backend::settingsDirectory(app->gameDir);
        if (app->editorMapsDir.empty() && settingsDir != app->gameDir) app->editorMapsDir = settingsDir / "maps";
        const auto loaded = nox::config::loadConfig(settingsDir, app->gameDir, &fromDefault);
        if (!loaded) {
            SDL_Log("neither %s nor %s read: no key bindings", nox::pathUtf8(settingsDir / "nox.cfg").c_str(),
                    nox::pathUtf8(app->gameDir / "default.cfg").c_str());
        }
        nox::config::Config config = loaded.value_or(nox::config::Config{});
        if (fromDefault || !config.setting("VideoMode")) {
            // default.cfg's 640 x 480 is for a monitor of the time; until the Options save one,
            // the widescreen 1024 x 576 (most screens are 16:9).
            config.set("VideoMode", "1024 576 16");
        }
        if (opts.videoMode) {
            config.set("VideoMode", std::to_string(opts.videoMode->first) + " " + std::to_string(opts.videoMode->second) + " 16");
        }
        if (opts.videoSize) {
            config.set("VideoSize", std::to_string(*opts.videoSize));
        }
        applySettings(*app, config);
    }

    SDL_SetAppMetadata("NoX Core", nox::version(), nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (app->screenshot.empty() ? 0 : SDL_WINDOW_HIDDEN) |
                                  backend::additionalWindowFlags();
    app->window = SDL_CreateWindow("NoX Core", opts.width, opts.height, flags);
    if (!app->window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    setWindowIcon(app->window); // (window_icon.hpp: on Windows the exe's own icon is the class's already)
    if (app->input) {
        app->input->layoutChanged(); // (the key labels by this keyboard's layout, known now SDL's up)
    }
    if (opts.useGpu) {
        // SDL's GPU renderer, on Direct3D 12 on Windows unless told otherwise (our shaders are
        // DXIL there); elsewhere on SDL's choice (Vulkan, for which they're SPIR-V).
        if (!SDL_GetHint(SDL_HINT_GPU_DRIVER)) {
#ifdef _WIN32
            SDL_SetHint(SDL_HINT_GPU_DRIVER, opts.gpuDriver.empty() ? "direct3d12" : opts.gpuDriver.c_str());
#else
            if (!opts.gpuDriver.empty()) {
                SDL_SetHint(SDL_HINT_GPU_DRIVER, opts.gpuDriver.c_str());
            }
#endif
        }
        app->renderer = SDL_CreateRenderer(app->window, SDL_GPU_RENDERER);
        if (!app->renderer) {
            SDL_Log("gpu renderer: %s; using the default one", SDL_GetError());
        }
    }
    if (!app->renderer) {
        app->renderer = SDL_CreateRenderer(app->window, nullptr);
    }
    if (!app->renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_Log("renderer: %s", SDL_GetRendererName(app->renderer));
    SDL_SetRenderVSync(app->renderer, 1); // (Options' VSync and Display follow from noxcore.cfg: applyExtras)

    try {
        const fs::path& gameDir = app->gameDir;
        app->things = nox::things::loadThingDb(gameDir / "thing.bin");
        try {
            app->modifiers = nox::things::loadModifierDb(gameDir / "modifier.bin");
        } catch (const std::exception& ex) {
            SDL_Log("modifier.bin: %s (armour drawn without item colours)", ex.what());
        }
        // The game's numbers (class stats, spell counts, trap costs...): needed as thing.bin is, so
        // a missing one fails the loading the same way.
        app->gameData = nox::things::loadGameData(gameDir / "gamedata.bin");
        app->bag = std::make_unique<nox::video::VideoBag>(gameDir / "video.bag", gameDir / "video.idx");
        app->sprites = std::make_unique<SpriteCache>(app->renderer, *app->bag);
        app->sprites->setPlaceholders(app->hdPlaceholders);
        app->shaders = GpuShaders::create(app->renderer); // (only on the GPU renderer)
        if (app->shaders) {
            app->sprites->setMaterialShader(app->shaders->material(), app->shaders->materialSplit());
            app->sprites->setPendingShader(app->shaders->pending()); // (a map editor session's pending edits)
            // Smooth effects and per-pixel lighting: the shaders draw on a white pixel.
            app->white = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 1, 1);
            if (app->white) {
                const std::uint32_t white = 0xFFFFFFFF;
                SDL_UpdateTexture(app->white, nullptr, &white, 4);
                if (app->shaders->weather()) {
                    app->weather = std::make_unique<WeatherFx>(app->shaders->weather(), app->white); // (nox.weather's drawing)
                }
            }
        }
        {
            // noxsdl's switches as the Options last left them (noxcore.cfg); the command line's
            // --stock-effects, --grid-light, --no-bloom and --no-fps turn them off for this run.
            if (app->imagePack.empty()) {
                // The HD image pack (video_hd.nxp): beside noxcore.exe, else in the game folder.
                const char* base = SDL_GetBasePath();
                for (const fs::path& p : {base ? fs::path(base) / "video_hd.nxp" : fs::path(), gameDir / "video_hd.nxp"}) {
                    if (!p.empty() && fs::exists(p)) {
                        app->imagePack = p;
                        break;
                    }
                }
            }
            SDL_Log("image pack: %s", app->imagePack.empty() ? "none (Use HD Images is greyed)"
                                                              : nox::pathUtf8(app->imagePack).c_str());
            OptionsWindow::Extras extras = loadExtras(backend::settingsDirectory(gameDir));
            extras.hdImages = (extras.hdImages || opts.hdImages) && !app->imagePack.empty();
            if (opts.originalFonts) {
                extras.originalFonts = *opts.originalFonts;
            }
            extras.smoothEffects = extras.smoothEffects && !opts.stockEffects;
            extras.hdSpellFx = extras.hdSpellFx && !opts.stockEffects;
            extras.perPixelLight = extras.perPixelLight && !opts.gridLight;
            extras.bloom = extras.bloom && !opts.noBloom;
            extras.frameRate = extras.frameRate && app->showFps;
            // Smoothing: 0 off, 1 movement, 2 network, 3 server (as --smoothing).
            if (opts.smoothing) {
                extras.smoothing = *opts.smoothing;
            }
            if (opts.display) {
                extras.display = *opts.display;
            } else if (!app->screenshot.empty()) {
                extras.display = 0; // (a --screenshot run's window stays hidden: not made fullscreen over the desktop)
            }
            if (opts.vsync) {
                extras.vsync = *opts.vsync;
            }
            if (opts.noSmoothMotion) {
                extras.smoothing = extras.smoothing == 2 ? 1 : extras.smoothing; // no clock: eased at most
            }
            if (opts.predictMovement) {
                app->predictMovement = *opts.predictMovement;
            }
            if (opts.noSmoothScrolling) {
                extras.smoothing = 0;
            }
            if (opts.otherUnits) {
                app->otherUnits = *opts.otherUnits;
            }
            app->bloomStrength = opts.bloomStrength;
            if (opts.pendingViewArg) {
                extras.pendingView = static_cast<int>(*opts.pendingViewArg); // (this run's)
            }
            applyExtras(*app, extras);
        }
        app->art = std::make_unique<ThingArt>(app->things);
        preloadSession(*app); // (the pack's set on first: the session's images queued now that the art is known)
        try {
            // The game text's faces (the Nox-style fonts from the fonts folder beside the exe) and
            // the two Fonts set to the ones Original Style Fonts and Use HD Images pick.
            const char* base = SDL_GetBasePath();
            app->fontFaces = std::make_unique<GameFonts>(app->renderer, gameDir, fs::path(base ? base : "") / "fonts");
            app->font = std::make_unique<Font>(Font::kDefaultCell);
            app->largeFont = std::make_unique<Font>(Font::kLargeCell);
            applyFonts(*app);
        } catch (const std::exception& ex) {
            app->font.reset();
            app->largeFont.reset();
            SDL_Log("font: %s (no name plates)", ex.what());
        }
        app->effects = std::make_unique<Effects>(app->things, *app->art);
        app->effects->setStaticPoint([app](std::uint16_t extent) -> std::optional<SDL_FPoint> {
            return app->view ? app->view->objectPosition(extent) : std::nullopt;
        });
        app->chant = std::make_unique<SpellChant>(app->things);
        if (!opts.noAudio) {
            app->audio = Audio::open(gameDir, app->things);
        }
        if (app->audio && !opts.audioCapture.empty()) {
            app->audio->capture(opts.audioCapture);
        }
        if (app->audio) {
            app->audio->setLog(opts.audioLog);
            app->audio->onChant = [app](std::uint8_t spell, std::uint32_t frame) {
                if (app->chant) {
                    app->chant->start(spell, frame);
                }
            };
        }
        if (app->audio) {
            // nox.cfg's volumes; a part whose volume is 0 starts switched off.
            const auto& config = app->input->config();
            app->audio->setVolumes(config);
            const auto on = [&](std::string_view name) {
                const auto v = config.setting(name);
                return !v || std::atoi(v->c_str()) != 0;
            };
            app->audio->setSwitches({on("FXVolume"), on("DialogVolume"), on("MusicVolume")});
            // Each type's AUDIOLOOP sound by type id (ids count from 1: slot 0 stays empty).
            app->loopSounds.assign(app->things.things.size() + 1, 0);
            for (std::size_t i = 0; i < app->things.things.size(); ++i) {
                if (const auto loop = app->things.things[i].attribute("AUDIOLOOP")) {
                    app->loopSounds[i + 1] = nox::audio::soundId(*loop).value_or(0);
                }
            }
        }
        {
            // The movies (movie_player.hpp): the HD ones from HDVideo\ beside noxcore.exe.
            const char* base = SDL_GetBasePath();
            const fs::path hdDir = !opts.hdDirArg.empty() ? opts.hdDirArg : (base ? fs::path(base) : fs::path()) / "HDVideo";
            app->movie = std::make_unique<MoviePlayer>(app->renderer, app->audio.get(), gameDir, hdDir);
            app->movie->setDisabled(opts.noMovies);
            app->movie->setHdAllowed(!opts.noHdMovies);
        }
        app->hud = std::make_unique<Hud>(app->things, *app->art);
        app->hud->setGameData(&app->gameData); // the trap costs, cooldowns
        app->minimap = std::make_unique<Minimap>(app->things);
        app->minimap->setModifiers(&app->modifiers);
        app->profile = opts.profile;
        app->mapCache = opts.mapCache;
        if (app->mapCache.empty()) {
            // (Beside the exe, else the user's settings folder: never the current folder.)
            fs::path exeDir;
            if (const char* base = SDL_GetBasePath()) {
                exeDir = fs::path(reinterpret_cast<const char8_t*>(base));
            }
            app->mapCache = defaultMapCache(exeDir, [] {
                fs::path dir;
                if (char* pref = SDL_GetPrefPath("noxcpp", "noxsdl")) {
                    dir = fs::path(reinterpret_cast<const char8_t*>(pref));
                    SDL_free(pref);
                }
                return dir;
            });
        }
        {
            const auto cache = app->mapCache.u8string();
            SDL_Log("downloaded maps: %s", reinterpret_cast<const char*>(cache.c_str()));
        }
        try {
            app->strings =
                std::make_unique<nox::strings::StringTable>(nox::strings::StringTable::load(gameDir / "nox.csf"));
        } catch (const std::exception& ex) {
            SDL_Log("nox.csf: %s (menus show string ids)", ex.what());
        }
        if (app->strings) {
            // Which way its U+0080-U+00FF are meant, once (applyTextCodepage takes it).
            app->csfGuess = nox::strings::guessLegacyCodepage(*app->strings);
            SDL_Log("nox.csf: language %u, %s: %zu letters A-Z, %zu in U+00C0-U+00FF (shifted %d), %zu Cyrillic: auto reads %s%s",
                    app->strings->language(), app->csfGuess.reason, app->csfGuess.asciiLetters, app->csfGuess.highLetters,
                    app->csfGuess.shift, app->csfGuess.cyrillic,
                    app->csfGuess.codepage == nox::strings::LegacyCodepage::Cp1251 ? "Windows-1251" : "Latin-1",
                    app->strings->tagsSwapped() ? " (its text was stored without its tags' inversion: read the other way round)"
                                                : "");
        }
        applyTextCodepage(*app); // (CsfText = cp1251 holds without a nox.csf too)
        applyLanguage(*app);     // (NoX Core's own text: before the windows below are built)
        app->hud->setStrings(app->strings.get()); // tooltips
        app->itemNames = std::make_unique<ItemNames>(app->things, *app->art, app->modifiers, app->strings.get());
        const gui::Resources res = guiResources(*app);
        // The game's cursor instead of the system's.
        app->cursor = std::make_unique<Cursor>(app->things, *app->sprites, app->font.get(), app->strings.get());
        SDL_HideCursor();
        makeGameWindows(app, res);
        // --host-edit MAP: an edit session hosted here, unlisted, and joined; the editor in front.
        const std::uint16_t hostEditPort = opts.hostSettings ? opts.hostSettings->port : std::uint16_t{18590};
        if (!opts.hostEditMap.empty() && opts.hostEditFromMenus) {
            opts.hostSettings.reset(); // (--host-port was the session's: no game of --host's hosted below)
        }
        const auto startHostEdit = [&] {
            app->profile = opts.profile;
            editor::SessionSetup setup;
            setup.sourceMap = opts.hostEditMap;
            setup.name = opts.hostEditMap;
            if (!opts.hostEditMod.empty()) {
                // (A mod's id alone: never a path that leads out of mods\.)
                if (nox::modes::script::validModeId(opts.hostEditMod)) {
                    setup.modFolder = modsFolderPath() / opts.hostEditMod;
                } else {
                    SDL_Log("--host-edit-mod: \"%s\" isn't a mod's id (a-z, 0-9 and _): the session carries no mod", opts.hostEditMod.c_str());
                }
            }
            if (nox::mapedit::isStockMapName(opts.hostEditMap) || nox::mapedit::mapNameProblem(opts.hostEditMap)) {
                // (A copy's name within the game's 8 characters: "oasis_ed", "ed_estat".)
                setup.name = nox::mapedit::suggestedMapName(opts.hostEditMap);
            }
            openEditor(*app, {});
            if (const std::string error = hostEditSession(*app, setup, hostEditPort, false); !error.empty()) {
                throw std::runtime_error("edit session: " + error);
            }
            SDL_Log("noxsdl %s: edit session %s", nox::version(), setup.name.c_str());
        };
        if (!opts.hostEditMap.empty() && !opts.hostEditFromMenus) {
            startHostEdit(); // (its close ends the program: no menus)
            return SDL_APP_CONTINUE;
        }
        if (opts.hostSettings) {
            // Our own game: the server here on its port, and we join it like anyone else.
            if (opts.hostSettings->solo && opts.hostSettings->map.empty()) {
                const nox::modes::Mode* campaign =
                    app->hostCoopCampaignTest.empty() ? nullptr : nox::modes::registry().find(app->hostCoopCampaignTest);
                opts.hostSettings->map = campaign && campaign->info.campaign // (--host-coop-campaign: its first map, the one it ships)
                                             ? campaignMapPath(app->gameDir, campaign->info.id, campaign->info.campaign->firstMap).string()
                                             : std::string(nox::client::solo_menu::firstMap(opts.profile.playerClass));
            }
            if (!opts.hostSettings->solo) {
                // nox.cfg's server switches (the command line's name, mode and limits stand).
                HostSettings fromConfig;
                hostSettingsFromConfig(*app, fromConfig);
                opts.hostSettings->sendMotd = fromConfig.sendMotd;
                opts.hostSettings->mapCycle = fromConfig.mapCycle;
                opts.hostSettings->itemRespawn = fromConfig.itemRespawn;
                opts.hostSettings->camperAlarm = fromConfig.camperAlarm;
                opts.hostSettings->updateRate = fromConfig.updateRate; // (noxcore.cfg's)
                opts.hostSettings->mapVote = fromConfig.mapVote;
            }
            if (app->privateTestRun) {
                // (A session's private test, as the editor's play-test hosts: its map alone, no
                // cycle, vote or message of the day; the test commands on.)
                opts.hostSettings->name = "Private test";
                app->playTestMap = fs::path(opts.hostSettings->map).stem().string(); // (--start-at's map)
                opts.hostSettings->mapCycle = false;
                opts.hostSettings->mapVote = false;
                opts.hostSettings->sendMotd = false;
                opts.hostSettings->testCommands = true;
            }
            if (opts.hostSettings->solo) {
                opts.hostSettings->updateRate = app->serverUpdateRate; // (noxcore.cfg's)
                if (!app->hostSoloScreensTest.empty()) {
                    // (--host-coop: as Host Custom > Co-op Campaign's new campaign of --class's chain)
                    beginSoloScreensHost(*app, app->hostSoloScreensTest, opts.profile.playerClass, *opts.hostSettings,
                                         app->hostCoopCampaignTest); // (--host-coop-campaign: that mod's campaign)
                }
                opts.hostSettings->saveRoot = app->saveRoot;
                newSoloCharacter(*app, opts.profile, opts.hostSettings->map);
                opts.hostSettings->characterFile = app->characterFile; // (clientCurSave, as the join's after-load sends it)
            }
            opts.hostSettings->loopbackOnly = app->hostLoopback;
            app->host = makeHost(*app, *opts.hostSettings);
            opts.connect = app->host->address();
            SDL_Log("hosting %s at %s", opts.hostSettings->map.c_str(), opts.connect.c_str());
            notePortMoved(*app);
            startPortMapping(*app, !opts.hostSettings->solo && !app->privateTestRun); // (a session's private test: unlisted)
            if (opts.hostBots > 0 && !opts.hostSettings->solo) {
                // (Tests and timing runs: as the Bots tab's choice, warriors, wizards and conjurers in turn.)
                setLocalBots(*app, {(opts.hostBots + 2) / 3, (opts.hostBots + 1) / 3, opts.hostBots / 3}, 1, 1);  // split as evenly as can be, warriors first
            }
        }
        // --map-editor: the map editor alone (its close ends the program).
        if (app->editorDirect && opts.connect.empty()) {
            openEditor(*app, opts.editorMap);
            SDL_Log("noxsdl %s: map editor", nox::version());
            return SDL_APP_CONTINUE;
        }
        // No game to join and no --map: the menus (or --browse's game list).
        if (opts.connect.empty() && (opts.browse || !opts.mapGiven)) {
            if (!app->font) {
                throw std::runtime_error("the server browser needs a font");
            }
            // servers.txt: the settings folder's (where Add/Remove saves) and the one beside the exe.
            const char* base = SDL_GetBasePath();
            app->browser = std::make_unique<BrowserScreen>(
                *app->font, opts.browserOptions,
                serverListPaths(backend::settingsDirectory(gameDir), base ? fs::path(base) : fs::path{}));
            // The game's menus; --browse goes straight to the game list.
            Menus::Callbacks callbacks = menusCallbacks(app, opts);
            app->menus =
                std::make_unique<Menus>(res, gameDir, *app->browser, opts.profile, app->modifiers, std::move(callbacks));
            if (!opts.saveDir.empty()) {
                app->menus->setSaveDir(opts.saveDir);
            }
            app->menus->setSaveRoot(app->saveRoot);
            app->menus->setCharacterListModeOnly(app->charactersModeOnly); // (noxcore.cfg's: character_labels.hpp)
            app->menus->setThings(&app->things);
            app->menus->setAudio(app->audio.get());
            SDL_StartTextInput(app->window); // for the menus' entry fields
            // The start-up's movies and legal screen (startUp), unless a test drives the menus:
            // --menu-clicks, --mouse-script and --screenshot go straight to the main menu (or
            // --browse to the game list); --movie NAME plays that movie first.
            const bool automated =
                !opts.forceStartUp && (!opts.menuClicks.empty() || !app->mouseScript.empty() || !app->screenshot.empty());
            if (opts.browse) {
                if (app->audio) {
                    app->audio->music(25); // the menus' music, "shell" (game_showMainMenu)
                }
                app->menus->showGameBrowser();
                if (!app->askPasswordTest.empty()) {
                    // (--ask-password HOST: the join's password asked for over the list, as a game
                    // that wants one leaves it asked)
                    app->menus->askJoinPassword(app->askPasswordTest, opts.profile, false);
                }
            } else if (opts.fakeWolChat) {
                startMainMenu(*app);
                app->menus->showFakeChat();
            } else if (opts.fakeWolConnecting) {
                startMainMenu(*app);
                app->menus->showFakeConnecting();
            } else if (opts.fakeXwisGames) {
                // Made-up XWIS listings on the loopback: one where a test may host (18731), the
                // others at ports nothing answers on (a stock host whose ping never comes back).
                startMainMenu(*app);
                const auto listing = [](const char* name, const char* map, std::uint16_t port, int players, int most) {
                    nox::lobby::GameListing g;
                    g.name = name;
                    g.map = map;
                    g.address = 0x7F000001;
                    g.port = port;
                    g.players = players;
                    g.maxPlayers = most;
                    g.gameFlags = 0x100;
                    return g;
                };
                app->menus->showFakeXwisGames({listing("Hosted Here", "DD_0A", 18731, 0, 16),
                                               listing("Kor] Newbies", "ext5", 18791, 0, 31),
                                               listing("USA Midwest #1", "!TakRP", 18792, 1, 15),
                                               listing("User_Game", "DD_0A", 18793, 1, 3),
                                               listing("Waystation", "estate", 18794, 0, 16)});
            } else if (!opts.movieArg.empty()) {
                app->movie->play(opts.movieArg, movies::Skip::AnyInput, [app] { startMainMenu(*app); });
            } else if (automated) {
                startMainMenu(*app);
            } else {
                startUp(*app);
            }
            app->menus->queueClicks(std::move(opts.menuClicks));
            SDL_Log("noxsdl %s: menus", nox::version());
            if (!opts.hostEditMap.empty() && opts.hostEditFromMenus) {
                startHostEdit(); // (--host-edit-menus: the session hosted with the menus behind it)
            }
            return SDL_APP_CONTINUE;
        }
        // --connect (or the game --host / --solo just started): straight into it, as startLive does from the menus.
        if (!opts.connect.empty()) {
            app->profile = opts.profile;
            joinLive(*app, opts.connect);
            return SDL_APP_CONTINUE;
        }
        // Else the map viewer: --map's map (Estate by default).
        const fs::path mapPath = resolveMap(gameDir, opts.mapArg);
        app->map = nox::map::loadMapFile(mapPath);
        app->view = std::make_unique<MapView>(app->map, app->things, *app->art, app->modifiers);
        app->viewMap = mapPath.filename().string();
        preloadMap(*app, app->viewMap);
        if (!opts.haveAt) {
            app->camera.x = app->view->centre().x;
            app->camera.y = app->view->centre().y;
        }
        SDL_Log("noxsdl %s: %s", nox::version(), nox::pathUtf8(mapPath).c_str());
        const auto& lights = app->view->lights();
        const auto animated = std::ranges::count_if(lights, [](const MapView::MapLight& l) {
            return l.colorKeys.size() > 1 || l.intensityKeys.size() > 1;
        });
        SDL_Log("%zu lights, %zu animated", lights.size(), static_cast<std::size_t>(animated));
    } catch (const std::exception& ex) {
        SDL_Log("loading failed: %s", ex.what());
        if (app->screenshot.empty()) {
            // A windowed run would otherwise just vanish (the log goes to stderr).
            const std::string text = nox::l10n::format(
#ifdef _WIN32
                NOX_TR("game.load_failed.error_windows",
                       "{error}\n\nGame folder: {folder}\nPut noxcore.exe (with its files) in your Nox folder, the one with GAME.EXE "
                       "and thing.bin, or pass --game-dir (or set NOX_DATA) to that folder."),
#else
                NOX_TR("game.load_failed.error_other",
                       "{error}\n\nGame folder: {folder}\nNox wasn't found where it's usually installed. Pass --game-dir (or set "
                       "NOX_DATA) to the folder with GAME.EXE and thing.bin."),
#endif
                {{"error", ex.what()}, {"folder", nox::pathUtf8(app->gameDir)}});
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NOX_TRC("game.load_failed.title", "NoX Core: loading failed"), text.c_str(),
                                     app->window);
        }
        return SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;
}

/// A button's press with a hand cursor up (Use, Talk, Trade): its Action does `act` instead of a
/// swing (let go at once), its other controls as ever.
template <typename Act>
void pressWithHand(App& app, std::uint32_t code, Act act)
{
    auto controls = app.input->press(code);
    if (std::ranges::find(controls, "Action") != controls.end()) {
        act();
        app.input->release(code); // (no swing with it)
        std::erase(controls, "Action");
    }
    liveControls(app, controls);
}

/// SDL's event callback. Each event goes to the first that takes it: the 3D view's look, a playing movie,
/// the input config and Options windows, the menus (out of a game); in a game the briefings, console,
/// map download, chat and the game's windows; then the bindings, or the viewer's own keys and mouse.
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto* app = static_cast<App*>(appstate);
    if (event->type == SDL_EVENT_KEYMAP_CHANGED && app->input) {
        app->input->layoutChanged(); // (another keyboard layout: the keys' labels as it prints them)
    }
    // The play-test's game has the window; F2 goes back to the editor (and the editor's Play keys:
    // Release Mouse, Stop, Pause).
    if (app->editor && app->playTestInGame) {
        if (event->type == SDL_EVENT_QUIT) {
            return app->editor->requestQuit() ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F2 && !event->key.repeat) {
            playTestToEditor(*app);
            return SDL_APP_CONTINUE;
        }
        if (app->editor->playKey(*event)) {
            return SDL_APP_CONTINUE;
        }
    }
    // A play-test in the editor's Map panel: the editor has the keyboard and mouse till the game's
    // view is clicked (play_view.cpp), then the game has them till F2, the editor's Release Mouse
    // (Shift+F1), a click outside its view (where the pointer isn't kept on it: a hidden run) or
    // the window's focus lost gives them back. The editor's Play keys (Stop, Pause) work in it.
    if (gameInPanel(*app)) {
        if (event->type == SDL_EVENT_QUIT) {
            if (app->editor->requestQuit()) {
                return SDL_APP_SUCCESS;
            }
            capturePlayTest(*app, false); // (its prompt is up: the mouse the editor's to answer it)
            return SDL_APP_CONTINUE;
        }
        // The session's game in the panel: F2 back to editing, the game's input or not.
        if (app->sessionEmbedded && event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.key == SDLK_F2) {
            sessionToEditor(*app);
            return SDL_APP_CONTINUE;
        }
        if (!app->playTestCaptured) {
            app->editor->handleEvent(*event);
            return SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.key == SDLK_F2) {
            capturePlayTest(*app, false);
            return SDL_APP_CONTINUE;
        }
        if (app->editor->playKey(*event)) {
            return SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const SDL_FRect& a = app->gameArea;
            const float x = event->button.x;
            const float y = event->button.y;
            if (x < a.x || y < a.y || x >= a.x + a.w || y >= a.y + a.h) {
                capturePlayTest(*app, false);
                SDL_Event moved{}; // (ImGui told where first: it saw no motion while the game had the mouse)
                moved.type = SDL_EVENT_MOUSE_MOTION;
                moved.motion.windowID = event->button.windowID;
                moved.motion.which = event->button.which;
                moved.motion.x = x;
                moved.motion.y = y;
                app->editor->handleEvent(moved);
                app->editor->handleEvent(*event);
                return SDL_APP_CONTINUE;
            }
        }
        if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            capturePlayTest(*app, false);
            app->editor->handleEvent(*event);
            return SDL_APP_CONTINUE;
        }
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) {
            app->editor->handleEvent(*event); // (ImGui keeps up with the window; the game gets it too)
        }
        // On to the game (its mouse in its area: letterboxed, below).
    }
    // Our session's game over the window, its editor waiting behind: the window's close button asks
    // as the editor's would (it ends the session for everyone), the editor brought in front for it.
    if (app->editor && app->sessionInGame && event->type == SDL_EVENT_QUIT) {
        if (app->editor->requestQuit()) {
            return SDL_APP_SUCCESS;
        }
        sessionToEditor(*app);
        return SDL_APP_CONTINUE;
    }
    // A map editor session's game: F2 brings the editor in front; the game runs on.
    if (app->editSession && app->editSession->inSession() && app->live && (!app->editor || app->sessionInGame) &&
        event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F2 && !event->key.repeat) {
        sessionToEditor(*app);
        return SDL_APP_CONTINUE;
    }
    // The map editor, while open, has the window (its panels, keys and mouse).
    if (app->editor && !app->playTestInGame && !app->sessionInGame && !gameInPanel(*app)) {
        if (event->type == SDL_EVENT_QUIT) {
            return app->editor->requestQuit() ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
        }
        if (app->editor->inSession() && event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F2 && !event->key.repeat) {
            sessionToGame(*app); // (back to the session's game)
            return SDL_APP_CONTINUE;
        }
        app->editor->handleEvent(*event);
        return SDL_APP_CONTINUE;
    }
    // The mouse in the game's output pixels (a letterboxed game's from its area's corner).
    if (mapsPointer(*app)) {
        switch (event->type) {
        case SDL_EVENT_MOUSE_MOTION:
            intoGameArea(*app, event->motion.x, event->motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            intoGameArea(*app, event->button.x, event->button.y);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            intoGameArea(*app, event->wheel.mouse_x, event->wheel.mouse_y);
            break;
        default:
            break;
        }
    }
    // The 3D view's mouse look, W A S D, and 1-5 / E / R as the game's controls (view3d.hpp).
    if (std::string control; app->view3d.handleEvent(*event, control)) {
        if (!control.empty() && app->live) {
            liveControls(*app, {control});
        }
        return SDL_APP_CONTINUE;
    }
    // A movie takes every key and button (movie_player.hpp: Escape, or any for some, stops it).
    if (app->movie && app->movie->active()) {
        if (event->type == SDL_EVENT_QUIT) {
            return SDL_APP_SUCCESS;
        }
        if (app->input && (event->type == SDL_EVENT_KEY_UP || event->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            app->input->releaseAll();
        }
        app->movie->handleEvent(*event);
        return SDL_APP_CONTINUE;
    }
    // The input configuration, then the Options window, while open, have the mouse and keys
    // (over the menus or a game).
    if (app->inputConfig && app->inputConfig->isOpen() && event->type != SDL_EVENT_QUIT) {
        if (app->input) {
            app->input->releaseAll();
        }
        app->inputConfig->handleEvent(*event, app->camera.viewWidth, app->camera.viewHeight);
        return SDL_APP_CONTINUE;
    }
    if (app->options && app->options->isOpen() && event->type != SDL_EVENT_QUIT) {
        if (app->input && (event->type == SDL_EVENT_KEY_UP || event->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            app->input->releaseAll();
        }
        app->options->handleEvent(*event, app->camera.viewWidth, app->camera.viewHeight);
        if (app->menus && (!app->live || app->joinWaiting) && app->menus->optionsUp() &&
            (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            // The menus' Options screen: the bar below the window (its Back) has the mouse too.
            app->menus->handleEvent(*event, app->camera.viewWidth, app->camera.viewHeight, SDL_GetTicks());
        }
        return SDL_APP_CONTINUE;
    }
    if (app->menus && (!app->live || app->joinWaiting)) { // (the join's wait: the menus have the input)
        if (event->type == SDL_EVENT_QUIT) {
            return SDL_APP_SUCCESS;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
            app->shellPressed = true; // the pointer's burst and ShellMouseBoom (shellSparks)
        }
        if (app->legal) {
            app->legal->handleEvent(*event, app->camera.viewWidth, app->camera.viewHeight);
            return SDL_APP_CONTINUE;
        }
        app->menus->handleEvent(*event, app->camera.viewWidth, app->camera.viewHeight, SDL_GetTicks());
        return app->quitRequested ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
    }
    Camera& cam = app->camera;
    const bool live = app->live != nullptr;
    // Releases reach the bindings whatever has the input, so nothing is left held down.
    if (app->input) {
        if (event->type == SDL_EVENT_KEY_UP) {
            if (const auto code = InputBindings::keyCode(event->key.scancode)) {
                app->input->release(*code);
            }
        } else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (const auto code = InputBindings::mouseCode(event->button.button)) {
                app->input->release(*code);
            }
        } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            app->input->releaseAll();
            // Whatever a press began waits for its release, which goes to the other program
            // now (Alt+Tab while dragging a spell, or with the target cursor up): let it go.
            app->armed.reset();
            app->barDrag.reset();
            if (app->book) {
                app->book->cancelDrag();
            }
        }
    }
    // A mode's window's press ends at its release whoever has the pointer now (its press went to
    // no one else), and is let go when the focus goes.
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        for (auto& [id, ui] : app->modeUis) {
            if (ui->release(*app, *event)) {
                return SDL_APP_CONTINUE;
            }
        }
    } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        for (auto& [id, ui] : app->modeUis) {
            ui->inputLost(*app);
        }
    }
    // A briefing or the load menu (cinematics.hpp) has the mouse and keys.
    if (live && app->cinematics && app->cinematics->screenUp() &&
        app->cinematics->handleEvent(*event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // While the console is open, it has the keyboard (the mouse still plays). In the viewer,
    // where there are no bindings, F1 opens it.
    if (app->console && app->console->isOpen() && app->console->handleEvent(app->window, *event)) {
        return SDL_APP_CONTINUE;
    }
    if (!live && app->console && event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F1 && !event->key.repeat) {
        app->console->toggle(app->window);
        return SDL_APP_CONTINUE;
    }
    // While the map downloads, its screen has the input (Cancel, Escape).
    if (live && app->mapDownload && app->live->client().downloadPercent() && event->type != SDL_EVENT_QUIT &&
        app->mapDownload->handleEvent(*event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // While a chat line is being typed, it has the keyboard (the mouse still plays).
    if (live && app->chat && app->chat->isOpen() &&
        app->chat->handleEvent(app->window, *event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // The in-game windows below give way to the in-game menu; the HUD's are laid out at its scale.
    const auto menuClosed = [&] { return app->quitMenu && !app->quitMenu->isOpen(); };
    const float hudScale = Hud::scale(cam.viewWidth, cam.viewHeight);
    // The shop, dialog and trade windows take the mouse over them (and every click while their
    // amount box or message is up).
    if (live && app->tradeScreens && menuClosed() &&
        (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
         event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_WHEEL) &&
        app->tradeScreens->handleEvent(*event, app->live->client().world(), cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // noxcpp's map vote takes the mouse over it; a click on a map votes for it.
    if (live && app->mapVote && app->mapVote->shown() && menuClosed()) {
        std::optional<int> choice;
        if (app->mapVote->handleEvent(*event, app->live->client().mapVote(), cam.viewWidth, cam.viewHeight,
                                      hudScale, choice)) {
            if (choice) {
                app->live->client().voteMap(*choice);
            }
            return SDL_APP_CONTINUE;
        }
    }
    // The modes' own windows (mode_ui.hpp: a mode's map, its questions, cards, results and
    // the like).
    if (live && menuClosed()) {
        for (auto& [id, ui] : app->modeUis) {
            if (ui->handleEvent(*app, *event, cam.viewWidth, cam.viewHeight, hudScale)) {
                return SDL_APP_CONTINUE;
            }
        }
    }
    // The message of the day, while it's up, has the mouse and keys.
    if (live && app->motdWindow && app->motdWindow->isOpen() && event->type != SDL_EVENT_QUIT &&
        app->motdWindow->handleEvent(*event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // A quest's game over and shared keys windows take a click on them; the game goes on.
    if (live && app->questWindows && event->type != SDL_EVENT_QUIT && !(app->quitMenu && app->quitMenu->isOpen()) &&
        app->questWindows->handleEvent(*event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // The lag window (the server gone quiet), while it's up, has the mouse and keys (modal).
    if (live && app->discon && app->discon->handleEvent(*event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // While the in-game menu is open it has the mouse and keys.
    if (live && app->quitMenu && app->quitMenu->isOpen() && event->type != SDL_EVENT_QUIT) {
        app->quitMenu->handleEvent(*event, cam.viewWidth, cam.viewHeight);
        return SDL_APP_CONTINUE;
    }
    // The host's Server Settings take the mouse over them, Escape, and the keys while a field's
    // typed in; the game goes on round them.
    if (live && app->serverOptions && app->serverOptions->isOpen() && event->type != SDL_EVENT_QUIT &&
        app->serverOptions->handleEvent(app->window, *event, cam.viewWidth, cam.viewHeight)) {
        return SDL_APP_CONTINUE;
    }
    // Nothing above took it: the bindings in a game, the viewer's own keys and mouse otherwise.
    switch (event->type) {
    case SDL_EVENT_QUIT:
        if (live) {
            app->live->quit();
            return SDL_APP_CONTINUE;
        }
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN: {
        const float step = 64 / cam.zoom;  // the viewer's keys pan 64 screen pixels
        // In a game, the bindings (nox.cfg); a bound key goes no further.
        if (live && app->input && !event->key.repeat) {
            if (const auto code = InputBindings::keyCode(event->key.scancode)) {
                if (liveControls(*app, app->input->press(*code))) {
                    break;
                }
                // A chat macro the console bound to the key (bind KEY "line").
                if (const auto* key = nox::config::findKey(*code)) {
                    if (const auto m = app->macros.find(std::string(key->name)); m != app->macros.end()) {
                        app->live->client().say(m->second);
                        break;
                    }
                }
            }
        }
        if (live && event->key.repeat) {
            break;
        }
        switch (event->key.key) {
        case SDLK_ESCAPE:
            if (live) {
                app->live->quit();
                return SDL_APP_CONTINUE;
            }
            return SDL_APP_SUCCESS;
        // The map viewer's switches; in a game these letters are the game's (an unbound one did
        // nothing in the original, and here N turned everyone's name off without a word).
        case SDLK_L:
            if (!live) {
                app->lighting = !app->lighting;
            }
            break;
        case SDLK_V:
            if (!live) {
                app->lineOfSight = !app->lineOfSight.value_or(app->live != nullptr);
            }
            break;
        case SDLK_N:
            if (!live) {
                app->namePlates = !app->namePlates;
            }
            break;
        case SDLK_SPACE:
            if (live) {
                app->live->client().spawn();
            }
            break;
        case SDLK_LEFT:
        case SDLK_A:
            cam.x -= step;
            break;
        case SDLK_RIGHT:
        case SDLK_D:
            cam.x += step;
            break;
        case SDLK_UP:
        case SDLK_W:
            cam.y -= step;
            break;
        case SDLK_DOWN:
        case SDLK_S:
            cam.y += step;
            break;
        case SDLK_EQUALS:
        case SDLK_KP_PLUS:
        case SDLK_PAGEUP:
            if (!live) { // the viewer zooms freely; a game's view is fixed (App::viewWidth)
                zoomBy(cam, 1.25f);
            }
            break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
        case SDLK_PAGEDOWN:
            if (!live) {
                zoomBy(cam, 0.8f);
            }
            break;
        default:
            break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (live) {
            // Over the inventory it scrolls that; else MOUSE_WHEEL_UP / DOWN's bindings (the
            // spell sets).
            const SDL_FPoint m = mousePos(*app);
            if (event->wheel.y == 0 ||
                (app->inventory &&
                 app->inventory->wheel(m.x, m.y, cam.viewWidth, cam.viewHeight, event->wheel.y > 0 ? 1 : -1))) {
                break;
            }
            if (app->input) {
                const std::uint32_t code = event->wheel.y > 0 ? nox::config::kWheelUp : nox::config::kWheelDown;
                const auto controls = app->input->press(code);
                app->input->release(code);
                liveControls(*app, controls);
            }
            break;
        }
        zoomBy(cam, event->wheel.y > 0 ? 1.25f : 0.8f);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (!live) {
            if (event->button.button == SDL_BUTTON_RIGHT) {
                app->dragging = down;
            }
            break;
        }
        // (The scoreboard doesn't take the mouse: its draw shows the quest list and the full board
        // modal, but with nothing on them to click, that only left players unable to move or fight
        // while the board was up.)
        if (event->button.button == SDL_BUTTON_RIGHT && down && app->inventory && app->inventory->identifying() &&
            app->inventory->contains(event->button.x, event->button.y, cam.viewWidth, cam.viewHeight)) {
            app->inventory->stopIdentify(); // the right button leaves identify mode (the inventory window's case 9)
            break;
        }
        if (event->button.button == SDL_BUTTON_RIGHT && down && app->inventory &&
            app->inventory->onSecondaryBox(event->button.x, event->button.y, cam.viewWidth, cam.viewHeight)) {
            // The right button on the secondary weapon's box swaps (its window's case 7).
            if (app->live->client().secondaryWeapon() != 0 && app->live->client().swapWeapons()) {
                uiSound(*app, 0x37F); // NextWeapon
            }
            break;
        }
        if (event->button.button == SDL_BUTTON_LEFT) {
            // The windows take the left button first: the book, the inventory, then the HUD.
            if (leftMouse(*app, down, event->button.x, event->button.y)) {
                break;
            }
        }
        const auto code = InputBindings::mouseCode(event->button.button);
        if (!down || !code || !app->input) {
            break; // (releases reached the bindings above)
        }
        if (app->cursorType == CursorType::Use && (app->cursorUseable || app->cursorHovered)) {
            // Action with the Use hand (Cli_ProcessControlEvents, cursor 0xD): MSG_TRY_COLLIDE for
            // the cursor's pick, a map object's net code being its extent with bit 15, a drawable's
            // its own, with bit 15 for an IMMOBILE or CLIENT_PERSIST one (CliNet_SendTryCollide).
            pressWithHand(*app, *code, [app] {
                if (app->cursorUseable) {
                    app->live->client().collide(static_cast<std::uint16_t>(app->cursorUseable->extent | 0x8000));
                } else if (app->cursorHovered) {
                    const auto* d = app->live->client().world().drawable(*app->cursorHovered);
                    const TypeArt* a = d ? app->art->get(d->typeId) : nullptr;
                    constexpr std::uint32_t kStatic = nox::things::ObjectClass::Immobile | nox::things::ObjectClass::ClientPersist;
                    const bool fixed = a && (a->classes & kStatic);
                    app->live->client().collide(static_cast<std::uint16_t>(*app->cursorHovered | (fixed ? 0x8000 : 0)));
                }
            });
            break;
        }
        if ((app->cursorType == CursorType::Talk || app->cursorType == CursorType::Trade) && app->cursorHovered) {
            // Action with the Talk or Trade hand (Cli_ProcessControlEvents' cursor 4 / 3):
            // MSG_DIALOG start (CliNet_SendDialogStart) or MSG_TRADE open shop (CliNet_SendOpenShop).
            pressWithHand(*app, *code, [app] {
                if (!app->cursorHovered) {
                    return;
                }
                if (app->cursorType == CursorType::Talk) {
                    app->live->client().dialogStart(*app->cursorHovered);
                } else {
                    app->live->client().openShop(*app->cursorHovered);
                }
            });
            break;
        }
        if (*code == app->input->pickupButton() &&
            (app->cursorType == CursorType::Pickup || app->cursorType == CursorType::Caution) && app->cursorHovered) {
            // The pickup hand: the MousePickup button picks it up instead (CliNet_SendPickup).
            // Caution too (what our class can't use): cursorUpdate sends it all the same, and the
            // server refuses it with its line and NoCanDo.
            tryPickup(*app, *app->cursorHovered);
            break;
        }
        // Its bindings: MoveForward and Action act while held (SDL_AppIterate), others now.
        liveControls(*app, app->input->press(*code));
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (live && app->book) {
            app->book->mouseMove(event->motion.x, event->motion.y, cam.viewWidth, cam.viewHeight); // (held: dragged)
        }
        if (app->dragging) {
            cam.x -= event->motion.xrel / cam.zoom;
            cam.y -= event->motion.yrel / cam.zoom;
        }
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

namespace {

/// A window or menu is up that has the mouse (so the 3D view gives the cursor back).
bool windowHasMouse(const App& app)
{
    return menuWindowUp(app) || (app.inventory && app.inventory->isOpen()) || (app.book && app.book->isOpen()) ||
           (app.console && app.console->isOpen()) || (app.chat && app.chat->isOpen()) ||
           (app.serverOptions && app.serverOptions->isOpen()) || (app.motdWindow && app.motdWindow->isOpen()) ||
           (app.mapVote && app.mapVote->shown()) ||
           (app.tradeScreens && (app.tradeScreens->shopOpen() || app.tradeScreens->dialogOpen() || app.tradeScreens->tradeOpen())) ||
           (app.cinematics && app.cinematics->screenUp());
}

/// The view's size this frame: the editor's Map panel's game area, else the window, a game's
/// letterboxed to its resolution's shape (gameArea); a game's zoom fixed by its Options.
void layoutView(App& app, bool embedded)
{
    // (SDL's mouse is in window points, the view in output pixels: pointer_map.hpp,
    // window_density.hpp. The editor lays its Map panel out in points.)
    app.pixelDensity = embedded ? 1.0f : windowPixelDensity(app.window);
    if (embedded) {
        playTestLayout(app); // (the game's area in the editor's Map panel)
    } else {
        SDL_GetCurrentRenderOutputSize(app.renderer, &app.camera.viewWidth, &app.camera.viewHeight);
        const int outW = app.camera.viewWidth, outH = app.camera.viewHeight;
        const SDL_Rect area = backend::usableRenderArea(app.renderer).value_or(SDL_Rect{0, 0, outW, outH});
        app.camera.viewWidth = area.w;
        app.camera.viewHeight = area.h;
        // A game's view keeps its resolution's shape: the largest such area that fits, black
        // bars round it (the frame is drawn at its size: beginFrame / endFrame).
        const float ow = static_cast<float>(app.camera.viewWidth);
        const float oh = static_cast<float>(app.camera.viewHeight);
        app.gameArea = SDL_FRect{0, 0, ow, oh};
        app.letterboxed = false;
        if (app.live && !app.joinWaiting && app.viewWidth > 0 && app.viewHeight > 0) { // (the join's wait: the menus')
            const float fit = std::min(ow / static_cast<float>(app.viewWidth), oh / static_cast<float>(app.viewHeight));
            const float w = std::min(ow, std::round(static_cast<float>(app.viewWidth) * fit));
            const float h = std::min(oh, std::round(static_cast<float>(app.viewHeight) * fit));
            if (w + 1 < ow || h + 1 < oh) {  // only when the bars would be over a pixel wide
                app.gameArea = SDL_FRect{std::floor((ow - w) / 2), std::floor((oh - h) / 2), w, h};
                app.letterboxed = true;
                app.camera.viewWidth = static_cast<int>(w);
                app.camera.viewHeight = static_cast<int>(h);
            }
        }
        app.gameArea.x += static_cast<float>(area.x);
        app.gameArea.y += static_cast<float>(area.y);
        app.letterboxed = app.letterboxed || area.x != 0 || area.y != 0 || area.w != outW || area.h != outH;
    }
    if (app.live) {
        app.camera.zoom = viewZoom(app); // a game's view is fixed by its Options
    }
}

/// A frame of the menus (out of a game): --mouse-script's steps, a movie or the legal screen, the
/// menus and Options with the pointer's sparks, presented (a --screenshot run's shot once due).
SDL_AppResult menusFrame(App& app, bool shotReady)
{
    while (!app.mouseScript.empty() && SDL_GetTicks() >= app.mouseScript.front().atMs) {
        // --mouse-script in the menus: times from start-up.
        const auto m = app.mouseScript.front();
        app.mouseScript.erase(app.mouseScript.begin());
        app.testMouse = SDL_FPoint{m.x, m.y};
        SDL_Event e{};
        if (m.action == 5) {
            e = keyEvent(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE); // (Escape: a movie's, the legal screen's key)
        } else if (m.action == 8) {
            e = scriptedKey(m); // (a key: x its SDL keycode; the menus act on the press)
            app.testMouse.reset(); // (the mouse left where it was)
        } else {
            e = scriptedMouseEvent(m);
        }
        SDL_PushEvent(&e);
    }
    if (app.movie && app.movie->active()) {
        // A movie (the start-up's, or the main menu's Intro): it alone is drawn.
        app.movie->update(SDL_GetTicks());
        if (app.movie->active()) {
            return showMovie(app, shotReady);
        }
    }
    if (app.legal) {
        app.legal->update(SDL_GetTicks());
        if (app.legal->over()) {
            app.legal.reset(); // (on to the main menu: it was shown from its end)
        }
    }
    app.menus->update(SDL_GetTicks());
    if (app.options) {
        app.options->update(SDL_GetTicks());
    }
    if (app.inputConfig) {
        app.inputConfig->update(SDL_GetTicks());
    }
    if (app.quitRequested) {
        return SDL_APP_SUCCESS;
    }
    if (app.live && !app.joinWaiting) {
        return SDL_APP_CONTINUE; // a game was just chosen (another's: the join's wait from the next frame)
    }
    beginFrame(app);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(app.renderer);
    if (app.legal) {
        app.legal->draw(app.renderer, app.camera.viewWidth, app.camera.viewHeight);
    } else {
        app.menus->draw(app.renderer, app.camera.viewWidth, app.camera.viewHeight);
    }
    if (app.options) {
        app.options->draw(app.renderer, app.camera.viewWidth, app.camera.viewHeight);
    }
    if (app.inputConfig) {
        app.inputConfig->draw(app.renderer, app.camera.viewWidth, app.camera.viewHeight);
    }
    if (const float scale = menuSparkScale(static_cast<float>(app.camera.viewWidth), static_cast<float>(app.camera.viewHeight));
        scale > 0) {
        // The pointer's sparks (mainloop) in the menus' 640 x 480, over everything but the
        // pointer itself. (None while the output has no area: a minimised window.)
        if (!app.shellSparks) {
            app.shellSparks = std::make_unique<ScreenParticles>();
        }
        const float w = static_cast<float>(app.camera.viewWidth);
        const float h = static_cast<float>(app.camera.viewHeight);
        const float ox = std::floor((w - 640 * scale) / 2);
        const float oy = std::floor((h - 480 * scale) / 2);
        const std::uint64_t now = SDL_GetTicks();
        if (app.shellSparkMs == 0 || now - app.shellSparkMs > 1000) {
            app.shellSparkMs = now;
        }
        // Stepped 30 times a second; a press since the last step counts (shellPressed).
        for (; now - app.shellSparkMs >= 33; app.shellSparkMs += 1000 / 30) {
            const SDL_FPoint m = mousePos(app);
            SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
            const bool down = app.shellPressed || (buttons & SDL_BUTTON_LMASK) != 0;
            app.shellPressed = false;
            const SDL_Point at{static_cast<int>(std::floor((m.x - ox) / scale)),
                               static_cast<int>(std::floor((m.y - oy) / scale))};
            app.shellSparks->shellPointer(at, down, [&app] { uiSound(app, 0x39C); }); // ShellMouseBoom
        }
        app.shellSparks->draw(app.renderer, static_cast<std::uint32_t>(now * 30 / 1000), scale, ox, oy, 640,
                               480, app.renderGlow);
    }
    if (app.options && app.options->isOpen()) {
        app.tooltip = app.options->tooltip(); // noxsdl's switches say what they do
        drawTooltip(app, true);
    }
    drawFps(app);
    if (std::exchange(app.logMenuPointer, false)) {
        // (Back from the editor or a game: the pointer drawn where the mouse is, never a held place.)
        const SDL_FPoint drawn = mousePos(app);
        SDL_FPoint mouse{};
        SDL_GetMouseState(&mouse.x, &mouse.y);
        SDL_Log("menus: pointer drawn at %.0f, %.0f (from %s), the mouse at %.0f, %.0f; relative %s, pointer %s", drawn.x, drawn.y,
                pointerSourceName(pointerSource(app.testMouse.has_value(), app.playTestMouse.has_value(), false)), mouse.x, mouse.y,
                SDL_GetWindowRelativeMouseMode(app.window) ? "on" : "off", SDL_CursorVisible() ? "shown" : "hidden");
    }
    // The menus' pointer, at their 640 x 480 layout's scale.
    drawCursor(app, CursorType::Select,
               std::min(static_cast<float>(app.camera.viewWidth) / 640.0f,
                        static_cast<float>(app.camera.viewHeight) / 480.0f));
    if (!app.legal) {
        app.menus->drawOverAll(app.renderer, app.camera.viewWidth, app.camera.viewHeight); // the credits' fades
    }
    endFrame(app);
    if (shotReady && SDL_GetTicks() >= static_cast<std::uint64_t>(app.shotAfterSeconds * 1000)) {
        app.shotTaken = true;
        return saveScreenshot(app) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    present(app);
    return SDL_APP_CONTINUE;
}

/// A game's windows stepped each frame (the --open-* tests' openings with them), and the host's
/// bots and router port.
void updateGameWindows(App& app)
{
    if (app.quitMenu) {
        if (app.openQuitMenu &&
            app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame) {
            app.openQuitMenu = false;
            app.quitMenu->toggle();
        }
        app.quitMenu->update(SDL_GetTicks());
    }
    tickLocalBots(app);
    tickPortMapping(app);
    if (app.motdWindow) {
        // The MOTD each frame while one waits: shown once no menu is up (not in a
        // quest's chat map).
        const bool menuUp = (app.quitMenu && app.quitMenu->isOpen()) || (app.options && app.options->isOpen());
        if (!app.motdWindow->isOpen() && !menuUp && app.autoDone &&
            app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame && app.live->takeNewMotd()) {
            const auto flags = app.live->client().world().game().gameFlags;
            if (!((flags & 0x1000u) && (flags & 0x80u)) && !app.noMotd) {  // quest (0x1000) with chat (0x80): a quest's chat map
                app.motdWindow->show(app.live->motd());
            }
        }
        app.motdWindow->update(SDL_GetTicks());
    }
    if (app.questWindows) {
        const auto& client = app.live->client();
        std::optional<GameOverWords> words; // (a mode's own, as one's in a run)
        for (const auto& [id, ui] : app.modeUis) {
            if (!words) {
                words = ui->gameOver();
            }
        }
        app.questWindows->setGameOverWords(std::move(words));
        app.questWindows->update(client.world(), client.frame(), app.host != nullptr, SDL_GetTicks());
    }
    if (app.serverOptions) {
        if (!app.host) {
            app.serverOptions->close(); // (the host's own)
        } else if (app.openServerOptions && app.autoDone &&
                   app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame) {
            app.openServerOptions = false;
            if (!app.serverOptions->isOpen()) {
                app.serverOptions->toggle();
            }
            if (app.serverOptionsPage != 0) {
                app.serverOptions->showPageForTest(app.serverOptionsPage);
            }
        }
        app.serverOptions->update(SDL_GetTicks());
    }
    if (app.options) {
        app.options->update(SDL_GetTicks());
    }
    if (app.inputConfig) {
        app.inputConfig->update(SDL_GetTicks());
    }
    if (app.inventory) {
        if (app.openInventory && app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame &&
            app.autoDone) {
            app.openInventory = false;
            app.inventory->toggle();
        }
        app.inventory->setMapShown(app.minimap && app.minimap->shown());
        app.inventory->update(app.live->client().world(),
                               gameFrame(app));
    }
    if (app.rankBoard) {
        // A game's end opens the scoreboard; the next map puts its mode back (Gui_Rank_ShowOnGameOver).
        const auto& game = app.live->client().world().game();
        const bool quest = (game.gameFlags & 0x1000) != 0;
        if (game.overCount != app.gameOversSeen) {
            app.gameOversSeen = game.overCount;
            app.rankBoard->gameOver(quest, app.online);
            app.banner = gameOverBanner(app);
        }
        if (app.gameWasOver && !game.over) {
            app.banner.reset(); // (the next map)
        }
        app.gameWasOver = game.over;
        app.rankBoard->update(game.over, quest, app.online);
    }
    if (app.mapVote) {
        app.mapVote->update(app.live->client().mapVote(), SDL_GetTicks());
    }
    for (auto& [id, ui] : app.modeUis) {
        ui->update(app); // (the modes' own: a mode's map)
    }
    if (app.summon) {
        const SDL_FPoint m = mousePos(app);
        app.summon->update(app.live->client().world(), gameFrame(app),
                            m.x, m.y, app.camera.viewWidth, app.camera.viewHeight);
    }
    if (app.book && app.live->client().stage() == nox::client::HeadlessClient::Stage::InGame && app.autoDone) {
        if (app.openBook) {
            const auto ctx = bookContext(app);
            app.book->show(ctx);
            if (*app.openBook >= 0) {
                app.book->turnTo(*app.openBook, ctx);
            }
            app.openBook.reset();
        }
        if (std::exchange(app.expandBarsTest, false)) {
            expandBars(app, true);
        }
    }
}

/// How a join's wait ended (joinWaitFrame), for gameLeft: not one (a game left), failed (its dialog
/// says why, over the game list it's up over), or its Cancel (the menus go back to the list).
enum class JoinWaitEnd : std::uint8_t { None, Failed, Cancelled };

/// The game left (or never joined): its windows closed, a game we hosted retired, and back to the
/// menus' game list or main menu with the notice of why; without the menus (or after a
/// screenshot) the program's end. A join's wait that ended (`joinEnd`): the game list is up
/// already, its dialog says why (join_wait.hpp).
SDL_AppResult gameLeft(App& app, JoinWaitEnd joinEnd = JoinWaitEnd::None)
{
    app.joinWaiting = false;
    app.joinWaitShow = false;
    if (joinEnd == JoinWaitEnd::Cancelled) {
        SDL_Log("join: cancelled (the dialog's Cancel)");
    }
    // (the join's dialog's words for why it failed, read before the session goes)
    const std::optional<join_wait::Words> joinFailure =
        joinEnd == JoinWaitEnd::Failed ? join_wait::failure(app.live->client()) : std::nullopt;
    if (app.quitMenu) {
        app.quitMenu->close();
    }
    if (app.serverOptions) {
        app.serverOptions->close();
    }
    if (app.motdWindow) {
        app.motdWindow->close();
    }
    app.questWindows.reset();
    app.localBots.squad.reset(); // (the game we host ends as we leave it, its bots too)
    app.localBots.wanted = {};
    PortMapper::retire(std::move(app.portMapper)); // (the router's mapping removed off this thread)
    // A game we hosted ends as we leave it, as the exe's host's: its players told
    // (MSG_SERVER_QUIT, then SERVER_CLOSE as it goes), in the background (LocalHost::retire).
    const bool ownGame = app.host != nullptr;
    LocalHost::retire(std::move(app.host));
    const bool failed = app.live->client().stage() == nox::client::HeadlessClient::Stage::Failed || app.live->tooOld();
    using EndReason = nox::client::HeadlessClient::EndReason;
    const EndReason endReason = app.live->client().endReason();
    // (The lobby asked for the game's password and we had none, or the one given was refused:
    // asked for over the game list, the game joined again with it.)
    const bool wrongPassword = app.live->client().joinError() == nox::net::JoinError::WrongPassword;
    const bool askPassword = failed && !ownGame && endReason == EndReason::None && (app.live->client().passwordAsked() || wrongPassword);
    if (app.menus && !app.shotTaken) { // (a screenshot run ends once it has left)
        // Back to the game list.
        std::string couldNot =
            nox::l10n::format(NOX_TR("game.join.couldnt_join_log", "Couldn't join {host} (see noxcore-log.txt)"), {{"host", app.liveHost}});
        for (const auto& [id, ui] : app.modeUis) {
            // (a class refused: a campaign mode's says which)
            if (const auto why = ui->joinRefused(static_cast<std::uint8_t>(app.live->client().joinError()), app.browser->selectedEntry())) {
                couldNot = nox::l10n::format(NOX_TR("game.join.couldnt_join_why", "Couldn't join {host}: {why}"),
                                             {{"host", app.liveHost}, {"why", *why}});
                break;
            }
        }
        // (a host whose content packs add object types lets in only a game with the same ones: refused
        // at the join, or its connect turned away as when full)
        if (const auto& ext = app.live->extended(); ext && ext->typesHash != 0 && ext->typesHash != nox::content::newThingTypes().hash) {
            couldNot = nox::l10n::format(
                nox::content::newThingTypes().hash != 0
                    ? NOX_TR("game.join.packs_differ",
                             "Couldn't join {host}: its content packs ({packs}) add object types, and yours add other ones. Install "
                             "the same packs in the mods folder to join.")
                    : NOX_TR("game.join.packs_missing",
                             "Couldn't join {host}: its content packs ({packs}) add object types. Install the same packs in the "
                             "mods folder to join."),
                {{"host", app.liveHost}, {"packs", ext->typesPacks}});
        }
        // (a host with a lowest client level above ours refuses us at the join, or the session saw
        // its level before joining: LiveSession::tooOld; the notice in a message box over the game
        // list, as choosing a game marked "Update needed" shows it: client_level_gate.hpp)
        const bool tooOldNotice = noxsdl::client_level::noticeAfterJoin(failed, endReason != EndReason::None, app.live->extended());
        if (tooOldNotice) {
            couldNot = nox::l10n::format(NOX_TR("game.join.couldnt_join", "Couldn't join {host}"), {{"host", app.liveHost}});
        } else if (askPassword) {
            couldNot = nox::l10n::format(wrongPassword ? NOX_TR("game.join.wrong_password", "Couldn't join {host}: the password was wrong")
                                                       : NOX_TR("game.join.needs_password", "Couldn't join {host}: it asks for a password"),
                                         {{"host", app.liveHost}});
        }
        app.browser->setMessage(joinEnd == JoinWaitEnd::Cancelled                ? std::string()
                                : failed && endReason == EndReason::None ? couldNot
                                                                         : nox::l10n::format(NOX_TR("game.join.left", "Left {host}"),
                                                                                             {{"host", app.liveHost}}));
        saveBars(app);
        app.book.reset();
        app.cinematics.reset();
        unbindModeClients(app);   // (the modes' UIs let go of their client sides, before the client that has them goes)
        endSoloScreensHost(app); // (a campaign mode host's saves' root back)
        app.live.reset();
        app.view.reset();
        useInstallData(app); // (a mod's own game data: the install's tables again)
        if (app.sprites) {
            app.sprites->beginMap({}); // (the map's images no longer kept)
            app.sprites->setHolding(false);
        }
        app.mapHeldSinceMs = 0;
        app.mapHeld = false;
        app.spellLimits.clear();
        if (app.audio) {
            app.audio->leaveGame();
        }
        app.speaking.clear();
        if (app.chant) {
            app.chant->clear();
        }
        app.xwisHost.reset(); // (the game off XWIS's list: PART, QUIT)
        gui::setShellUp(true); // (cliWaitForJoinData)
        if (joinEnd != JoinWaitEnd::None) {
            // The join's wait over: the game list is up under its dialog. Cancel's goes back to the
            // list itself (Menus::cancelJoinWait); a failure's dialog says why, as the 1.2b client's
            // join dialog turns to it, OK back to the list; the password's and a newer NoX Core's
            // over the list as before; any other, the list again with the line of why.
            if (joinEnd == JoinWaitEnd::Failed) {
                if (tooOldNotice) {
                    app.menus->endJoinWait();
                    app.menus->tooOldNotice();
                } else if (askPassword) {
                    app.menus->endJoinWait();
                    app.menus->askJoinPassword(app.liveHost, app.profile, wrongPassword);
                } else if (joinFailure) {
                    app.menus->joinWaitFailed(*joinFailure);
                } else {
                    app.menus->endJoinWait();
                    app.menus->showGameBrowser();
                }
            }
            handMouseTo(app, MouseOwner::Menus);
            app.logMenuPointer = true;
            SDL_StartTextInput(app.window);
            return SDL_APP_CONTINUE;
        }
        if (std::exchange(app.soloFromMenus, false)) {
            // A solo game's end: its host gone, back to the main menu (the screens' state
            // codes were dropped as the game started: windowSelCharProc).
            app.menus->showMainMenu();
        } else {
            app.menus->showGameBrowser();
        }
        // Gui_ShowTimeoutNotice (MSG_TIMEOUT_NOTIFICATION, the lag window's Exit) and
        // Gui_ShowKickedNotice: their notice over the game list.
        if (endReason == EndReason::TimedOut || endReason == EndReason::LostConnection) {
            app.menus->notice("Noxworld.c:Notification", "Noxworld.c:Timeout");
        } else if (endReason == EndReason::Kicked) {
            app.menus->notice("Noxworld.c:Notification", "Noxworld.c:Kicked");
        } else if (tooOldNotice) {
            app.menus->tooOldNotice();
        } else if (askPassword) {
            app.menus->askJoinPassword(app.liveHost, app.profile, wrongPassword);
        }
        handMouseTo(app, MouseOwner::Menus); // (nothing of a play view's mouse left behind)
        app.logMenuPointer = true;
        SDL_StartTextInput(app.window);
        return SDL_APP_CONTINUE;
    }
    return failed ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
}

/// A frame of a join's wait (join_wait.hpp): the session pumped, the menus drawn with the 1.2b
/// client's join dialog over the game list (its words as the join goes: attempting, the host's ping
/// test, connected); a failure ends it with its dialog (gameLeft); once the host has let us in, the
/// game's frame as before (none returned).
std::optional<SDL_AppResult> joinWaitFrame(App& app, bool shotReady)
{
    if (std::exchange(app.joinWaitShow, false)) {
        app.menus->showJoinWait([&app] {
            if (app.live) {
                gameLeft(app, JoinWaitEnd::Cancelled);
                return;
            }
            // (Cancel while the game's ping was still out: no session yet, nothing to close.)
            SDL_Log("join: cancelled (the dialog's Cancel) while %s's ping was out", app.pingWaitHost.c_str());
            app.pingWaitHost.clear();
            app.joinWaiting = false;
        });
    }
    if (!app.live) {
        // The wait for an XWIS game's ping (PingWait): the game list kept polling under the
        // dialog; the session once the ping has answered or given up, or at the browser's own
        // give-up time.
        if (app.pingWaitHost.empty()) {
            app.joinWaiting = false; // (nothing to wait for)
            return menusFrame(app, shotReady);
        }
        const PingWait wait{app.pingWaitStartedAt};
        const auto now = static_cast<std::uint32_t>(SDL_GetTicks());
        const auto step = wait.step(app.browser ? app.browser->entryFor(app.pingWaitHost) : nullptr, now);
        if (step == PingWait::Step::Wait) {
            return menusFrame(app, shotReady);
        }
        const std::string host = std::exchange(app.pingWaitHost, {});
        SDL_Log("join: %s's ping: %s after %u ms; joining", host.c_str(), PingWait::name(step), now - wait.startedAt);
        startLive(app, host);
        gui::setShellUp(true); // (the join's dialog stays up: the menus' till the host lets us in)
    }
    app.live->update();
    if (app.live->done()) {
        gameLeft(app, JoinWaitEnd::Failed);
        return menusFrame(app, shotReady);
    }
    const auto& client = app.live->client();
    if (join_wait::stillWaiting(client)) {
        app.menus->joinWaitWords(join_wait::waiting(client));
        return menusFrame(app, shotReady);
    }
    // In (the connection up): the dialog goes and the game takes over, its windows small.
    app.joinWaiting = false;
    app.menus->endJoinWait();
    gui::setShellUp(false);
    return std::nullopt;
}

/// This frame's input to the server: facing the cursor or moving to it, Action held, the spells' target
/// flag turned while InvertSpellTarget is held, and where the cursor is.
void sendGameInput(App& app)
{
    auto& client = app.live->client();
    // Turn to the cursor (moving there while MoveForward's binding is held: the right button),
    // Action while its binding is held (the left button: swing, or use what's there), and
    // tell the server where the cursor is: aimed spells go there, and it runs or walks by it.
    // (The controls by their place in the game's table, looked up once.)
    static const int kAction = InputBindings::control("Action");
    static const int kMoveForward = InputBindings::control("MoveForward");
    static const int kInvertSpellTarget = InputBindings::control("InvertSpellTarget");
    const bool keysFree = !(app.quitMenu && app.quitMenu->isOpen());
    const bool attacking3d = keysFree && app.input && app.input->held(kAction); // (the 3D view faces the crosshair while attacking)
    const bool moving = keysFree && ((app.input && app.input->held(kMoveForward)) || app.view3d.walking(attacking3d));
    if (moving) {
        client.move(app.view3d.walkFacing(facingToMouse(app), attacking3d)); // (the 3D view's keys)
    } else {
        if (app.running) {
            client.move(std::nullopt);
        }
        client.face(facingToMouse(app));
    }
    app.running = moving;
    client.setAttack(keysFree && app.input && app.input->held(kAction));
    {
        // InvertSpellTarget held: the set's spells turned on or off
        // us while it's down, and back when it's let go.
        const bool invert = keysFree && app.input && app.input->held(kInvertSpellTarget) && !app.barsExpanded;
        if (invert != app.barsInverted) {
            app.barsInverted = invert;
            for (auto& slot : app.bars.current()) {
                if (slot.id != 0 && !(nox::client::spellFlags(app.things, slot.id) & 0x200400)) {  // those spell flags: a target that can't change
                    slot.target = !slot.target;
                }
            }
            uiSound(app, 0x399);
        }
    }
    {
        const SDL_FPoint m = mousePos(app);
        client.setCursor(static_cast<int>(app.camera.left() + m.x / app.camera.zoom),
                         static_cast<int>(app.camera.top() + m.y / app.camera.zoom));
        // The drawable the cursor's on (the frame's hover pick, clientOnCursorHover): its y goes as the
        // cursor's, so an attack or spell aims where it stands (CliNet_QueuePlayerInput).
        client.setCursorSprite(app.hovered);
    }
}

/// The world drawn: lit (LightMap), the line of sight worked out, then the 3D view, or the 2D
/// floor, scene, effects and bloom; a session's pending edits with it.
void drawWorld(App& app, std::vector<MapView::SceneSprite>& extra, const std::set<std::uint32_t>& liveCodes)
{
    SightMap* sight = nullptr;
    LightMap* light = nullptr;
    // The weather (a script mode's nox.weather, or the console's test): moved on before the light
    // pass, which it darkens and flashes.
    WeatherFx* weather = app.weather && !app.view3d.active() ? app.weather.get() : nullptr;
    if (weather) {
        static const nox::modes::script::weather::Told kNone{};
        const nox::modes::script::weather::Told* told = app.live && app.scriptUi ? &app.scriptUi->weather() : &kNone;
#ifndef NOX_NO_TEST_COMMANDS
        if (app.weatherTest) {
            told = &*app.weatherTest;
        }
#endif
        weather->update(*told, SDL_GetTicksNS(), app.camera, static_cast<WeatherFx::Level>(std::clamp(app.weatherEffects, 0, 2)));
    }
    if (app.lighting) {
            const auto frame = app.live ? gameFrame(app)
                                     : clockFrame();
        // The camera centre is the local player in a live game.
        const Rgb ambient = app.ambient.value_or(app.view->ambientAt(app.camera.x, app.camera.y));
        app.light.build(app.view3d.worldCamera(app.camera), weather ? weather->ambient(ambient) : ambient, frameLights(app, liveCodes), frame);
        light = &app.light;
    }
    if (app.lineOfSight.value_or(app.live != nullptr)) {
        auto& blockers = app.sightBlockerList;
        blockers.clear();
        sightBlockers(app, blockers); // (the live ones, then the map's)
        app.view->sightBlockers(blockers, app.live ? &liveCodes : nullptr);
        // The eye: the view's centre (which holds the shake the camera took) plus the shake as
        // the scene draw swings it before working out the sight, so a shaken view keeps the
        // eye within a unit of the player.
        const int jiggle = app.effects->jiggle();
        const int eyeShift = jiggle > 0 ? 1 - jiggle : jiggle < 0 ? -1 - jiggle : 0;
        app.sight.setClassicWalls(app.classicWalls);
        app.sight.build(app.camera, app.view->walls(), &app.view->wallGrid(), app.view->windows(), app.view->doorWalls(), app.things,
                        blockers, eyeShift);
        app.sight.setSoftEdges(app.softShadowEdge);
        sight = &app.sight;
    }
    if (app.live) {
        // Client-predicted drawables (cleansing flames) go once out of sight, as the game's
        // client drops them; the server never removes them. (Their scene items, built above,
        // go with them: they point into the drawables.)
        const Camera& c = app.camera;
        dropPredictedWithItems(app.live->client(), extra, [&](const nox::client::Drawable& d) {
            const auto x = static_cast<float>(d.x);
            const auto y = static_cast<float>(d.y);
            if (sight) {
                return !sight->visible(x, y);
            }
            return x < c.left() || x > c.right() || y < c.top() || y > c.bottom();
        });
    }
    updatePending(app); // (a map editor session's walls, floor and things not yet published: drawing only)
    if (app.live && app.minimap && app.view3d.active()) {
        // (In 2D the scene's sort lists the obelisks it draws: liveSprites, drawnMarkers.)
        app.minimap->observe(app.live->client().world(), app.camera, sight); // obelisks seen
    }
    if (app.view3d.active()) {
        // The world in first person (view3d.hpp); the HUD and windows draw over it. The ring
        // under what the crosshair is on, on its floor (only while it aims).
        app.view3d.setRing(app.view3d.looking() ? hoverRing(app) : std::nullopt);
        static const int kActionControl = InputBindings::control("Action"); // (its place in the control table)
        // In a game, release builds hold it to the 2D view's sight: nothing it hides (behind
        // sight blockers, past the screen's edge) is drawn in 3D either.
#ifdef NOX_NO_TEST_COMMANDS
        const SightMap* sight3d = sight;
#else
        const SightMap* sight3d = app.view3dSight ? sight : nullptr;
#endif
        app.view3d.draw({app.renderer, *app.view, *app.sprites, app.camera, light, extra,
                          app.live ? &liveCodes : nullptr, app.live ? &app.live->client() : nullptr,
                          app.art.get(), &app.modifiers,
                          app.live ? app.interpolator.gameTime() : static_cast<double>(SDL_GetTicks()) * 30 / 1000,
                          app.input && app.input->held(kActionControl), app.effects.get(), sight3d});
    } else {
        const std::uint64_t worldDrawStart = app.frameStats ? SDL_GetTicksNS() : 0;
        app.view->drawFloor(app.renderer, app.camera, *app.sprites, light, sight);
        drawHoverRing(app); // (on the floor, under the objects: the world's draw draws it before them)
        if (app.live) {
            for (auto& [id, ui] : app.modeUis) {
                ui->drawFloor(app, app.renderer); // (a mode's ground decals: on the floor, under the things)
            }
        }
        app.view->setCollectBloom(app.bloom != nullptr);
        app.view->setTime(app.live ? app.interpolator.gameTime() : static_cast<double>(SDL_GetTicks()) * 30 / 1000);
        if (app.effects) {
            // The effects' sort (the particles sorted into the scene, and the sight test of
            // the drawables drawn over it), with this frame's sight.
            app.effects->setSceneDraw(sight, app.renderGlow, app.fadeObjects);
        }
        app.view->setFadeObjects(app.fadeObjects);
        app.view->drawScene(app.renderer, app.camera, *app.sprites, extra, app.live ? &liveCodes : nullptr,
                             light, sight);
        if (app.frameStats) {
            app.worldDrawTimes.push_back(static_cast<float>(static_cast<double>(SDL_GetTicksNS() - worldDrawStart) / 1e6));
        }
        if (app.live && app.minimap) {
            // The map's own obelisks the sort drew go on the minimap (their net code: the
            // extent with bit 15 set); the server's, by their items (liveSprites).
            for (const std::uint32_t extent : app.view->drawnMarkers()) {
                app.minimap->markDrawn(static_cast<std::uint16_t>(extent | 0x8000u));
            }            }
#ifndef NOX_NO_TEST_COMMANDS
        if (app.traceFade && app.live) {
            // --trace-fade (tests): every drawable lingering or told into shadows, as the sort left it.
            const double now = app.interpolator.gameTime();
            for (const auto& [netCode, d] : app.live->client().world().drawables()) {
                if (d.fade.lingering || d.fade.told) {
                    SDL_Log("trace-fade %.2f %s #%u at %d,%d lingering %d told %d in-sight %u alpha %u%s", now,
                            app.live->client().world().thingName(d.typeId).c_str(), netCode, d.x, d.y,
                            d.fade.lingering, d.fade.told, d.fade.inSightFrame,
                            nox::client::lingerAlpha(now - static_cast<double>(d.fade.inSightFrame)),
                            d.fade.expireAt ? (" expires " + std::to_string(d.fade.expireAt)).c_str() : "");
                }
            }
            app.view->traceFade();
        }
#endif
        if (app.effects) {
            app.effects->setLight(light);
            app.effects->drawParticles(app.renderer, app.camera, sight, app.renderGlow);
            // Bloom: the effects again into the bloom target, blurred and added over the scene.
            // Before them, the bright parts of the light sources' sprites the scene drew, less what
            // was drawn in front of them, in the scene's own order.
            if (app.bloom && app.bloom->begin(app.renderer, app.camera.viewWidth, app.camera.viewHeight)) {
                const auto& list = app.view->bloomSprites();
                SDL_FRect lit{};
                bool anyLit = false;
                // Light sources' sprites go in (lit) and widen the lit box; a later sprite over that box hides
                // what it covers.
                for (const auto& b : list) {
                    // (A sprite whose BC7 upload failed isn't drawn: ready() false.)
                    if (b.source) {
                        if (!b.sprite->ready()) {
                            continue;
                        }
                        if (!anyLit) {
                            lit = b.dst;
                            anyLit = true;
                        } else {
                            SDL_GetRectUnionFloat(&lit, &b.dst, &lit);
                        }
                        app.bloom->addSprite(app.renderer, b.sprite->texture, b.sprite->source(&b.src), b.dst, b.light, b.alpha);
                    } else if (anyLit && SDL_HasRectIntersectionFloat(&lit, &b.dst)) {
                        if (!b.sprite->ready()) {
                            continue;
                        }
                        app.bloom->hide(app.renderer, b.sprite->texture, b.sprite->source(&b.src), b.dst, b.alpha);
                    }
                }
                app.effects->drawParticles(app.renderer, app.camera, sight, app.renderGlow, false, true);
                app.bloom->end(app.renderer);
            }
        }
        if (weather) {
            weather->draw(app.renderer, app.camera, light != nullptr, sight); // (over the world, under the HUD)
        }
    }
}

/// The HUD and the game's windows over the world, bottom to top: the chant's gestures, the minimap,
/// the centred messages, the bars, the book, the banner, the scoreboard and the windows.
void drawGameHud(App& app)
{
    const auto& client = app.live->client();
    if (app.chant) {
        // The chant's hand gestures, round the view's centre.
        app.chant->draw(app.renderer, *app.sprites, app.camera, app.camera.x, app.camera.y,
                         gameFrame(app));
    }
    if (app.minimap) {
        // The map over the world, under the HUD; 1 / 2 held zoom it.
        const int w = app.camera.viewWidth;
        const int h = app.camera.viewHeight;
        if (std::exchange(app.openMap, false) && !app.minimap->shown()) {
            // Pressed as a key, so the test goes through the binding (TAB = ToggleMap).
            SDL_Event press = keyEvent(SDLK_TAB, SDL_SCANCODE_TAB);
            SDL_PushEvent(&press);
        }
        const bool keysFree = !(app.quitMenu && app.quitMenu->isOpen()) && app.input;
        const std::uint64_t now = SDL_GetTicks();
        static const int kZoomIn = InputBindings::control("MapZoomIn"); // (their places in the control table)
        static const int kZoomOut = InputBindings::control("MapZoomOut");
        app.minimap->zoom(keysFree && app.input->held(kZoomIn), keysFree && app.input->held(kZoomOut),
                           app.lastFrameMs ? now - app.lastFrameMs : 0);
        app.lastFrameMs = now;
        app.minimap->draw(app.renderer, client.world(), *app.view, client.netCode(), app.camera, w, h,
                           Hud::scale(w, h));
    }
    {
        // The centred messages over it (drawMinimapAndLines's drawMessageLines), placed by
        // the game's view (the windowSize box) and centred across the screen.
        const int w = app.camera.viewWidth;
        const int h = app.camera.viewHeight;
        app.centred.draw(app.renderer, app.font.get(), w, gameWindowBox(app), Hud::scale(w, h),
                          gameFrame(app));
    }
    app.spellLimits.update(client.world(), client.netCode(), clockFrame());
    app.hud->setLimited(app.spellLimits.limited(client.world(), app.gameData, gameFlags(app), client.modeOwnsBook()));
    app.hud->setGameFlags(gameFlags(app)); // (the cooldowns and trap prices: the game's own gamedata lines)
    // (The game's own HUD pieces: not while we observe, nor under a mod's covering window.)
    const bool hudPieces = gameHudShown(app);
    if (app.renderGui && hudPieces && !(app.tradeScreens && app.tradeScreens->hidesHud())) {
        app.hud->draw(app.renderer, app.font.get(), *app.sprites, client.world(), client.netCode(),
                       quickbarState(app), app.camera.viewWidth, app.camera.viewHeight,
                       gameFrame(app));
    }
    // The windows over the HUD below: at the HUD's scale, with the mouse.
    const int hudW = app.camera.viewWidth;
    const int hudH = app.camera.viewHeight;
    const float hudScale = Hud::scale(hudW, hudH);
    const SDL_FPoint hudMouse = mousePos(app);
    const HudFrame modeHud{.width = hudW, .height = hudH, .scale = hudScale, .mouse = hudMouse, .nowMs = SDL_GetTicks(), .renderGui = app.renderGui};
    for (auto& [id, ui] : app.modeUis) {
        ui->drawUnderWindows(app, app.renderer, modeHud); // (a mode's HUD pieces, under every window)
    }
    if (app.book && app.renderGui) {
        app.book->draw(app.renderer, bookContext(app), app.camera.viewWidth, app.camera.viewHeight);
        drawFlight(app); // (the award flying to the bars)
    }
    if (app.renderGui) {
        drawGameOverBanner(app); // (under the windows, the scoreboard among them)
    }
    if (app.rankBoard && app.renderGui) {
        app.rankBoard->draw(app.renderer, client.world(), client.netCode(), app.camera.viewWidth,
                             app.camera.viewHeight);
    }
    if (app.mapVote && app.renderGui) {
        // (Over the scoreboard a match's end opens.)
        app.mapVote->draw(app.renderer, client.mapVote(), hudW, hudH, hudScale, hudMouse);
    }
    if (!app.modeUis.empty()) {
        // (The modes' own: updated each frame, drawn with the GUI.)
        for (auto& [id, ui] : app.modeUis) {
            ui->drawHud(app, app.renderer, modeHud);
        }
    }
    if (app.inventory && app.renderGui && (hudPieces || app.inventory->isOpen())) {
        // (Shut, it's the weapon panel and the buffs' row: HUD pieces.)
        app.inventory->setMulticlass(client.multiclass());
        app.inventory->setGameFlags(gameFlags(app));
        app.inventory->setGameFrame(gameFrame(app));
        app.inventory->draw(app.renderer, client.world(), client.netCode(), app.camera.viewWidth,
                             app.camera.viewHeight, mousePos(app));
    }
    const bool hudHidden = app.tradeScreens && app.tradeScreens->hidesHud(); // (renderGUI off)
    if (app.summon && app.renderGui && !hudHidden && hudPieces) {
        app.summon->draw(app.renderer, client.world(), app.camera.viewWidth, app.camera.viewHeight,
                          mousePos(app));
    }
    // (Not under RenderGUI: the exe's hides the HUD's windows, not the shop's or the
    // dialog's, so a cutscene's talk shows under the widescreen.)
    if (app.tradeScreens) {
        app.tradeScreens->draw(app.renderer, client.world(), app.camera.viewWidth, app.camera.viewHeight);
    }
    // A spell being dragged from the book or a bar, under the cursor.
    const std::uint32_t dragged =
        app.book && app.book->dragging() ? app.book->dragging() : app.barDrag ? app.barDrag->what.id : 0;
    if (dragged != 0 && app.renderGui) {
        const SDL_FPoint m = mousePos(app);
        app.hud->drawDragged(app.renderer, *app.sprites, dragged, isWarrior(app), m.x, m.y,
                              app.camera.viewWidth, app.camera.viewHeight);
    }
}

/// --console: once due, the console opened and its commands run (";" between them); "wait MS"
/// runs the rest that many milliseconds later.
void runConsoleScript(App& app)
{
    if (app.console && !app.consoleRun.empty() && SDL_GetTicks() >= app.consoleWaitUntil &&
        (!app.live || (app.inGameSinceMs != 0 && SDL_GetTicks() - app.inGameSinceMs > 1500))) {
        // --console: open it and run its commands (";" between them); "wait MS" runs the rest
        // that many milliseconds later.
        app.console->setClient(app.live != nullptr && !app.host); // (hosting: the host's console)
        if (!app.console->isOpen()) {
            app.console->toggle(app.window);
        }
        std::string_view rest = app.consoleRun;
        while (!rest.empty()) {
            const auto semi = rest.find(';');
            std::string_view one = rest.substr(0, semi);
            while (!one.empty() && one.front() == ' ') {
                one.remove_prefix(1);
            }
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            if (one.starts_with("wait ")) {
                app.consoleWaitUntil = SDL_GetTicks() + std::strtoull(std::string(one.substr(5)).c_str(), nullptr, 10);
                break;
            }
            if (!one.empty()) {
                app.console->run(one);
            }
        }
        app.consoleRun = std::string(rest);
        if (!app.screenshot.empty() && app.console->isOpen()) {
            app.console->toggle(app.window); // (out of the way of the screenshot)
        }
    }
}

} // namespace

/// SDL's frame callback: the game we host's output taken, the view sized (letterboxed in a game), then
/// one frame: a movie, the menus, or a game or the map viewer (stepped, then drawn: the world, the HUD
/// and windows, the console, the cursor), presented.
SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* app = static_cast<App*>(appstate);
    LocalHost::pumpRetired(); // (hosts that left their game, gone once their players acked)
    tickHost(*app);
    checkSession(*app); // (a map editor session's client side)
    // A play-test in the editor's Map panel: the game's frame below, drawn into its texture, then
    // the editor's (editorFrame) round it.
    const bool embedded = gameInPanel(*app);
    if (app->editor && !app->playTestInGame && !app->sessionInGame && !embedded) {
        // The map editor: its frame, presented (a --screenshot run's shot once it's due).
        editorMouse(*app); // (the system's pointer: relative mode off)
        if (app->audio) {
            app->audio->update();
        }
        if (app->live) {
            // A play-test (or an edit session's game) left running behind the editor (F2): its
            // session kept up.
            app->live->update();
            if (app->editSession) {
                app->editSession->tick(static_cast<std::uint32_t>(SDL_GetTicks()));
            }
            playTestJump(*app); // (jump in at the cursor, once our player's in)
            if (app->live->done()) {
                endPlayTest(*app);
            }
        }
        return editorFrame(*app);
    }
    layoutView(*app, embedded);
    if (embedded && !app->playTestCaptured) {
        editorMouse(*app); // (the game in the editor's Map panel, the input the editor's: the system's pointer)
    } else {
        applyMouse(*app); // (a raw count is a game pixel, whose size this frame may have changed)
    }
    // The 3D view has the world, unless a window or menu has the mouse (its cursor back).
    app->view3d.beginFrame(inWorld(*app), windowHasMouse(*app));

    if (app->audio) {
        app->audio->update();
    }
    std::vector<MapView::SceneSprite> extra;
    std::vector<SDL_FPoint> markers;
    std::vector<HealthBar> healthBars;
    std::set<std::uint32_t> liveCodes;
    bool shotReady = !app->screenshot.empty() && !app->shotTaken && !embedded; // (in the editor's panel: the editor's shot)
    if (app->menus && app->joinWaiting && (app->live || !app->pingWaitHost.empty())) {
        // (the join's wait: for the host, or before it for an XWIS game's ping)
        if (const auto result = joinWaitFrame(*app, shotReady)) {
            return *result;
        }
    }
    if (app->menus && !app->live) {
        return menusFrame(*app, shotReady);
    }
    if (app->live && app->movie && app->movie->active()) {
        // A class's ending movie (Cinematics::endGame): the game kept going underneath, unseen.
        app->live->update();
        app->movie->update(SDL_GetTicks());
        if (app->movie->active()) {
            if (embedded) {
                // A play-test in the editor's Map panel: the movie in the game's area there (its
                // frame, as the game's), the editor round it.
                beginFrame(*app);
                app->movie->draw(static_cast<int>(app->gameArea.w), static_cast<int>(app->gameArea.h));
                endFrame(*app);
                return editorFrame(*app);
            }
            return showMovie(*app, shotReady);
        }
    }
    // A game: the client stepped and the windows updated, leaving handled, then the input sent, the tests
    // run and this frame's sprites gathered.
    if (app->live) {
        if (app->editor && app->playTestInGame) {
            // The editor's play-test: its file watch and idle work go on; --playtest-return's test.
            app->editor->backgroundTick();
            if (app->playTestReturnAfterMs != 0 && SDL_GetTicks() - app->playTestStartedMs >= app->playTestReturnAfterMs) {
                app->playTestReturnAfterMs = 0;
                playTestToEditor(*app);
            }
        }
        if (app->editor && app->sessionInGame) {
            app->editor->backgroundTick(); // (the session's editor waits behind the game)
        }
        if (embedded && app->playTestReturnAfterMs != 0 && SDL_GetTicks() - app->playTestStartedMs >= app->playTestReturnAfterMs) {
            app->playTestReturnAfterMs = 0; // (in the Map panel the editor has its frame anyway: the input back to it)
            capturePlayTest(*app, false);
        }
        app->live->update();
        if (app->sessionLeaveAtMs != 0 && app->editSession && SDL_GetTicks() >= app->sessionLeaveAtMs) {
            app->sessionLeaveAtMs = 0;
            SDL_Log("(test) the session's game left, as its Quit menu's Leave");
            clientQuit(*app);
        }
        if (app->host) {
            // The exe's host has no network client: its own never watches for a quiet server. Our
            // leave starting is the host's quit (Game_BeginQuit: MSG_SERVER_QUIT to the others).
            app->live->client().setWatchSilence(false);
            if (app->live->quitting()) {
                app->host->beginQuit();
            }
        }
        if (app->discon) {
            const auto& c = app->live->client();
            app->discon->update(c.silenceIconShown(), c.lagWindowShown(), SDL_GetTicks());
            // Our leave waiting for the server's ack (up to 20 s): the leaving notice says so
            // (the exe's window stands frozen; VD-33).
            std::optional<std::string> notice;
            if (const auto waited = c.quitWaitedMs()) {
                notice = LeavingNotice::text(*waited, c.endReason() == nox::client::HeadlessClient::EndReason::LostConnection);
            } else if (app->leavingNoticeTestMs != 0) {
                notice = LeavingNotice::text(static_cast<std::uint32_t>(SDL_GetTicks() - app->leavingNoticeTestMs), true);
            }
            app->discon->setNotice(notice);
        }
        if (app->editSession) {
            app->editSession->tick(static_cast<std::uint32_t>(SDL_GetTicks()));
        }
        if (app->playTest || app->privateTestRun) {
            playTestJump(*app); // (jump in at the cursor, once our player's in; a private test's --start-at)
        }
        updateGameWindows(*app);
        if (app->live->done() && app->sessionHosting) {
            // Our edit session's game left (its Quit menu, in the Map panel or over the window): the
            // session ends with it for everyone, the editor goes too, and the menus, in one step
            // (as Close Editor's End Session).
            SDL_Log("edit session: our game left: the session ends, to the %s", app->editorFromList ? "game list" : "main menu");
            detachEditor(*app);
            endEditorGame(*app);
            app->editor.reset();
            return app->shotTaken ? SDL_APP_SUCCESS : toMainMenu(*app);
        }
        if (app->live->done() && app->playTest) {
            endPlayTest(*app); // (the play-test's game over: back to the editor)
            return app->shotTaken ? SDL_APP_SUCCESS : SDL_APP_CONTINUE; // (a screenshot run ends once it has left)
        }
        if (app->live->done()) {
            return gameLeft(*app);
        }
        auto& client = app->live->client();
        client.setFadeObjects(app->fadeObjects); // (Options' Fade Objects, applied live)
        syncLiveMap(*app);
        syncWalls(*app);
        holdMap(*app);
        if (app->view) {
            app->view->setDoorAngles(client.world().game().doorAngles);
            app->view->setStatics(client.world().game().statics);
            app->view->setQuestLocks((client.world().game().gameFlags & 0x1000u) != 0);
        }
        // (A stock server rounds our own place to even units; a noxcpp one, that answered our
        // extended query, sends it whole: the averaging only for the first.)
        app->interpolator.setSelfRounded(!app->live->extended().has_value());
        app->interpolator.update(client, static_cast<double>(SDL_GetTicksNS()) / 1e6);
        if (app->smoothing == 3 && app->view) {
            // Match Server follows base game frames and movement substeps (a paused solo game's
            // frames count too). It shows the latest state without interpolation, or redraws after
            // 50 ms without one.
            const std::uint64_t nowMs = SDL_GetTicks();
            const std::uint32_t update = client.snapshotCount();
            if (!snapshotFrameDue(update, app->pacedSnapshot, nowMs, app->pacedAtMs)) {
                if (embedded) {
                    return editorFrame(*app); // (the editor round the game's last frame, still in its texture)
                }
                SDL_Delay(1);
                return SDL_APP_CONTINUE;
            }
            app->pacedSnapshot = update;
            app->pacedAtMs = nowMs;
        }
        for (auto& fx : client.takeEffects()) {
            app->effects->add(std::move(fx));
        }
        if (app->audio) {
            // Sounds come with the frame they belong to, played as the drawing reaches it.
            for (const auto& cue : client.takeAudio()) {
                app->audio->cue(cue);
            }
            app->audio->releaseCues(app->interpolator.gameTime());
        } else {
            for (const auto& cue : client.takeAudio()) { // silent: the gestures still show
                if (cue.kind == nox::client::AudioCue::Kind::SpellChant && app->chant) {
                    app->chant->start(static_cast<std::uint8_t>(cue.id), cue.frame);
                }
            }
        }
        bookEvents(*app);
        if (app->flight) {
            // A frame of the flight per client frame (bookDrawFn draws it: 30 a second), by the wall
            // clock: a solo game is paused meanwhile, its frames stopped.
            const std::uint32_t now = clockFrame();
            // Resynced when it starts, when the clock went back, or when over a second behind (no catching up).
            if (app->flightFrame == 0 || now < app->flightFrame || now - app->flightFrame > 30) {
                app->flightFrame = now;
            }
            for (; app->flightFrame < now && app->flight; ++app->flightFrame) {
                stepFlight(*app);
            }
        }
        if (app->hud && client.stage() == nox::client::HeadlessClient::Stage::InGame) {
            updateTrapBar(*app);
        }
        if (app->chant) {
            // The phoneme due, said in our voice.
            const auto* me = ownPlayer(client);
            const bool female = me && me->female;
            app->chant->update(gameFrame(*app), female,
                               [&](std::uint32_t id) { uiSound(*app, id); });
        }
        if (const auto pos = app->interpolator.camera()) {
            // On whole screen pixels, so the floor and the sprites scroll in step.
            app->camera.x = std::round(pos->x * app->camera.zoom) / app->camera.zoom;
            app->camera.y = std::round(pos->y * app->camera.zoom) / app->camera.zoom;
        }
        for (auto& [id, ui] : app->modeUis) {
            ui->cameraSet(*app); // (a mode guest's view on the scene's actor while its bars are up)
        }
        if (app->traceSelf && client.stage() == nox::client::HeadlessClient::Stage::InGame) {
            // --trace-self (tests): each render frame, the camera, our drawable as the server last
            // put it and where it's drawn, and its animation, to see a hitch in our own motion.
            const auto& ds = client.world().drawables();
            if (const auto it = ds.find(client.netCode()); it != ds.end()) {
                const auto& d = it->second;
                const auto drawn = app->interpolator.position(it->first, d);
                SDL_Log("trace-self %llu cam %.2f %.2f pos %d %d drawn %.2f %.2f anim %u state %u frame %d seen %u",
                        static_cast<unsigned long long>(SDL_GetTicks()), app->camera.x, app->camera.y, d.x, d.y, drawn.x, drawn.y,
                        d.anim, d.animState, d.frame ? static_cast<int>(*d.frame) : -1, d.lastSeenFrame);
            }
        }
        sendGameInput(*app);
        const bool inGame = client.stage() == nox::client::HeadlessClient::Stage::InGame && app->view;
        // questStartRul: SrvNet_OnClientReady for the host (0x1F) in a chat game opens its server
        // options (guiServerOptsLoad), whose first load in a quest runs
        // "execrul OTQuest.rul" (its line startSoloQuest begins the quest 30 frames on).
        if (inGame && app->hostQuestRul && app->host && (client.world().game().gameFlags & 0x80u)) {
            app->hostQuestRul = false;
            app->serverOptionsLoad = app->liveMapLoads; // (the quest begins at once: not shown)
            app->host->command("execrul OTQuest.rul", client.netCode(), [app](const std::vector<std::string>& lines) {
                for (const auto& line : lines) {
                    if (app->console) {
                        app->console->print(line, White);
                    }
                }
            });
        }
        // Out of a quest the host's own arrival in a chat map (hosting's first map, "Form a new
        // game") brings the window up, for the game type and map to be picked. (The exe's
        // Sanctuary help and WOL connection-type windows that come first instead aren't done.)
        if (inGame && app->host && !app->host->directed() && app->serverOptions && (client.world().game().gameFlags & 0x80u) &&
            !(client.world().game().gameFlags & 0x1000u) && app->serverOptionsLoad != app->liveMapLoads) {
            app->serverOptionsLoad = app->liveMapLoads;
            if (!app->serverOptions->isOpen()) {
                app->serverOptions->toggle();
            }
        }
        if (inGame && app->inGameSinceMs == 0) {
            app->inGameSinceMs = SDL_GetTicks();
            characterPlayedHere(*app); // (noxcpp: a mode's game in the labels of the character we joined with: character_play.hpp)
            if (app->joinTeam != 0) {
                client.joinTeam(app->joinTeam);
            }
        }
        runGameTests(*app, inGame);
        // Shoot once we've been in the game long enough; if we never get there, shoot anyway after
        // a while (showing what went wrong) so a headless run always leaves cleanly.
        const std::uint64_t waitMs = static_cast<std::uint64_t>(app->shotAfterSeconds * 1000);
        const bool timedOut = SDL_GetTicks() > waitMs + 30'000;  // 30 s past the wait
        shotReady = shotReady && ((inGame && !app->mapHeld && SDL_GetTicks() - app->inGameSinceMs >= waitMs) || timedOut);
        if (app->view) {
            extra = liveSprites(*app, markers, healthBars);
            app->effects->setView(app->camera);
            app->effects->setGameData(&app->gameData, gameFlags(*app));
            app->effects->update(app->interpolator.gameTime(), app->anchors, app->sprites.get());
            app->effects->sceneSprites(*app->sprites, extra);
            // Screen shake moves the view down (Cli_SetCameraCenter adds it to the centre's y).
            app->camera.y += static_cast<float>(app->effects->jiggle());
            // Map objects the server also sends (the same number, where the map has it): ours
            // isn't drawn, lit or sounded twice. Only those: the server's net codes are its own
            // series, and a mere match of numbers hid door extents 1 and 2 on FlagWar whenever a
            // drawable was numbered so.
            for (const auto& [netCode, d] : client.world().drawables()) {
                if (app->view && app->view->duplicates(netCode, static_cast<float>(d.x), static_cast<float>(d.y))) {
                    liveCodes.insert(netCode);
                }
            }
        }
        // The server made us another class than the character's (a mode's conjurers), or
        // multiclass (a mode's noxsdl players): the bars for it.
        if (app->barsClass >= 0 && (ownClass(*app) != app->barsClass || multiclass(*app) != app->barsMulticlass)) {
            loadBars(*app);
        }
        if (app->cinematics) {
            // The fades, bars and briefings' client frames (cinematics.hpp).
            // (Held: a movie playing, the map held for its HD images; the black's watch waits.)
            const bool held = app->mapHeld || (app->movie && app->movie->active());
            app->cinematics->update(SDL_GetTicks(), inGame, app->viewHeight, ownClass(*app), held);
        }
        chooseCursor(*app, inGame);
    } else {
        // The viewer runs effects on its own 30 fps clock (only --fx-demo makes any).
        const double time = static_cast<double>(SDL_GetTicks()) * 30 / 1000;
        if (app->fxDemo) {
            fxDemo(*app, static_cast<std::uint32_t>(time), extra);
        }
        app->effects->setView(app->camera);
        app->effects->setGameData(&app->gameData, 0);  // (no game: flags 0 read every line, SOLO and ARENA too, the last of a name)
        app->effects->update(time, app->anchors, app->sprites.get());
        app->effects->sceneSprites(*app->sprites, extra);
        shotReady = shotReady && SDL_GetTicks() >= static_cast<std::uint64_t>(app->shotAfterSeconds * 1000);
    }
    soundLoops(*app, liveCodes);
    heartBeat(*app);
    speech(*app);
    if (app->live && app->tradeScreens) {
        app->tradeScreens->update(app->live->client().world(), SDL_GetTicks(),
                                  gameFrame(*app),
                                  app->audio && app->audio->dialogBusy(), app->camera.viewWidth,
                                  app->camera.viewHeight);
    }

    // The map not drawn (a cinematic's black, gameDisableMapDraw): the view black.
    // Nor while it's held back for its HD images (holdMap).
    const bool worldShown = (!app->cinematics || app->cinematics->mapDrawn()) && !(app->live && app->mapHeld);
    // A white-flash frame is all white: the game clears the screen and draws nothing else of the
    // world that frame (one game frame's time, however long the game's clock stands: white_flash.hpp).
    const bool whiteFlash = app->live && app->effects->whiteFlash(SDL_GetTicks(), app->view && worldShown);
    const std::uint8_t clear = whiteFlash ? 255 : 0;
    beginFrame(*app);
    SDL_SetRenderDrawColor(app->renderer, clear, clear, clear, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(app->renderer);
    if (app->view && !whiteFlash && worldShown) {
        drawWorld(*app, extra, liveCodes);
    }
    // Drawables without art yet (players: PlayerDraw isn't decoded) as markers.
    SDL_SetRenderDrawColor(app->renderer, 255, 220, 64, SDL_ALPHA_OPAQUE);
    for (const auto& m : markers) {
        const SDL_FRect r = app->camera.toScreen(m.x - 6, m.y - 40, 12, 40);
        if (worldShown && !app->view3d.active()) { // (2D only)
            SDL_RenderFillRect(app->renderer, &r);
        }
    }
    if (!whiteFlash && worldShown && !app->view3d.active()) { // (2D only)
        drawFlagLabels(*app);
        drawHealthBars(*app, healthBars);
        drawMinionDots(*app);
        drawDamageNumbers(*app);
        if (app->renderBubbles) {
            drawChatBubbles(*app, gameFrame(*app));
        }
        if (app->live) {
            for (auto& [id, ui] : app->modeUis) {
                ui->drawWorld(*app, app->renderer); // (a mode's labels over things, under the HUD)
            }
        }
    }
    if (app->live && app->cinematics) {
        // The fades and bars under the GUI, at the world's end (the fades' draw with 0).
        app->cinematics->drawUnderGui(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->windowSize < 100) {
        // The game window at its size (VideoSize, draw_setCutSize): the world only in the middle
        // windowSize percent each way, black around it; the HUD draws over the whole screen.
        const float w = static_cast<float>(app->camera.viewWidth);
        const float h = static_cast<float>(app->camera.viewHeight);
        const SDL_FRect in = gameWindowBox(*app);
        const SDL_FRect bars[4] = {{0, 0, w, in.y}, {0, in.y + in.h, w, h - in.y - in.h}, {0, in.y, in.x, in.h},
                                   {in.x + in.w, in.y, w - in.x - in.w, in.h}};  // above, below, left, right
        SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
        SDL_RenderFillRects(app->renderer, bars, 4);
    }
    // The HUD and the game's windows, bottom to top.
    if (app->live && app->hud && app->view &&
        app->live->client().stage() == nox::client::HeadlessClient::Stage::InGame) {
        drawGameHud(*app);
    }
    if (app->live && app->chat) {
        app->chat->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->mapDownload) {
        if (const auto percent = app->live->client().downloadPercent()) {
            app->mapDownloaded = true; // (the download screen's way in: no chat map notice)
            app->mapDownload->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight, *percent);
        }
    }
    if (app->live && app->serverOptions) {
        app->serverOptions->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->motdWindow) {
        app->motdWindow->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->questWindows) {
        app->questWindows->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->quitMenu) {
        app->quitMenu->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->options) {
        app->options->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->inputConfig) {
        app->inputConfig->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    if (app->live && app->discon) {
        app->discon->draw(app->renderer, app->camera.viewWidth, app->camera.viewHeight); // (the icon; the lag window over the game's)
    }
    runConsoleScript(*app);
    if (app->console) {
        // Over everything but the cursor; its lines run as a client in a game, as the host in
        // the viewer.
        const int w = app->camera.viewWidth;
        const int h = app->camera.viewHeight;
        const bool translucent = !app->input || app->input->translucentConsole();
        app->console->setClient(app->live != nullptr && !app->host); // (hosting: the host's console)
        app->console->draw(app->renderer, w, h, Hud::scale(w, h), translucent);
    }
    if (app->live) {
        // The netstat (F10): counted every frame, drawn when shown.
        const auto& client = app->live->client();
        const auto* me = ownPlayer(client);
        const int w = app->camera.viewWidth;
        const int h = app->camera.viewHeight;
        app->netStat.draw(app->renderer, app->font.get(), w, h, Hud::scale(w, h), client.datagramsReceived(),
                          gameFrame(*app), me ? me->latency : 0);
    }
    const bool briefing = app->live && app->cinematics && app->cinematics->screenUp();
    if (briefing) {
        // A briefing or the load menu, over the windows.
        app->cinematics->drawScreens(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    app->tipItem = 0; // (liveTooltip's to set)
    if (app->options && app->options->isOpen()) {
        app->tooltip = app->options->tooltip(); // noxsdl's switches, in the menus and in a game
        drawTooltip(*app, true);
    } else if (app->live && app->renderGui && !briefing) {
        app->tooltip = liveTooltip(*app);
        // (A mode's own tooltip for the item, shown by its drawOver: the game's name gives way.)
        for (auto& [id, ui] : app->modeUis) {
            if (app->tipItem != 0 && ui->hidesItemName(*app)) {
                app->tooltip.clear();
            }
        }
        drawTooltip(*app);
        // A mode's tooltip over everything this frame: as the game's own is, never over a menu or
        // the game's windows drawn over the mode's, and not beside the game's own.
        const bool windowUp = menuWindowUp(*app) || (app->console && app->console->isOpen()) ||
                              (app->discon && (app->discon->windowShown() || app->discon->noticeShown())) ||
                              (app->motdWindow && app->motdWindow->isOpen()) ||
                              (app->questWindows && (app->questWindows->gameOverShown() || app->questWindows->keysShown())) ||
                              (app->serverOptions && app->serverOptions->isOpen());
        const bool gameTipShown = !app->tooltip.empty() && app->tooltipStill > 9; // (drawTooltip's own test)
        if (!gameTipShown && !windowUp) {
            const int w = app->camera.viewWidth;
            const int h = app->camera.viewHeight;
            const HudFrame over{.width = w, .height = h, .scale = Hud::scale(w, h), .mouse = mousePos(*app), .nowMs = SDL_GetTicks()};
            for (auto& [id, ui] : app->modeUis) {
                ui->drawOver(*app, app->renderer, over);
            }
        }
    }
    drawFps(*app);
    drawKillFeed(*app);
    drawCursor(*app, app->live && !briefing ? app->cursorType : CursorType::Select, app->camera.zoom);
    if (app->live && app->cinematics && !app->live->client().downloadPercent()) {
        // The fades over everything, the pointer too (mainloop's fades' draw with 1). Not while a
        // map downloads: the exe's download screen runs its own loop, which draws
        // only it; a fade's black over it hid the bar for the whole download.
        app->cinematics->drawOverAll(app->renderer, app->camera.viewWidth, app->camera.viewHeight);
    }
    endFrame(*app);

    // A screenshot run: the shot saved, then a game is left (the viewer just ends).
    if (shotReady) {
        app->shotTaken = true;
        const bool saved = saveScreenshot(*app);
        if (!app->live) {
            return saved ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
        }
        app->live->quit();
    }
    if (embedded) {
        return editorFrame(*app); // (the editor round it, the frame in its Map panel)
    }
    present(*app);
    ++app->frames;
    if (app->frameLimit >= 0 && app->frames >= app->frameLimit) {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

/// SDL's last callback: the bars saved if we're still in a game, then everything let go in order
/// (textures before the renderer, the renderer before the window).
void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/)
{
    auto* app = static_cast<App*>(appstate);
    if (app) {
        if (app->live) {
            saveBars(*app); // leaving the game by closing the window
        }
        characterNotesEnd(*app); // (noxcpp: labels no write took, written as the game ends: character_play.hpp)
        app->editor.reset(); // (its textures and ImGui's before the renderer)
        app->cinematics.reset();
        app->book.reset();
        app->live.reset();
        app->movie.reset(); // (its texture before the renderer goes)
        app->mapVote.reset();
        app->mapPictures.reset(); // (the map vote's pictures too)
        app->legal.reset();
        app->audio.reset(); // (writes --audio-capture's file)
        if (app->gammaTarget) {
            SDL_DestroyTexture(app->gammaTarget);
        }
        if (app->playTestGamma) {
            SDL_DestroyTexture(app->playTestGamma);
        }
        app->view.reset();
        app->sprites.reset();
        fxdraw::smooth() = {};
        fxdraw::spellFx() = {};
        app->view3d.releaseGpu(); // (its depth pass's GPU objects: on the renderer's device, gone below)
        app->bloom.reset();
        app->shaders.reset(); // before the renderer and its GPU device go
        if (app->renderer) {
            SDL_DestroyRenderer(app->renderer);
        }
        if (app->window) {
            SDL_DestroyWindow(app->window);
        }
        PortMapper::retire(std::move(app->portMapper));
        // A game still hosted, and those left in the background, end with their players told: at most
        // two seconds for their acks (the exe's host waits behind its dialog), then SERVER_CLOSE.
        LocalHost::retire(std::move(app->host));
        LocalHost::finishRetired(2000);
        LocalHost::finishReleased(); // (a mode's levels left building read app->things)
        delete app;
    }
    PortMapper::joinRetired(); // (the window gone: the router's mapping removed before we end)
}
