// The Server Settings window's Server tab (ServOpts.c's 0x27AF): its General, Access and Teams
// pages and the General page's Advanced Server window.
#include "server_options.hpp"

#include "noxcore/strings/string_table.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <set>

namespace noxsdl {

namespace {

// servopts.wnd
constexpr std::int32_t kServerTab = 10159, kSubAccess = 10161, kSubTeams = 10162, kSubGeneral = 10163;
// general.wnd (windowServerOptionsGeneralProc)
constexpr std::int32_t kGeneralPage = 10300, kForceRespawn = 10301, kShowMotd = 10302, kMapCycle = 10304,
                       kItemRespawn = 10305, kCamperAlarm = 10306, kGenAdvanced = 10319, kGenTitle = 10303;
// noxsdl's own on the General page: the update rate to noxsdl players, and the map vote (not in
// the file).
constexpr std::int32_t kRateLabel = 12200, kRateButton = 12201, kMapVote = 12202;
// advserv.wnd
constexpr std::int32_t kBones = 2102, kGestures = 2103, kLatency0 = 2106, kLatencyAmount = 2110, kAdvDone = 2130;
// servopts.wnd's team options (0x285A-0x285D)
constexpr std::int32_t kTeamsGroup = 10199, kUseTeams = 10330, kAutoAssign = 10331, kShuffle = 10332, kTeamDamage = 10333;
// players.wnd
constexpr std::int32_t kPlayersPage = 10500, kPlrTitle = 10504, kPlrList = 10501, kPlrUp = 10515, kPlrDown = 10516,
                       kPlrSlider = 10517, kAssign = 10503, kTeamList = 10502, kTeamUp = 10518, kTeamDown = 10519,
                       kTeamSlider = 10520, kJoin = 10507, kRename = 10509;
// dlg.wnd
constexpr std::int32_t kDlgOk = 4001, kDlgCancel = 4002, kDlgEntry = 4008, kDlgText = 4004, kDlgTitle = 4005;
// access.wnd (windowAccessProc)
constexpr std::int32_t kAccessPage = 10151, kAccMaxPlayers = 10133, kAccClosed = 10102, kAccPrivate = 10103,
                       kAccPassword = 10104, kAccClasses = 10123, kAccLimitRes = 10124, kAccMinPingBox = 10129,
                       kAccMinPing = 10130, kAccMaxPingBox = 10131, kAccMaxPing = 10132, kAccSysop = 10136,
                       kAccBanned = 10105, kAccBannedUp = 10185, kAccBannedDown = 10186, kAccBannedSlider = 10187,
                       kAccAllowed = 10109, kAccAllowedUp = 10188, kAccAllowedDown = 10189, kAccAllowedSlider = 10190,
                       kAccUsers = 10200, kAccUsersUp = 10201, kAccUsersDown = 10202, kAccUsersSlider = 10203,
                       kAccName = 10111, kAccAdd = 10112, kAccRemove = 10113, kAccKick = 10191, kAccBan = 10192,
                       kAccAllowedRadio = 10206, kAccBannedRadio = 10207;

/// A scrolled list's slider and arrows: {slider, up, down, list}.
struct Scroller {
    std::int32_t slider, up, down, list;
};
constexpr std::array<Scroller, 5> kScrollers{{
    {kPlrSlider, kPlrUp, kPlrDown, kPlrList},
    {kTeamSlider, kTeamUp, kTeamDown, kTeamList},
    {kAccBannedSlider, kAccBannedUp, kAccBannedDown, kAccBanned},
    {kAccAllowedSlider, kAccAllowedUp, kAccAllowedDown, kAccAllowed},
    {kAccUsersSlider, kAccUsersUp, kAccUsersDown, kAccUsers},
}};

/// wcstol's number at the front of a field (none: nullopt).
std::optional<int> fieldNumber(const gui::Window* w)
{
    if (!w || w->entry.empty()) {
        return std::nullopt;
    }
    const std::wstring s(w->entry.begin(), w->entry.end());
    return static_cast<int>(std::wcstol(s.c_str(), nullptr, 10));
}

// A field's text set, cut to the field's length.
void setField(gui::Window* w, const std::string& text)
{
    if (w) {
        w->entry = nox::strings::fromUtf8(text).substr(0, w->entryMax);
    }
}

/// A team colour's (materialGetTeamColor's, team_colors.hpp).
nox::gui::Color teamColour(std::uint8_t color, const nox::gui::Color& none)
{
    static constexpr std::array<std::array<std::uint8_t, 3>, 10> kColours{{
        {255, 255, 255}, {255, 128, 128}, {0, 160, 255}, {128, 255, 128}, {0, 0, 255},
        {255, 255, 0}, {100, 0, 0}, {0, 0, 0}, {255, 255, 255}, {240, 180, 42}}};
    if (color == 0 || color >= kColours.size()) {
        return none;
    }
    const auto& c = kColours[color];
    return {c[0], c[1], c[2], false};
}

/// The window and everything in it.
void enable(gui::Window* w, bool on)
{
    if (!w) {
        return;
    }
    gui::setEnabled(w, on);
    for (auto& c : w->children) {
        enable(c.get(), on);
    }
}

} // namespace

void ServerOptions::addServerPages(nox::gui::WindowDef& def)
{
    // The pages are the exe's windows of their own made children of this one as their tab is
    // chosen; here they're loaded with it, hidden, and filled as they're shown.
    // (The WOL host's General page, wGeneral.wnd with the connection type and the update rate,
    // isn't done yet: general.wnd's.)
    for (const char* file : {"general.wnd", "access.wnd", "players.wnd"}) {
        try {
            nox::gui::WindowDef page = nox::gui::loadWindowFile(windowDir_ / file);
            page.status |= nox::gui::status::Hidden;
            if (page.id == kGeneralPage) {
                // noxsdl's Update Rate under the switches (its label in the title's style, its
                // button Advanced...'s), in a LAN game and a WOL one.
                const nox::gui::WindowDef* title = page.find(kGenTitle);
                const nox::gui::WindowDef* advanced = page.find(kGenAdvanced);
                const nox::gui::WindowDef* camper = page.find(kCamperAlarm);
                if (title && advanced && camper) {
                    // noxsdl's Map Vote under it, a checkbox as the camper alarm's.
                    nox::gui::WindowDef vote = *camper;
                    vote.id = kMapVote;
                    vote.y = 200;
                    vote.text.clear(); // (set by fillGeneral)
                    vote.tooltip.clear();
                    vote.children.clear();
                    page.children.push_back(std::move(vote));
                    nox::gui::WindowDef label = *title;
                    label.id = kRateLabel;
                    label.x = 15;
                    label.y = 175;
                    label.width = 80;
                    label.height = 15;
                    label.text.clear(); // (set by fillGeneral: not a string table's)
                    label.tooltip.clear();
                    label.children.clear();
                    nox::gui::WindowDef button = *advanced;
                    button.id = kRateButton;
                    button.x = 95;
                    button.y = 172;
                    button.text.clear();
                    button.tooltip.clear();
                    button.children.clear();
                    page.children.push_back(std::move(label));
                    page.children.push_back(std::move(button));
                }
            }
            def.children.push_back(std::move(page));
        } catch (const std::exception&) {
        }
    }
}

void ServerOptions::setupServerTab()
{
    // The host's (with Game_HasFlags(1)): the tab usable; its pages' tabs hidden till
    // it's chosen.
    if (gui::Window* tab = window_->find(kServerTab)) {
        gui::setEnabled(tab, static_cast<bool>(callbacks_.switches));
    }
    for (const auto id : {kSubGeneral, kSubAccess, kSubTeams}) {
        if (gui::Window* w = window_->find(id)) {
            w->show(false);
        }
    }
    gui::setEnabled(window_->find(kSubAccess), static_cast<bool>(callbacks_.access));
    gui::setEnabled(window_->find(kSubTeams), static_cast<bool>(callbacks_.teams));
    // The lists' slider thumbs (with UISlider / UISliderLit).
    for (const auto& s : kScrollers) {
        gui::useSliderThumb(window_->find(s.slider));
    }
    serverPage_ = -1;
}

void ServerOptions::showServerTab(bool on)
{
    // 0x27AF: the pages' tabs shown, General chosen; 0x27B0 (another tab): they and the page go.
    for (const auto id : {kSubGeneral, kSubAccess, kSubTeams}) {
        if (gui::Window* w = window_->find(id)) {
            w->show(on);
        }
    }
    if (on) {
        gui::setChecked(window_->find(kSubGeneral), true);
        gui::setChecked(window_->find(kSubAccess), false);
        gui::setChecked(window_->find(kSubTeams), false);
        showServerPage(0);
    } else {
        if (serverPage_ == 1) {
            commitAccessFields();
        }
        for (const auto id : {kGeneralPage, kAccessPage, kPlayersPage}) {
            if (gui::Window* page = window_->find(id)) {
                page->show(false);
            }
        }
        serverPage_ = -1;
        closeAdvServ();
        closeRename();
    }
}

void ServerOptions::showServerPage(int page)
{
    // The page's window made, the others' gone.
    if (serverPage_ == 1 && page != 1) {
        commitAccessFields();
    }
    serverPage_ = page;
    if (gui::Window* general = window_->find(kGeneralPage)) {
        general->show(page == 0);
    }
    if (gui::Window* access = window_->find(kAccessPage)) {
        access->show(page == 1);
    }
    if (gui::Window* players = window_->find(kPlayersPage)) {
        players->show(page == 2);
    }
    if (page == 1) {
        fillAccess();
    }
    if (page != 0) {
        closeAdvServ();
    }
    if (page != 2) {
        closeRename();
    }
    if (page == 0) {
        fillGeneral();
    } else if (page == 2) {
        // guiServerPlayersLoad: the lists emptied and filled, nothing chosen; its
        // title "Team Setup" in a chat game.
        for (const auto id : {kPlrList, kTeamList}) {
            if (gui::Window* list = window_->find(id)) {
                list->lines.clear();
                list->selectedLine = -1;
                list->scroll = 0;
            }
        }
        if (gui::Window* title = window_->find(kPlrTitle); title && callbacks_.teams && callbacks_.teams().chat) {
            title->text = gui_.text("Playrlst.c:Title1");
        }
        fillTeamsPage();
    }
}

void ServerOptions::fillAccess()
{
    // The settings as they stand.
    if (!callbacks_.access) {
        return;
    }
    const AccessSettings a = callbacks_.access();
    setField(window_->find(kAccSysop), a.sysopPassword);
    const auto limit = [&](std::int32_t box, std::int32_t field, const std::optional<int>& v) {
        gui::setChecked(window_->find(box), v.has_value());
        gui::setEnabled(window_->find(field), v.has_value());
        setField(window_->find(field), v ? std::to_string(*v) : std::string());
    };
    limit(kAccMinPingBox, kAccMinPing, a.minPing);
    limit(kAccMaxPingBox, kAccMaxPing, a.maxPing);
    gui::setChecked(window_->find(kAccLimitRes), a.limitResolution);
    gui::setChecked(window_->find(kAccPrivate), a.privateGame);
    gui::setEnabled(window_->find(kAccPassword), a.privateGame);
    setField(window_->find(kAccPassword), a.password);
    gui::setChecked(window_->find(kAccClosed), a.closed);
    gui::setEnabled(window_->find(kAccAllowedRadio), a.closed);
    gui::setChecked(window_->find(kAccAllowedRadio), a.closed);
    gui::setChecked(window_->find(kAccBannedRadio), !a.closed);
    showAccessList(a.closed);
    // The classes a join may be refused for (access.c's names), as wide as the widest;
    // those refused chosen.
    if (gui::Window* classes = window_->find(kAccClasses)) {
        classes->lines.clear();
        int i = 0;
        gui::Painter p(nullptr, gui_.resources(), 1.0f, 0, 0);
        int widest = 0;
        for (const char* id : {"access.c:WARRIOR", "access.c:WIZARD", "access.c:CONJURER"}) {
            gui::Window::Line line{gui_.text(id), classes->textColor};
            line.marked = (a.restrictedClasses >> i++) & 1u;  // (bit i: class i refused)
            widest = std::max(widest, p.textWidth(line.text, classes->largeFont));
            classes->lines.push_back(std::move(line));
        }
        classes->selectedLine = -1;
        classes->rect.w = widest + 7;
        classes->rect.h = (p.fontHeight(classes->largeFont) + 1) * 4 + 2; // (its title and the three)
    }
    setField(window_->find(kAccMaxPlayers), std::to_string(a.maxPlayers));
    fillAccessLists();
}

void ServerOptions::fillAccessLists()
{
    // The users (Gui_Access_AddPlayerName): what was chosen stays chosen.
    if (!callbacks_.access) {
        return;
    }
    const AccessSettings a = callbacks_.access();
    const auto refill = [](gui::Window* list, const std::vector<std::string>& names) {
        if (!list) {
            return;
        }
        std::set<std::string> marked;
        for (const auto& l : list->lines) {
            if (l.marked) {
                marked.insert(l.text);
            }
        }
        const int chosen = list->selectedLine;
        list->lines.clear();
        for (const auto& n : names) {
            gui::Window::Line line{n, list->textColor};
            line.marked = marked.contains(n);
            list->lines.push_back(std::move(line));
        }
        list->selectedLine = chosen < static_cast<int>(list->lines.size()) ? chosen : -1;
    };
    std::vector<std::string> users;
    if (callbacks_.teams) {
        for (const auto& p : callbacks_.teams().players) {
            users.push_back(p.name);
        }
    }
    refill(window_->find(kAccUsers), users);
    refill(window_->find(kAccAllowed), a.allowed);
    std::vector<std::string> banned;
    if (callbacks_.banned) {
        for (const auto& n : callbacks_.banned()) {
            banned.push_back("*" + n); // (the format "*%s": a name banned, not a user)
        }
    }
    refill(window_->find(kAccBanned), banned);
    accessButtons();
}

void ServerOptions::accessButtons()
{
    // The host's draw: Add with a name typed, Remove with one chosen in the list
    // shown; 0x27D8's: Kick and Ban with a user chosen who isn't our own player.
    gui::Window* name = window_->find(kAccName);
    gui::setEnabled(window_->find(kAccAdd), name && !name->entry.empty());
    gui::Window* list = window_->find(accessAllowedShown_ ? kAccAllowed : kAccBanned);
    gui::setEnabled(window_->find(kAccRemove), list && list->selectedLine >= 0);
    bool others = false;
    if (gui::Window* users = window_->find(kAccUsers); users && callbacks_.teams) {
        const std::uint32_t own = callbacks_.ownNetCode ? callbacks_.ownNetCode() : 0;
        const auto players = callbacks_.teams().players;
        for (const auto& l : users->lines) {
            if (!l.marked) {
                continue;
            }
            for (const auto& p : players) {
                others = others || (p.name == l.text && p.netCode != own);
            }
        }
    }
    gui::setEnabled(window_->find(kAccKick), others);
    gui::setEnabled(window_->find(kAccBan), others);
}

void ServerOptions::showAccessList(bool allowed)
{
    // 0x27DE / 0x27DF: one list and its arrows shown, the other's hidden.
    accessAllowedShown_ = allowed;
    for (const auto id : {kAccAllowed, kAccAllowedUp, kAccAllowedDown, kAccAllowedSlider}) {
        if (gui::Window* w = window_->find(id)) {
            w->show(allowed);
        }
    }
    for (const auto id : {kAccBanned, kAccBannedUp, kAccBannedDown, kAccBannedSlider}) {
        if (gui::Window* w = window_->find(id)) {
            w->show(!allowed);
        }
    }
    accessButtons();
}

void ServerOptions::commitAccessFields()
{
    // 0x4003 / 0x401F: the typed fields as they're left: the player limit (1-32, written back),
    // the ping limits (with their boxes), the game's password, the sysop's.
    if (!window_ || !callbacks_.access || !callbacks_.setAccess) {
        return;
    }
    AccessSettings a = callbacks_.access();
    if (const auto n = fieldNumber(window_->find(kAccMaxPlayers))) {
        a.maxPlayers = std::clamp(*n, 1, 32);
        setField(window_->find(kAccMaxPlayers), std::to_string(a.maxPlayers));
    }
    if (gui::Window* box = window_->find(kAccMinPingBox); box && gui::isChecked(*box)) {
        a.minPing = std::max(0, fieldNumber(window_->find(kAccMinPing)).value_or(0));
    }
    if (gui::Window* box = window_->find(kAccMaxPingBox); box && gui::isChecked(*box)) {
        a.maxPing = std::max(0, fieldNumber(window_->find(kAccMaxPing)).value_or(0));
    }
    if (gui::Window* w = window_->find(kAccPassword)) {
        a.password = nox::strings::toUtf8(w->entry);
    }
    if (gui::Window* w = window_->find(kAccSysop)) {
        a.sysopPassword = nox::strings::toUtf8(w->entry);
    }
    callbacks_.setAccess(a);
}

void ServerOptions::fillTeamOptions()
{
    // The Server tab setup's teams part (the host's): in a chat game, without teams only Use Teams; in a
    // game, with teams only Shuffle and Team Damage, without none. Auto-Assign and Team Damage
    // ticked by their gameplay flags (2, 1), Use Teams by 4 in a chat game only (out of one it's
    // left as the file has it); their tooltips by the state.
    gui::Window* group = window_->find(kTeamsGroup);
    if (!group || !callbacks_.teams) {
        enable(group, false);
        return;
    }
    const TeamsView t = callbacks_.teams();
    enable(group, true);
    if (t.chat) {
        gui::setChecked(window_->find(kUseTeams), t.teams);
    }
    gui::setChecked(window_->find(kAutoAssign), t.autoAssign);
    gui::setChecked(window_->find(kTeamDamage), t.teamDamage);
    const auto off = [&](std::int32_t from, std::int32_t to) {
        for (std::int32_t id = from; id <= to; ++id) {
            gui::setEnabled(window_->find(id), false);
        }
    };
    if (t.chat) {
        if (!t.teams) {
            off(kAutoAssign, kTeamDamage);
        }
    } else {
        off(kUseTeams, t.teams ? kAutoAssign : kTeamDamage);
    }
    if (gui::Window* w = window_->find(kAutoAssign)) {
        w->tooltip = gui_.text(t.autoAssign ? "guiserv.c:AutoAssignOnTT" : "guiserv.c:AutoAssignOffTT");
    }
    if (gui::Window* w = window_->find(kTeamDamage)) {
        w->tooltip = gui_.text(t.teamDamage ? "guiserv.c:TeamDamageOnTT" : "guiserv.c:TeamDamageOffTT");
    }
}

void ServerOptions::fillTeamsPage()
{
    // The teams (Gui_PlayerList_AddTeam) and the players (Gui_PlayerList_AddPlayer,
    // in their team's colour); in a quest both lists off. What's chosen stays chosen.
    if (!callbacks_.teams) {
        return;
    }
    gui::Window* players = window_->find(kPlrList);
    gui::Window* teams = window_->find(kTeamList);
    if (!players || !teams) {
        return;
    }
    const TeamsView t = callbacks_.teams();
    std::set<std::string> marked;
    for (const auto& l : players->lines) {
        if (l.marked) {
            marked.insert(l.text);
        }
    }
    const std::string chosenTeam =
        teams->selectedLine >= 0 && teams->selectedLine < static_cast<int>(teams->lines.size()) ? teams->lines[static_cast<std::size_t>(teams->selectedLine)].text : std::string();
    teams->lines.clear();
    teams->selectedLine = -1;
    for (const auto& team : t.list) {
        if (team.name == chosenTeam) {
            teams->selectedLine = static_cast<int>(teams->lines.size());
        }
        teams->lines.push_back({team.name, teamColour(team.color, teams->textColor)});
    }
    players->lines.clear();
    players->selectedLine = -1;
    for (const auto& p : t.players) {
        nox::gui::Color colour = players->textColor;
        for (const auto& team : t.list) {
            if (team.id == p.team && p.team != 0) {
                colour = teamColour(team.color, players->textColor);
            }
        }
        gui::Window::Line line{p.name, colour};
        line.marked = marked.contains(p.name);
        players->lines.push_back(std::move(line));
    }
    teamsPageButtons();
}

void ServerOptions::teamsPageButtons()
{
    // players.wnd's 0x4010: Assign with a team and players chosen; Join and
    // Rename with a team chosen.
    gui::Window* players = window_->find(kPlrList);
    gui::Window* teams = window_->find(kTeamList);
    if (!players || !teams) {
        return;
    }
    const bool team = teams->selectedLine >= 0;
    const bool any = std::ranges::any_of(players->lines, [](const auto& l) { return l.marked; });
    gui::setEnabled(window_->find(kAssign), team && any);
    gui::setEnabled(window_->find(kJoin), team);
    gui::setEnabled(window_->find(kRename), team);
}

void ServerOptions::openRename()
{
    // 0x290D: Gui_Dialog_Create(Playrlst.c:Rename, Playrlst.c:NewName, 0xA3: OK, Cancel, closing
    // by itself, the long entry), its OK (0xFA1) to the page.
    closeRename();
    try {
        rename_ = gui_.load(windowDir_ / "dlg.wnd",
                            [this](gui::Window&, int message, gui::Window& from) { return onRename(from, message); });
    } catch (const std::exception&) {
        rename_ = nullptr;
        return;
    }
    gui::centreOnScreen(*rename_);
    rename_->status |= nox::gui::status::Above;
    for (auto& c : rename_->children) {
        const bool keep = c->id == kDlgOk || c->id == kDlgCancel || c->id == kDlgEntry || c->id == kDlgText || c->id == kDlgTitle;
        c->show(keep);
    }
    if (gui::Window* t = rename_->find(kDlgTitle)) {
        t->text = gui_.text("Playrlst.c:Rename");
    }
    if (gui::Window* t = rename_->find(kDlgText)) {
        t->text = gui_.text("Playrlst.c:NewName");
    }
    gui_.setModal(rename_);
    if (gui::Window* entry = rename_->find(kDlgEntry)) {
        gui_.setFocus(entry);
        if (sdlWindow_) {
            SDL_StartTextInput(sdlWindow_);
        }
    }
}

void ServerOptions::closeRename()
{
    if (!rename_) {
        return;
    }
    if (gui_.focus() && sdlWindow_) {
        SDL_StopTextInput(sdlWindow_);
    }
    gui_.setFocus(nullptr);
    gui_.setModal(nullptr);
    gui_.destroy(rename_);
    rename_ = nullptr;
}

bool ServerOptions::onRename(gui::Window& from, int message)
{
    if (message != gui::Clicked || (from.id != kDlgOk && from.id != kDlgCancel)) {
        return false;
    }
    gui_.resources().sound(0x2FE);
    std::string name;
    if (gui::Window* entry = rename_ ? rename_->find(kDlgEntry) : nullptr) {
        name = nox::strings::toUtf8(entry->entry);
    }
    const bool ok = from.id == kDlgOk;
    deferred_.push_back([this, ok, name] {
        gui::Window* teams = window_ ? window_->find(kTeamList) : nullptr;
        if (ok && teams && teams->selectedLine >= 0 && callbacks_.teams && callbacks_.renameTeam) {
            // 0xFA1: the team chosen by its name; a name no team has.
            const std::string old = teams->lines[static_cast<std::size_t>(teams->selectedLine)].text;
            for (const auto& t : callbacks_.teams().list) {
                if (t.name == old) {
                    callbacks_.renameTeam(t.id, name);
                }
            }
        }
        closeRename();
        fillTeamsPage();
    });
    return true;
}

void ServerOptions::fillGeneral()
{
    // The switches as they stand; then the camper alarm off in
    // elimination and arena (0x420, where it's always on), force respawn off in elimination where
    // it's on by itself.
    if (!callbacks_.switches) {
        return;
    }
    const Switches s = callbacks_.switches();
    const std::uint32_t mode = callbacks_.current ? callbacks_.current().mode : 0;
    gui::Window* respawn = window_->find(kForceRespawn);
    gui::setChecked(respawn, s.forceRespawn);
    gui::setEnabled(respawn, !(s.forceRespawn && (mode & 0x400)));  // (0x400: elimination)
    gui::setChecked(window_->find(kShowMotd), s.sendMotd);
    gui::setChecked(window_->find(kMapCycle), s.mapCycle);
    gui::setChecked(window_->find(kItemRespawn), s.itemRespawn);
    gui::Window* camper = window_->find(kCamperAlarm);
    gui::setChecked(camper, s.camperAlarm);
    gui::setEnabled(camper, !(mode & 0x420));
    // Advanced... (advserv.wnd) only in a WOL game; a LAN game's page hides it
    // (0x284F hidden).
    if (gui::Window* advanced = window_->find(kGenAdvanced)) {
        advanced->show(s.wol);
    }
    if (gui::Window* label = window_->find(kRateLabel)) {
        label->text = "Update Rate";
    }
    if (gui::Window* rate = window_->find(kRateButton)) {
        rate->text = std::to_string(s.updateRate) + " Hz";
        rate->tooltip = "How often NoX Core players get the characters' and spells' positions: smoother at 60-120 Hz. "
                        "The game itself, and players on the original game, stay at 30.";
    }
    if (gui::Window* vote = window_->find(kMapVote)) {
        vote->text = "Map Vote";
        vote->tooltip = "At a match's end the players vote for the next map (NoX Core players click its picture, others "
                        "say its number), and anyone can call a vote (NoX Core players with the in-game menu's Vote Map, others by "
                        "saying \"?MapVote\"), once "
                        "every 5 minutes (more than half changes the map).";
        gui::setChecked(vote, s.mapVote);
    }
}

// The Server tab's controls (its General, Access and Teams pages, and the scroll bars of their
// lists), asked first by onMessage; returns whether the message was handled here.
bool ServerOptions::onServerMessage(gui::Window& from, int message)
{
    // The pages' lists: their sliders and arrows (the list's own, as the exe ties them).
    // How many lines a list shows: 11 units a line, less one for its title.
    const auto shown = [](const gui::Window& list) { return std::max(1, list.rect.h / 11 - (list.text.empty() ? 0 : 1)); };
    for (const Scroller& s : kScrollers) {
        if (from.id != s.slider && from.id != s.up && from.id != s.down) {
            continue;
        }
        gui::Window* list = window_->find(s.list);
        if (!list) {
            return true;
        }
        const int most = std::max(0, static_cast<int>(list->lines.size()) - shown(*list));
        if (message == gui::SliderChanged && from.id == s.slider) {
            if (const auto* sl = std::get_if<nox::gui::SliderData>(&from.data)) {
                list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
            }
            return true;
        }
        if (message == gui::Clicked && from.id != s.slider) {
            list->scroll = std::clamp(list->scroll + (from.id == s.up ? -1 : 1), 0, most);
            return true;
        }
        return false;
    }
    if (message != gui::Clicked) {
        return false;
    }
    switch (from.id) {
    case kPlrList:
        // (a list that takes several: each click turns its line on or off)
        gui_.resources().sound(0x2FE);
        if (from.selectedLine >= 0 && from.selectedLine < static_cast<int>(from.lines.size())) {
            auto& line = from.lines[static_cast<std::size_t>(from.selectedLine)];
            line.marked = !line.marked;
        }
        from.selectedLine = -1;
        teamsPageButtons();
        return true;
    case kTeamList:
        gui_.resources().sound(0x2FE);
        teamsPageButtons();
        return true;
    case kAssign:
    case kJoin: {
        // 0x2907 / 0x290B: the team chosen by its name.
        gui_.resources().sound(0x2FE);
        gui::Window* teams = window_->find(kTeamList);
        gui::Window* players = window_->find(kPlrList);
        if (!teams || !players || teams->selectedLine < 0 || !callbacks_.teams) {
            return true;
        }
        const std::string teamName = teams->lines[static_cast<std::size_t>(teams->selectedLine)].text;
        std::uint8_t team = 0;
        for (const auto& t : callbacks_.teams().list) {
            if (t.name == teamName) {
                team = t.id;
            }
        }
        if (team == 0) {
            return true;
        }
        // Join puts us on the team; Assign puts every marked player there.
        if (from.id == kJoin) {
            if (callbacks_.joinTeam) {
                callbacks_.joinTeam(team);
            }
        } else if (callbacks_.assignTeam) {
            std::vector<std::string> names;
            for (auto& l : players->lines) {
                if (l.marked) {
                    names.push_back(l.text);
                }
            }
            callbacks_.assignTeam(names, team);
            // (both lists' choices let go: 0x4013 -1)
            for (auto& l : players->lines) {
                l.marked = false;
            }
            teams->selectedLine = -1;
        }
        deferred_.push_back([this] {
            fillTeamsPage();
            fillTeamOptions();
        });
        return true;
    }
    case kRename:
        gui_.resources().sound(0x2FE);
        deferred_.push_back([this] { openRename(); });
        return true;
    // ---- the Access page (windowAccessProc)
    case kAccClosed:
    case kAccPrivate:
    case kAccLimitRes:
    case kAccMinPingBox:
    case kAccMaxPingBox: {
        gui_.resources().sound(0x2FE);
        if (!callbacks_.access || !callbacks_.setAccess) {
            return true;
        }
        AccessSettings a = callbacks_.access();
        const bool on = gui::isChecked(from);
        if (from.id == kAccClosed) {
            // 0x2776: closed, the allowed list's tab usable; open, it off and the banned list shown.
            a.closed = on;
            gui::setEnabled(window_->find(kAccAllowedRadio), on);
            if (!on) {
                gui::setChecked(window_->find(kAccAllowedRadio), false);
                gui::setChecked(window_->find(kAccBannedRadio), true);
                showAccessList(false);
            }
        } else if (from.id == kAccPrivate) {
            a.privateGame = on; // (0x2777: the password's field with it)
            gui::setEnabled(window_->find(kAccPassword), on);
        } else if (from.id == kAccLimitRes) {
            a.limitResolution = on; // (0x278C)
        } else {
            // 0x2791 / 0x2793: a limit's field with its box; off, none (-1).
            gui::Window* field = window_->find(from.id == kAccMinPingBox ? kAccMinPing : kAccMaxPing);
            gui::setEnabled(field, on);
            std::optional<int>& limit = from.id == kAccMinPingBox ? a.minPing : a.maxPing;
            limit = on ? std::optional<int>(std::max(0, fieldNumber(field).value_or(0))) : std::nullopt;
        }
        callbacks_.setAccess(a);
        return true;
    }
    case kAccClasses:
        // 0x4010 from 0x278B: a class's bit flipped.
        gui_.resources().sound(0x2FE);
        if (from.selectedLine >= 0 && from.selectedLine < 3 && callbacks_.access && callbacks_.setAccess) {
            auto& line = from.lines[static_cast<std::size_t>(from.selectedLine)];
            line.marked = !line.marked;
            AccessSettings a = callbacks_.access();
            a.restrictedClasses ^= static_cast<std::uint8_t>(1u << from.selectedLine);  // a bit per class, by its line
            callbacks_.setAccess(a);
        }
        from.selectedLine = -1;
        return true;
    case kAccUsers:
        // 0x27D8: several may be chosen; Kick and Ban with one not ours.
        gui_.resources().sound(0x2FE);
        if (from.selectedLine >= 0 && from.selectedLine < static_cast<int>(from.lines.size())) {
            auto& line = from.lines[static_cast<std::size_t>(from.selectedLine)];
            line.marked = !line.marked;
        }
        from.selectedLine = -1;
        accessButtons();
        return true;
    case kAccAllowed:
    case kAccBanned:
        gui_.resources().sound(0x2FE);
        accessButtons();
        return true;
    case kAccAllowedRadio:
    case kAccBannedRadio:
        gui_.resources().sound(0x2FE);
        showAccessList(from.id == kAccAllowedRadio); // (0x27DE / 0x27DF)
        return true;
    case kAccAdd: {
        // 0x2780: the name onto the list shown, the field emptied.
        gui_.resources().sound(0x2FE);
        gui::Window* name = window_->find(kAccName);
        if (name && !name->entry.empty() && callbacks_.addAccessName) {
            callbacks_.addAccessName(nox::strings::toUtf8(name->entry), accessAllowedShown_);
            name->entry.clear();
            fillAccessLists();
        }
        return true;
    }
    case kAccRemove: {
        // 0x2781: the shown list's chosen one.
        gui_.resources().sound(0x2FE);
        gui::Window* list = window_->find(accessAllowedShown_ ? kAccAllowed : kAccBanned);
        if (list && list->selectedLine >= 0 && callbacks_.removeAccessName) {
            callbacks_.removeAccessName(static_cast<std::size_t>(list->selectedLine), accessAllowedShown_);
            list->selectedLine = -1;
            fillAccessLists();
        }
        return true;
    }
    case kAccKick:
    case kAccBan: {
        // 0x27CF / 0x27D0: the chosen users (not our own).
        gui_.resources().sound(0x2FE);
        gui::Window* users = window_->find(kAccUsers);
        if (!users || !callbacks_.kickPlayers) {
            return true;
        }
        std::vector<std::string> names;
        for (auto& l : users->lines) {
            if (l.marked) {
                names.push_back(l.text);
                l.marked = false;
            }
        }
        callbacks_.kickPlayers(names, from.id == kAccBan);
        deferred_.push_back([this] { fillAccessLists(); });
        return true;
    }
    case kUseTeams:
    case kAutoAssign:
    case kTeamDamage:
    case kShuffle: {
        // 0x285A-0x285D.
        gui_.resources().sound(0x2FE);
        if (from.id == kShuffle) {
            if (callbacks_.shuffleTeams) {
                callbacks_.shuffleTeams();
            }
        } else if (callbacks_.setTeamOption) {
            callbacks_.setTeamOption(from.id == kUseTeams ? 0 : from.id == kAutoAssign ? 1 : 2, gui::isChecked(from));  // options 0 use teams, 1 auto-assign, 2 team damage
        }
        deferred_.push_back([this] {
            fillTeamOptions();
            if (serverPage_ == 2) {
                fillTeamsPage();
            }
        });
        return true;
    }
    case kSubGeneral:
    case kSubAccess:
    case kSubTeams: {
        // 0x27B3 / 0x27B1 / 0x27B2.
        gui_.resources().sound(0x2FE);
        const int page = from.id == kSubGeneral ? 0 : from.id == kSubAccess ? 1 : 2;
        deferred_.push_back([this, page] { showServerPage(page); });
        return true;
    }
    case kForceRespawn:
    case kShowMotd:
    case kMapCycle:
    case kItemRespawn:
    case kCamperAlarm:
    case kMapVote: {
        // windowServerOptionsGeneralProc: each at once (the box has ticked itself).
        gui_.resources().sound(0x2FE);
        if (!callbacks_.switches || !callbacks_.setSwitches) {
            return true;
        }
        Switches s = callbacks_.switches();
        const bool on = gui::isChecked(from);
        switch (from.id) {
        case kForceRespawn: s.forceRespawn = on; break;
        case kShowMotd: s.sendMotd = on; break;
        case kMapCycle: s.mapCycle = on; break;
        case kItemRespawn: s.itemRespawn = on; break;
        case kMapVote: s.mapVote = on; break;
        default: s.camperAlarm = on; break;
        }
        callbacks_.setSwitches(s);
        deferred_.push_back([this] { fillGeneral(); });
        return true;
    }
    case kGenAdvanced:
        // 0x284F: the Advanced Server window.
        gui_.resources().sound(0x2FE);
        deferred_.push_back([this] { openAdvServ(); });
        return true;
    case kRateButton:
        // noxsdl's: 30, 60, 90, 120 Hz in turn (from the next frame).
        gui_.resources().sound(0x2FE);
        if (callbacks_.switches && callbacks_.setSwitches) {
            Switches s = callbacks_.switches();
            s.updateRate = s.updateRate >= 120 ? 30 : s.updateRate + 30;
            callbacks_.setSwitches(s);
            deferred_.push_back([this] { fillGeneral(); });
        }
        return true;
    default:
        return false;
    }
}

void ServerOptions::openAdvServ()
{
    // advserv.wnd its own window over this one (+15, +0x50 from its corner), taking
    // the input; then the switches.
    closeAdvServ();
    if (!callbacks_.switches) {
        return;
    }
    try {
        advserv_ = gui_.load(windowDir_ / "advserv.wnd",
                             [this](gui::Window&, int message, gui::Window& from) { return onAdvServ(from, message); });
    } catch (const std::exception&) {
        advserv_ = nullptr;
        return;
    }
    const std::function<void(gui::Window&)> hear = [&](gui::Window& w) {
        if (!w.children.empty()) {
            w.proc = [this](gui::Window&, int message, gui::Window& from) { return onAdvServ(from, message); };
        }
        for (auto& c : w.children) {
            hear(*c);
        }
    };
    hear(*advserv_);
    advserv_->rect.x = window_->rect.x + 15;
    advserv_->rect.y = window_->rect.y + 0x50;
    advserv_->status |= nox::gui::status::Above;
    const Switches s = callbacks_.switches();
    gui::setChecked(advserv_->find(kBones), s.bones);
    gui::setChecked(advserv_->find(kGestures), s.gestures);
    for (int i = 0; i < 4; ++i) {
        gui::setChecked(advserv_->find(kLatency0 + i), s.latency == i);
    }
    if (gui::Window* amount = advserv_->find(kLatencyAmount)) {
        amount->entry = nox::strings::fromUtf8(std::to_string(s.latencyAmount)).substr(0, amount->entryMax);
        gui::setEnabled(amount, s.latency == 3);
    }
    gui_.setModal(advserv_);
}

void ServerOptions::closeAdvServ()
{
    if (!advserv_) {
        return;
    }
    if (gui_.focus() && sdlWindow_) {
        SDL_StopTextInput(sdlWindow_);
    }
    gui_.setFocus(nullptr);
    gui_.setModal(nullptr);
    gui_.destroy(advserv_);
    advserv_ = nullptr;
}

void ServerOptions::applyAdvServAmount()
{
    // 0x4003 / 0x401F from 2110: the amount (below 0, 0).
    gui::Window* amount = advserv_ ? advserv_->find(kLatencyAmount) : nullptr;
    if (!amount || amount->entry.empty() || !callbacks_.switches || !callbacks_.setSwitches) {
        return;
    }
    const std::wstring w(amount->entry.begin(), amount->entry.end());
    Switches s = callbacks_.switches();
    s.latencyAmount = std::max(0, static_cast<int>(std::wcstol(w.c_str(), nullptr, 10)));
    callbacks_.setSwitches(s);
}

bool ServerOptions::onAdvServ(gui::Window& from, int message)
{
    if (message != gui::Clicked) {
        return false;
    }
    gui_.resources().sound(0x2FE);
    if (!callbacks_.switches || !callbacks_.setSwitches) {
        return true;
    }
    Switches s = callbacks_.switches();
    if (from.id == kBones || from.id == kGestures) {
        // 0x836 / 0x837: flipped.
        (from.id == kBones ? s.bones : s.gestures) = gui::isChecked(from);
        callbacks_.setSwitches(s);
        return true;
    }
    if (from.id >= kLatency0 && from.id < kLatency0 + 4) {
        // 0x83A-0x83D: the mode; the amount's field only for the user's.
        s.latency = from.id - kLatency0;
        s.announceLatency = true; // (1 on every click)
        callbacks_.setSwitches(s);
        gui::setEnabled(advserv_->find(kLatencyAmount), s.latency == 3);
        return true;
    }
    if (from.id == kAdvDone) {
        // 0x852: the window closed (the amount as typed).
        deferred_.push_back([this] {
            applyAdvServAmount();
            closeAdvServ();
        });
        return true;
    }
    return true;
}

} // namespace noxsdl
