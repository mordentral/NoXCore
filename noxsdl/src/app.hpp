// The front end's state (App: SDL's appstate, made in SDL_AppInit, main.cpp), and the functions
// its files share. Each group below is defined in the file it's under:
//   app.cpp               small shared helpers: the mouse, the string table, centred messages, flags
//   app_settings.cpp      nox.cfg's and noxcore.cfg's settings, the display and the mouse modes
//   game_windows.cpp      the game's windows made at start-up, with their callbacks
//   console_commands.cpp  the console's commands
//   hosting.cpp           a game hosted here: its settings, XWIS post, bots and router port
//   game_session.cpp      a game joined: the live session, the game's windows, chat and saves
//   quickbar.cpp          the spell bars, traps and the award flight, as played
//   live_scene.cpp        the world's drawables as the scene draws them, lights and sounds
//   overlays.cpp          what's drawn over the world: names, bars, bubbles, banner, tooltips
//   test_drive.cpp        the tests' fake screens and scripted input
// main.cpp has SDL's callbacks, the frame, the input's routing and the map editor's glue;
// command_line.hpp the command line's reading.
#pragma once

#include <SDL3/SDL.h>

#include "audio.hpp"
#include "bloom.hpp"
#include "browser_screen.hpp"
#include "camera.hpp"
#include "centred_messages.hpp"
#include "chat_entry.hpp"
#include "cinematics.hpp"
#include "console.hpp"
#include "creature_bar.hpp"
#include "cursor.hpp"
#include "discon_window.hpp"
#include "editor/map_editor.hpp"
#include "editor/session_setup.hpp"
#include "effects.hpp"
#include "gpu_shaders.hpp"
#include "host_listing.hpp"
#include "hud.hpp"
#include "input.hpp"
#include "input_config_window.hpp"
#include "interpolator.hpp"
#include "inventory.hpp"
#include "item_names.hpp"
#include "legal_screen.hpp"
#include "lighting.hpp"
#include "live_session.hpp"
#include "local_bots.hpp"
#include "local_host.hpp"
#include "map_download.hpp"
#include "map_thumbnail.hpp"
#include "map_view.hpp"
#include "map_vote_window.hpp"
#include "menus.hpp"
#include "minimap.hpp"
#include "mode_ui.hpp"
#include "motd_window.hpp"
#include "mouse_handover.hpp"
#include "movie_player.hpp"
#include "net_stat.hpp"
#include "options_window.hpp"
#include "pencil_icon.hpp"
#include "port_mapper.hpp"
#include "quest_windows.hpp"
#include "quit_menu.hpp"
#include "rank_board.hpp"
#include "screen_particles.hpp"
#include "server_options.hpp"
#include "sight.hpp"
#include "spell_chant.hpp"
#include "spell_limits.hpp"
#include "spellbook.hpp"
#include "sprite_cache.hpp"
#include "summon_window.hpp"
#include "text.hpp"
#include "thing_art.hpp"
#include "trade_screens.hpp"
#include "view3d.hpp"
#include "weather.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/client/state_draw.hpp"
#include "noxcore/io/wol_login.hpp"
#include "noxcore/lobby/xwis_host.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/server/play_from_here.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/video/video_bag.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nox::modes::script {
class ScriptUiClient;
}

namespace noxsdl {

namespace movies = nox::client::movies;

struct AnimTrack {
    std::uint8_t state = 0;
    double start = 0;   ///< game frame (interpolated clock) the state was first seen
    double landed = -1; ///< a player's jump held past its landing: when the landing came (else -1)
};

/// A player's name over their head, anchored at their feet (x, y) in world units.
struct NamePlate {
    float x = 0;
    float y = 0;
    std::string name;
    SDL_Color color{};
    /// A map editor session's editor (the host's Editors message): the pencil before the name
    /// (alone when there's no name to draw), in their colour, greyed while they're idle.
    struct Pencil {
        PeerRgb colour;
        bool idle = false;
    };
    std::optional<Pencil> pencil;
};

/// The health bar PlayerDraw puts beside another player in a quest game.
struct HealthBar {
    float x = 0; ///< the player's feet, in world units
    float y = 0;
    float z = 0;    ///< their height
    float side = 0; ///< the bar's left from x: twice their radius
    std::uint8_t percent = 0;
    bool poisoned = false;
};

/// The dot over a creature on our friends list, in our team's colour (MonsterDraw).
struct MinionDot {
    float x = 0; ///< where it's drawn, in world units: twice its height over its feet
    float y = 0;
    SDL_Color color{};
};

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    long frameLimit = -1;
    long frames = 0;
    std::string screenshot;
    bool shotTaken = false;
    float shotAfterSeconds = 0;
    std::uint64_t inGameSinceMs = 0;
    bool dragging = false;
    bool running = false; ///< live: right button held, running toward the cursor
    std::filesystem::path gameDir;
    /// The folder a solo game's Save\ is under (Game_GetWorkDir's in the exe): the game's, or with
    /// --save-dir DIR, DIR's parent when DIR is a "Save" folder, else DIR.
    std::filesystem::path saveRoot;
    bool soloFromMenus = false; ///< the game was the main menu's Solo (its end goes back there)

    nox::things::ThingDb things;
    nox::things::ModifierDb modifiers; ///< item colours (empty if modifier.bin is missing)
    nox::things::GameData gameData; ///< gamedata.bin's tables (read at start-up, as thing.bin)
    SpellLimits spellLimits; ///< live: spells refused while our last one is out (the HUD marks them)
    std::unique_ptr<nox::video::VideoBag> bag;
    std::unique_ptr<SpriteCache> sprites;
    std::unique_ptr<GpuShaders> shaders; ///< our shaders on SDL's GPU renderer
    SDL_Texture* white = nullptr;        ///< the white pixel smooth effects draw on
    std::unique_ptr<Bloom> bloom;        ///< the effects' bloom (GPU renderer; --no-bloom)
    /// nox.weather's rain, snow, storm and wind (weather.hpp; null without the GPU renderer's
    /// shaders: none drawn), and Options' Weather Effects (0 On, 1 Reduced, 2 Off).
    std::unique_ptr<WeatherFx> weather;
    int weatherEffects = 0;
    /// The live game's script mode UI side (its weather: ScriptUiClient::weather); null: none.
    nox::modes::script::ScriptUiClient* scriptUi = nullptr;
#ifndef NOX_NO_TEST_COMMANDS
    /// The console's "weather KIND ...": drawn in place of the server's (tests); unset: the server's.
    std::optional<nox::modes::script::weather::Told> weatherTest;
#endif
    float bloomStrength = 0.2f;          ///< --bloom N
    std::unique_ptr<ThingArt> art;
    /// The types the scene names itself (live_scene.cpp: NewPlayer for NPCs, the skull and crown
    /// over a player, the carried flag), looked up in `art` once each.
    ThingArt::Named newPlayerArt, skullArt, crownArt, flagArt;
    nox::map::MapFile map;
    std::unique_ptr<MapView> view;
    Camera camera;
    View3D view3d; ///< the world in first person (the console's "nx 3d"; view3d.hpp)
#ifndef NOX_NO_TEST_COMMANDS
    /// The console's "view3d sight": the 3D view held to the 2D view's sight in a game (release
    /// builds always are; tests of that here).
    bool view3dSight = false;
#endif

    /// The map editor (editor/map_editor.hpp), from the mode lists or --map-editor: while open it
    /// has the window (its own panels; the game's screens wait under it).
    std::unique_ptr<MapEditor> editor;
    std::filesystem::path editorMapsDir;   ///< --editor-maps-dir: where it lists and saves maps (else the game's maps\)
    std::filesystem::path editorUserDir;   ///< --editor-user-dir: its favourites, prefabs, categories and thumbnail cache (else the user's editor folder)
    std::string editorRun;    ///< --editor-run "cmd;cmd": its commands run once it's up (tests)
    bool editorDirect = false; ///< opened by --map-editor (its close ends the program)
    bool editorFromList = false; ///< opened from the game list (Host Custom > Map Editor): its close goes back there
    /// The editor's play-test (editor/play_test.hpp): the map hosted here, played in the editor's
    /// Map panel (playTestEmbedded: drawn into gammaTarget for it, gameArea there; the game has the
    /// keyboard and mouse while playTestCaptured), or full window, the editor waiting behind the
    /// game while playTestInGame (F2 comes back); the map cache the test swapped in.
    bool playTest = false;
    bool playTestInGame = false;
    bool playTestEmbedded = false;
    bool playTestCaptured = false;
    bool playTestPaused = false;
    std::optional<SDL_FPoint> playTestMouse; ///< (in the panel, the input the editor's) the game's mouse where it was left
    SDL_Rect playTestMouseRect{};            ///< the pointer's confinement set (the game's area while it has the mouse)
    SDL_Texture* playTestFrame = nullptr;    ///< (in the panel) the game's last frame for it: gammaTarget, or playTestGamma
    SDL_Texture* playTestGamma = nullptr;    ///< (in the panel) the frame through the gamma ramp (Options' gamma above 1)
    std::filesystem::path playTestSavedCache;
    /// What a test changed for its game and puts back at its end (playTestRestore): the profile
    /// (its class), and a solo or quest test's save folder and character (the test's own, in its
    /// folder: never the player's saves).
    bool playTestRestore = false;
    nox::client::PlayerProfile playTestSavedProfile;
    std::filesystem::path playTestSavedRoot;
    std::filesystem::path playTestSavedCharacter;
    bool playTestSavedHostQuestRul = false;
    std::optional<nox::player::FileInfo> playTestSavedHeld;
    /// Jump in at the cursor: where the test's player goes once it's in (playTestJump); a quest
    /// test's once the quest has begun on the test's map (not in the chat map before it).
    std::optional<SDL_FPoint> playTestJumpTo;
    /// The placing under way (server/play_from_here.hpp: in, then kept off the map's spawns a while).
    std::optional<nox::server::PlayFromHere> playTestPlacing;
    bool playTestQuest = false;
    std::string playTestMap; ///< the test's map's name (EDTEST)
    /// A collaborative map editor session's client side (edit_session_client.hpp) on the game we're
    /// in: made with every live game (idle unless its server is an edit session); the client it's
    /// on (when that goes, so does it). sessionInGame: the editor waits behind the game (F2
    /// toggles); sessionHosting: ours (Host Custom > Map Editor, the editor's File > Host a Session,
    /// --host-edit).
    std::unique_ptr<nox::client::EditSessionClient> editSession;
    const nox::client::HeadlessClient* editSessionOn = nullptr;
    /// Hosting a mode started through the solo screens (a campaign mode: mode_ui.hpp's
    /// SoloScreensStart): the solo saves' root, kept while saveRoot is the mode's (back at the
    /// game's end: endSoloScreensHost). Empty: not hosting one.
    std::filesystem::path modeKeptRoot;
    std::string hostSoloScreensTest; ///< (tests: --host-coop, that mode of --class's hosted: its id)
    bool sessionInGame = false;
    bool sessionHosting = false;
    bool sessionNoticed = false;
    /// A session's chat that came before its editor was attached: given to the editor's Chat panel
    /// as it attaches (at most the panel's 500 lines).
    std::deque<editor::ChatLine> sessionChat;
    bool sessionEditorAuto = false; ///< --session-editor: the editor in front as soon as a session greets us (tests)
    /// The session's game in the editor's Map panel (Play with the editor up: the play-test's
    /// presentation, gameArea there, playTestCaptured while it has the input); F2 goes back to
    /// editing (our character parked as an observer).
    bool sessionEmbedded = false;
    /// The hosted session's "approve always" list changed (on the host's thread): the main loop
    /// keeps it in the editor's settings.
    struct AlwaysEditors {
        std::mutex m;
        std::optional<std::vector<std::string>> changed;
    };
    std::shared_ptr<AlwaysEditors> alwaysEditors = std::make_shared<AlwaysEditors>();
    /// A private local test of a session's working copy: a game of its own in its own process
    /// (unlisted, loopback, a free port); the session's game here untouched.
    SDL_Process* privateTest = nullptr;
    bool privateTestRun = false;    ///< --private-test: this process is one (no router port asked for)
    std::filesystem::path exePath;               ///< argv[0] (a private test runs another of us)
    bool hostLoopback = false;      ///< --host-loopback: a game hosted here binds the loopback alone (local tests)
    std::uint16_t menuHostPort = 0; ///< --menu-host-port: a campaign mode or an edit session hosted from the menus on it (0: the game's)
    /// The session's pending edits in our game view: "Pending changes" Shaded / As final / Hidden
    /// (noxsdl pending, noxcore.cfg's PendingChanges; --pending-view for one run), and what the
    /// view was last given (updatePending).
    nox::mapedit::PendingView pendingView = nox::mapedit::PendingView::Shaded;
    struct PendingShown {
        std::uint64_t view = 0; ///< MapView::serial
        std::uint64_t revision = ~0ull;
        std::uint32_t version = ~0u;
        nox::mapedit::PendingView mode = nox::mapedit::PendingView::Shaded;
        bool any = false;
    } pendingShown;
    /// The map the game view loaded as a document (the pending edits' reference), made once per
    /// view in a session.
    std::unique_ptr<nox::mapedit::MapDocument> pendingReference;
    std::uint64_t pendingReferenceFor = 0; ///< MapView::serial
    std::uint64_t playTestReturnAfterMs = 0; ///< --playtest-return S: back to the editor S seconds into a test (tests)
    std::uint64_t playTestStartedMs = 0;

    std::unique_ptr<BrowserScreen> browser; ///< the game list, when started without --map / --connect
    std::unique_ptr<nox::strings::StringTable> strings; ///< nox.csf, for the menus
    std::unique_ptr<Menus> menus;                       ///< the game's menus (hold the game list)
    /// The movie player (movie_player.hpp): over everything while a movie plays.
    std::unique_ptr<MoviePlayer> movie;
    /// The legal screen after the start-up movies (legal_screen.hpp), till the main menu.
    std::unique_ptr<LegalScreen> legal;
    /// The menus' pointer sparks (mainloop), stepped 30 times a second; a press since
    /// the last step counts even if it's let go already.
    std::unique_ptr<ScreenParticles> shellSparks;
    std::uint64_t shellSparkMs = 0;
    bool shellPressed = false;
    std::unique_ptr<QuitMenu> quitMenu;                 ///< in game, on Escape
    std::unique_ptr<ServerOptions> serverOptions;       ///< the host's Server Settings (Gameplay Controls)
    std::unique_ptr<MotdWindow> motdWindow;             ///< the server's message of the day
    std::unique_ptr<DisconWindow> discon;               ///< the server gone quiet: the icon and the lag window
    std::unique_ptr<QuestWindows> questWindows;         ///< a quest's game over and shared keys (per game)
    bool openQuitMenu = false;                         ///< --quit-menu: open it once in the game
    bool openServerOptions = false;                     ///< --server-options: the host's, once in the game
    bool openInventory = false;                         ///< --open-inventory: likewise (tests)
    std::optional<int> openBook;                        ///< --open-book [N]: the book, at entry N's page (tests)
    /// --mouse-script "MS down|up|rdown|rup|move|esc|goto X Y; ...": the left (or right: moving) button
    /// pressed, let go or the mouse moved at window pixel (X, Y), or Escape (in a game), or (goto) the console's "nx goto X Y" (in a game), MS after joining a game, or in the menus after start-up
    /// (tests of the windows' mouse handling).
    struct ScriptedMouse {
        std::uint64_t atMs = 0;
        int action = 0; ///< 0 move, 1 down, 2 up
        float x = 0, y = 0;
    };
    std::vector<ScriptedMouse> mouseScript;
    bool expandBarsTest = false;                        ///< --all-spell-sets: the five sets shown (tests)
    std::uint32_t awardTest = 0; ///< --award-test N: spell N awarded as a quest does (shown, to the bars), 2 s in
    bool dropEquipped = false;   ///< --drop-equipped: what we wear and hold taken off, 1.5 s in (tests)
    bool quitRequested = false;
    nox::client::PlayerProfile profile;     ///< for joining from the browser
    std::filesystem::path mapCache;
    std::unique_ptr<LiveSession> live;
    /// Each mode's UI (mode_ui.hpp), by its mode's id: made with the game windows, for the program's life.
    std::vector<std::pair<std::string, std::unique_ptr<ModeUi>>> modeUis;
    /// A mode's --fake-* screen asked for (fakeScreen kFakeModeScreen): its flag as typed.
    std::string fakeModeFlag;
    /// A game we host (--host, the Host button): the server in this process, pumped each frame;
    /// `live` joins it. Its console takes the host's commands.
    std::unique_ptr<LocalHost> host;
    /// Bots the game we host runs (Server Settings' Bots tab): network bots joined to it over the
    /// loopback, on a thread of their own (local_bots.hpp); gone with the game.
    struct HostBots {
        std::unique_ptr<nox::bot::ItemKinds> kinds;
        std::unique_ptr<LocalBots> squad;
        std::array<int, 3> wanted{};
        int minTier = 1; ///< the difficulty, from minTier to maxTier (0 easy, 1 medium, 2 hard)
        int maxTier = 1;
    } localBots;
    /// The game we host's port opened on the router by UPnP (port_mapper.hpp); gone with the game.
    /// Not mapped: a reminder to forward it, once, as we're first in.
    std::unique_ptr<PortMapper> portMapper;
    bool portReported = false;
    /// The game we host posted to XWIS (xwis_host.hpp) while we're logged in there; its record
    /// renewed every 2 s.
    std::unique_ptr<nox::lobby::XwisHost> xwisHost;
    std::uint64_t xwisRecordAt = 0;
    bool xwisPosted = false;   ///< (logged: the game in XWIS's channel, its record its topic)
    std::string xwisError;     ///< (logged: why the post last failed)
    std::string liveHost; ///< the server `live` joined
    std::optional<std::uint32_t> wallsApplied; ///< the world's wallsVersion the view shows
    std::string liveMap; ///< the map `view` shows in a live game
    std::string viewMap; ///< the map `view` shows, by file name (the HD preload's log)
    bool hdDecodeSync = false; ///< --hd-decode-sync: the pack's images decoded on the drawing thread (for comparison)
    bool hdPlaceholders = false; ///< --hd-placeholders: the originals drawn till the HD images are in (for comparison)
    /// When the live map was loaded while it's held back for its HD images (0: not held).
    std::uint64_t mapHeldSinceMs = 0;
    bool mapHeld = false; ///< this frame: the map not drawn yet (its HD images on their way)
    std::uint32_t liveMapLoads = 0; ///< the client's mapLoads() when `view` was made from its map
    Interpolator interpolator;
    std::unique_ptr<Effects> effects; ///< sparks, explosions and the like
    std::unique_ptr<Audio> audio;     ///< sound effects, music and speech (nullptr: silent)
    std::unique_ptr<SpellChant> chant; ///< our spells' chant: phonemes and hand gestures
    std::vector<std::uint32_t> loopSounds; ///< by type id: the AUDIOLOOP sound's id, 0 for none
    double lastHeartBeat = -1e9;           ///< game frame of the last low-health heartbeat
    std::string speaking;                  ///< the NPC dialog or shop greeting being spoken
    std::size_t speakingVariant = 0;       ///< its string's variant (the dialog window shows that one)
    std::mt19937 rng{std::random_device{}()}; ///< the client's own random picks (Str_GetString's variant)
    std::unique_ptr<Hud> hud;         ///< live: bars and the spell bar
    std::unique_ptr<Minimap> minimap; ///< live: the map (TAB; 1 / 2 zoom)
    /// nox.cfg's key bindings (default.cfg's until something is saved).
    std::unique_ptr<InputBindings> input;
    /// The view in a game, as the game's Options set it: how much of the world shows (one of its
    /// screen resolutions: one screen pixel was one world unit), and the game window's size in
    /// percent (VideoSize, 40-100; Page Up / Page Down step it by 2, draw_setCutSize). The resolution's
    /// area fills the largest part of the window of its shape, black bars round it (gameArea).
    int viewWidth = 1024;
    int viewHeight = 768;
    int windowSize = 100;
    bool renderGui = true;     ///< ToggleGUI (F11), and nox.cfg's RenderGUI: the HUD drawn or not
    bool renderGlow = true;    ///< RenderGlow: the effects' glows
    bool softShadowEdge = true; ///< SoftShadowEdge: the line of sight's soft edge (engine flag 0x400)
    bool fadeObjects = true;    ///< FadeObjects: what's seen that leaves sight fades out over a second
    bool classicWalls = false;  ///< the game's own wall culling (noxsdl walls classic on); off: noxsdl's improved one
    bool traceFade = false;     ///< --trace-fade (tests): each lingering drawable's fade, every frame
    /// The brightness (1-10; Insert / Delete; noxcore.cfg's Gamma, default 1, not nox.cfg's), the
    /// game's gamma ramp over the finished frame:
    /// above 1 the frame is drawn into gammaTarget and through the gamma shader onto the screen.
    int gamma = 1;
    /// The mouse (Options' Win Mouse Accel and Mouse speed; noxcore.cfg): applyMouse.
    bool mouseAccel = true;
    int mouseSpeed = 100; ///< percent
    int mouseSpeed3D = 100; ///< percent, the 3D view's mouse look in place of mouseSpeed
    bool mouseLooking = false; ///< applyMouse's last: the 3D view's mouse look had it
    /// Kill Feed: the game's own, centred at the top; Options can switch it off.
    bool killFeed = true;
    /// A game we host's update rate to noxsdl clients (Server Settings' General page; noxcore.cfg's
    /// ServerUpdateRate): 30, 60, 90 or 120; 60 unless set (every game noxsdl hosts, solo too).
    int serverUpdateRate = 60;
    /// A game we host's map vote (the General page's Map Vote; noxcore.cfg's ServerMapVote).
    bool serverMapVote = true;
    /// Games we host (solo, quest and editor sessions too) figure their floats as the original's x87
    /// (Options' More page, Exact Original Maths; noxcore.cfg's ExactMaths; off: VD-37).
    bool exactMaths = true;
    /// Options' Display (0 windowed, 1 fullscreen windowed, 2 fullscreen) and VSync: applyDisplay.
    int display = 0;
    bool vsync = true;
    struct KillLine {
        std::uint16_t attacker = 0;
        std::uint16_t assist = 0;
        std::uint16_t victim = 0;
        std::uint16_t weapon = 0;
        std::uint8_t kind = 0;
        std::uint32_t frame = 0; ///< the game frame it came
    };
    std::deque<KillLine> kills; ///< the latest 100 (the kill feed's ring)
    CentredMessages centred; ///< Gui_PrintCenteredMessage's slots (guimsg.c)
    bool mapDownloaded = false; ///< the map being loaded came by download (no EnterChat)
    /// A quest we host (Solo Quest) whose OTQuest.rul hasn't run yet (questStartRul).
    bool hostQuestRul = false;
    /// The map load (liveMapLoads) the host's Server Settings last came up for on arriving in a
    /// chat map (SrvNet_OnClientReady's guiServerOptsLoad).
    std::uint32_t serverOptionsLoad = 0;
    bool mouseApplied = false; ///< the mode set as these say (applyMouse)
    bool logMenuPointer = false; ///< the menus' next frame logs where their pointer is drawn (back from the editor or a game)
    std::uint64_t sessionLeaveAtMs = 0; ///< --session-leave-after (tests): the session's game left then, as its Quit menu's Leave
    float mouseScale = 0;      ///< SDL's relative speed scale as last set
    SDL_Texture* gammaTarget = nullptr; ///< the frame's target (gamma, or the game's area: below)
    /// In a game, where its view sits in the window (output pixels): the largest area of the
    /// chosen resolution's shape that fits, centred, black bars round it, so a window of another
    /// shape never crops the view. The frame is drawn at its size and put there. The whole
    /// window in the menus (they centre their own layout).
    SDL_FRect gameArea{0, 0, 0, 0};
    bool letterboxed = false; ///< gameArea is less than the window
    NetStat netStat;                ///< F10 (ToggleNetstat)
    std::unique_ptr<ItemNames> itemNames; ///< items' tooltip names (ToolTip.c)
    /// The tooltip (cursorSetTooltip, drawn by client_drawCursorAndTooltips): this frame's text, and
    /// how many frames (30 a second) the mouse has kept still, under 2 pixels
    /// (client_processMouseEvents: shown from the tenth).
    std::string tooltip;
    int tooltipStill = 0;
    SDL_FPoint tooltipMouse{};
    std::uint64_t tooltipStepMs = 0;
    bool screenshotWanted = false;  ///< F12 (ScreenShot): this frame to nox<n>.bmp
    int screenshotNumber = 0;       ///< g_ScreenshotPrefix_: the last number used
    bool renderBubbles = true; ///< RenderBubbles: chat bubbles
    std::unique_ptr<OptionsWindow> options; ///< the game's Options window
    std::unique_ptr<InputConfigWindow> inputConfig; ///< Options' Input Config: the key bindings
    std::unique_ptr<ChatEntry> chat;        ///< typing a chat line (Chat / TeamChat)
    std::unique_ptr<MapDownload> mapDownload; ///< the map download screen (mapdnld.wnd)
    std::unique_ptr<RankBoard> rankBoard;     ///< the scoreboard (ToggleRank, F9)
    std::unique_ptr<MapThumbnails> mapPictures; ///< the map vote's candidates drawn from above
    std::unique_ptr<MapVoteWindow> mapVote;     ///< noxcpp's map vote
    std::unique_ptr<Console> console;      ///< the console (F1)
    std::map<std::string, std::string> macros; ///< console bind: a key's name -> a line to say
    /// Console mute: players whose chat and bubbles aren't shown (the game's status bit 8, set
    /// only on this client: playerCanTalkMB), until they leave.
    std::set<std::uint16_t> muted;
    bool cheats = false;                   ///< the console's cheat word was typed
    std::uint64_t lastFrameMs = 0;    ///< SDL_GetTicks() at the last frame (held keys)
    bool openMap = false;             ///< --show-map (tests): TAB pressed once in the game
    std::unique_ptr<Inventory> inventory; ///< live: the inventory screen and weapon panel (per game)
    std::unique_ptr<SummonWindow> summon; ///< live: the creature cage and its command list (per game)
    std::unique_ptr<TradeScreens> tradeScreens; ///< live: shops, NPC dialogs, trades (per game)
    /// Live: the fades, widescreen bars, map intro, briefings (per game); our host's own client.
    std::unique_ptr<Cinematics> cinematics;
    struct HudBeforeCinematic { ///< the HUD switch's memory
        bool book = false, inventory = false, map = false;
    } hudBefore;
    std::unique_ptr<Spellbook> book;      ///< live: the spellbook (per game)
    /// Live: the spell bars, from the character's .plr (characterFile) and saved back to it when
    /// the game is left; without one, the class's starting sets.
    nox::client::SpellBars bars;
    std::filesystem::path characterFile;
    bool barsExpanded = false;          ///< all five sets showing
    int barsClass = -1;                 ///< the class the bars were loaded for (ownClass)
    bool barsMulticlass = false;        ///< loaded for noxcpp's multiclass (a warrior's abilities tagged)
    bool barsInverted = false;          ///< InvertSpellTarget held
    std::uint8_t barsRowBefore = 0;     ///< the set in use before
    int lastSlot = -1;                  ///< the slot last used, lit for 10 frames (clientStoreLastButton)
    std::uint32_t lastSlotFrame = 0;
    int barFlash = -1;                  ///< the set arrow or all-sets button just clicked
    std::uint64_t barFlashMs = 0;
    /// The trap / bomber bar (casters): the tray open, the right end showing the trap or bomber,
    /// its slide off and back on when the Glyph is learned (quickbarAddTrap: one game pixel
    /// a frame down past the screen's bottom, then up), the button's lit moment.
    bool trapOpen = false;
    bool trapArt = false;
    int trapSlide = 0;
    int trapSlideDir = 0; ///< 1 going down, -1 coming back up, 0 still
    std::uint32_t trapSlideFrame = 0;
    std::uint64_t trapHitMs = 0;
    bool lastTargetFlag = false; ///< the last cast's target flag, which a trap is built with
    /// A slot's spell or ability being dragged (quickBarWnd): where from, and the press.
    struct BarDrag {
        int set = 0; ///< -1: the trap tray's set in use
        int slot = 0;
        nox::client::BarSlot what;
    };
    std::optional<BarDrag> barDrag;
    /// A spell clicked on a bar or in the book, waiting for a click in the world to cast it at
    /// (guiSpellSetCursor, the Target cursor), or an aimed ability.
    struct Armed {
        std::uint32_t id = 0;
        bool ability = false;
        bool trap = false; ///< a wizard's trap with an at-location spell, waiting to be placed (quickbarTrapButtonProc)
    };
    std::optional<Armed> armed;
    Anchors anchors;                  ///< this frame's drawables, for effects that follow them
    bool fxDemo = false;              ///< --fx-demo: the viewer plays a set of effects on a loop
    std::uint32_t fxDemoNext = 0;     ///< the frame of the next round
    std::uint32_t fxDemoFrame = 0;    ///< the last frame the demo sent its held beams
    std::string fxDemoOnly;           ///< --fx-demo-only: one group of effects
    LightMap light;
    bool lighting = true; ///< L toggles
    /// The frame-rate counter in the top right (--no-fps hides it; an Options setting later).
    bool showFps = true;
    /// Options' Smoothing: 0 Off, 1 Smooth Movement (eased to each new position), 2 Smooth Network
    /// (everything drawn between snapshots: the default), 3 Lock to 30 FPS (Off, drawn as
    /// snapshots come).
    int smoothing = 2;
    bool smoothMotion = true;    ///< Smoothing = Smooth Network: everything drawn between snapshots
    bool smoothScrolling = true; ///< Smoothing = Smooth Network: the camera (and us) on that clock too
    std::uint32_t lockedSnapshot = 0; ///< Lock to 30 FPS: the frame update the last frame drew (frameUpdateCount)
    std::uint64_t lockedAtMs = 0;     ///< and when
    bool predictMovement = false; ///< --predict-movement (an experiment; runs only with smoothMotion)
    int otherUnits = 0; ///< --other-units: 0 smooth, 1 newest, 2 ahead (Interpolator::Others; an experiment)
    /// The HD image pack Options' Use HD Images draws from (empty: none found): --image-pack, else
    /// video_hd.nxp beside noxcore.exe, else in the game folder.
    std::filesystem::path imagePack;
    std::uint64_t fpsSince = 0; ///< SDL_GetTicks() when the current count started
    int fpsFrames = 0;
    /// --frame-stats: every 5 s the frame times (average, 99th percentile, worst, how many over
    /// 33 ms) and the images made into textures, logged (performance checks).
    bool frameStats = false;
    std::vector<float> frameTimes;
    std::vector<float> worldDrawTimes; ///< --frame-stats: the 2D world draw's time on this thread, ms
    std::uint64_t frameStatsSince = 0;
    std::uint64_t lastPresentNs = 0;
    std::size_t madeBefore = 0;
    float fps = 0;           ///< frames a second over the last whole second
    float frameMs = 0;       ///< the average frame time over it
    std::optional<Rgb> ambient; ///< --ambient R G B: instead of the map's (tests of lights)
    SightMap sight;
    std::vector<SightMap::Blocker> sightBlockerList; ///< drawWorld's, kept between frames (its memory)
    /// V toggles; unset: on in a live game (the eye is our player), off in the viewer.
    std::optional<bool> lineOfSight;
    std::unordered_map<std::uint16_t, AnimTrack> animTracks; ///< live: when each drawable's state began
    /// Live: each NPC's player state (npcPlayerState) for the game frame it was worked out in;
    /// the game works it out as it draws, 30 times a second, its random picks kept in `choice`.
    struct NpcPick {
        std::uint32_t frame = ~0u;
        std::uint8_t state = 0;
        std::uint8_t choice = 0; ///< the random pick (the exe keeps it on the drawable)
    };
    std::unordered_map<std::uint16_t, NpcPick> npcPicks;
    /// Live: each BoulderDraw drawable's roll, as the game steps it each frame it draws it (on
    /// screen), stepped here at most once a game frame (`frame`: the last it stepped in).
    struct BoulderPick {
        std::uint32_t frame = ~0u;
        nox::client::BoulderRoll roll;
    };
    std::unordered_map<std::uint16_t, BoulderPick> boulderRolls;
    /// Live: this frame's overlays drawn over the scene, each noted by its drawable's scene item as
    /// the scene's sort draws the drawable (liveSprites' overlay): the friend dots, the flags whose
    /// team name is drawn, the drawables whose damage numbers are drawn.
    std::vector<MinionDot> minionDots;
    std::vector<std::uint16_t> flagLabels;
    std::vector<std::uint16_t> damageShown;
    /// Live: a script mode UI's world labels (UI API 3): the net codes it labels with each one's
    /// index among its labels, sorted by net code, set each frame before the scene
    /// (ModeUi::update); and those the scene's sort drew this frame (liveSprites' overlay), each
    /// with the label's index, the drawable's place and its top in world units.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> modeLabels;
    struct ModeLabelSpot {
        std::uint16_t index = 0;
        float x = 0, y = 0, top = 0;
    };
    std::vector<ModeLabelSpot> modeLabelSpots;
    /// Live: a script mode's draw scales (nox.setDrawScale, client level 5), sorted by net code,
    /// set each frame before the scene (ModeUi::update): a drawable of that code and type has
    /// its body drawn that much larger (liveSprites). Empty: nothing looked up.
    struct ModeDrawScale {
        std::uint16_t object = 0;
        std::uint16_t type = 0;
        float scale = 1;
    };
    std::vector<ModeDrawScale> modeDrawScales;
    std::unique_ptr<Font> font;
    bool namePlates = true; ///< N toggles
    std::uint32_t joinTeam = 0; ///< --team: asked for once we're in the game
    bool autoSpawn = false;     ///< --spawn: leave observer mode once in the game (for tests)
    std::optional<SDL_Point> autoGoto; ///< --goto X Y: then walk there (for tests)
    bool autoDone = false;
    bool noMotd = false; ///< --no-motd: the server's message of the day never shown (screenshots)
    bool traceSelf = false; ///< --trace-self: our own motion logged each render frame (tests)

    std::unique_ptr<Cursor> cursor;            ///< the game's cursor (the system one is hidden)
    CursorType cursorType = CursorType::Select; ///< live: this frame's
    /// Live: the drawable under the mouse furthest forward (the front pick: its ring, the arrow's
    /// colour, the aim and the spells' target).
    std::optional<std::uint16_t> hovered;
    /// Live: a useable map object under the mouse (a lever, switch, button), when no drawable is
    /// in front of it.
    std::optional<MapView::Useable> hoveredUseable;
    /// Live: the cursor's pick, the same but for Glyphs (only a wizard's, and only alone): what
    /// talking, trading, picking up and using go by. A drawable, or a useable map object.
    std::optional<std::uint16_t> cursorHovered;
    std::optional<MapView::Useable> cursorUseable;
    std::optional<SDL_FPoint> testMouse;       ///< --mouse X Y: the mouse there (tests; window pixels)
    bool pickupTest = false;                   ///< --pickup-test: walk to the nearest item, pick it up
    std::uint32_t castTest = 0;                ///< --cast-test N: cast spell N once, 2.5 s in
    bool online = false;            ///< the game was joined from the XWIS (WOL) list: the scoreboard's WOL ranks
    /// A solo game's award on its way from the book to the bars (bookDrawFn):
    /// two seconds' wait, a burst, then the icon along a curve to the slot.
    struct AwardFlight {
        std::uint32_t id = 0;
        bool ability = false;
        int row = 0, slot = 0, before = 0;
        std::vector<std::pair<SDL_FPoint, SDL_FPoint>> path; ///< the curve's pieces (19 of them)
        std::size_t piece = 0;
        SDL_FPoint pos{}, step{}, target{};
        std::uint32_t frames = 0; ///< client frames (30 a second) since it started
        bool burst = false;
    };
    std::optional<AwardFlight> flight;
    /// The character's file info the client holds (a solo save's File Info is
    /// written from it; a load reads it back). Unset: the profile's.
    std::optional<nox::player::FileInfo> heldFileInfo;
    std::uint32_t flightFrame = 0; ///< the last client frame (30 a second) it was stepped on
    bool forceAwardFlight = false; ///< --award-flight: fly awards as a solo game does (tests)
    std::uint32_t gameOversSeen = 0; ///< the World's game-over count the scoreboard has opened for
    /// The game-over banner (Gui_SetGameOverBanner): its words, GameDefeat rather than
    /// GameVictory, and when it went up (it's shown 10 seconds).
    struct GameOverBanner {
        std::string text;
        bool defeat = false;
        std::uint64_t shownMs = 0;
    };
    std::optional<GameOverBanner> banner;
    std::uint64_t leavingNoticeTestMs = 0; ///< console "leavenotice test": the leaving notice from then, no leave (tests)
    bool gameWasOver = false; ///< the World's game-over state last frame
    int fakeScreen = 0; ///< --fake-shop (1) / --fake-dialog (2) / --fake-sell (3, then 4: an offer) / --fake-quest-keys (7) / --fake-game-over (8) / a mode's own (kFakeModeScreen: fakeModeFlag: its --fake-* flags): shown locally, 2 s in (tests)
    static constexpr int kFakeModeScreen = 100;
    bool holdMoveTest = false;                 ///< --hold-move: the move button held from 1.5 s in
    std::string chatTest;                      ///< --chat-test TEXT: says it (Enter, TEXT, Enter), 2.5 s in
    std::string consoleRun; ///< --console "cmd; cmd": the console opened and these run (in a game, once in)
    std::uint64_t consoleWaitUntil = 0; ///< --console's "wait MS": the rest runs from then
    bool testMoving = false;                   ///< set by --hold-move once it has pressed the button
    std::uint64_t holdMoveLogMs = 0;
    std::optional<std::uint16_t> pickupTarget;
    std::uint64_t pickupSentMs = 0;
};

/// The game frame being drawn (the interpolated clock's), whole.
inline std::uint32_t gameFrame(const App& app)
{
    return static_cast<std::uint32_t>(app.interpolator.gameTime());
}

/// Our own player's entry; nullptr till the server has listed us.
inline const nox::client::PlayerEntry* ownPlayer(const nox::client::HeadlessClient& client)
{
    const auto& players = client.world().players();
    const auto self = players.find(client.netCode());
    return self != players.end() ? &self->second : nullptr;
}

/// We're observing (our player's status bit 0).
inline bool observing(const nox::client::HeadlessClient& client)
{
    const auto* self = ownPlayer(client);
    return self && (self->status & 1);
}

/// We're in play: listed, neither observing nor dead.
inline bool playing(const nox::client::HeadlessClient& client)
{
    const auto* self = ownPlayer(client);
    return self && !(self->status & 1) && !self->dead;
}

// app.cpp
void intoGameArea(const App& app, float& x, float& y);
SDL_FPoint mousePos(const App& app);
std::optional<std::string> tableText(const App& app, std::string_view id);
std::uint16_t typeIdOf(const nox::things::ThingDb& things, std::string_view name);
std::string resolveStringId(const App& app, const std::string& text);
std::string chatLine(const App& app, const std::string& text, bool team);
std::filesystem::path resolveMap(const std::filesystem::path& gameDir, const std::string& arg);
std::size_t pickVariant(App& app, const nox::strings::StringEntry& e);
std::string gameString(App& app, std::string_view label);
void centredMessage(App& app, const std::string& text);
bool soloGame(App& app);
std::uint32_t gameFlags(App& app);
int ownAnimState(App& app);
bool ownDeadFlag(App& app);
bool menuWindowUp(const App& app);
void uiSound(App& app, std::uint32_t id, int volume = 100);
gui::Resources guiResources(App& app);
void playMovie(App& app, const std::string& name, movies::Skip skip, std::function<void()> next);
std::uint32_t clockFrame();

// app_settings.cpp
std::optional<std::pair<int, int>> parseSize(std::string_view s);
void applySettings(App& app, const nox::config::Config& config);
void saveSettings(App& app);
void storeConfig(App& app, const nox::config::Config& c);
OptionsWindow::Extras loadExtras(const std::filesystem::path& gameDir);
void saveExtras(const App& app, const OptionsWindow::Extras& e);
OptionsWindow::Extras currentExtras(const App& app);
void applyMouse(App& app);
void handMouseTo(App& app, MouseOwner owner);
void setOtherUnits(App& app);
void preloadMap(App& app, const std::string& name);
void preloadSession(App& app);
void applyExtras(App& app, const OptionsWindow::Extras& e);
void openOptions(App& app);
int trackData(App& app);
void setTrackData(App& app, int mask);
float viewZoom(const App& app);
void stepWindowSize(App& app, int by);
SDL_FRect gameWindowBox(const App& app);

// game_windows.cpp
void makeGameWindows(App* app, const gui::Resources& res);

// console_commands.cpp
std::vector<Console::Command> consoleCommands(App& app);

// hosting.cpp
void postToXwis(App& app);
void startXwisPost(App& app, const nox::io::SavedLogin& login);
HostListing menusHostListing(const App& app, bool listed);
void setLocalBots(App& app, const std::array<int, 3>& wanted, int minTier, int maxTier);
ServerOptions::BotsView localBotsView(App& app);
std::unique_ptr<LocalHost> makeHost(App& app, const HostSettings& settings);
void notePortMoved(App& app);
void hostSettingsFromConfig(const App& app, HostSettings& s);
void startPortMapping(App& app, bool network);
void tickPortMapping(App& app);
void tickLocalBots(App& app);
void tickHost(App& app);
nox::player::Music soloMusic(App& app);
void newSoloCharacter(App& app, const nox::client::PlayerProfile& profile, const std::string& map);

// game_session.cpp
void sendChat(App& app, const std::string& typed, bool team);
void clientQuit(App& app);
void joinLive(App& app, const std::string& host);
void startLive(App& app, const std::string& host);

// quickbar.cpp
void noteCast(App& app, std::uint32_t spell);
std::uint8_t ownClass(const App& app);
bool multiclass(const App& app);
bool isWarrior(const App& app);
BookContext bookContext(const App& app);
QuickbarState quickbarState(const App& app);
void castSlot(App& app, std::size_t i);
void selectSpellSet(App& app, int set);
void turnSpellSet(App& app, int by);
void expandBars(App& app, bool on);
void buildTrap(App& app);
void updateTrapBar(App& app);
void drinkPotion(App& app, int kind);
void fillBars(App& app, std::uint32_t id, bool ability);
void landFlightAtOnce(App& app);
void stepFlight(App& app);
void drawFlight(App& app);
void bookEvents(App& app);
void loadBars(App& app);
void saveBars(App& app);
bool leftMouse(App& app, bool down, float x, float y);

// live_scene.cpp
void syncWalls(App& app);
/// The live drawables' sight blockers, added to `out`.
void sightBlockers(App& app, std::vector<SightMap::Blocker>& out);
std::vector<MapView::SceneSprite> liveSprites(App& app, std::vector<SDL_FPoint>& unsprited, std::vector<HealthBar>& bars);
void fxDemo(App& app, std::uint32_t frame, std::vector<MapView::SceneSprite>& extra);
void soundLoops(App& app, const std::set<std::uint32_t>& liveCodes);
std::vector<LightSource> frameLights(App& app, const std::set<std::uint32_t>& liveCodes);

// overlays.cpp
void drawFps(App& app);
bool hoveredEnemy(const App& app);
std::optional<View3D::Ring> hoverRing(App& app);
void drawHoverRing(App& app);
void drawKillFeed(App& app);
void drawNamePlate(App& app, const NamePlate& plate);
void drawFlagLabels(App& app);
std::optional<App::GameOverBanner> gameOverBanner(const App& app);
void drawGameOverBanner(App& app);
void drawHealthBars(App& app, const std::vector<HealthBar>& bars);
void drawCreatureBar(SDL_Renderer* r, const Camera& cam, const CreatureBar& bar, float fx, float fy, bool poisoned);
void drawMinionDots(App& app);
void drawDamageNumbers(App& app);
void drawChatBubbles(App& app, std::uint32_t frame);
std::string liveTooltip(App& app);
void drawTooltip(App& app, bool below = false);

// test_drive.cpp
void pickupTest(App& app);
void fakeScreen(App& app, int which);
SDL_Event keyEvent(SDL_Keycode key, SDL_Scancode scancode, bool down = true);
void pushKeyTap(SDL_Keycode key, SDL_Scancode scancode);
SDL_Event scriptedKey(const App::ScriptedMouse& m);
SDL_Event scriptedMouseEvent(const App::ScriptedMouse& m);
void runGameTests(App& app, bool inGame);

} // namespace noxsdl
