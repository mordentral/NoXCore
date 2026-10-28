// The game's screen-wide effects and briefing screens (GAME.EXE's client side of a solo game's
// cinematics), and the host's own client (noxcore server/host_client.hpp) on them.
//
//   records     four screen records (one call takes a free one, one ends
//               those of a kind, calling their callbacks, one finds one), each run once a
//               client frame (30 a second) by their runner: those under the GUI at the end of the
//               world's drawing (only while the map is drawn), those over it (flag 4) at
//               the very end of the frame, over the pointer too (mainloop). A record
//               counts its frames down, then ends (its callback run) unless it lasts (flag 2):
//                 to black    Draw_FadeInScreen: the whole frame darkened by a
//                             level rising 0-255 in 16.16 steps of 0xFF0000 / frames (each colour
//                             less the level, kept at 0);
//                 from black  Draw_FadeOutScreen: the same by 255 less the
//                             level; starting one ends the map intro's black;
//                 the bars    bars in (16, 10): opaque black bars at the top and
//                             bottom growing to 16% of the back buffer's height over 10 frames,
//                             and staying; bars out (16, 30): the bars-in record
//                             ended, the bars shrinking back over 30;
//                 the intro   the whole frame opaque black, lasting
//                             till a fade from black ends it (under the GUI, or over everything
//                             for ImmediateBlind's and the ending's).
//   map drawing gameDisableMapDraw: off, the world isn't drawn (the view black)
//               and the records under the GUI don't run; the GUI still is.
//   widescreen  the HUD and its windows hidden, and the controls the
//               exe refuses meanwhile refused (refuses()).
//   messages    MSG_USE_MAP 0x2B: the map's change (gameChangeMap once it's loaded):
//               the map off and blackScreen(1)'s black over everything till the fade in.
//               MSG_FADE_BEGIN 0xE4 {1, menu}: the map drawn, our host paused, a fade from black
//               over 25 frames (unpaused at its end); {0, menu}: a fade to black,
//               the map off after it. MSG_CHAPTER_END 0xD6 {chapter, kind}:
//               Gui_Brief_ShowLockScreen.
//   briefings   Briefing.wnd over a black screen: the chapter's picture and text (the briefing's
//               table: "Briefing:<Class>ChapterBegin<n>" / "...ChapterLoss<n>", their voices, the
//               music 1, then 4 / 2 / 3 by class, then the chapter + 3), faded in from black; the
//               voice once in; then after the voice and 15 s, or a key or click, a startup screen
//               fades to black and back to the game (our host unpaused at the end),
//               a death screen opens the load menu (one seen before in this game goes straight to
//               it), the ending's credits ("Nox:Credits" rolling up a pixel a frame, an endgame
//               voice by the classes finished) end the game. The main menu's Credits shows the
//               same credits (showCredits: no ending, so no voice), on a Cinematics of its own.
//   save menu   GUISave.c on selchar.wnd (savegame_sub, after a death or from the
//               in-game menu's Save): the 14 slots (AUTOSAVE, SAVE0001-13: solo_saves.hpp), the
//               newest selected; Save (off after a death screen or while dying / dead) asks our
//               host for the slot's save (an empty slot at once, a saved one after "Overwrite
//               save game?"), Load (asked first while alive: on our host, the map
//               intro, the fade out of a briefing), Cancel / Quit; paused while it's up. The
//               message boxes are Gui_Dialog_Create's (one at a time).
//   quest       MSG_GAUNTLET's screens (0xF0, as the briefing's chapter 0xFE, no voice, no music,
//               nothing seen before): 0x0E the quest's start (Gui_Brief_ShowQuestStart:
//               GauntletInstructionBackground, how to play), 0x0D the next stage's
//               picture and text (Gui_Brief_ShowQuestNextLevel; shown at once only with
//               its flag 1: "Stage: n" and the text), 0x0C a level won
//               (Gui_Brief_ShowQuestLevelWon: MenuSystemBG, up to six players' tallies).
//               "Click to Continue" blinks a second on, a second off. The start's
//               and the results' screens wait for a key or click, the stage's for 15 s or one;
//               after the first two the stage's comes, after it the game.
//   saves       the host's client's own .plr part (an award still flying landed,
//               then the GUI, File Info and Music sections) and its read at a load
//               (Plr_LoadClientPlayerData), through HostClient; the in-game menu's and the keys'
//               AutoLoad question.
#pragma once

#include "audio.hpp"
#include "gui.hpp"
#include "thing_art.hpp"

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/mode_hooks.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

class Cinematics {
public:
    struct Hooks {
        /// The HUD and its windows hidden (true) or back (false).
        std::function<void(bool hide)> hideHud;
        /// The game we host, a solo one sharing GAME.EXE's pause: Game_SetPaused(1) (1), (0) (0),
        /// or -1 (the timed pause forgotten, then unpaused), and why (a NoX Core mode's say:
        /// ModeHooks::mayPause / mayUnpause). Empty: not hosting.
        std::function<void(int how, nox::server::PauseReason why)> hostPause;
        /// Runs on our host's side before its next tick (a server call's `done`); empty: at once.
        std::function<void(std::function<void()>)> toHost;
        /// The saved game `slot` (AUTOSAVE, SAVE0001...) loaded on our host.
        std::function<bool(const std::string& slot)> loadGame;
        /// Leaving the game (client_quit, Game_ExitMainLoop).
        std::function<void()> quitGame;
        /// nox.cfg's TrackData (the classes whose ending was seen).
        std::function<int()> trackData;
        std::function<void(int)> setTrackData;
        /// The ending movie (movies\<Class>.vqa) played, then `done` (the
        /// credits); straight to `done` without it. Empty: no movie.
        std::function<void(const std::string& name, std::function<void()> done)> playMovie;
        /// Called as it goes (the host's client detached).
        std::function<void()> detach;
        /// An award flying from the book to the bars (HostClient::bookFlying).
        std::function<bool()> bookFlying;
        /// The inventory's cells with room for `count` more of a type
        /// (HostClient::inventoryRoom).
        std::function<int(std::uint16_t typeId, int count)> inventoryRoom;
        /// The inventory window as the .plr's Inventory section reads it
        /// (HostClient::inventoryWindow).
        std::function<nox::server::HostClient::InventoryWindow()> inventoryWindow;
        /// A solo .plr read clears the window's secondary weapon and last quiver
        /// (HostClient::clearInventoryWeapons).
        std::function<void()> clearInventoryWeapons;

        /// The minimap's memory a solo save keeps (HostClient::wallDrawn / minimapMarked).
        std::function<bool(int x, int y)> wallDrawn;
        std::function<bool(std::uint16_t netCode)> minimapMarked;

        // The save menu and the client's own part of a solo save (GUISave.c).
        /// Our player's drawable's animation state (1, 2, 0x33 dying or dead,
        /// playerAnimCheck); -1 without one.
        std::function<int()> animState;
        /// Our drawable's flags (MSG_REPORT_PLAYER_STATUS) have DEAD (0x8000).
        std::function<bool()> deadFlag;
        /// setSaveFileName(name) and requestSave(1, 0, 0) on our host.
        std::function<void(const std::string& name)> requestSave;
        /// A flying award landed at once.
        std::function<void()> landFlight;
        /// Gui_CancelDragAndDrop and quickBarClose.
        std::function<void()> cancelDrags;
        /// The spell bars as the GUI section holds them (plr_rwSpellBars, the set and
        /// trap set chosen; the summon order is the server's).
        std::function<nox::player::Gui()> bars;
        /// The bars cleared, and a GUI section read back into them.
        std::function<void()> clearBars;
        std::function<void(const nox::player::Gui&)> setBars;
        /// The character's file info the client holds (the profile's at first).
        std::function<nox::player::FileInfo()> fileInfo;
        std::function<void(const nox::player::FileInfo&)> setFileInfo;
        /// The briefing byte = the chapter + 1 (Gui_Brief_ShowLockScreen's, read by the
        /// server's save: World::briefingByte).
        std::function<void(std::uint8_t)> setStage;
        /// The console's lines cleared.
        std::function<void()> clearConsole;

        // The quest's screens.
        /// client_resetScreenParticles and bookHideMB(1).
        std::function<void()> hideBook;
        /// Plr_GetByNetCode's player's name; nullopt: no such player.
        std::function<std::optional<std::string>(std::uint16_t netCode)> playerName;
        /// player_netCode: ours.
        std::function<std::uint16_t()> ownNetCode;
    };

    Cinematics(gui::Resources res, std::filesystem::path gameDir, Audio* audio, Hooks hooks);
    ~Cinematics();
    Cinematics(const Cinematics&) = delete;
    Cinematics& operator=(const Cinematics&) = delete;

    /// The host's own client, for GameHost::setHostClient.
    nox::server::HostClient& hostClient();

    /// A game message the client received (its tracer): MSG_FADE_BEGIN and MSG_CHAPTER_END are
    /// taken, at the next update.
    void message(std::span<const std::uint8_t> m);
    /// The messages taken and not yet handled handled now (before the calls our host queued after
    /// them run: LocalHost keeps their order).
    void flushMessages();

    /// Each frame: the client frames (30 a second) due run. `inGame`: the world view is up;
    /// `backbufferHeight` the game's resolution's height; `playerClass` ours (0-2).
    void update(std::uint64_t nowMs, bool inGame, int backbufferHeight, int playerClass);

    /// gameDisableMapDraw off.
    bool mapDrawn() const { return mapDrawn_; }
    /// The widescreen on (guiCursor).
    bool wideScreen() const { return wideScreen_; }
    /// A briefing shown, till the fade back from it.
    bool briefingShown() const { return briefed_; }
    /// A briefing or the load menu is up (it takes the input).
    bool screenUp() const;
    /// Whether the game refuses `control` now (the widescreen's, a fade's Escape).
    bool refuses(std::string_view control) const;

    /// The records under the GUI, at the world's end (`width` x `height`: the game's area).
    void drawUnderGui(SDL_Renderer* renderer, int width, int height) const;
    /// The briefing and the load menu.
    void drawScreens(SDL_Renderer* renderer, int width, int height);
    /// The records over everything (after the pointer).
    void drawOverAll(SDL_Renderer* renderer, int width, int height) const;
    /// While a screen is up it takes every event; returns whether it took this one.
    bool handleEvent(const SDL_Event& event, int width, int height);

    /// The map editor's Stop out of a session's game (it runs on behind the editor): what a
    /// cinematic left on the screen dropped, so the next Play starts clear: the fades and black
    /// ended (their callbacks run: the server's waits with them), the bars gone, the HUD back.
    void clearScreen();

    /// The main menu's Credits (windowMainMenuProc's 0x7A): the briefing window made and
    /// Gui_Brief_ShowLockScreen(0xFF, 1, 0), the ending's credits screen with no ending behind it
    /// (the ending's kind -1: GetEndgameDialog gives no voice). A black screen, "Nox:Credits"
    /// rolling up from 500 a pixel a client frame till 20 above the top, the credits music;
    /// faded in, a key or click (once in) or the roll's end fades out, and back from black.
    /// Menus runs it on an instance of its own (no hooks: nothing to quit at the end).
    void showCredits() { showBriefing(0xFF, 1); }

    /// The in-game menu's Save in a solo game (menuGameOnButton's 0x232B): the save
    /// menu (savegame_sub).
    void openSaveMenu() { openLoadMenu(); }
    /// The folder the saved games' Save\ is under (default: the game's; noxsdl's --save-dir).
    void setSaveRoot(std::filesystem::path root) { saveRoot_ = std::move(root); }
    /// (noxcpp: a campaign mode's guest, set by its UI) A chapter's loss screen ends as a
    /// start's does, faded back to the game, not in the load menu (the host loads); and isn't
    /// skipped for being seen before.
    void setGuest(bool guest) { guest_ = guest; }
    /// The in-game menu's AutoLoad (0x2329, after the menu's closed): paused; alive, asked first
    /// (SelChar.c:LoadLabel / GUIQuit.c:ReallyLoadMessage, Yes / No);
    /// dying or dead at once (unpaused, AUTOSAVE loaded, an error box if not).
    void autoLoadFromMenu();
    /// The AutoLoad key (Cli_ProcessControlEvents' 0x37, game allowing): ShellClick,
    /// paused, asked (yes: unpaused and AUTOSAVE loaded; no: unpaused).
    void autoLoadKey();
    /// Gui_Dialog_Create(0, title, text, 0x21, 0, 0): a message box with OK, closed after it
    /// (e.g. CliNet_OnInform's 0x11).
    void notice(const std::string& title, const std::string& text) { dialog(DialogParent::None, title, text, 0x21, {}, {}); }
    /// The client's .plr part after a network game's SAVEDATA was kept: the client's
    /// sections appended to `plr`, its File Info's flags and the Music section by `gameFlags`
    /// (in the network: 4 a quest's, the Music's byte 0 and nothing more).
    bool appendNetworkSave(const std::filesystem::path& plr, const nox::server::HostClient::ClientSave& info,
                           std::uint32_t gameFlags)
    {
        return saveClientSections(plr, info, gameFlags);
    }

private:
    class Link;

    enum class Fx : std::uint8_t {
        None,
        ToBlack,
        FromBlack,
        Bars,
        BarsOut,
        Black,
    };
    struct Record {
        std::uint32_t flags = 0; ///< 1 running, 2 lasting, 4 over the GUI
        std::int32_t frames = 0; ///< [1]
        std::int32_t acc = 0;    ///< [2] 16.16
        std::int32_t step = 0;   ///< [3]
        std::int32_t once = 0;   ///< [4]
        std::function<void()> done; ///< [5]
        Fx fx = Fx::None;           ///< [6]
    };
    struct Drawn {
        Fx fx = Fx::None;
        int value = 0; ///< a darkening level, or the bars' height in back-buffer pixels
    };

    // The records and their runner.
    Record* freeRecord();
    bool end(Fx fx);
    const Record* find(Fx fx) const;
    void run(int pass);
    void drawRecord(Record& r, std::vector<Drawn>& out);
    void drawList(SDL_Renderer* renderer, int width, int height, const std::vector<Drawn>& list) const;

    // The client's calls.
    void fadeToBlack(int frames, bool menu, std::function<void()> done); ///< Draw_FadeInScreen
    void fadeFromBlack(int frames, bool menu, std::function<void()> done); ///< Draw_FadeOutScreen
    bool barsIn();  ///< (0x10, 10): whether it got a record
    bool barsOut(); ///< (0x10, 0x1E): whether it got a record
    void mapIntro(bool immediate); ///< cliPlayMapIntro
    void blackScreen(bool overAll);
    void stopAllFades();            ///< video_stopAllFades (but its Game_SetPaused(0))
    void setHud(bool hidden);
    void blind();
    void endGame(int kind);
    void pauseHost(int how, nox::server::PauseReason why);
    void clientFrame(bool inGame);
    void handle(const std::vector<std::uint8_t>& m);

    // The briefing (GUIBrief.c).
    void showBriefing(int chapter, int variant, int questMode = 0); ///< Gui_Brief_ShowLockScreen
    void questBriefing(const std::vector<std::uint8_t>& m);         ///< Gui_Brief_ShowQuestStart / NextLevel
    void questLevelWon(const std::vector<std::uint8_t>& m);         ///< Gui_Brief_ShowQuestLevelWon
    void questBlink();                                              ///< the blinking line's flag
    void drawQuestStart(gui::Painter& p);
    void drawQuestStage(gui::Painter& p);
    void drawQuestResults(gui::Painter& p);
    void drawThing(gui::Painter& p, const char* type, int x, int y); ///< a drawable of the type at a screen point
    void briefingVoice();                        ///< playGMCAPsmth
    void stopBriefing();
    void briefingFrame();                        ///< the briefing draw's per-frame part
    void briefingInput();                        ///< client_wndQuestBriefProc
    void fadeOutOfBriefing();
    void briefingOver();
    std::string endgameVoice() const;            ///< GetEndgameDialog
    void drawBriefing(SDL_Renderer* renderer, int width, int height);

    // The save / load menu (GUISave.c on selchar.wnd).
    void openLoadMenu();  ///< savegame_sub
    bool closeLoadMenu(); ///< (whether it was open)
    void fillLoadMenu();  ///< savegame_sub
    bool onLoadMenu(gui::Window& from); ///< savegame_sub
    void loadSlot();      ///< savegame_sub
    int selected() const; ///< the names list's selection (list 0x1FF)
    /// Whether our player is dying or dead (playerAnimCheck).
    bool dyingOrDead() const;
    /// Unpaused, AUTOSAVE loaded, an error box (OK, auto-close) if it failed.
    void autoLoadNow();

    // The message box (Gui_Dialog_Create / Gui_Dialog_Close, dlg.wnd): one at a
    // time; a new one first runs the old one's cancel and then takes its window. Flags: 1 OK,
    // 2 Cancel, 8 Yes, 0x10 No, 0x20 closed after its answer. `parent` (the save menu or none)
    // hears the click (its procedure's ButtonPress).
    enum class DialogParent : std::uint8_t { None, SaveMenu };
    void dialog(DialogParent parent, const std::string& title, const std::string& text, std::uint32_t flags,
                std::function<void()> onOk, std::function<void()> onCancel);
    void dialogClose();
    void dialogAnswer(bool ok);

    // The client's own .plr part (Plr_LoadClientPlayerData).
    bool saveClientSections(const std::filesystem::path& plr, const nox::server::HostClient::ClientSave& info,
                            std::uint32_t gameFlags = 0);
    bool loadClientSections(const std::filesystem::path& plr, nox::server::HostClient::ClientLoad& out);

    std::string text(const std::string& id) const;

    gui::Resources res_;
    ThingArt art_{res_.things};
    std::filesystem::path gameDir_;
    std::filesystem::path saveRoot_; ///< (empty: gameDir_)
    Audio* audio_ = nullptr;
    Hooks hooks_;
    std::unique_ptr<Link> link_;

    std::array<Record, 4> records_{};
    std::vector<Drawn> under_, over_; ///< what this client frame drew
    bool mapDrawn_ = true;
    bool wideScreen_ = false;
    int backbufferHeight_ = 768;
    int playerClass_ = 0;
    std::uint64_t nowMs_ = 0;
    std::uint64_t lastFrame_ = 0;
    std::deque<std::vector<std::uint8_t>> messages_;

    // The widescreen's memory: what was open.
    bool hudHidden_ = false;

    struct Briefing {
        bool shown = false;      ///< shown
        bool proc = false;       ///< its window procedure set (the input taken)
        bool textDrawn = true;   ///< (else nothing)
        int chapter = 0;
        int variant = 0;         ///< g_briefVariant
        std::string image;
        std::string text;
        std::string voice;
        float y = 0;             ///< the text's top in the 640 x 480
        int stop = 0;            ///< where it stops
        int height = 0;          ///< the text window's height (measured at 480 wide)
        bool waiting = false;    ///< g_briefWaitingForInput
        bool voiced = true;
        bool input = false;      ///< the fade in over, input taken
        bool finished = false;
        bool loadOpened = false;
        std::uint64_t since = 0;
        int questMode = 0;       ///< g_briefQuestMode: 1 results, 2 a stage, 4 the start
    } brief_;
    struct Quest {
        std::string image = "WarriorChapterBegin8";
        std::string text;
        int stage = 0;
        bool bossSound = false;                     ///< g_questBriefFlag2
        struct Entry {
            bool player = false; ///< Plr_GetByNetCode found it
            std::string name;
            std::uint16_t netCode = 0;
            std::uint16_t kills = 0, generators = 0, secrets = 0, credited = 0;
            std::uint32_t score = 0;
        };
        std::array<Entry, 6> won{}; ///< g_questWinPlayers
        int secretsTotal = 0;
        int wonStage = 0;
        int labelWidth = 0;         ///< g_questWinLabelWidth
        /// The three draw functions' own blink flags, and whether this frame shows the line.
        std::array<bool, 3> blink{true, true, true};
        bool blinkShown = false;
    } quest_;
    std::uint64_t clientFrames_ = 0; ///< gameFrame__
    /// g_questBriefingNoSkip (MSG_GAUNTLET 0x1C, Cli_SetQuestBriefingNoSkip: the server sending
    /// our quest save; 0 at a game's start): 1, the briefing's input refused.
    std::uint8_t noSkip_ = 0;
    bool briefed_ = false;       ///< a briefing shown, till the fade back from it
    std::array<bool, 256> seen_{}; ///< each chapter's loss screen seen (the exe keeps it on the player)
    bool guest_ = false;           ///< (setGuest)
    int endKind_ = -1;

    gui::Gui gui_;
    gui::Window* loadMenu_ = nullptr;
    gui::Window* dialog_ = nullptr;
    struct DialogState {
        DialogParent parent = DialogParent::None; ///< g_guiDialogParent
        std::uint32_t flags = 0;
        std::function<void()> onOk, onCancel;      ///< g_guiDialogOnOk / OnCancel
    } dialogState_;
    std::vector<nox::client::solo_saves::Slot> slots_; ///< savegame_arr
    std::string overwrite_;                            ///< the slot to save over
    std::vector<std::function<void()>> deferred_;
    /// Held by what may call back later (the ending movie's end): gone with this.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace noxsdl
