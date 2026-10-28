// The host's Server Settings window (servopts.wnd; GAME.EXE's ServOpts.c: guiServerOptsLoad
// opens or closes it, guiServerOptionsProcPre drives it), on the game we host
// (GameHost::serverSettings / applyServerSettings). Opened by the in-game menu's Gameplay
// Controls (menuGameOnButton's 0x2330). At the screen's top right (its width + 10
// from the right edge).
//
// The Game page, as the host's (Game_HasFlags(1)) is filled:
//   name        the game's name (0x2775), up to 15 letters;
//   game type   in a chat map, a button with the type (0x2787) that drops the types' list
//               (0x2788: CTF, Arena, Elimination, KotR, Flag Ball, Chat; the quest's left out:
//               windowServerOptionsFillGametypeList); a type chosen fills the maps
//               and its limits. Out of a chat map the type stays, and "Form a new game" (0x278A)
//               takes the game back to the chat map;
//   maps        the maps of that type ("<name>\t<min>-<max> Players", client_guiserv_updateMapList),
//               the current one selected;
//   limits      the lesson limit ("Capture Limit" in CTF, "Elim Limit" in elimination) and the
//               time limit, in minutes; Defaults puts the type's back;
//   Go!         the name and limits set; another map or type: loaded.
//   Cancel      closes it; so does Escape (0xE7).
//   teams       Use Teams, Auto-Assign, Team Damage, Shuffle (0x285A-0x285D: the page fill's
//               rules for which may change).
// The Server tab (server_options_server.cpp): its General page (general.wnd: force respawn, the
// message of the day, the map cycle, items respawning, the camper alarm; Advanced..., advserv.wnd:
// bones, gestures, latency compensation), its Access page (access.wnd: the player limit, ping
// limits, closed with its allowed list, private with its password, the classes refused, the
// sysop's password, the users kicked or banned, the banned list) and its Teams page (players.wnd:
// players into a team, joining one, renaming one).
// The Game page's Advanced... (server_options_advanced.cpp: advanced.wnd): the map's rule sets
// (rulelist.wnd), the spells (spelllst.wnd), weapons and armour (objlst.wnd) allowed; Go writes
// them into the map's user.rul.
// Not yet: a guest's read-only view.
//
// noxsdl's own Bots tab (not in the stock game), after Server: bots this game runs (noxcore's
// BotSquad, joined to us over the loopback): how many of each class, - and +, at once; the
// difficulty, from one to another (Easy to Hard, Hard to Hard...; each round on a click:
// each bot gets a level in it); those in the game, kept up; All Leave.
#pragma once

#include "gui.hpp"

#include "noxcore/server/game_host.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class ServerOptions {
public:
    using Settings = nox::server::GameHost::ServerSettings;
    using MapChoice = nox::server::GameHost::MapChoice;
    using Switches = nox::server::GameHost::HostSwitches;
    using TeamsView = nox::server::GameHost::TeamsView;
    using AccessSettings = nox::server::GameHost::AccessSettings;
    using RuleSet = nox::server::RuleSet;
    using SettingsWindow = nox::server::GameHost::SettingsWindow;
    using SettingsEdit = nox::server::GameHost::SettingsEdit;

    /// The Bots tab's: the bots wanted by class (warrior, wizard, conjurer), the difficulty from
    /// minTier to maxTier (0 easy, 1 medium, 2 hard), how many are in, the most there may be, a
    /// line a bot.
    struct BotsView {
        std::array<int, 3> wanted{};
        int minTier = 1;
        int maxTier = 1;
        int in = 0;
        int most = 0;
        std::vector<std::string> lines;
    };

    struct Callbacks {
        std::function<Settings()> current;
        std::function<std::vector<MapChoice>()> maps;
        std::function<std::pair<int, int>(std::uint32_t mode)> limits;   ///< as they stand
        std::function<std::pair<int, int>(std::uint32_t mode)> defaults; ///< Defaults'
        std::function<void(const Settings&)> apply;
        /// The name field left (0x4003 for 0x2775): the game's name at once.
        std::function<void(const std::string&)> setName;
        /// The chat map "Form a new game" goes back to (the one the host began on).
        std::function<std::string()> chatMap;
        /// The Bots tab (empty: the tab off).
        std::function<BotsView()> bots;
        std::function<void(const std::array<int, 3>& wanted, int minTier, int maxTier)> setBots;
        /// The Server tab's General page and its Advanced Server window (empty: the tab off).
        std::function<Switches()> switches;
        std::function<void(const Switches&)> setSwitches;
        /// The teams: the Game page's options and the Server tab's Teams page.
        std::function<TeamsView()> teams;
        std::function<void(int option, bool on)> setTeamOption; ///< 0 Use Teams, 1 Auto-Assign, 2 Team Damage
        std::function<void()> shuffleTeams;
        std::function<void(const std::vector<std::string>& players, std::uint8_t team)> assignTeam;
        std::function<void(std::uint8_t team)> joinTeam; ///< our own player
        std::function<void(std::uint8_t team, const std::string& name)> renameTeam;
        /// The Server tab's Access page.
        std::function<AccessSettings()> access;
        std::function<void(const AccessSettings&)> setAccess;
        std::function<std::vector<std::string>()> banned; ///< the banned list's names
        std::function<void(const std::string& name, bool allowedList)> addAccessName;
        std::function<void(std::size_t index, bool allowedList)> removeAccessName;
        std::function<void(const std::vector<std::string>& players, bool ban)> kickPlayers;
        std::function<std::uint32_t()> ownNetCode; ///< our own player's (not to be kicked)
        /// Advanced...: a map's rules for a mode (or a rule set of its folder), its rule sets, one
        /// written or deleted; in a WOL game internet.rul's (empty: the button off).
        std::function<RuleSet(const std::string& map, std::uint32_t mode, const std::optional<std::string>& file)> rulesFor;
        std::function<std::optional<RuleSet>()> internetRules;
        std::function<std::vector<std::string>(const std::string& map)> ruleSets;
        std::function<bool(const std::string& map, const std::string& name, const RuleSet& rules, std::uint32_t mode)> saveRuleSet;
        std::function<void(const std::string& map, const std::string& name)> deleteRuleSet;
        /// The game's spells (thing.bin's, by id: their flags and titles) for the Spells page.
        const nox::things::ThingDb* things = nullptr;
        /// The settings' slot 1 and what the other players are told of the window
        /// (MSG_UPDATE_GUI_GAME_SETTINGS: GameHost::settingsWindowOpened / settingsWindowState /
        /// settingsWindowClosed / editSettings / settingsWindowGo; empty: none). The state is
        /// the exe's reading of the window (windowState()), told as it opens and each update.
        std::function<void(const SettingsWindow&)> windowOpened;
        std::function<void(const SettingsWindow&)> windowState;
        std::function<void()> windowClosed;
        std::function<void(const SettingsEdit&)> editSettings;
        std::function<void(const SettingsWindow&)> windowGo;
    };

    ServerOptions(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks);

    bool isOpen() const { return window_ != nullptr; }
    /// guiServerOptsLoad: opens it, or (open) closes it.
    void toggle();
    void close();

    void update(std::uint64_t nowMs)
    {
        gui_.update(nowMs);
        runDeferred();
        if (window_ && botsShown_ && nowMs - refreshedAt_ >= 500) { // (the bots' list kept up)
            refreshedAt_ = nowMs;
            refreshBots();
        }
        if (window_ && serverPage_ == 2 && !rename_ && nowMs - teamsAt_ >= 500) { // (the players and teams kept up)
            teamsAt_ = nowMs;
            fillTeamsPage();
        }
        if (window_ && serverPage_ == 1 && nowMs - teamsAt_ >= 500) { // (the users and the lists kept up)
            teamsAt_ = nowMs;
            fillAccessLists();
        }
        if (window_ && callbacks_.windowState) { // (what the exe's reading would give now)
            callbacks_.windowState(windowState());
        }
    }
    /// The exe's reading of the window: the name field, the type button's type, the pages'
    /// spells, weapons and armour, the limit fields' numbers when not empty, "Form a new game",
    /// the map list's chosen line.
    SettingsWindow windowState() const;
    void draw(SDL_Renderer* renderer, int width, int height);
    /// While it's open: the mouse over it, Escape, and the keys while one of its fields is being
    /// typed in (text input on for it). Returns whether it took the event.
    bool handleEvent(SDL_Window* sdlWindow, const SDL_Event& event, int width, int height);

private:
    void open();
    bool onMessage(gui::Window& from, int message);
    std::string typeName(std::uint32_t mode) const;
    void sizeTypeList();
    void showTypes(bool on);
    void fillMaps();
    void scrollMaps(int first);
    void fillLimits();
    void showLimits();
    void setLabels();
    void go();
    void addBotsPage(nox::gui::WindowDef& def);
    void setupBotsPage();
    enum class Tab { Game, Server, Bots };
    void showTab(Tab tab);
    // The Server tab (server_options_server.cpp).
    void addServerPages(nox::gui::WindowDef& def);
    void setupServerTab();
    void showServerTab(bool on);
    void showServerPage(int page); ///< 0 General, 1 Access, 2 Teams
    void fillGeneral();
    bool onServerMessage(gui::Window& from, int message);
    void openAdvServ();
    void closeAdvServ();
    bool onAdvServ(gui::Window& from, int message);
    void applyAdvServAmount();
    void fillTeamOptions(); ///< the Game page's (its fill's teams part)
    void fillTeamsPage();
    void teamsPageButtons(); ///< on the page's 0x4010
    void openRename();       ///< 0x290D: Gui_Dialog_Create with the long entry
    void closeRename();
    bool onRename(gui::Window& from, int message);
    void fillAccess();
    void fillAccessLists();  ///< the users, allowed and banned lists
    void accessButtons();    ///< 0x4010's: Add, Remove, Kick, Ban usable or not
    void showAccessList(bool allowed); ///< 0x27DE / 0x27DF
    void commitAccessFields(); ///< the typed fields as left (0x4003 / 0x401F)
    bool accessAllowedShown_ = false;
    // Advanced... (server_options_advanced.cpp: advanced.wnd, loadAdvancedWnd).
    void openAdvanced();
    void closeAdvanced();
    bool onAdvanced(gui::Window& from, int message);
    void showAdvancedPage(int page); ///< 0 rules, 1 spells, 2 weapons, 3 armour
    void fillSpellPage();            ///< guiSpelllistLoad
    void spellChecks();
    void fillObjectPage(bool armour); ///< guiObjlistLoad
    void objectChecks();
    void fillRulePage();
    void ruleButtons();
    bool onRulePage(gui::Window& from, int message);
    bool onSpellPage(gui::Window& from, int message);
    bool onObjectPage(gui::Window& from, int message);
    int spellByTitle(const std::string& title) const; ///< spellByTitle
    void readPendingRules(); ///< the chosen map's for the type (flags 7, user.rul)
    /// Gui_Dialog_Create: its buttons by `flags` (1 OK, 2 Cancel, 8 Yes, 0x10 No); OK or Yes runs
    /// `onOk` once it's gone.
    void openNotice(const std::string& title, const std::string& message, std::uint32_t flags = 0x21,
                    std::function<void()> onOk = {});
    void commitName(); ///< 0x4003 for 0x2775: the name typed, as the field is left
    /// 0x4003 for 0x2796 / 0x2797: a limit typed (not empty), as the field is left: into the
    /// settings' slot 1 (the time 255 at most, the field showing it).
    void commitLimit(std::int32_t field);
    /// A write into the settings' slot 1 (GameHost::editSettings).
    void editSlot(const SettingsEdit& e);
    /// The map list's chosen line's first word (strtok on a tab), empty with none chosen.
    std::string chosenMapWord() const;
    void closeNotice();
    gui::Window* advanced_ = nullptr;
    gui::Window* notice_ = nullptr;
    int advancedPage_ = 0;
    bool armourPage_ = false;
    RuleSet rules_; ///< the settings' slot 1 rules (what Go writes)
    RuleSet work_;  ///< the pages', into rules_ on Done
    void refreshBots();
    void changeBots(int playerClass, int by);
    /// The difficulty's from (`to` false) or to button clicked: round to the next level, the
    /// other moved along to keep from <= to.
    void changeTier(bool to);
    void runDeferred();
    std::string text(const std::string& id) const;

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    Callbacks callbacks_;
    SDL_Window* sdlWindow_ = nullptr;
    gui::Window* window_ = nullptr;
    Settings settings_;
    std::uint32_t startMode_ = 0; ///< the type when it opened (chat or not)
    bool newGame_ = false;        ///< "Form a new game" ticked
    bool botsShown_ = false;      ///< the Bots tab's page up
    gui::Window* advserv_ = nullptr; ///< the Advanced Server window (advserv.wnd), over it
    gui::Window* rename_ = nullptr;  ///< the Teams page's rename dialog (dlg.wnd)
    int serverPage_ = -1;            ///< the Server tab's page shown (-1: none)
    std::uint64_t teamsAt_ = 0;      ///< the Teams page last refilled
    BotsView bots_;
    std::uint64_t refreshedAt_ = 0;
    std::vector<MapChoice> maps_;
    std::vector<std::string> shown_; ///< the map list's names, by line
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
