// The game's front-end menus, from its window files and run as GAME.EXE runs them: a stack of
// screens (its state codes), each sliding in, and sliding out before the next one appears.
//
//   100    main menu (game_showMainMenu, MainMenu.wnd): its halves slide in from
//          above and below. Multiplay goes on to 400; Solo (windowMainMenuProc's
//          0x6F, game flag 0x800) to 500 with a saved game (client_countSaveFiles:
//          AUTOSAVE's, an original bug), else to 600 (client/solo_menu.hpp).
//   400    Westwood Online or network (ArnaMain.wnd). Westwood Online logs in first
//          (1700); Network goes straight to 10000 without a login, as before. Network's game
//          list is LAN play's, as the game's: the LAN's games (and servers.txt's, after them),
//          never XWIS's. Westwood Online's lists XWIS, plus the LAN and servers.txt.
//   1700   the XWIS login (wollogin.c, wolapi.wnd): the saved logins (WOLAPI's
//          Wchat\Nick1-128, io/wol_login.hpp) in the list, a pick filling the name, the password
//          (saved encoded) and "Save This Login?"; Login logs in to XWIS (lobby/wol_chat), saves
//          the login if asked and goes on to 1900 with the nick as our account name, the
//          connection kept. Register is "Skip Login" here (XWIS makes an account on its first
//          login): on to 10000 without one, joining with no account name (not even a saved
//          login's) and listing XWIS through a throwaway login. Bots log in their own way (a
//          throwaway listing nick), untouched.
//   1900   the WOL chat lobby (wolchat.c, wolchat.wnd; menus_wolchat.cpp): the lobbies,
//          one joined, its users and chat; Games goes on to 10000 (listing XWIS through the same
//          connection), Back logs off to 1700.
//   10000  the game browser (game_showGameSel, noxworld.wnd): the world map and
//          its game list are replaced by our game list, drawn in the same frame. Joining a
//          game goes on to 500 when there are saved characters.
//   500    character select (game_showSelChar, SelChar.wnd): the Save folder's
//          characters that suit the game (findAutosaves: quest ones for a quest game,
//          the others otherwise); Load joins the game as the one picked, Delete asks and
//          deletes, New goes on to 600. Joining with none goes straight to 600. Solo: the 14
//          saved games (savegame_sub, "LoadLabel", no arrows); Load continues the one
//          picked (windowSelCharProc's 0x1F6), Delete deletes it (not AUTOSAVE).
//   600    class (game_showSelClass, SelClass.wnd): pointing at a portrait picks
//          that class; Accept goes on to 700.
//   700    looks and name (game_showSelColor, SelColor.wnd): skin, hair and
//          facial hair from the game's palettes, clothes from modifier.bin's UserColor1-32, a
//          colour picker for each, the name. Accept saves the character to Save\ (saveNewCharacter)
//          and joins the game as it; solo, to Save\WORKING\Player.plr, and starts the class's
//          first map.
//
// Behind them all: MainBG.wnd (the background) and OptsBack.wnd (the Options / Back bar, "Quit"
// on the main menu), loaded once (wndLoadMainBG). Back and Escape return to the previous
// screen (game_checkStateSwitch); on the main menu they quit.
//
// The main menu's Intro plays movies\Intro.vqa (callbacks' intro: the movie player). Its Credits
// (windowMainMenuProc's 0x7A) hides the menu behind the ending's credits screen (a Cinematics of
// its own: Cinematics::showCredits), black with "Nox:Credits" rolling up and the credits music,
// till a key or click or the roll's end; then the menu as it was, faded back from black, its
// music back.
// Not yet: Solo Quest, hosting, filters and Find (their buttons are off).
#pragma once

#include "browser_screen.hpp"
#include "gui.hpp"
#include "mode_list.hpp"

#include "noxcore/client/headless_client.hpp"
#include "noxcore/client/solo_menu.hpp"
#include "noxcore/client/solo_saves.hpp"
#include "noxcore/io/wol_login.hpp"
#include "noxcore/lobby/wol_chat.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/things/modifier.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace noxsdl {

class Audio;
class Cinematics;

class Menus {
public:
    /// The join callback's host for the Host button: host our own game, then join it.
    static constexpr std::string_view kHostGame = "@host";
    /// The join callback's host for the Host Quest button (0x2713): host a quest here, others
    /// may join, then join it.
    static constexpr std::string_view kHostQuestGame = "@hostnetquest";
    /// Host Custom's Map Editor: a collaborative edit session hosted here (edit_session.hpp: the
    /// editor's session dialog picks the map and settings), others may join and play it while the
    /// map is edited, then join it (a network character; not a quest game).
    static constexpr std::string_view kHostEditGame = "@hostnetedit";
    /// The main menu's Solo Quest (windowMainMenuProc's 0x83): host a quest here, then join it.
    static constexpr std::string_view kSoloQuestHost = "@hostquest";
    /// A NoX Core mode (noxcore/modes' registry: built in, or a mod's) hosted here, then
    /// joined with a network character: Host Custom's listed for others (the router's port, XWIS
    /// when logged in), or the main menu's Game Modes' local and unlisted (the end back to the
    /// main menu). Its settings: pendingModeOptions().
    struct ModeHost {
        std::string id;
        bool local = false;
    };
    static std::string modeHostKey(std::string_view modeId, bool local);
    /// The mode a join callback's host names (none: not a mode's).
    static std::optional<ModeHost> modeOfHostKey(std::string_view host);
    /// The settings Host Custom's list had for the mode it's hosting.
    const nox::modes::OptionValues& pendingModeOptions() const { return pendingModeOptions_; }
    /// A solo game to start.
    struct SoloStart {
        nox::client::PlayerProfile profile;
        nox::player::FileInfo info;          ///< the client's File Info held
        std::filesystem::path characterFile; ///< clientCurSave: its Player.plr (the bars)
        std::filesystem::path map;           ///< the map the game starts on
        std::string continueSlot;            ///< the saved game continued (windowSelCharProc); empty: new
    };
    struct Callbacks {
        /// Join "a.b.c.d:port" as this character (kHostGame: host one here first).
        std::function<void(const std::string& host, const nox::client::PlayerProfile& profile)> join;
        std::function<void()> quit;
        std::function<void()> options; ///< the Options bar's button (options_window.hpp)
        /// Logged in to XWIS as this nick (now our account name).
        std::function<void(const std::string& nick)> loggedIn;
        /// Start a solo game (the character screen's Load, or a new character's Accept).
        std::function<void(const SoloStart& start)> solo;
        /// Host Custom > a mode started through the solo screens (a campaign mode: mode_ui.hpp's
        /// SoloScreensStart): host its game for others to join, as the solo screens started it
        /// (their saves the mode's: modeSaveRoot).
        std::function<void(const std::string& modeId, const SoloStart& start)> soloScreensMode;
        /// The music's state now (plr_rwMusic's write), for a new solo character's file.
        std::function<nox::player::Music()> music;
        /// The main menu's Intro (windowMainMenuProc's 0x79): whether movies\Intro.vqa is there
        /// (game_setMovieFile), and playing it (the movie player), then `after`.
        std::function<bool()> introAvailable;
        std::function<void(std::function<void()> after)> intro;
        /// Game Modes' Map Editor (noxcpp's): the single-player map editor.
        std::function<void()> editor;
    };

    /// `gameDir` is the Nox folder (its window and Save folders). `profile` supplies what the
    /// menus don't choose (the serial and such). `modifiers` gives the clothes' colours.
    Menus(gui::Resources res, std::filesystem::path gameDir, BrowserScreen& browser,
          nox::client::PlayerProfile profile, const nox::things::ModifierDb& modifiers, Callbacks callbacks);
    ~Menus();
    Menus(const Menus&) = delete;
    Menus& operator=(const Menus&) = delete;

    void showMainMenu();
    /// Straight to the game browser (main menu and online / network on the back stack).
    void showGameBrowser();
    /// A notice over the screen (Gui_Dialog_Create with flags 0x21: OK), its title and text string
    /// ids ("Noxworld.c:Notification", "Noxworld.c:Timeout": Gui_ShowTimeoutNotice's).
    void notice(const std::string& titleId, const std::string& textId);
    /// noxcpp's: lobby::kClientTooOldNotice in that notice (client_level_gate.hpp), for a game that
    /// wants a newer NoX Core.
    void tooOldNotice();
    /// The join's password asked for (the lobby's PASSWORD_REQUIRED, which our join had none to
    /// answer; `wrong`: the one given was refused): the game's dialog, noxworld.c's Password and
    /// PasswordRequired (Gui_Dialog_Create's 7: OK, Cancel, the entry; OK only with something
    /// typed); OK joins `host` (the game list's game just left) again with `profile` and what was
    /// typed, Cancel stays here.
    void askJoinPassword(const std::string& host, const nox::client::PlayerProfile& profile, bool wrong);
    /// Where characters are listed from and saved to (default: the game's Save folder).
    void setSaveDir(std::filesystem::path dir) { saveDir_ = std::move(dir); }
    /// The folder a solo game's Save\ is under (default: the game's).
    void setSaveRoot(std::filesystem::path dir) { saveRoot_ = std::move(dir); }
    /// thing.bin, for a new character's starting spell bars (their spells' flags).
    void setThings(const nox::things::ThingDb* things) { things_ = things; }
    /// The music and voices, for the credits (none: they're silent).
    void setAudio(Audio* audio) { audio_ = audio; }
    /// The .plr of the character last loaded or made (empty if none): its spell bars.
    const std::filesystem::path& characterFile() const { return characterFile_; }
    /// Whether the player chose Skip Login (since its last login): games are joined with no
    /// account name, and XWIS is listed through a throwaway login.
    bool skippedLogin() const { return skippedLogin_; }
    /// The WOL login XWIS took (its nick and encoded password), for posting a game we host.
    const std::optional<nox::io::SavedLogin>& wolLogin() const { return wolLogin_; }
    /// Whether the game list is Network's (LAN play: no XWIS listed, nothing posted there),
    /// not Westwood Online's (logged in, or Skip Login). --browse's list is Network's.
    bool lanOnly() const { return lanOnly_; }

    /// For tests: steps done one after another, each once the screen has settled: "123" clicks
    /// window 123, "~123" points at it, "123=text" types text into entry field 123, "123!" is Enter
    /// in it, "+5000" waits 5 seconds before the next.
    void queueClicks(std::vector<std::string> steps) { clicks_ = std::move(steps); }

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// The credits' fades, over everything (after the pointer, as the exe's records over the GUI).
    void drawOverAll(SDL_Renderer* renderer, int width, int height) const;
    void handleEvent(const SDL_Event& event, int width, int height, std::uint64_t nowMs);

private:
    enum class State {
        MainMenu = 100,
        Online = 400,
        SelChar = 500,
        SelClass = 600,
        SelColor = 700,
        Login = 1700,
        Chat = 1900,
        Browser = 10000
    };
    using Rgb = std::array<std::uint8_t, 3>;

    /// A saved character (Save\*.plr) as the character list shows it (its path empty for a record
    /// no file was read into: findAutosaves).
    using Character = nox::client::solo_menu::Character;

    void reset(); ///< drops the current screen at once
    void enter(State s);
    /// Slides the current screen out, destroys it, then runs `next`.
    void leave(std::function<void()> next);
    void back();
    bool shellSound(int message) const;
    bool onMainMenu(gui::Window& from);
    bool onOnline(gui::Window& from);
    bool onBrowser(gui::Window& from);
    bool onOptsBack(gui::Window& from);
    bool onSelChar(gui::Window& from);
    bool onSelClass(int message, gui::Window& from);
    bool onSelColor(gui::Window& from);
    void setupSelColor();
    // The XWIS login (1700).
    void setupLogin();
    bool onLogin(int message, gui::Window& from);
    /// Shows saved login `slot` in the name, password and save fields (the login window's 0x4010).
    void selectLogin(int slot);
    void scrollLogins(int first);
    void startLogin();
    void loginDone();
    // The WOL chat (1900) and the connection the login keeps (menus_wolchat.cpp).
    void setupChat();
    bool onChat(int message, gui::Window& from);
    /// Pumps the WOL connection and acts on its news (each frame's).
    void pollWol();
    void chatEvent(const nox::lobby::WolChatSession::Event& e);
    /// A line at the messages' end in palette colour `color`.
    void chatLine(const std::string& text, int color);
    void chatTitle(bool inLobby = true); ///< (false: as given 0)
    void fillLobbies();
    void fillUsers();
    void lobbyLine(int index);   ///<
    void userLine(int index);    ///<
    void removeUserLine(int index);
    void joinLobby(int index);
    void lobbyUsersChanged(int delta); ///< the lobby's user count changed by `delta`
    std::string lobbyText(int index) const;
    void sendChat(bool action);
    void scrollList(std::int32_t list, int first);
    void focusChatEntry();
    void closeDialog();          ///< Gui_Dialog_Close
    /// The game list's XWIS games: through the WOL connection while there is one (state 8's
    /// RequestChannelList(37), "Loading games"), else a search of its own.
    void refreshGames();
    /// RequestLogout; the connection let go once the server closes it.
    void logOffWol();
    /// A colour button's or swatch's colour (its data: palette table low, index high).
    Rgb colorOf(std::uint32_t data) const;
    Rgb colorOf(std::int32_t id) const;
    bool isEnabled(std::int32_t id) const;
    void drawPreview(gui::Window& w, gui::Painter& p) const;
    void openPicker(std::int32_t target, SDL_Point at);
    /// Picks palette index `index` for the button the picker was opened for; an index of 32 or
    /// more just closes it.
    void pick(std::uint32_t index);
    /// Saves the character made on SelColor and returns it to join as, or nothing
    /// if it couldn't be saved.
    std::optional<nox::client::PlayerProfile> saveNewCharacter();
    void setBackText(const char* id);
    void enable(gui::Window* top, std::int32_t id, bool on);
    /// A button that isn't hooked up yet: disabled, " - WIP" after its text.
    void wip(gui::Window* top, std::int32_t id);
    /// Chosen a game: pick the character to join as (as noxworld's Join, windowMultiplayerSub).
    void chooseCharacter(const std::string& host);
    /// A game clicked in the list (client_gui_serverInfoBlockCheckExp):
    /// its description box (10033) at the click, filled (client_gui_serverInfoBlock)
    /// and taking the mouse till Join or Escape.
    void showGameInfo(const nox::lobby::ServerEntry& entry, SDL_FPoint at, int width, int height);
    void closeGameInfo();
    void scrollGameInfo(int first);
    void loadCharacters();
    void fillCharacterList();
    void join(const nox::client::PlayerProfile& profile);
    // Solo (game flag 0x800).
    void onSolo();
    void onSoloQuest();
    /// noxcpp's Host Custom > a mode started through the solo screens (a campaign mode): the solo
    /// screens on the mode's own saves.
    void onSoloScreensMode(const std::string& modeId);
    /// The saves the solo screens list and write: the mode's while soloScreensMode_, else the solo game's.
    std::filesystem::path soloRoot() const;
    /// The solo screens' game started: the mode hosted (callbacks_.soloScreensMode) or a solo game.
    void startSolo(const SoloStart& start);
    void onCredits();
    /// The credits screen is up (the menu hidden behind it, the input its).
    bool creditsUp() const;
    void fillSoloList();
    bool onSoloSelChar(gui::Window& from);
    /// The character as SelColor makes it: name, colours, clothes,
    /// class; false-y name: the screen isn't up.
    nox::player::FileInfo madeCharacter(std::u16string& name) const;
    nox::client::PlayerProfile profileOf(const nox::player::FileInfo& info) const;
    void startNewSolo();
    void showLoading(bool on);
    /// The game's message box (Gui_Dialog_Create, dlg.wnd): flags 1 OK, 2 Cancel,
    /// 8 Yes, 0x10 No. Modal; OK / Yes run `onOk`, Cancel / No `onCancel`. `yesText` / `noText`:
    /// the Yes and No buttons' own words instead (noxcpp's choices), if given.
    void dialog(const std::string& title, const std::string& text, std::uint32_t flags,
                std::function<void()> onOk, std::function<void()> onCancel = {}, const std::string& yesText = {},
                const std::string& noText = {});
    /// noxcpp's Host Custom (the Host Quest button, 0x2713): modeList's, hosted for others. Hosting
    /// picks a character for it (the Map Editor's, then its session dialog: kHostEditGame).
    void hostCustom();
    /// noxcpp's Game Modes on the main menu's Solo Quest: modeList's, played here: Quest (the stock
    /// flow, unchanged), the NoX Core modes (local and unlisted) or the single-player Map Editor.
    void soloModes();
    /// The mode list both are (menus_modes.cpp): what's built in first and marked (the program's
    /// NoX Core modes, Quest, the Map Editor), then the mods folder's (a broken mod greyed with
    /// why), laid out as the game list (mode_list.hpp's rows); the chosen row's settings under it;
    /// Host (Game Modes: Start), Open Mods Folder, Refresh. A double click or Enter starts a row.
    void modeList(ModeListFor list);
    /// A string-table format ("%s the %s") filled in order.
    std::string format(const std::string& id, std::initializer_list<std::string> args) const;

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    /// The background's blinking eyes (MainBG, the exe's table): each eye open for a
    /// while (its lid not drawn), then a while blinking: its lid (BlinkEyeF / BlinkEyeD) drawn
    /// whenever it isn't waiting, and each frame it shows a 24% chance of a rest of 4-8 frames
    /// that pushes the wait to 60-120: so it shuts for a few frames every two to four seconds.
    /// Stepped 30 times a second.
    struct Eye {
        const char* image;
        int x, y;
        bool open;             ///< the eye's long open spell (rec[4] 1)
        int blinkMin, blinkMax; ///< how long a blinking spell lasts, in frames
        int openMin, openMax;   ///< how long an open spell lasts
        int timer;              ///< frames left of this spell
        int rest = 0;           ///< frames of rest, each pushing the wait back (rec[10])
        int wait = 0;           ///< frames before the lid shows again (rec[11])
    };
    void stepEyes();
    void drawBackground(gui::Window& w, gui::Painter& p);
    std::array<Eye, 5> eyes_{{
        {"BlinkEyeF", 184, 237, false, 120, 230, 105, 200, 1},
        {"BlinkEyeD", 126, 377, true, 100, 206, 105, 196, 3},
        {"BlinkEyeF", 455, 219, true, 100, 242, 87, 182, 5},
        {"BlinkEyeD", 569, 313, true, 100, 197, 105, 188, 7},
        {"BlinkEyeF", 548, 64, false, 80, 180, 132, 224, 8},
    }};
    std::uint64_t eyesStepMs_ = 0;
    std::uint32_t eyeSeed_ = 0xE7E5;
    std::filesystem::path gameDir_;
    std::filesystem::path saveDir_;
    std::filesystem::path saveRoot_;
    std::filesystem::path characterFile_;
    bool solo_ = false;         ///< game flag 0x800 (the main menu's Solo; Multiplay clears it)
    std::string soloScreensMode_; ///< (noxcpp) the solo screens are Host Custom's for this mode (a campaign mode's)
    bool viaSelChar_ = false;   ///< the class screen reached from the character screen
    bool questGame_ = false;    ///< isQuest: the game joined is a quest
    std::vector<nox::client::solo_saves::Slot> soloSlots_; ///< the solo list's 14 (savegame_sub)
    std::string deleteName_;    ///< the remembered save name (solo_menu.hpp soloDelete)
    const nox::things::ThingDb* things_ = nullptr;
    Audio* audio_ = nullptr;
    /// The main menu's credits while they run (till the fade back from them ends).
    std::unique_ptr<Cinematics> credits_;
    /// The game the description box is up for (its Join joins it).
    std::optional<std::string> infoServer_;
    nox::client::PlayerProfile profile_;
    bool skippedLogin_ = false;
    std::optional<nox::io::SavedLogin> wolLogin_;
    std::string pendingHost_;
    std::vector<Character> characters_;
    gui::Window* dialog_ = nullptr;
    /// The keys of the dialog `dialogKeysOf_` (noxcpp's mode lists: arrows and Enter) while it's
    /// the one up; true when it took the key.
    std::function<bool(const SDL_KeyboardEvent&)> dialogKeys_;
    gui::Window* dialogKeysOf_ = nullptr;
    std::string hostModeChosen_; ///< Host Custom's row chosen (mode_list.hpp's modeListKey; none: Quest), kept for next time
    std::string soloModeChosen_; ///< Game Modes' row chosen, the same way
    std::map<std::string, nox::modes::OptionValues, std::less<>> modeSettings_; ///< each mode's settings as last set in either list
    nox::modes::OptionValues pendingModeOptions_; ///< the mode being hosted's
    BrowserScreen& browser_;
    Callbacks callbacks_;
    std::vector<State> stack_;
    gui::Window* background_ = nullptr;
    gui::Window* optsBack_ = nullptr;
    gui::Window* screen_ = nullptr;
    std::vector<gui::Slide*> slides_;
    bool leaving_ = false;
    std::vector<std::function<void()>> deferred_;
    std::uint64_t nowMs_ = 0;
    std::vector<std::string> clicks_;
    std::uint64_t clickWaitMs_ = 0; ///< a "+ms" step's end
    SDL_Point mouse_{}; ///< in the 640 x 480 layout

    // Character creation.
    const nox::things::ModifierDb& modifiers_;
    std::array<std::array<Rgb, 32>, 3> palette_{}; ///< clothes, hair, skin
    int chosenClass_ = -1;                         ///< its window id on SelClass, -1 none yet
    std::uint8_t newClass_ = 0;
    gui::Window* picker_ = nullptr;                ///< SelColor's 760, made top-level
    std::int32_t pickerTarget_ = 0;
    std::uint32_t pickerTable_ = 0;

    // The XWIS login (1700).
    std::vector<nox::io::SavedLogin> logins_; ///< WOLAPI's slots, as loaded
    int loginSlot_ = 0;                       ///< the list's pick
    nox::io::SavedLogin pendingLogin_; ///< what's being checked (password encoded)
    int pendingSlot_ = -1;             ///< where it's saved once it works (-1: not kept)

    // The WOL connection and its chat.
    std::optional<nox::lobby::WolChat> wol_; ///< the game's IChat: the login's connection
    bool wolLoggingIn_ = false;   ///< the login screen's "Connecting..." waits on it
    int wolState_ = 0;            ///< the WOL state: 7 the chat, 8 the game list; 0 neither
    bool wolMode_ = false;        ///< the game list came from the chat (back to it after a game)
    bool lanOnly_ = true;         ///< the game list is Network's (lanOnly())
    /// A list to ask the connection for once it's logged in (kWolAllChannels: the chat's; the
    /// game code: the game list's).
    std::optional<int> wolListWanted_;
    nox::lobby::WolLobbyLists wolLists_;
    std::string wolMotd_;         ///< the welcome's (its text table's slot 0), shown as the chat opens
    std::optional<nox::client::PlayerProfile> joinAfterLogOff_; ///< a join waiting for the log-off
    std::uint32_t wolRandom_ = 0x1234; ///< (randInt's picks of a lobby)
};

} // namespace noxsdl
