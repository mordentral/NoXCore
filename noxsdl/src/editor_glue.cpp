// The map editor's glue (main.cpp's frame and events call it): the editor opened over everything,
// the play-test (the working copy hosted here and joined, in the editor's Map panel or over the
// whole window), the collaborative map editor session (hosted or joined, the editor attached to its
// client side), a session's private test in a second noxcore, and the editor's own frame and close.

#include "app.hpp"
#include "editor/close_plan.hpp"
#include "editor/editor_settings.hpp"
#include "editor/editor_text.hpp"
#include "mods_folder.hpp"
#include "new_game.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/modes/builtin.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace noxsdl {

namespace {

using editor::pathText;

std::string startPlayTest(App& app, const editor::PlayTestRequest& req);
void restorePlayTestGame(App& app);
void pausePlayTest(App& app, bool on);
std::string startPrivateTest(App& app, const editor::PlayTestRequest& req);
void attachSessionEditor(App& app);

} // namespace

/// The game drawn in the editor's Map panel: an in-panel play-test, or the session's game.
bool gameInPanel(const App& app)
{
    return app.editor && app.live && (app.playTestEmbedded || app.sessionEmbedded);
}

/// Opens the map editor over everything (the mode lists' Map Editor, --map-editor), with `map`
/// if given (a name in the maps folder, or a path), else its start dialog.
void openEditor(App& app, const std::string& map)
{
    MapEditor::Context c;
    c.window = app.window;
    c.renderer = app.renderer;
    c.things = &app.things;
    c.art = app.art.get();
    c.modifiers = &app.modifiers;
    c.strings = app.strings.get();
    c.sprites = app.sprites.get();
    c.gameData = &app.gameData;
    c.gameDir = app.gameDir;
    c.imagePack = app.sprites && app.sprites->usingPack() ? app.imagePack : fs::path();
    c.userDir = app.editorUserDir;
    const fs::path mapsDir = !app.editorMapsDir.empty() ? app.editorMapsDir : app.gameDir / "maps";
    c.mapsDir = mapsDir;
    c.prefDir = editor::editorPrefDir();
    c.screenshotRun = !app.screenshot.empty();
    // The play-test: the map hosted here (as Game Modes': unlisted, no router port, no XWIS post).
    c.playTestDir = !app.editorMapsDir.empty() ? app.editorMapsDir / "_playtest" : c.prefDir / "playtest";
    c.playTest.start = [&app](const editor::PlayTestRequest& r) { return startPlayTest(app, r); };
    c.playTest.resume = [&app] {
        if (app.sessionEmbedded && app.live) {
            capturePlayTest(app, true); // (the session's game in the panel: the input to it)
            return;
        }
        if (app.playTest && app.live) {
            if (app.playTestEmbedded) {
                capturePlayTest(app, true); // (in the Map panel: the keyboard and mouse to it)
                return;
            }
            app.playTestInGame = true;
            gameTakesInput(app); // (back over the window: nothing held from the editor)
            SDL_StopTextInput(app.window);
            SDL_HideCursor();
        }
    };
    c.playTest.view = [&app] {
        editor::PlayTestView v;
        if (app.sessionEmbedded && app.live) {
            // The session's game in the Map panel (the session bar's Play).
            v.session = true;
            v.captured = app.playTestCaptured;
            v.texture = app.playTestFrame;
            v.area = app.gameArea;
            return v;
        }
        if (app.playTest && app.live) {
            v.fullWindow = !app.playTestEmbedded;
            v.captured = app.playTestEmbedded ? app.playTestCaptured : app.playTestInGame;
            v.paused = app.playTestPaused;
            if (app.playTestEmbedded) {
                v.texture = app.playTestFrame; // (the frame drawn into gammaTarget: beginFrame / endFrame)
                v.area = app.gameArea;
            }
        }
        return v;
    };
    c.playTest.capture = [&app](bool on) {
        if (app.sessionEmbedded && app.live) {
            capturePlayTest(app, on);
            return;
        }
        if (!app.playTest || !app.live) {
            return;
        }
        if (app.playTestEmbedded) {
            capturePlayTest(app, on);
        } else if (!on && app.playTestInGame) {
            playTestToEditor(app);
        }
    };
    c.playTest.pause = [&app](bool on) { pausePlayTest(app, on); };
    c.playTest.jumpTo = [&app](SDL_FPoint at) {
        if (app.playTest && app.host) {
            app.playTestJumpTo = at; // (playTestJump: once our player is in)
            playTestJump(app);
        }
    };
    c.playTest.gameWorldAt = [&app](float x, float y) -> std::optional<SDL_FPoint> {
        // The in-panel game's frame (gameArea) as its camera draws the world.
        if (!(app.playTest || app.sessionEmbedded) || !app.live || !(app.playTestEmbedded || app.sessionEmbedded) || app.gameArea.w < 1 ||
            x < app.gameArea.x || y < app.gameArea.y ||
            x >= app.gameArea.x + app.gameArea.w || y >= app.gameArea.y + app.gameArea.h || app.camera.zoom <= 0) {
            return std::nullopt;
        }
        return SDL_FPoint{app.camera.left() + (x - app.gameArea.x) / app.camera.zoom, app.camera.top() + (y - app.gameArea.y) / app.camera.zoom};
    };
    c.playTest.stop = [&app] {
        if (app.sessionEmbedded) {
            sessionToEditor(app); // (the session's game: back to editing, never ended from here)
            return;
        }
        if (app.playTest) {
            endPlayTest(app);
        }
    };
    c.playTest.running = [&app] { return (app.playTest && app.host != nullptr) || (app.sessionEmbedded && app.live != nullptr); };
    c.playTest.startPrivate = [&app](const editor::PlayTestRequest& r) { return startPrivateTest(app, r); };
    c.playTest.withGame = [&app](const std::function<void(nox::server::GameHost&)>& fn) {
        if (app.host) {
            app.host->withGame([&](nox::server::GameHost& g) { fn(g); });
        }
    };
    c.playTest.modeCommand = [&app](const std::string& line, std::function<void(std::vector<std::string>)> answered) {
        if (app.host && app.playTest) {
            app.host->modeCommand(line, std::move(answered));
        }
    };
    c.modsDir = modsFolderPath(); // (mod projects: the folder the game loads mods from)
    c.playTest.notify = [&app](const std::string& line) {
        if (app.console) {
            app.console->print(line, Yellow);
        }
    };
    c.hostSession = [&app] { showEditSessionSetup(app); };
    app.editor = std::make_unique<MapEditor>(std::move(c));
    SDL_Log("map editor: open (maps in %s)", mapsDir.string().c_str());
    editorMouse(app);
    SDL_Log("map editor: pointer %s, mouse %s", SDL_CursorVisible() ? "shown" : "hidden",
            SDL_GetWindowRelativeMouseMode(app.window) ? "relative" : "absolute");
    if (!map.empty()) {
        // A path, or a map's name: in the editor's maps folder, else the game's.
        fs::path p(reinterpret_cast<const char8_t*>(map.c_str()));
        if (!p.has_extension()) {
            const fs::path mine = nox::resolveNoCase(mapsDir / p / (map + ".map"));
            p = fs::exists(mine) ? mine : nox::resolveNoCase(app.gameDir / "maps" / p / (map + ".map"));
        }
        app.editor->open(p);
    } else if (app.screenshot.empty()) {
        app.editor->showStart();
    }
    if (!app.editorRun.empty()) {
        app.editor->runCommands(app.editorRun);
        app.editorRun.clear();
    }
}

/// The map editor's mouse: the system's pointer, moving as the desktop's. The menus and a game
/// (a play-test's too) may run in relative mode (applyMouse), and SDL hides the pointer whatever
/// SDL_ShowCursor says while that's on, so it goes off here, every editor frame (the editor
/// opened from the menus, back from a play-test's game, opened again); the next frame that isn't
/// the editor's sets the mode again (applyMouse, mouseMode Unknown). The game draws its own pointer
/// outside the editor (SDL_HideCursor where the editor hands over).
void editorMouse(App& app)
{
    if (!app.window) {
        return;
    }
    const bool relative = SDL_GetWindowRelativeMouseMode(app.window);
    const bool visible = SDL_CursorVisible();
    app.mouseMode = MouseMode::Unknown; // (set again in the menus or a game: applyMouse)
    if (relative) {
        if (!SDL_SetWindowRelativeMouseMode(app.window, false)) {
            SDL_Log("mouse: relative mode off: %s", SDL_GetError());
        }
    }
    if (!visible) {
        SDL_ShowCursor();
    }
    if (relative || !visible) {
        SDL_Log("mouse: map editor, system pointer (was %s, %s; now %s, %s)", relative ? "relative" : "absolute",
                visible ? "shown" : "hidden", SDL_GetWindowRelativeMouseMode(app.window) ? "relative" : "absolute",
                SDL_CursorVisible() ? "shown" : "hidden");
    }
}

namespace {

/// The map editor's play-test (editor/play_test.hpp): the working copy hosted here in its game
/// mode on a free port, as Game Modes hosts (unlisted: no router port, no XWIS post), the sandbox's
/// players-can't-die unless asked, and joined; the editor waits behind it (F2 comes back). The
/// client finds the map in the test's folder (its map cache for the test).
std::string startPlayTest(App& app, const editor::PlayTestRequest& req)
{
    if (app.host || app.live) {
        return "a game is already running here";
    }
    HostSettings s;
    s.map = req.map.string();
    s.port = 0; // (any free port: never the usual one, so nothing else here is disturbed)
    s.loopbackOnly = true; // (nothing outside this computer reaches a test)
    s.name = "Play-test";
    s.mode = req.mode;
    s.mapCycle = false;
    s.mapVote = false;
    s.sendMotd = false;
    s.updateRate = app.serverUpdateRate;
    s.testCommands = true;
    s.playersCantDie = !req.playersCanDie;
    // The test's folder is the maps folder its game loads from (a quest's map, an exit's).
    s.mapsDir = req.cacheDir;
    if (!req.modeId.empty()) {
        // A mod project's play-test: its mode by its id, the mods folder read again first so the
        // project's mode.json as saved counts (between games: nothing is hosted here now).
        refreshMods();
        if (!nox::modes::registry().find(req.modeId)) {
            for (const auto& b : modsReport().broken) {
                if (b.folder.filename().string() == req.modeId) {
                    return "the mod " + req.modeId + " doesn't load: " + b.why;
                }
            }
            return "no mod " + req.modeId + " in " + modsFolderPath().string();
        }
        s.modeId = req.modeId;
        if (req.modeMap) {
            s.mapsDir.clear(); // (the mode's own map: the game's maps folder, where it names one)
        } else {
            s.mapOverMode = true; // (the editor's working copy, whatever the mode names)
        }
    }
    // What the test changes for its game, put back at its end (endPlayTest).
    app.playTestRestore = true;
    app.playTestSavedProfile = app.profile;
    app.playTestSavedRoot = app.saveRoot;
    app.playTestSavedCharacter = app.characterFile;
    app.playTestSavedHeld = app.heldFileInfo;
    app.playTestSavedHostQuestRul = app.hostQuestRul;
    if (req.playerClass >= 0 && req.playerClass <= 2) {
        app.profile.playerClass = static_cast<std::uint8_t>(req.playerClass);
    }
    if (req.kind != editor::PlayKind::Network) {
        // A solo or quest test's saves (the solo game's AUTOSAVE and saves, a quest's saves sent
        // its client) and its new character go in the test's own folder, never the player's.
        const fs::path root = req.cacheDir / "save_root";
        std::error_code ec;
        fs::create_directories(root / "Save", ec);
        app.saveRoot = root;
        newSoloCharacter(app, app.profile, req.map.stem().string());
        s.saveRoot = root;
        s.characterFile = app.characterFile;
    }
    if (req.kind == editor::PlayKind::Solo) {
        // As the main menu's Solo hosts the campaign (a new character: its file loaded onto our
        // player as it's made).
        s.solo = true;
    } else if (req.kind == editor::PlayKind::Quest) {
        // As Solo Quest hosts one: the chat map So_Druid, OTQuest.rul at our first ready
        // (questStartRul) begins the quest, whose maps are the test's alone (as a mode's may be
        // its generated one).
        s.quest = true;
        s.map = nox::resolveNoCase(app.gameDir / "maps" / "So_Druid" / "So_Druid.map").string();
        s.questMapsOnly = {req.map.stem().string()};
        app.hostQuestRul = true;
    }
    try {
        app.host = makeHost(app, s);
    } catch (const std::exception& ex) {
        restorePlayTestGame(app);
        return ex.what();
    }
    app.playTestJumpTo = req.startAt;
    app.playTestQuest = req.kind == editor::PlayKind::Quest;
    app.playTestMap = req.map.stem().string();
    app.playTestSavedCache = app.mapCache;
    app.mapCache = req.cacheDir;
    app.online = false;
    app.xwisHost.reset();
    startLive(app, app.host->address());
    app.playTest = true;
    app.playTestEmbedded = !req.fullWindow;
    app.playTestInGame = req.fullWindow;
    app.playTestCaptured = false;
    app.playTestPaused = false;
    app.playTestMouse.reset();
    app.playTestFrame = nullptr; // (none till its first frame)
    app.playTestStartedMs = SDL_GetTicks();
    if (app.playTestEmbedded) {
        capturePlayTest(app, true); // (as Unreal's play-in-editor: the game has the input from the start)
    } else {
        SDL_HideCursor();
    }
    SDL_Log("play-test: hosting %s at %s (%s 0x%x, class %d%s), %s", s.map.c_str(), app.host->address().c_str(),
            s.solo ? "solo" : s.quest ? "quest" : "network mode", s.mode, static_cast<int>(app.profile.playerClass),
            s.playersCantDie ? ", players can't die" : "", req.fullWindow ? "full window" : "in the editor's Map panel");
    if (req.startAt) {
        SDL_Log("play-test: the player to %.0f, %.0f once in", req.startAt->x, req.startAt->y);
    }
    return {};
}

/// What a test changed for its game put back (its start's failure, its end).
void restorePlayTestGame(App& app)
{
    if (!std::exchange(app.playTestRestore, false)) {
        return;
    }
    app.profile = app.playTestSavedProfile;
    app.saveRoot = app.playTestSavedRoot;
    app.characterFile = app.playTestSavedCharacter;
    app.heldFileInfo = app.playTestSavedHeld;
    app.hostQuestRul = app.playTestSavedHostQuestRul;
    app.playTestJumpTo.reset();
    app.playTestPlacing.reset();
}

} // namespace

/// Jump in at the cursor: the test's player put at the point asked for once it's in the game (our
/// own player: the host's peer's, or a solo game's host player), as the test commands' "goto" puts
/// one (setPosition). The editor's tooling only: no ordinary game asks for it.
void playTestJump(App& app)
{
    if (app.playTestJumpTo) {
        // A new point asked for (the test's start, or Play From Here while it runs): it replaces
        // any placing still under way.
        const SDL_FPoint at = *std::exchange(app.playTestJumpTo, std::nullopt);
        app.playTestPlacing.emplace(at.x, at.y, app.playTestMap, app.playTestQuest);
    }
    if (!app.playTestPlacing || !app.host || !app.live || app.live->client().stage() != nox::client::HeadlessClient::Stage::InGame) {
        return;
    }
    bool placedNow = false;
    const bool done = app.host->withGame([&](nox::server::GameHost& g) {
        const bool before = app.playTestPlacing->placed();
        const bool finished = app.playTestPlacing->tick(g);
        placedNow = !before && app.playTestPlacing->placed();
        return finished;
    });
    if (placedNow) {
        const auto [x, y] = app.playTestPlacing->spot();
        SDL_Log("play-test: the player put at %.0f, %.0f", x, y);
    }
    if (done) {
        app.playTestPlacing.reset();
    }
}

/// A play-test in the editor's Map panel: the keyboard and mouse to its game (the system's pointer
/// hidden, the game's drawn, relative mode as a game's: applyMouse; the pointer kept on the game's
/// area: playTestLayout) or back to the editor (the game's mouse left where it was, what was held
/// let go). Never the desktop's mouse in a hidden run.
void capturePlayTest(App& app, bool on)
{
    if (on == app.playTestCaptured) {
        return;
    }
    app.playTestCaptured = on;
    if (app.input) {
        app.input->releaseAll();
    }
    if (on) {
        gameTakesInput(app); // (new_game.hpp: nothing the editor's presses began reaches the game)
        app.playTestMouse.reset();
        SDL_StopTextInput(app.window);
        SDL_HideCursor();
    } else {
        // Whatever a press began waits for its release, which goes to the editor now (as at a focus loss).
        app.armed.reset();
        app.barDrag.reset();
        if (app.book) {
            app.book->cancelDrag();
        }
        SDL_FPoint m{};
        SDL_GetMouseState(&m.x, &m.y);
        app.playTestMouse = m;
        if (app.playTestMouseRect.w > 0) {
            app.playTestMouseRect = {};
            SDL_SetWindowMouseRect(app.window, nullptr);
        }
        editorMouse(app); // (relative mode off, the system's pointer back)
        SDL_StartTextInput(app.window);
    }
    SDL_Log("play-test: the keyboard and mouse to the %s", on ? "game" : "editor");
}

namespace {

/// The play-test's world stood still or let go on: the pause flag (GameFlag::Paused) a solo game's
/// pause sets, set here on the test's own server (setPaused is a solo game's only); the server
/// still sends, so its client stays with it.
void pausePlayTest(App& app, bool on)
{
    if (!app.playTest || !app.host) {
        return;
    }
    const std::uint32_t frame = app.host->withGame([on](nox::server::GameHost& g) {
        auto& world = g.server().world();
        if (on) {
            world.gameFlags |= nox::server::GameFlag::Paused;
        } else {
            world.gameFlags &= ~nox::server::GameFlag::Paused;
        }
        return g.server().clock().frame();
    });
    app.playTestPaused = on;
    SDL_Log("play-test: %s at frame %u", on ? "paused" : "resumed", frame);
}

} // namespace

/// An in-panel play-test's view: the game's area the largest of its resolution's shape in the
/// editor's Map panel (as a letterboxed window's: the game's view, and so what the server was told,
/// stays its VideoMode's), its frame drawn into its own texture (beginFrame) for the panel; while
/// the game has the mouse the pointer is kept on its area (never in a hidden run).
void playTestLayout(App& app)
{
    SDL_FRect panel = app.editor->playViewport();
    if (panel.w < 16 || panel.h < 16) {
        // (The panel not shown, behind another tab: drawn small, unseen.)
        panel = SDL_FRect{0, 0, static_cast<float>(app.viewWidth) / 2, static_cast<float>(app.viewHeight) / 2};
    }
    const float vw = static_cast<float>(std::max(app.viewWidth, 1));
    const float vh = static_cast<float>(std::max(app.viewHeight, 1));
    const float fit = std::min(panel.w / vw, panel.h / vh);
    const float w = std::clamp(std::round(vw * fit), 16.0f, panel.w);
    const float h = std::clamp(std::round(vh * fit), 16.0f, panel.h);
    app.gameArea = SDL_FRect{std::floor(panel.x + (panel.w - w) / 2), std::floor(panel.y + (panel.h - h) / 2), w, h};
    app.letterboxed = true;
    app.camera.viewWidth = static_cast<int>(w);
    app.camera.viewHeight = static_cast<int>(h);
    SDL_Rect r{};
    if (app.playTestCaptured && app.screenshot.empty()) {
        r = SDL_Rect{static_cast<int>(app.gameArea.x), static_cast<int>(app.gameArea.y), static_cast<int>(w), static_cast<int>(h)};
    }
    if (r.x != app.playTestMouseRect.x || r.y != app.playTestMouseRect.y || r.w != app.playTestMouseRect.w ||
        r.h != app.playTestMouseRect.h) {
        app.playTestMouseRect = r;
        if (r.w > 0) {
            // Onto the area first if it's elsewhere (Play on the toolbar): SDL keeps a pointer it
            // finds past the right or bottom edge where it is.
            float x = 0, y = 0;
            SDL_GetMouseState(&x, &y);
            if (x < static_cast<float>(r.x) || y < static_cast<float>(r.y) || x >= static_cast<float>(r.x + r.w) ||
                y >= static_cast<float>(r.y + r.h)) {
                SDL_WarpMouseInWindow(app.window, static_cast<float>(r.x) + static_cast<float>(r.w) / 2,
                                      static_cast<float>(r.y) + static_cast<float>(r.h) / 2);
            }
        }
        SDL_SetWindowMouseRect(app.window, r.w > 0 ? &r : nullptr);
    }
}

/// The play-test over: its session and host gone (as a game's end), back to the editor.
void endPlayTest(App& app)
{
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
    app.localBots.squad.reset();
    app.localBots.wanted = {};
    app.book.reset();
    app.cinematics.reset();
    unbindModeClients(app); // (the modes' UIs let go of their client sides, before the client that has them goes)
    app.live.reset();
    app.view.reset();
    if (app.sprites) {
        app.sprites->beginMap({});
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
    gui::setShellUp(true);
    app.xwisHost.reset(); // (an edit session we listed off XWIS's list with it: PART, QUIT)
    if (app.sessionHosting) {
        PortMapper::retire(std::move(app.portMapper)); // (a listed session's router port, removed off this thread)
    }
    LocalHost::retire(std::move(app.host)); // (a session's players told it ended: MSG_SERVER_QUIT, SERVER_CLOSE)
    if (app.playTest || app.sessionHosting) {
        app.mapCache = app.playTestSavedCache; // (the cache the test or our edit session swapped in)
    }
    restorePlayTestGame(app); // (the test's profile, save folder and character put back)
    app.playTest = false;
    app.sessionHosting = false;
    app.sessionInGame = false;
    app.sessionEmbedded = false;
    app.playTestInGame = false;
    app.playTestEmbedded = false;
    app.playTestCaptured = false;
    app.playTestPaused = false;
    app.playTestMouse.reset();
    app.playTestFrame = nullptr;
    if (app.playTestMouseRect.w > 0) {
        app.playTestMouseRect = {};
        SDL_SetWindowMouseRect(app.window, nullptr);
    }
    if (app.input) {
        app.input->releaseAll();
    }
    SDL_ShowCursor();
    SDL_StartTextInput(app.window);
    SDL_Log("play-test: ended");
}

/// From the play-test's game back to the editor (F2): the game runs on behind it.
void playTestToEditor(App& app)
{
    app.playTestInGame = false;
    if (app.input) {
        app.input->releaseAll();
    }
    SDL_ShowCursor();
    SDL_StartTextInput(app.window);
}

// --- The collaborative map editor session ----------------------------------------------------------

/// Host Custom > Map Editor, or the editor's File > Host a Session...: its session dialog (the map,
/// who may edit, the sandbox...), saying where Host lists it (online only with an XWIS login); Host
/// hosts the session listed and joins it (--host-loopback: unlisted, the loopback alone;
/// --menu-host-port: on that port, not the game's). `closeOnCancel` (Host Custom's): Cancel closes
/// the editor too, back to the game list.
void showEditSessionSetup(App& app, bool closeOnCancel)
{
    if (!app.editor) {
        return;
    }
    const bool listed = !app.hostLoopback;
    const std::uint16_t port = app.menuHostPort != 0 ? app.menuHostPort : 18590;
    editor::SessionListingNote note;
    note.text = hostListingNote(menusHostListing(app, listed), &note.warning);
    app.editor->setSessionListingNote(std::move(note));
    std::function<void()> cancelled;
    if (closeOnCancel) {
        cancelled = [&app] {
            // (This runs inside the editor's own frame: destroying the editor here crashed. The
            // main loop closes it after the frame and shows the game list.)
            if (app.editor && !app.editor->inSession() && !app.host) {
                app.editor->requestClose();
            }
        };
    }
    app.editor->showSessionSetup(
        [&app, port, listed](const editor::SessionSetup& setup) {
            if (const std::string error = hostEditSession(app, setup, port, listed); !error.empty()) {
                SDL_Log("edit session: not hosted: %s", error.c_str());
                if (app.console) app.console->print("The edit session wasn't hosted: " + error, Yellow);
            }
        },
        std::move(cancelled));
}

/// Hosts an edit session (Host Custom > Map Editor, the editor's File > Host a Session..., or
/// --host-edit): the map written
/// where the session publishes (never a stock map's name), the host with its EditSessionDirector,
/// and joined; the editor in front, attached once the session greets us. `listed`: the menus'
/// network game (the router port asked for, posted to XWIS when logged in); a test's or LAN run
/// stays unlisted. An error text, empty on success.
std::string hostEditSession(App& app, const editor::SessionSetup& setup, std::uint16_t port, bool listed)
{
    if (app.host || app.live) {
        return "a game is already running here";
    }
    const fs::path mapsDir = !app.editorMapsDir.empty() ? app.editorMapsDir : app.gameDir / "maps";
    if (const std::string problem = editor::prepareSessionMap(setup, mapsDir, app.gameDir / "maps", app.things); !problem.empty()) {
        return problem;
    }
    HostSettings s;
    hostSettingsFromConfig(app, s);
    s.port = port;
    s.loopbackOnly = app.hostLoopback && !listed;
    s.name = app.profile.name.substr(0, 15);
    s.maxPlayers = std::clamp(setup.maxPlayers, 2, 32);
    s.password = setup.password;
    s.mapCycle = false;
    s.mapVote = false;
    s.testCommands = true;
    s.wolGame = listed && app.online && app.menus && app.menus->hostLogin();
    nox::server::EditSessionDirector::Options o;
    o.mapsDir = mapsDir;
    o.mapName = setup.name;
    o.editMode = setup.editMode;
    o.playersCantDie = setup.playersCantDie;
    o.liveView = setup.liveView;
    o.liveCreatures = setup.liveCreatures;
    o.autoPublish = setup.autoPublish;
    o.playTestMode = setup.playTestMode;
    o.modFolder = setup.modFolder; // (the editor's open mod, carried by the session: none when empty)
    // "Approve always", kept between sessions in the editor's settings: the list from there, and
    // each change back (it comes on the host's thread: the main loop stores it).
    if (app.editor) {
        o.alwaysEditors = app.editor->sessionAlwaysEditors();
    }
    o.alwaysEditorsChanged = [slot = app.alwaysEditors](const std::vector<std::string>& names) {
        const std::lock_guard lock(slot->m);
        slot->changed = names;
    };
    s.editSession = o;
    try {
        app.host = makeHost(app, s);
    } catch (const std::exception& ex) {
        return ex.what();
    }
    // The host's own client finds the session's map (and each published version) where the
    // session publishes.
    app.playTestSavedCache = app.mapCache;
    app.mapCache = mapsDir;
    app.xwisHost.reset();
    // Listed: the router's port asked for, posted to XWIS under the menus' login (as any game
    // the menus host: XWIS lists a game under its host's login), answering LAN game lists; the
    // log says which (host_listing.hpp), as the session dialog said before Host.
    const HostListing where = menusHostListing(app, listed);
    if (listed) {
        startPortMapping(app, true);
        if (where.where == HostListing::Where::Xwis) {
            startXwisPost(app, *app.menus->hostLogin());
        }
    } else {
        app.online = false;
    }
    startLive(app, app.host->address());
    app.sessionHosting = true;
    app.sessionInGame = false; // (the editor in front; the game runs behind it)
    SDL_Log("edit session: hosting %s at %s (%s)", setup.name.c_str(), app.host->address().c_str(), listed ? "listed" : "unlisted");
    SDL_Log("%s", hostListingLog(where, "edit session", app.host->port()).c_str());
    return {};
}

namespace {

/// A private local test of a session's working copy (for destructive tests: in its own host, not
/// listed, so it doesn't disturb the players): the map the editor wrote hosted and played by a
/// second noxcore of ours, in its own window (--host on a free port, --private-test: unlisted, the
/// loopback alone, no router port; its own log), so the session's game and our connection to it
/// here stay exactly as they are. A hidden run's test is hidden too (its own screenshot). "" when
/// it started.
std::string startPrivateTest(App& app, const editor::PlayTestRequest& req)
{
    if (app.privateTest) {
        int code = 0;
        if (!SDL_WaitProcess(app.privateTest, false, &code)) {
            return "a private test is running already (close its window first)";
        }
        SDL_DestroyProcess(app.privateTest);
        app.privateTest = nullptr;
    }
    const char* base = SDL_GetBasePath();
    const fs::path dir = base ? fs::path(reinterpret_cast<const char8_t*>(base)) : fs::path();
    const fs::path exe = app.exePath.has_filename() ? dir / app.exePath.filename() : dir / "noxcore.exe";
    const char* mode = req.mode == 0x20 ? "ctf" : req.mode == 0x40 ? "flagball" : req.mode == 0x10 ? "kotr" : req.mode == 0x400 ? "elimination" : "arena";
    std::vector<std::string> args = {pathText(exe), "--host", pathText(req.map), "--host-port", "0", "--host-mode", mode, "--private-test",
                                     "--map-cache", pathText(req.cacheDir), "--name", app.profile.name, "--display", "windowed",
                                     "--no-movies", "--log", pathText(dir / "noxcore-private-test-log.txt"),
                                     // (Our own game folder: never one the child finds by itself.)
                                     "--game-dir", pathText(app.gameDir)};
    if (!app.screenshot.empty()) {
        // (A hidden run: the test as hidden, a screenshot of its own, then it ends.)
        fs::path shot(reinterpret_cast<const char8_t*>(app.screenshot.c_str()));
        shot.replace_filename(shot.stem().u8string() + u8"_private" + shot.extension().u8string());
        args.insert(args.end(), {"--screenshot", pathText(shot), "--shot-after", std::to_string(std::max(6.0f, app.shotAfterSeconds))});
    }
    if (!req.modeId.empty()) {
        // A mod's private test (the Mod menu's Play Mod in a session): the mode by its id, read from
        // the editor's own copy of the session's mod; on the session's map, the mode's own ignored.
        args.insert(args.end(), {"--mods-dir", pathText(req.modsDir.empty() ? modsFolderPath() : req.modsDir), "--host-mod", req.modeId});
        if (!req.modeMap) {
            args.push_back("--host-map-over-mode");
        }
    }
    if (req.startAt) {
        // Play From Here: its player in at the cursor's point, not a map spawn (play_from_here.hpp).
        char at[48];
        std::snprintf(at, sizeof at, "%.0f,%.0f", req.startAt->x, req.startAt->y);
        args.insert(args.end(), {"--start-at", at});
    }
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    app.privateTest = SDL_CreateProcess(argv.data(), false);
    if (!app.privateTest) {
        return std::string("couldn't start ") + pathText(exe) + ": " + SDL_GetError();
    }
    SDL_Log("private test: %s in its own process (%s)", pathText(req.map).c_str(), req.modeId.empty() ? mode : req.modeId.c_str());
    return {};
}

} // namespace

/// The editor (made if need be) in front, on the session (F2 from the game).
void sessionToEditor(App& app)
{
    if (!app.editSession || !app.editSession->inSession()) {
        return;
    }
    if (!app.editor) {
        openEditor(app, {});
    }
    if (!app.editor->inSession()) {
        attachSessionEditor(app);
    }
    if (app.sessionEmbedded) {
        // (The game in the Map panel: the panel the editor's again.)
        capturePlayTest(app, false);
        app.sessionEmbedded = false;
        app.playTestFrame = nullptr;
    }
    app.playTestMouse.reset(); // (no game in the panel: no held pointer place to outlive it)
    app.sessionInGame = false;
    if (app.cinematics) {
        app.cinematics->clearScreen(); // (a cinematic's black, bars and hidden HUD not kept for the next Play)
    }
    // Edit: our character stands aside as an observer (stock goObserver: invisible, not colliding,
    // out of harm's way) till we play again.
    app.editSession->park(true);
    if (app.input) {
        app.input->releaseAll();
    }
    SDL_ShowCursor();
    SDL_StartTextInput(app.window);
}

/// The session's game in front again (the editor's Play, F2): in the editor's Map panel (the
/// play-test's presentation), or over the window with Play in Full Window (the editor waiting
/// behind it). `moveToCamera`: our character back in play at the editor's camera (else where it
/// stood aside: the host's fallback); false after "Jump in here", which placed it. `at`: Play From
/// Here's point (the cursor's) instead of the camera's (in a wall, the host takes the
/// nearest open spot).
void sessionToGame(App& app, bool moveToCamera, std::optional<std::pair<float, float>> at)
{
    if (!app.live) {
        return;
    }
    if (app.editSession && app.editSession->inSession() && moveToCamera) {
        std::optional<std::pair<float, float>> spot = at;
        if (!spot && app.editor && app.editor->inSession()) {
            spot = app.editor->cameraCentre();
        }
        app.editSession->park(false, spot);
    }
    if (app.editor && !app.editor->playFullWindow()) {
        app.sessionEmbedded = true;
        app.sessionInGame = false;
        app.playTestFrame = nullptr; // (none till its first frame)
        capturePlayTest(app, true);   // (as a play-test in the panel: the game has the input from the start)
        return;
    }
    app.sessionInGame = true;
    gameTakesInput(app); // (new_game.hpp: nothing held from the editor)
    SDL_StopTextInput(app.window);
    handMouseTo(app, MouseOwner::Game); // (no held place from an earlier in-panel game: the pointer follows the mouse)
}

namespace {

/// The editor on our session's client side, its Play hooks ours.
void attachSessionEditor(App& app)
{
    app.editor->attachSession(app.editSession.get(),
                              MapEditor::SessionHooks{.toGame = [&app] { sessionToGame(app, true); },
                                                      .toGameAsIs = [&app] { sessionToGame(app, false); },
                                                      // Play From Here: Play at the cursor's point.
                                                      .toGameAt = [&app](float x, float y) { sessionToGame(app, true, std::pair{x, y}); },
                                                      // The Chat panel's box: the game's own chat path.
                                                      .say = [&app](const std::string& text, bool team) { sendChat(app, text, team); },
                                                      // The live layer's players' colours and worn things.
                                                      .gameWorld = [&app]() -> const nox::client::World* {
                                                          return app.live ? &app.live->client().world() : nullptr;
                                                      },
                                                      .hosting = app.sessionHosting});
    for (auto& line : std::exchange(app.sessionChat, {})) {
        app.editor->addChat(std::move(line)); // (what was said before the editor attached)
    }
}

} // namespace

/// Each frame: the session's client side kept with the game it's on (gone with it: the editor
/// detached), joined once the session greets us, the editor attached for our own session.
void checkSession(App& app)
{
    if (app.editSession && (!app.live || app.editSessionOn != &app.live->client())) {
        if (app.editor && app.editor->inSession()) {
            app.editor->attachSession(nullptr);
        }
        if (app.sessionEmbedded) {
            capturePlayTest(app, false);
            app.sessionEmbedded = false;
            app.playTestFrame = nullptr;
        }
        app.playTestMouse.reset();
        app.editSession.reset();
        app.editSessionOn = nullptr;
        app.sessionInGame = false;
        app.sessionHosting = false;
        app.sessionChat.clear();
    }
    // The private test's process: gone once it ended.
    if (app.privateTest) {
        int code = 0;
        if (SDL_WaitProcess(app.privateTest, false, &code)) {
            SDL_Log("private test: ended (%d)", code);
            SDL_DestroyProcess(app.privateTest);
            app.privateTest = nullptr;
        }
    }
    // The hosted session's "approve always" list, kept in the editor's settings.
    if (app.alwaysEditors) {
        std::optional<std::vector<std::string>> changed;
        {
            const std::lock_guard lock(app.alwaysEditors->m);
            changed = std::exchange(app.alwaysEditors->changed, std::nullopt);
        }
        if (changed && app.editor) {
            app.editor->setSessionAlwaysEditors(std::move(*changed));
        }
    }
    if (!app.editSession || !app.editSession->inSession()) {
        return;
    }
    if (!app.editSession->joined()) {
        app.editSession->join(); // (a viewer: the working copy kept, edits seen as they happen)
    }
    if (!app.sessionNoticed) {
        app.sessionNoticed = true;
        if (app.console) {
            app.console->print("This game is a map editor session: F2 opens the editor (/edit asks the host for edit rights).", Yellow);
        }
        SDL_Log("edit session: %s, map %s", app.sessionHosting ? "ours" : "joined", app.editSession->hello()->map.c_str());
    }
    if (app.sessionEditorAuto && !app.sessionHosting && (!app.editor || !app.editor->inSession())) {
        app.sessionEditorAuto = false;
        sessionToEditor(app);
    }
    if (app.sessionHosting && app.editor && !app.editor->inSession()) {
        attachSessionEditor(app);
        app.editSession->park(true); // (the editor in front: our character stands aside; it comes in observing anyway)
    }
}

/// Each frame: a map editor session's pending edits into our game view: the working copy's walls,
/// floor and client-only things that the loaded map hasn't, greyed, and what it removed as ghosts,
/// as the "Pending changes" setting says. Worked out again only when the working copy, the
/// published version, the view or the setting changes; outside a session the view has none (and
/// nothing is done).
void updatePending(App& app)
{
    if (!app.view) {
        return;
    }
    const bool session = app.live && app.editSession && app.editSession->inSession() && app.editSession->synced();
    App::PendingShown& shown = app.pendingShown;
    if (!session) {
        if (shown.any && shown.view == app.view->serial()) {
            app.view->setPending({});
        }
        shown = {};
        app.pendingReference.reset();
        app.pendingReferenceFor = 0;
        return;
    }
    const nox::client::EditSessionClient& s = *app.editSession;
    if (shown.view == app.view->serial() && shown.revision == s.revision() && shown.version == s.version() && shown.mode == app.pendingView) {
        return;
    }
    // The reference: the map this view was made from (what the game shows), else the session's
    // published copy.
    if (app.pendingReferenceFor != app.view->serial()) {
        app.pendingReferenceFor = app.view->serial();
        try {
            app.pendingReference = std::make_unique<nox::mapedit::MapDocument>(nox::mapedit::MapDocument::fromMapFile(app.map, app.things));
        } catch (const std::exception& ex) {
            SDL_Log("pending edits: the loaded map as a document: %s (the published copy instead)", ex.what());
            app.pendingReference.reset();
        }
    }
    const nox::mapedit::MapDocument& reference = app.pendingReference ? *app.pendingReference : s.published();
    const auto edits = nox::mapedit::diffPending(s.document(), reference, app.things);
    const auto plan = nox::mapedit::planPending(edits, nox::mapedit::PendingBase::Reference, app.pendingView);
    app.view->setPending(plan);
    if (plan.empty() != !shown.any || shown.mode != app.pendingView) {
        SDL_Log("pending edits: %zu walls, %zu tiles, %zu things (%s)", edits.walls.size(), edits.tiles.size(), edits.objects.size(),
                nox::mapedit::pendingViewName(app.pendingView));
    }
    shown = {app.view->serial(), s.revision(), s.version(), app.pendingView, !plan.empty()};
}

namespace {

/// Where the editor stands, for close_plan.hpp.
editor::CloseSituation closeSituation(const App& app)
{
    editor::CloseSituation s;
    s.direct = app.editorDirect;
    s.menus = app.menus != nullptr;
    s.hostingSession = app.sessionHosting && app.host != nullptr;
    s.joinedSession = !s.hostingSession && app.live && app.editSession && app.editSession->inSession();
    s.playTest = app.playTest && app.live;
    return s;
}

} // namespace

/// The editor let go of its session (before the session's client side goes, or the editor does).
void detachEditor(App& app)
{
    if (app.editor && app.editor->inSession()) {
        app.editor->attachSession(nullptr);
    }
}

/// The editor's game ended: a play-test's, or our own edit session's (our players told it ended:
/// GameHost::beginQuit's MSG_SERVER_QUIT, the host retired in the background with LocalHost::retire,
/// a listed session's router port and XWIS listing gone), the session's client side with it.
void endEditorGame(App& app)
{
    if (app.sessionHosting && app.host) {
        app.host->beginQuit(); // (the goodbye at once, on the host's thread; retire waits for the acks)
    }
    endPlayTest(app);
    if (app.editSession) {
        app.editSession.reset();
        app.editSessionOn = nullptr;
    }
}

/// The menus after the editor (or its session's game) has gone: the game list it was opened from
/// (editorFromList), else the main menu; the menus' mouse back whole.
SDL_AppResult toMainMenu(App& app)
{
    if (app.editorDirect || !app.menus) {
        return SDL_APP_SUCCESS;
    }
    handMouseTo(app, MouseOwner::Menus);
    SDL_StartTextInput(app.window); // (the menus' entry fields)
    if (std::exchange(app.editorFromList, false)) {
        app.menus->showGameBrowser();
    } else {
        app.menus->showMainMenu();
    }
    app.logMenuPointer = true;
    return SDL_APP_CONTINUE;
}

namespace {

/// The editor's close (File > Close Editor, confirmed; asked for in its own frame and carried out
/// after it): close_plan.hpp says what, for where it stands. One step: never the session's game put
/// in front of a closing host, never a game left running without its editor.
SDL_AppResult closeEditor(App& app)
{
    const editor::ClosePlan plan = editor::closePlan(closeSituation(app));
    SDL_Log("map editor: closed%s, to %s", plan.endSession ? ", our session ended for everyone in it" : plan.endPlayTest ? ", the play-test ended" : "",
            editor::afterCloseName(plan.next));
    std::optional<std::pair<float, float>> spot;
    if (app.editor && app.editor->inSession()) {
        spot = app.editor->cameraCentre(); // (someone else's session: our character back in play there, as Play)
    }
    detachEditor(app);
    if (plan.endSession || plan.endPlayTest) {
        endEditorGame(app); // (first: the editor's script panel then has no play-test of its own to end as it goes)
    }
    app.editor.reset();
    switch (plan.next) {
    case editor::AfterClose::SessionGame:
        // Someone else's session: its game over the whole window (F2 opens the editor again; its
        // Quit menu leaves the session).
        app.sessionEmbedded = false;
        app.sessionInGame = false;
        app.playTestFrame = nullptr;
        if (app.editSession && app.editSession->inSession()) {
            app.editSession->park(false, spot);
        }
        SDL_StopTextInput(app.window);
        handMouseTo(app, MouseOwner::Game);
        return SDL_APP_CONTINUE;
    case editor::AfterClose::Quit:
        return SDL_APP_SUCCESS;
    case editor::AfterClose::MainMenu:
        break;
    }
    return toMainMenu(app);
}

} // namespace

/// The map editor's frame onto the window (a play-test's game in its Map panel already drawn into
/// its texture), presented; a --screenshot run's shot once it's due; the editor's quit or close.
SDL_AppResult editorFrame(App& app)
{
    app.editor->frame();
    if (!app.screenshot.empty() && !app.shotTaken && SDL_GetTicks() >= static_cast<std::uint64_t>(app.shotAfterSeconds * 1000)) {
        app.shotTaken = true;
        return saveScreenshot(app) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    present(app);
    if (app.editor->quitConfirmed()) {
        return SDL_APP_SUCCESS;
    }
    if (app.editor->wantsClose()) {
        const SDL_AppResult r = closeEditor(app);
        if (r != SDL_APP_CONTINUE) {
            return r;
        }
    }
    ++app.frames;
    if (app.frameLimit >= 0 && app.frames >= app.frameLimit) {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

} // namespace noxsdl
