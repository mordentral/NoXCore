// The command line (command_line.hpp).
#include "command_line.hpp"

#include "log_output.hpp"
#include "mods_folder.hpp"

#include "noxcore/io/wol_login.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace noxsdl {

namespace {

/// A --fake-* flag's screen (fakeScreen's number), 0 for any other argument.
int fakeScreenFlag(std::string_view arg)
{
    static constexpr std::pair<std::string_view, int> kFlags[] = {
        {"--fake-shop", 1},
        {"--fake-dialog", 2},
        {"--fake-sell", 3}, // the shop, and an offer for our first item
        {"--fake-quest-keys", 7},
        {"--fake-game-over", 8},
    };
    for (const auto& [flag, screen] : kFlags) {
        if (arg == flag) {
            return screen;
        }
    }
    return 0;
}

} // namespace

StartOptions parseCommandLine(App& app, int argc, char* argv[])
{
    StartOptions opts;
    opts.profile.name = "NoXPlayer";
    // 5 x RGB: hair, skin, mustache, goatee, beard. All-zero draws a black silhouette.
    opts.profile.colors = {0x5A, 0x3A, 0x1E, 0xE0, 0xAC, 0x88, 0x5A, 0x3A, 0x1E, 0x5A, 0x3A, 0x1E, 0x5A, 0x3A, 0x1E};
    // The command line, flag by flag; next() takes a flag's value (empty past the end).
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        const auto hosting = [&]() -> HostSettings& {
            if (!opts.hostSettings) {
                opts.hostSettings.emplace();
            }
            return *opts.hostSettings;
        };
        // (outside the chain below, which is at the compiler's nesting limit)
        if (a == "--fake-dialog-text") {
            app.fakeScreen = 2; // --fake-dialog with this string id's line (tests)
            app.fakeDialogText = next();
            continue;
        }
        if (a == "--host") {
            hosting().map = next();
        } else if (a == "--host-port") {
            hosting().port = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (a == "--host-name") {
            hosting().name = next();
        } else if (a == "--host-mode") {
            const std::string_view m = next();
            hosting().mode = m == "ctf" ? 0x20u : m == "flagball" ? 0x40u : m == "kotr" ? 0x10u : m == "elimination" ? 0x400u : 0x100u;  // game flags: 0x20 CTF, 0x40 flag ball, 0x10 king of the realm, 0x400 elimination, else 0x100 arena (deathmatch)
        } else if (a == "--host-lessons") {
            hosting().lessons = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (a == "--host-time") {
            hosting().minutes = static_cast<std::uint8_t>(std::atoi(next()));
        } else if (a == "--host-teams") {
            hosting().teams = true;
        } else if (a == "--host-sysop") {
            hosting().sysopPassword = next();
        } else if (a == "--host-mod") {
            // Tests: a NoX Core mode by its id (the registry's: built in, or a mod's) hosted as
            // Host Custom hosts it, its settings their defaults, and joined as any client.
            hosting().modeId = next();
        } else if (a == "--host-map-over-mode") {
            // (A map editor session's private test of its mod on the session's map) --host's map
            // hosted even when the mode names one of its own.
            hosting().mapOverMode = true;
        } else if (a == "--mods-dir") {
            // (A map editor session's private test of its mod) the mods read from DIR, the
            // editor's own copy of the session's mod, instead of mods\ beside noxcore.exe: taken
            // before the mods load (main.cpp's SDL_AppInit), so only consumed here.
            next();
        } else if (a == "--host-delve") {
            // Tests: that built-in mode hosted as Host Custom's (its generated lobby), joined as any client.
            hosting().modeId = "delve";
        } else if (a == "--host-quest") {
            // Tests: Host Quest's game (So_Druid, OTQuest.rul at the first ready), as the menu's.
            hosting().quest = true;
            opts.hostSettings->map = "So_Druid";
            app.hostQuestRul = true;
        } else if (a == "--host-coop") {
            // Tests: Host Custom > Co-op Campaign's new campaign of --class's chain (its first map,
            // a new character; its saves the co-op campaigns'), hosted and joined.
            hosting().solo = true;
            app.hostSoloScreensTest = "coop";
            if (opts.hostSettings->map == "Estate") {
                opts.hostSettings->map.clear();
            }
        } else if (a == "--frame-stats") {
            app.frameStats = true;
        } else if (a == "--host-bots") {
            hosting();
            opts.hostBots = std::clamp(std::atoi(next()), 0, 31);
        } else if (a == "--solo") {
            // The campaign: a solo game of our class's first map (War01a / Wiz01a / Con01a, the
            // character screen's windowSelCharProc) hosted here and joined (--host MAP for another map).
            hosting().solo = true;
            if (opts.hostSettings->map == "Estate") {
                opts.hostSettings->map.clear();
            }
        } else if (a == "--frames") {
            app.frameLimit = std::strtol(next(), nullptr, 10);
        } else if (a == "--map") {
            opts.mapArg = next();
            opts.mapGiven = true;
        } else if (a == "--browse") {
            opts.browse = true;
        } else if (a == "--map-editor") {
            // The map editor straight away (no menus), with a map if one follows.
            app.editorDirect = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opts.editorMap = next();
            }
        } else if (a == "--editor-user-dir") {
            app.editorUserDir = fs::path(reinterpret_cast<const char8_t*>(next()));
        } else if (a == "--editor-maps-dir") {
            app.editorMapsDir = fs::path(reinterpret_cast<const char8_t*>(next()));
        } else if (a == "--editor-run") {
            app.editorRun = next();
        } else if (a == "--host-edit") {
            // A map editor session of MAP hosted here, unlisted (no router port, no XWIS post: LAN
            // and tests), on --host-port's port; the editor in front once it greets us. A stock map
            // (or one named past 8 characters) is copied under suggestedMapName's name ("oasis_ed",
            // "ed_estat") into --editor-maps-dir (else the game's maps folder).
            opts.hostEditMap = next();
        } else if (a == "--host-edit-mod") {
            // (Tests, recordings) --host-edit's session carries the mod mods\ID (as hosting with it
            // open in the Mod windows does).
            opts.hostEditMod = next();
        } else if (a == "--session-leave-after") {
            // (Tests) SECONDS from start-up: a map editor session's game, once in front (in the
            // editor's Map panel or over the window), left as its Quit menu's Leave would.
            app.sessionLeaveAtMs = static_cast<std::uint64_t>(std::strtod(next(), nullptr) * 1000);
        } else if (a == "--host-edit-menus") {
            // (Tests) --host-edit with the menus made first, behind the session's editor
            // behind the editor: closing it goes back to the main menu instead of ending the program.
            opts.hostEditFromMenus = true;
        } else if (a == "--host-loopback") {
            // (Local tests) the game hosted here (--host, --host-edit) binds 127.0.0.1 alone.
            app.hostLoopback = true;
        } else if (a == "--menu-host-port") {
            // (Local tests, recordings) a game hosted from the menus (Host Custom > a campaign mode,
            // the Map Editor's session) on this port instead of the game's own.
            app.menuHostPort = static_cast<std::uint16_t>(std::atoi(next()));
        } else if (a == "--session-editor") {
            // (Tests) a joining noxsdl client opens the session's editor as soon as it's greeted.
            app.sessionEditorAuto = true;
        } else if (a == "--pending-view") {
            // A map editor session's pending edits in the game for this run: shaded, final or hidden.
            opts.pendingViewArg = nox::mapedit::parsePendingView(next());
        } else if (a == "--private-test") {
            // A map editor session's private local test (the session's editor starts it with
            // --host): unlisted, no router port asked for, on the loopback alone.
            app.privateTestRun = true;
            app.hostLoopback = true;
        } else if (a == "--start-at") {
            // (A private test from Play From Here) our player in at X,Y once in, not at a spawn.
            float x = 0, y = 0;
            if (std::sscanf(next(), "%f,%f", &x, &y) == 2) {
                app.playTestJumpTo = SDL_FPoint{x, y};
            }
        } else if (a == "--log") {
            next(); // (the log's file: log_output.cpp's, read at start-up)
        } else if (a == "--playtest-return") {
            // (Tests) back to the editor this many seconds into a play-test, as F2 would.
            app.playTestReturnAfterMs = static_cast<std::uint64_t>(std::atof(next()) * 1000);
        } else if (a == "--menu-clicks") {
            // Test automation, comma-separated: "id" clicks a window, "~id" points at it,
            // "id=text" types into an entry field.
            const std::string list = next();
            for (std::size_t pos = 0; pos < list.size();) {
                const auto comma = list.find(',', pos);
                opts.menuClicks.push_back(list.substr(pos, comma - pos));
                pos = comma == std::string::npos ? list.size() : comma + 1;
            }
        } else if (a == "--mouse") {
            // Tests: the mouse at X Y (window pixels) whatever the real one does.
            const float mx = std::strtof(next(), nullptr);
            const float my = std::strtof(next(), nullptr);
            app.testMouse = SDL_FPoint{mx, my};
        } else if (a == "--award-flight") {
            app.forceAwardFlight = true;
        } else if (const int fake = fakeScreenFlag(a); fake != 0) {
            app.fakeScreen = fake;
        } else if (isModeFakeFlag(a)) {
            app.fakeScreen = App::kFakeModeScreen; // (a mode's own: mode_ui.hpp)
            app.fakeModeFlag = std::string(a);
        } else if (a == "--no-motd") {
            app.noMotd = true;
        } else if (a == "--trace-self") {
            app.traceSelf = true;
#ifndef NOX_NO_TEST_COMMANDS
        } else if (a == "--trace-fade") {
            app.traceFade = true;
#endif
        } else if (a == "--pickup-test") {
            app.pickupTest = true;
            app.autoSpawn = true;
        } else if (a == "--gpu") {
            opts.useGpu = true;
        } else if (a == "--no-gpu") {
            opts.useGpu = false;
        } else if (a == "--stock-effects") {
            opts.stockEffects = true;
        } else if (a == "--no-bloom") {
            opts.noBloom = true;
        } else if (a == "--no-smooth-motion") {
            opts.noSmoothMotion = true; // the newest snapshot as it is, as vanilla draws
        } else if (a == "--predict-movement" || a == "--no-predict-movement") {
            opts.predictMovement = a == "--predict-movement"; // our own movement predicted (an experiment)
        } else if (a == "--smoothing") {
            const std::string_view v = next(); // Options' Smoothing for this run
            opts.smoothing = v == "network" ? 2 : v == "movement" ? 1 : v == "lock30" ? 3 : 0;
        } else if (a == "--display") {
            const std::string_view v = next(); // Options' Display for this run
            opts.display = v == "fullscreen" ? 2 : v == "borderless" ? 1 : 0;
        } else if (a == "--vsync" || a == "--no-vsync") {
            opts.vsync = a == "--vsync"; // Options' VSync for this run
        } else if (a == "--no-smooth-scrolling") {
            opts.noSmoothScrolling = true; // the map scrolls in the server's steps, as vanilla
        } else if (a == "--other-units") {
            const std::string_view v = next(); // how others are drawn with Smooth Motion (an experiment)
            opts.otherUnits = v == "ahead" ? 2 : v == "newest" ? 1 : 0;
        } else if (a == "--no-audio") {
            opts.noAudio = true; // the game's -noaudio
        } else if (a == "--audio-capture") {
            opts.audioCapture = next(); // record what's played to a WAV file
        } else if (a == "--audio-log") {
            opts.audioLog = true;
        } else if (a == "--grid-light") {
            opts.gridLight = true;
        } else if (a == "--video-mode") {
            opts.videoMode = parseSize(next());
        } else if (a == "--video-size") {
            opts.videoSize = std::atoi(next());
        } else if (a == "--no-fps") {
            app.showFps = false;
        } else if (a == "--show-map") {
            app.openMap = true;
#ifndef NOX_NO_TEST_COMMANDS
        } else if (a == "--ambient") {
            // (Not in release builds, nor --no-light and --trace-fade: they'd show a game's dark
            // places, or log what's out of sight.)
            Rgb c{};
            for (auto& v : c) {
                v = static_cast<std::uint8_t>(std::clamp(std::atoi(next()), 0, 255));
            }
            app.ambient = c;
#endif
        } else if (a == "--bloom") {
            opts.bloomStrength = std::clamp(std::strtof(next(), nullptr), 0.0f, 1.0f); // how much halo, 0-1
            opts.noBloom = opts.bloomStrength <= 0;
        } else if (a == "--gpu-driver") {
            opts.useGpu = true;
            opts.gpuDriver = next();
        } else if (a == "--open-inventory") {
            app.openInventory = true;
        } else if (a == "--open-book") {
            // the book once in the game; with a number, turned to that entry's page
            app.openBook = i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))
                                ? std::atoi(next())
                                : -1;
        } else if (a == "--mouse-script") {
            std::string script = next();
            std::replace(script.begin(), script.end(), ';', '\n');
            std::istringstream lines(script);
            for (std::string line; std::getline(lines, line);) {
                std::istringstream in(line);
                App::ScriptedMouse m;
                std::string what;
                if (in >> m.atMs >> what >> m.x >> m.y) {
                    // Actions: 0 move, 1 down, 2 up, 3 / 4 the right button down / up, 5 Escape, 7 goto, 8 a key.
                    m.action = what == "down" ? 1 : what == "up" ? 2 : what == "rdown" ? 3 : what == "rup" ? 4 : what == "esc" ? 5 : what == "goto" ? 7
                             : what == "key" ? 8 // (x: an SDL keycode, pressed and let go)
                                             : 0;
                    app.mouseScript.push_back(m);
                }
            }
        } else if (a == "--3d") {
            app.view3d.setEnabled(true); // the 3D view from the start (tests)
        } else if (a == "--3d-look") {
            const float yaw = std::strtof(next(), nullptr); // its look (tests): degrees, 0 east, 90 south
            app.view3d.setLook(yaw, std::strtof(next(), nullptr));
        } else if (a == "--drop-equipped") {
            app.dropEquipped = true;
        } else if (a == "--image-pack") {
            app.imagePack = next(); // the HD image pack to use instead of the one found
        } else if (a == "--hd-images") {
            opts.hdImages = true; // Use HD Images on for this run
        } else if (a == "--hd-decode-sync") {
            app.hdDecodeSync = true; // (HD images decoded as they're first drawn: no workers, no map preload)
        } else if (a == "--hd-placeholders") {
            app.hdPlaceholders = true; // (the originals drawn till the workers' HD images are in, the map not held)
        } else if (a == "--all-spell-sets") {
            app.expandBarsTest = true;
        } else if (a == "--award-test") {
            app.awardTest = static_cast<std::uint32_t>(std::atoi(next()));
        } else if (a == "--plr") {
            app.characterFile = next(); // --connect: the character whose spell bars to use and save
        } else if (a == "--quit-menu") {
            app.openQuitMenu = true; // tests: the in-game menu, as if Escape were pressed
        } else if (a == "--server-options") {
            app.openServerOptions = true; // tests: the host's Server Settings, as Gameplay Controls opens them
        } else if (a == "--server-options-page") {
            app.openServerOptions = true; // tests: likewise, on page N (0 Game, 1 General, 2 Access, 3 Teams, 4 Bots)
            app.serverOptionsPage = std::atoi(next());
        } else if (a == "--ask-password") {
            app.askPasswordTest = next(); // tests (with --browse): the join's password asked for HOST over the game list
        } else if (a == "--save-dir") {
            opts.saveDir = next(); // characters listed from and made in here instead of <game>\Save; a solo game's saves too
        } else if (a == "--server") {
            opts.browserOptions.servers.emplace_back(next());
        } else if (a == "--no-xwis") {
            opts.browserOptions.xwis = false;
        } else if (a == "--no-lan") {
            opts.browserOptions.lan = false; // (tests: the game list from --server alone, no LAN broadcast)
        } else if (isConsoleFlag(a)) {
            // -cmd / --cmd: the log in a console as well (Windows; startLog has done it)
        } else if (a == "--game-dir") {
            app.gameDir = next();
            opts.gameDirGiven = true;
        } else if (a == "--zoom") {
            app.camera.zoom = std::strtof(next(), nullptr);
        } else if (a == "--at") {
            app.camera.x = std::strtof(next(), nullptr);
            app.camera.y = std::strtof(next(), nullptr);
            opts.haveAt = true;
        } else if (a == "--size") {
            const std::string s = next();
            opts.width = std::atoi(s.c_str());
            if (const auto x = s.find('x'); x != std::string::npos) {
                opts.height = std::atoi(s.c_str() + x + 1);
            }
        } else if (a == "--no-movies") {
            opts.noMovies = true; // no movie plays (as the exe's -safe, engine flag 0x200); the start-up goes on
        } else if (a == "--no-hd-movies") {
            opts.noHdMovies = true; // the VQAs even when HDVideo\ has the HD versions
        } else if (a == "--startup") {
            opts.forceStartUp = true; // the start-up's movies and legal screen even with --screenshot / --mouse-script (tests)
        } else if (a == "--movie") {
            opts.movieArg = next(); // play movies\NAME (or its HD version) first, then the main menu (tests)
        } else if (a == "--hd-dir") {
            opts.hdDirArg = next(); // where the HD movies and FFmpeg's DLLs are (default: HDVideo\ beside noxcore.exe)
        } else if (a == "--screenshot") {
            app.screenshot = next();
#ifndef NOX_NO_TEST_COMMANDS
        } else if (a == "--no-light") {
            app.lighting = false;
#endif
        } else if (a == "--sight") {
            app.lineOfSight = true; // line of sight from the camera centre, in the viewer too
        } else if (a == "--shot-after") {
            app.shotAfterSeconds = std::strtof(next(), nullptr);
        } else if (a == "--connect") {
            opts.connect = next();
        } else if (a == "--name") {
            opts.profile.name = next();
        } else if (a == "--account") {
            opts.profile.account = next(); // (else the saved online login's: below)
        } else if (a == "--class") {
            const std::string_view c = next();
            opts.profile.playerClass = c == "wizard" ? 1 : c == "conjurer" ? 2 : 0;  // 0 warrior, 1 wizard, 2 conjurer
        } else if (a == "--quest") {
            opts.profile.quest = true; // (--connect to a quest game: joined as a quest client)
        } else if (a == "--fx-demo") {
            app.fxDemo = true;
        } else if (a == "--fx-demo-only") {
            app.fxDemo = true;
            app.fxDemoOnly = next(); // bursts, beams, particles, objects, enchant, rain, chain, undead, summon or shields
        } else if (a == "--spawn") {
            app.autoSpawn = true;
        } else if (a == "--console") {
            app.consoleRun = next();
        } else if (a == "--chat-test") {
            app.chatTest = next();
            app.autoSpawn = true;
        } else if (a == "--hold-move") {
            app.holdMoveTest = true; // tests: walking or running toward the cursor (--mouse)
            app.autoSpawn = true;
        } else if (a == "--cast-test") {
            app.castTest = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10)); // e.g. 10 cleansing flame
            app.autoSpawn = true;
        } else if (a == "--goto") {
            const int gx = std::atoi(next());
            const int gy = std::atoi(next());
            app.autoGoto = SDL_Point{gx, gy};
            app.autoSpawn = true;
        } else if (a == "--team") {
            app.joinTeam = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        } else if (a == "--map-cache") {
            opts.mapCache = next();
        } else if (a == "--map-picture") {
            // --map-picture MAP OUT.png: the map vote's picture of a map, written out (no window).
            opts.mapPicture = next();
            opts.mapPictureOut = next();
        } else if (a == "--hair" || a == "--skin") {
            // RRGGBB; hair also colours the facial hair.
            const auto rgb = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 16));
            const std::array<std::uint8_t, 3> c{static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
                                                static_cast<std::uint8_t>(rgb)};
            // Skin is colour slot 1; hair slots 0, 2, 3 and 4 (the hair, mustache, goatee and beard).
            // (Static arrays: a conditional between two initializer_lists leaves the loop on arrays
            // already gone.)
            static constexpr std::size_t kSkinSlots[] = {1};
            static constexpr std::size_t kHairSlots[] = {0, 2, 3, 4};
            for (std::size_t slot : a == "--skin" ? std::span<const std::size_t>(kSkinSlots) : std::span<const std::size_t>(kHairSlots)) {
                std::copy(c.begin(), c.end(), opts.profile.colors.begin() + static_cast<std::ptrdiff_t>(slot * 3));
            }
        }
    }

    // The account: --account, else the player's saved online login (the game's Westwood Online
    // login screen keeps it), else none, as the game joins when not logged in.
    if (opts.profile.account.empty()) {
        if (const auto nicks = nox::io::savedOnlineNicks(); !nicks.empty()) {
            opts.profile.account = nicks.front();
        }
    }
    SDL_Log("account: %s", opts.profile.account.empty() ? "(none: no saved online login)" : opts.profile.account.c_str());
    return opts;
}

} // namespace noxsdl
