// ServerOptions (server_options.hpp): opening and closing the window, its Game page, and noxsdl's
// Bots tab. The Server tab's pages are in server_options_server.cpp, Advanced... in
// server_options_advanced.cpp.
#include "server_options.hpp"

#include "hud.hpp"

#include "noxcore/strings/string_table.hpp"

#include <algorithm>
#include <array>
#include <cwchar>

namespace noxsdl {

namespace {

// servopts.wnd
constexpr std::int32_t kName = 10101, kTeamsMsg = 10110, kMapList = 10114, kGoMsg = 10117, kTypeLabel = 10118,
                       kTypeButton = 10119, kTypeList = 10120, kSettingsMsg = 10121, kNewGame = 10122,
                       kLessonLimit = 10134, kTimeLimit = 10135, kDefault = 10141, kGo = 10145, kCancel = 10146,
                       kOk = 10149, kAdvanced = 10152, kServerTab = 10159, kGameTab = 10160, kMapUp = 10180,
                       kMapDown = 10181, kMapSlider = 10182, kMapGroup = 10183, kLimitsGroup = 10197,
                       kTeamsGroup = 10199, kPages = 10150, kGamePage = 10153, kUseTeams = 10330;
constexpr int kMapLinesShown = 7; // (84 high, 11 a line)
// noxsdl's Bots tab and page (not in the file).
constexpr std::int32_t kBotsTab = 12100, kBotsPage = 12101, kBotsBox = 12102, kBotsTitle = 12103,
                       kBotsLabel = 12110, kBotsMinus = 12113, kBotsCount = 12116, kBotsPlus = 12119,
                       kTierLabel = 12122, kTierButton = 12123, kBotsStatusBox = 12124, kBotsInfo = 12125,
                       kBotsList = 12126, kBotsLeave = 12127, kTierTo = 12128, kTierToButton = 12129;
constexpr std::array<const char*, 3> kClassNames{"Warriors", "Wizards", "Conjurers"};
constexpr std::array<const char*, 3> kTierNames{"Easy", "Medium", "Hard"};

// The game types (name, its text, its game flags), the quest's left out of the list
// (windowServerOptionsFillGametypeList); the names are guiserv.c's, the file
// they're looked up from (ServOpts\guiserv.c).
struct GameType {
    const char* name;
    std::uint32_t flags;
};
constexpr std::array<GameType, 7> kTypes{{
    {"CTF", 0x20},
    {"Arena", 0x100},
    {"Highlander", 0x400},
    {"KotR", 0x10},
    {"Flagball", 0x40},
    {"Quest", 0x1000},
    {"Noxworld.c:Chat", 0x80},
}};

/// The format's %S / %s and %d's filled in turn (the exe's swprintf).
std::string format(std::string f, const std::vector<std::string>& args)
{
    std::size_t at = 0;
    for (const auto& a : args) {
        const auto p = f.find('%', at);
        if (p == std::string::npos || p + 1 >= f.size()) {
            break;
        }
        f.replace(p, 2, a);
        at = p + a.size();
    }
    return f;
}

void enable(gui::Window* w, bool on)
{
    // The window and everything in it.
    if (!w) {
        return;
    }
    if (on) {
        w->status |= nox::gui::status::Enabled;
    } else {
        w->status &= ~nox::gui::status::Enabled;
    }
    for (auto& c : w->children) {
        enable(c.get(), on);
    }
}

/// wcstol's number at the front of the field (0 if none).
int number(const gui::Window* w)
{
    if (!w) {
        return 0;
    }
    std::wstring s(w->entry.begin(), w->entry.end());
    return static_cast<int>(std::wcstol(s.c_str(), nullptr, 10));
}

} // namespace

ServerOptions::ServerOptions(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks)
    : gui_(std::move(res)), windowDir_(std::move(windowDir)), callbacks_(std::move(callbacks))
{
    // At the screen's top right at the game's scale (window_setPos: the screen's width
    // less its own, less 10, at the top; the file has it at 400 of 640).
    gui_.setPlacement([](int w, int h) {
        const float s = Hud::scale(w, h);
        return std::array<float, 3>{s, static_cast<float>(w) - 640 * s, 0};
    });
}

std::string ServerOptions::text(const std::string& id) const
{
    return gui_.text(id, "guiserv.c");
}

void ServerOptions::toggle()
{
    // guiServerOptsLoad: open, it closes (0xE7).
    if (window_) {
        gui_.resources().sound(0xE7);
        close();
        return;
    }
    open();
}

void ServerOptions::close()
{
    // Gui_ServerOptions_Hide: destroyed, the focus dropped if it was in it.
    if (!window_) {
        return;
    }
    if (gui_.focus() && sdlWindow_) {
        SDL_StopTextInput(sdlWindow_); // the game's keys are keys again
    }
    if (serverPage_ == 1) {
        commitAccessFields(); // (the fields as left)
    }
    serverPage_ = -1;
    gui_.setFocus(nullptr);
    closeAdvServ();
    closeRename();
    closeAdvanced();
    gui_.setModal(nullptr);
    gui_.destroy(window_);
    window_ = nullptr;
    if (callbacks_.windowClosed) {
        callbacks_.windowClosed();
    }
}

ServerOptions::SettingsWindow ServerOptions::windowState() const
{
    // The window read into a record.
    SettingsWindow w;
    if (!window_) {
        return w;
    }
    if (const gui::Window* name = window_->find(kName)) {
        w.name = nox::strings::toUtf8(name->entry);
    }
    // The type button's text looked up: the flags of the type of that name (0
    // for none). The button shows the type chosen from the list (Chat's too), or at the window's
    // opening guiServerOptionsGetGametypeName's (Arena's for a chat game).
    w.type = 0;
    if (const gui::Window* button = window_->find(kTypeButton)) {
        for (const auto& t : kTypes) {
            if (button->text == text(t.name)) {
                w.type = static_cast<std::uint16_t>(t.flags);
                break;
            }
        }
    }
    w.rules = work_; // (the pages')
    if (const gui::Window* f = window_->find(kLessonLimit); f && !f->entry.empty()) {
        w.lessonLimit = number(f);
    }
    if (const gui::Window* f = window_->find(kTimeLimit); f && !f->entry.empty()) {
        w.timeLimit = number(f);
    }
    w.newGame = newGame_;
    if (const gui::Window* list = window_->find(kMapList);
        list && list->selectedLine >= 0 && list->selectedLine < static_cast<int>(list->lines.size())) {
        w.mapLine = list->lines[static_cast<std::size_t>(list->selectedLine)].text;
    }
    return w;
}

std::string ServerOptions::chosenMapWord() const
{
    const SettingsWindow w = windowState();
    if (!w.mapLine) {
        return {};
    }
    const std::string& line = *w.mapLine;
    const auto from = line.find_first_not_of('\t');
    return from == std::string::npos ? std::string() : line.substr(from, line.find('\t', from) - from);
}

void ServerOptions::editSlot(const SettingsEdit& e)
{
    if (callbacks_.windowState) {
        callbacks_.windowState(windowState()); // (the window as it is when it writes)
    }
    if (callbacks_.editSettings) {
        callbacks_.editSettings(e);
    }
}

void ServerOptions::commitLimit(std::int32_t field)
{
    // 0x4003 for 0x2796 / 0x2797: wcstol of the field (below 0, 0); the time over 255, 255 and the
    // field shows it; into the current slot (its lesson / time limit), the window changed.
    gui::Window* w = window_ ? window_->find(field) : nullptr;
    if (!w || w->entry.empty()) {
        return;
    }
    int n = std::max(0, number(w));
    SettingsEdit e;
    if (field == kTimeLimit) {
        if (0xFF < n) {
            n = 0xFF;
            w->entry = nox::strings::fromUtf8(std::to_string(n)).substr(0, w->entryMax);
        }
        e.timeLimit = n;
    } else {
        e.lessonLimit = n;
    }
    editSlot(e);
}

void ServerOptions::open()
{
    if (!callbacks_.current) {
        return;
    }
    const gui::Proc proc = [this](gui::Window&, int message, gui::Window& from) { return onMessage(from, message); };
    nox::gui::WindowDef def = nox::gui::loadWindowFile(windowDir_ / "servopts.wnd");
    addBotsPage(def);
    addServerPages(def);
    window_ = gui_.load(def, proc);
    // (wndSetProc: the boxes the widgets are in hear them too)
    const std::function<void(gui::Window&)> hear = [&](gui::Window& w) {
        if (!w.children.empty()) {
            w.proc = proc;
        }
        for (auto& c : w.children) {
            hear(*c);
        }
    };
    hear(*window_);
    // windowServerOptionsDrawProc: the screen behind it darkened below its tabs.
    window_->draw = [](gui::Window& self, gui::Painter& p) {
        const SDL_Point pos = self.screenPos();
        p.fillAlpha(pos.x, pos.y + 0x19, self.rect.w, self.rect.h - 0x19, {0, 0, 0, 128});
    };
    // The Game tab chosen; the Server tab, its pages and the teams' options not done yet.
    if (gui::Window* game = window_->find(kGameTab)) {
        game->state |= gui::kPressed;
    }
    setupServerTab();
    setupBotsPage();
    if (gui::Window* advanced = window_->find(kAdvanced); advanced && !callbacks_.rulesFor) {
        advanced->status &= ~nox::gui::status::Enabled;
    }
    // The map list's slider thumb (with UISlider / UISliderLit).
    gui::useSliderThumb(window_->find(kMapSlider));
    // The host's: Go and Cancel, not OK.
    if (gui::Window* ok = window_->find(kOk)) {
        ok->show(false);
    }

    // The settings as they stand, the host's.
    settings_ = callbacks_.current();
    startMode_ = settings_.mode;
    newGame_ = false;
    if (gui::Window* name = window_->find(kName)) {
        name->entry = nox::strings::fromUtf8(settings_.name).substr(0, name->entryMax);
    }
    if (gui::Window* teams = window_->find(kTeamsMsg)) {
        teams->text = format(text("NumTeamsMsg"), {std::to_string(settings_.teams)});
    }
    const bool chat = (startMode_ & 0x80) != 0;  // (0x80: the chat type)
    // In a chat map the game type is chosen (its button); in a game, "Form a new game" instead.
    if (gui::Window* w = window_->find(chat ? kNewGame : kTypeButton)) {
        w->show(false);
    }
    fillTeamOptions();
    maps_ = callbacks_.maps ? callbacks_.maps() : std::vector<MapChoice>{};
    fillMaps();
    fillLimits();
    if (gui::Window* button = window_->find(kTypeButton)) {
        button->text = typeName(settings_.mode);
    }
    sizeTypeList();
    setLabels();
    // (guiServerOptsLoad, the host's: the window marked changed, slot 0 refreshed and copied
    // to slot 1, the current one.)
    if (callbacks_.windowOpened) {
        callbacks_.windowOpened(windowState());
    }
}

void ServerOptions::addBotsPage(nox::gui::WindowDef& def)
{
    // A third tab after Game and Server, and its page in the Game page's place, made of the
    // file's own widgets: the tabs', the boxes', the green texts', the lists' and the buttons'.
    namespace st = nox::gui::status;
    const auto* tab = def.find(kServerTab);
    const auto* page = def.find(kPages);
    const auto* box = def.find(kMapGroup);
    const auto* centred = def.find(kGoMsg);
    const auto* left = def.find(kTypeLabel);
    const auto* list = def.find(kMapList);
    const auto* bigButton = def.find(kDefault);
    if (!tab || !page || !box || !centred || !left || !list || !bigButton) {
        return;
    }
    // The tab: a copy of the Server tab, 60 to its right.
    nox::gui::WindowDef botsTab = *tab;
    botsTab.id = kBotsTab;
    botsTab.x = tab->x + 60;
    botsTab.status = st::Image | st::Enabled;
    botsTab.text.clear();
    botsTab.tooltip.clear();

    // The page: an empty, hidden copy of the Game page's frame.
    nox::gui::WindowDef botsPage = *page;
    botsPage.id = kBotsPage;
    botsPage.status |= st::Hidden;
    botsPage.children.clear();

    // A plain button (the panel's own colours, as Options' drop-downs).
    nox::gui::WindowDef plain = *bigButton;
    plain.backgroundImage.clear();
    plain.enabledImage.clear();
    plain.disabledImage.clear();
    plain.hiliteImage.clear();
    plain.selectedImage.clear();
    plain.text.clear();
    plain.tooltip.clear();
    plain.status = st::Enabled | st::NoFocus;
    plain.style = nox::gui::style::PushButton | nox::gui::style::MouseTrack;
    plain.enabledColor = {150, 110, 40};
    plain.hiliteColor = {240, 180, 42};
    plain.backgroundColor = {16, 12, 8};
    plain.selectedColor = {70, 52, 20};
    plain.disabledColor = {24, 24, 24};
    plain.children.clear();
    // A text copied from one of the file's labels (keeping its font and colours), moved and emptied.
    const auto text = [](const nox::gui::WindowDef& from, std::int32_t id, int x, int y, int w, int h) {
        nox::gui::WindowDef t = from;
        t.id = id;
        t.x = x;
        t.y = y;
        t.width = w;
        t.height = h;
        t.text.clear();
        t.tooltip.clear();
        return t;
    };
    const auto button = [&](std::int32_t id, int x, int y, int w, int h) {
        nox::gui::WindowDef b = plain;
        b.id = id;
        b.x = x;
        b.y = y;
        b.width = w;
        b.height = h;
        return b;
    };

    // The counts: a row a class, and the difficulty.
    nox::gui::WindowDef counts = *box;
    counts.id = kBotsBox;
    counts.x = 10;
    counts.y = 20;
    counts.height = 118;
    counts.children.clear();
    counts.children.push_back(text(*centred, kBotsTitle, 10, 4, 170, 10));
    for (int i = 0; i < 3; ++i) {
        // A row per class (warriors, wizards, conjurers): its name, minus, the count, plus.
        const int y = 20 + 22 * i;
        counts.children.push_back(text(*left, kBotsLabel + i, 8, y, 80, 20));
        counts.children.push_back(button(kBotsMinus + i, 100, y + 1, 18, 18));
        counts.children.push_back(text(*centred, kBotsCount + i, 120, y + 5, 30, 10));
        counts.children.push_back(button(kBotsPlus + i, 152, y + 1, 18, 18));
    }
    // "Difficulty [Easy] to [Hard]": the range each bot's level is picked from.
    counts.children.push_back(text(*left, kTierLabel, 8, 88, 56, 20));
    counts.children.push_back(button(kTierButton, 64, 89, 52, 18));
    counts.children.push_back(text(*centred, kTierTo, 116, 93, 18, 10));
    counts.children.push_back(button(kTierToButton, 134, 89, 52, 18)); // (the box: 190 wide)
    botsPage.children.push_back(std::move(counts));

    // Those in the game.
    nox::gui::WindowDef status = *box;
    status.id = kBotsStatusBox;
    status.x = 10;
    status.y = 144;
    status.height = 170;
    status.children.clear();
    status.children.push_back(text(*centred, kBotsInfo, 10, 4, 170, 10));
    nox::gui::WindowDef lines = text(*list, kBotsList, 5, 18, 180, 148);
    lines.style &= ~nox::gui::style::MouseTrack;
    status.children.push_back(std::move(lines));
    botsPage.children.push_back(std::move(status));

    // "All Leave" along the bottom, in the Defaults button's look.
    nox::gui::WindowDef leave = *bigButton;
    leave.id = kBotsLeave;
    leave.x = 65;
    leave.y = 322;
    leave.text.clear();
    leave.tooltip.clear();
    leave.status |= st::Enabled;
    botsPage.children.push_back(std::move(leave));

    // (after the Game page, so it's under the tabs and buttons as that page is)
    auto& kids = def.children;
    const auto at = std::ranges::find(kids, kPages, &nox::gui::WindowDef::id);
    kids.insert(at == kids.end() ? kids.end() : at + 1, std::move(botsPage));
    kids.push_back(std::move(botsTab));
}

void ServerOptions::setupBotsPage()
{
    const auto set = [this](std::int32_t id, std::string text, std::string tip = {}) {
        if (gui::Window* w = window_->find(id)) {
            w->text = std::move(text);
            w->tooltip = std::move(tip);
        }
    };
    const bool can = static_cast<bool>(callbacks_.bots);
    set(kBotsTab, "Bots", "Bots this game runs itself, to play with or against");
    set(kBotsTitle, "Bots this game runs:");
    for (int i = 0; i < 3; ++i) {
        set(kBotsLabel + i, kClassNames[static_cast<std::size_t>(i)]);
        set(kBotsMinus + i, "-", "One fewer");
        set(kBotsPlus + i, "+", "One more");
    }
    set(kTierLabel, "Difficulty");
    set(kTierButton, "", "The easiest the bots are: click for the next");
    set(kTierTo, "to");
    set(kTierToButton, "", "The hardest the bots are: click for the next");
    set(kBotsLeave, "All Leave", "Every bot leaves");
    if (gui::Window* tab = window_->find(kBotsTab); tab && !can) {
        tab->status &= ~nox::gui::status::Enabled;
    }
    botsShown_ = false;
    refreshBots();
}

void ServerOptions::showTab(Tab tab)
{
    // The Game tab's page (0x27B0), the Server tab's (0x27AF: its General, Access and Teams
    // pages), or noxsdl's Bots tab's.
    botsShown_ = tab == Tab::Bots;
    showTypes(false);
    if (gui::Window* game = window_->find(kPages)) {
        game->show(tab == Tab::Game);
    }
    if (gui::Window* bots = window_->find(kBotsPage)) {
        bots->show(tab == Tab::Bots);
    }
    showServerTab(tab == Tab::Server);
    if (tab == Tab::Bots) {
        refreshBots();
    }
}

// The Bots page refilled from the game: the counts, the difficulty range, how many are in, a line
// a bot.
void ServerOptions::refreshBots()
{
    if (!window_ || !callbacks_.bots) {
        return;
    }
    const BotsView v = callbacks_.bots();
    bots_ = v;
    for (int i = 0; i < 3; ++i) {
        if (gui::Window* w = window_->find(kBotsCount + i)) {
            w->text = std::to_string(v.wanted[static_cast<std::size_t>(i)]);
        }
    }
    if (gui::Window* w = window_->find(kTierButton)) {
        w->text = kTierNames[static_cast<std::size_t>(std::clamp(v.minTier, 0, 2))];
    }
    if (gui::Window* w = window_->find(kTierToButton)) {
        w->text = kTierNames[static_cast<std::size_t>(std::clamp(v.maxTier, 0, 2))];
    }
    if (gui::Window* w = window_->find(kBotsInfo)) {
        const int wanted = v.wanted[0] + v.wanted[1] + v.wanted[2];
        w->text = "In the game: " + std::to_string(v.in) + " of " + std::to_string(wanted) + " (" +
                  std::to_string(v.most) + " at most)";
    }
    if (gui::Window* list = window_->find(kBotsList)) {
        list->lines.clear();
        for (const auto& line : v.lines) {
            list->lines.push_back({line, list->textColor});
        }
    }
}

void ServerOptions::changeBots(int playerClass, int by)
{
    // A count up or down (to the most there may be, all classes together): at once.
    if (!callbacks_.setBots) {
        return;
    }
    auto wanted = bots_.wanted;
    auto& n = wanted[static_cast<std::size_t>(playerClass)];
    const int total = wanted[0] + wanted[1] + wanted[2];
    if (by > 0 && total >= bots_.most) {
        return;
    }
    n = std::max(0, n + by);
    callbacks_.setBots(wanted, bots_.minTier, bots_.maxTier);
    refreshBots();
}

void ServerOptions::changeTier(bool to)
{
    if (!callbacks_.setBots) {
        return;
    }
    int lo = std::clamp(bots_.minTier, 0, 2);
    int hi = std::clamp(bots_.maxTier, lo, 2);
    if (to) {
        hi = (hi + 1) % 3;
        lo = std::min(lo, hi); // (Hard round to Easy: from comes down with it)
    } else {
        lo = (lo + 1) % 3;
        hi = std::max(hi, lo); // (from past to: to goes up with it)
    }
    callbacks_.setBots(bots_.wanted, lo, hi);
    refreshBots();
}

std::string ServerOptions::typeName(std::uint32_t mode) const
{
    // guiServerOptionsGetGametypeName: else the second's (Arena).
    for (const auto& t : kTypes) {
        if ((mode & 0x17F0) == t.flags) {  // (0x17F0: the type bits of the game flags)
            return text(t.name);
        }
    }
    return text(kTypes[1].name);
}

void ServerOptions::sizeTypeList()
{
    // six lines high, as wide as the widest name and 7, its right edge kept.
    gui::Window* list = window_->find(kTypeList);
    if (!list) {
        return;
    }
    gui::Painter p(nullptr, gui_.resources(), 1.0f, 0, 0);
    int widest = 0;
    for (const auto& t : kTypes) {
        widest = std::max(widest, p.textWidth(text(t.name), list->largeFont));
    }
    const int right = list->rect.x + list->rect.w;
    list->rect.h = (p.fontHeight(list->largeFont) + 1) * 6 + 2;
    list->rect.w = widest + 7;
    list->rect.x = right - list->rect.w;
}

void ServerOptions::fillMaps()
{
    // client_guiserv_updateMapList: the maps of the type, "<name>\t<min>-<max>
    // Players" (RecPlayers), the current one selected and scrolled to (else the first selected);
    // the selected one is the settings' map.
    gui::Window* list = window_->find(kMapList);
    if (!list) {
        return;
    }
    list->lines.clear();
    shown_.clear();
    int selected = -1;
    for (const auto& m : maps_) {
        if (!(m.modes & settings_.mode)) {
            continue;
        }
        if (m.name == settings_.map) {
            selected = static_cast<int>(shown_.size());
        }
        shown_.push_back(m.name);
        list->lines.push_back({format(text("RecPlayers"), {m.name, std::to_string(m.minPlayers),
                                                           std::to_string(m.maxPlayers)}),
                               list->textColor});
    }
    list->selectedLine = selected < 0 ? 0 : selected;
    if (shown_.empty()) {
        list->selectedLine = -1;
    }
    scrollMaps(selected < 0 ? 0 : selected);
    settings_.map = list->selectedLine >= 0 ? shown_[static_cast<std::size_t>(list->selectedLine)] : std::string();
    readPendingRules(); // (its user.rul for the type: flags 7)
}

// The map list scrolled to line `first` (clamped to what fits).
void ServerOptions::scrollMaps(int first)
{
    gui::Window* list = window_->find(kMapList);
    if (!list) {
        return;
    }
    const int most = std::max(0, static_cast<int>(list->lines.size()) - kMapLinesShown);
    list->scroll = std::clamp(first, 0, most);
    // The slider (its maximum at the top) follows.
    if (gui::Window* slider = window_->find(kMapSlider)) {
        if (const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data)) {
            slider->sliderValue = gui::sliderFromScroll(*sl, list->scroll, most);
        }
    }
}

void ServerOptions::fillLimits()
{
    // The type's limits as they stand (servGamedataGet).
    if (callbacks_.limits) {
        const auto [lesson, time] = callbacks_.limits(settings_.mode);
        settings_.lessonLimit = lesson;
        settings_.timeLimit = time;
    }
    showLimits();
}

void ServerOptions::showLimits()
{
    if (gui::Window* w = window_->find(kLessonLimit)) {
        w->entry = nox::strings::fromUtf8(std::to_string(settings_.lessonLimit)).substr(0, w->entryMax);
    }
    if (gui::Window* w = window_->find(kTimeLimit)) {
        w->entry = nox::strings::fromUtf8(std::to_string(settings_.timeLimit)).substr(0, w->entryMax);
    }
}

void ServerOptions::setLabels()
{
    // the lesson limit's label (Capture Limit in CTF, Elim Limit in elimination);
    // the maps and limits off while a new game's chosen.
    if (gui::Window* w = window_->find(kLessonLimit)) {
        const char* label = (settings_.mode & 0x20)    ? "Servopts.wnd:CaptureLimit"
                            : (settings_.mode & 0x400) ? "Servopts.wnd:DeathLimit"
                                                       : "Servopts.wnd:KillLimit";
        w->text = gui_.text(label);
    }
    enable(window_->find(kMapGroup), !newGame_);
    enable(window_->find(kLimitsGroup), !newGame_);
    if (gui::Window* box = window_->find(kNewGame)) {
        box->state = newGame_ ? box->state | gui::kPressed : box->state & ~gui::kPressed;
    }
    // the settings' map, the type, and what to do.
    if (gui::Window* w = window_->find(kSettingsMsg)) {
        w->text = format(text("SettingsMsg"), {settings_.map});
    }
    if (gui::Window* w = window_->find(kTypeLabel)) {
        w->text = (startMode_ & 0x80) ? text("GameType") : format(text("GameTypeIs"), {typeName(startMode_)});
    }
    if (gui::Window* w = window_->find(kGoMsg)) {
        w->text = text("GoMessage");
    }
}

void ServerOptions::showTypes(bool on)
{
    // 0x2787: the types' list over the button, taking the input
    // (windowServerOptionsFillGametypeList); the mouse off it closes it again
    // (windowServerOptionsDrawProc).
    gui::Window* list = window_->find(kTypeList);
    if (!list) {
        return;
    }
    if (on) {
        list->lines.clear();
        for (const auto& t : kTypes) {
            if (t.flags != 0x1000) {
                list->lines.push_back({text(t.name), list->textColor});
            }
        }
        list->selectedLine = -1;
        list->scroll = 0;
        list->show(true);
        gui_.setModal(list);
    } else {
        list->show(false);
        gui_.setModal(nullptr);
    }
}

bool ServerOptions::onMessage(gui::Window& from, int message)
{
    // guiServerOptionsProcPre.
    // The map list's slider: its maximum is the top of the list.
    if (message == gui::SliderChanged && from.id == kMapSlider) {
        gui::Window* list = window_->find(kMapList);
        const auto* sl = std::get_if<nox::gui::SliderData>(&from.data);
        if (list && sl) {
            const int most = std::max(0, static_cast<int>(list->lines.size()) - kMapLinesShown);
            list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
        }
        return true;
    }
    // The Server tab's pages take their own controls first.
    if (onServerMessage(from, message)) {
        return true;
    }
    if (message != gui::Clicked) {
        return false;
    }
    if (from.id != kMapList && from.id != kTypeList && from.id != kBotsList) {
        gui_.resources().sound(0x2FE); // ButtonPress
    }
    // The bot count buttons, one minus and one plus per class.
    if (from.id >= kBotsMinus && from.id < kBotsMinus + 3) {
        changeBots(from.id - kBotsMinus, -1);
        return true;
    }
    if (from.id >= kBotsPlus && from.id < kBotsPlus + 3) {
        changeBots(from.id - kBotsPlus, 1);
        return true;
    }
    switch (from.id) {
    case kGameTab:
        deferred_.push_back([this] { showTab(Tab::Game); });
        return true;
    case kServerTab:
        deferred_.push_back([this] { showTab(Tab::Server); });
        return true;
    case kBotsTab:
        deferred_.push_back([this] { showTab(Tab::Bots); });
        return true;
    case kTierButton:
        changeTier(false);
        return true;
    case kTierToButton:
        changeTier(true);
        return true;
    // Every bot out, the difficulty range kept.
    case kBotsLeave:
        if (callbacks_.setBots) {
            callbacks_.setBots({0, 0, 0}, bots_.minTier, bots_.maxTier);
            refreshBots();
        }
        return true;
    case kBotsList:
        return true;
    case kTypeButton:
        deferred_.push_back([this] { showTypes(true); });
        return true;
    case kTypeList: {
        // 0x4010 from 0x2788: the type chosen; its maps, its limits.
        std::vector<std::uint32_t> listed;
        for (const auto& t : kTypes) {
            if (t.flags != 0x1000) {
                listed.push_back(t.flags);
            }
        }
        const int line = from.selectedLine;
        deferred_.push_back([this, line, listed] {
            showTypes(false);
            if (line < 0 || line >= static_cast<int>(listed.size())) {
                return;
            }
            const std::uint32_t mode = listed[static_cast<std::size_t>(line)];
            // (Into the current slot: the type; another type, client_guiserv_updateMapList's
            // chosen line's word and its rules; the type's limits(1).)
            // (0x2788's 0x4010: the line's text on the type button, 0x4001, and looked up,
            // the type's own flags, into the slot's type: Chat's 0x80 too.)
            SettingsEdit e;
            e.type = static_cast<std::uint16_t>(mode);
            if (mode != settings_.mode) {
                settings_.mode = mode;
                fillMaps();
                e.map = chosenMapWord();
                e.rules = rules_;
            }
            if (gui::Window* button = window_->find(kTypeButton)) {
                for (const auto& t : kTypes) {
                    if (t.flags == mode) {
                        button->text = text(t.name);
                    }
                }
            }
            fillLimits();
            setLabels();
            e.changed = true;
            editSlot(e);
        });
        return true;
    }
    case kMapList:
        if (from.selectedLine >= 0 && from.selectedLine < static_cast<int>(shown_.size())) {
            settings_.map = shown_[static_cast<std::size_t>(from.selectedLine)];
            readPendingRules(); // (0x2782: flags 7)
            setLabels();
            // (0x4010 from 0x2782: the line's word at the current slot's + 0, its rules there,
            // the window marked changed.)
            SettingsEdit e;
            e.map = chosenMapWord();
            e.rules = rules_;
            e.changed = true;
            editSlot(e);
        }
        return true;
    case kMapUp:
        if (gui::Window* list = window_->find(kMapList)) {
            scrollMaps(list->scroll - 1);
        }
        return true;
    case kMapDown:
        if (gui::Window* list = window_->find(kMapList)) {
            scrollMaps(list->scroll + 1);
        }
        return true;
    case kNewGame:
        newGame_ = (from.state & gui::kPressed) != 0;
        setLabels();
        editSlot({.changed = true}); // (0x278A: the window marked changed)
        return true;
    case kDefault:
        // 0x279D: the type's limits as the game began (here: no user.rul for each map yet).
        if (callbacks_.defaults) {
            const auto [lesson, time] = callbacks_.defaults(settings_.mode);
            settings_.lessonLimit = lesson;
            settings_.timeLimit = time;
            showLimits();
        }
        // Defaults: the map's user.rul deleted, its own rules read again
        // (flags 5).
        if (callbacks_.deleteRuleSet && !settings_.map.empty()) {
            callbacks_.deleteRuleSet(settings_.map, "user");
            readPendingRules();
        }
        {
            // (the map's rules into the current slot(1).)
            SettingsEdit e;
            e.rules = rules_;
            e.changed = true;
            editSlot(e);
        }
        return true;
    case kAdvanced:
        // 0x27A8: loadAdvancedWnd with the settings (Game_GetSettingsData(1)).
        deferred_.push_back([this] { openAdvanced(); });
        return true;
    case kGo:
        deferred_.push_back([this] { go(); });
        return true;
    case kCancel:
    case kOk:
        deferred_.push_back([this] { close(); });
        return true;
    default:
        return false;
    }
}

void ServerOptions::go()
{
    // 0x27A1: into slot 1, slot 1 copied to slot 0 (before the notices below).
    if (callbacks_.windowGo) {
        callbacks_.windowGo(windowState());
    }
    // Then Go's: the name (15 letters), the limits typed (an empty field
    // keeps its value; the time a byte), and "Form a new game" for the chat map.
    if (gui::Window* name = window_->find(kName)) {
        settings_.name = nox::strings::toUtf8(name->entry).substr(0, 15);
    }
    if (gui::Window* w = window_->find(kLessonLimit); w && !w->entry.empty()) {
        settings_.lessonLimit = std::clamp(number(w), 0, 0xFFFF);
    }
    if (gui::Window* w = window_->find(kTimeLimit); w && !w->entry.empty()) {
        settings_.timeLimit = std::clamp(number(w), 0, 0xFF); // (0x4003's clamp)
    }
    Settings s = settings_;
    s.rules = rules_; // (the settings' rules: written into the map's user.rul)
    s.flagTeams = (settings_.mode & 0x60) != 0;  // (CTF 0x20 or Flag Ball 0x40)
    if (newGame_) {
        s.mode = 0x80;
        s.map = callbacks_.chatMap ? callbacks_.chatMap() : std::string();
    }
    const auto apply = [this, s] {
        close();
        if (callbacks_.apply && !s.map.empty()) {
            callbacks_.apply(s);
        }
    };
    // 0x27A1 in a chat game with Capture the Flag or Flag Ball chosen (not by clan, the settings'
    // 0x8000: no clan games here): fewer than two teams, Gui_Dialog_Create(0x38: Yes / No)
    // guiserv.c:NeedTeams, its Yes (the teams made); more than two,
    // guiserv.c:TooManyTeams (0x21) and no Go. (Its team-KotR check, under game flag 0x10 in a
    // chat game, can't be met.)
    if ((startMode_ & 0x80) && (settings_.mode & 0x60) && callbacks_.current) {
        const int teams = callbacks_.current().teams;
        if (teams < 2) {
            openNotice(std::string(), text("NeedTeams"), 0x38, apply);
            return;
        }
        if (2 < teams) {
            openNotice(text("Notice"), text("TooManyTeams"));
            return;
        }
    }
    apply();
}

void ServerOptions::commitName()
{
    // 0x4003 for 0x2775 (the field left): a name typed is the slot's and the game's at once
    // (gameSetServername), whether Go follows or not.
    gui::Window* w = window_ ? window_->find(kName) : nullptr;
    if (!w || w->entry.empty()) {
        return;
    }
    settings_.name = nox::strings::toUtf8(w->entry).substr(0, 15);
    if (callbacks_.editSettings) {
        SettingsEdit e; // (strcpy'd into the current slot's + 9, and the game's: GameHost::editSettings)
        e.name = settings_.name;
        editSlot(e);
    } else if (callbacks_.setName) {
        callbacks_.setName(settings_.name);
    }
}

// The buttons' queued work, run from update(): it may open, close or destroy windows.
void ServerOptions::runDeferred()
{
    gui::runQueued(deferred_);
}

void ServerOptions::draw(SDL_Renderer* renderer, int width, int height)
{
    if (window_) {
        gui_.draw(renderer, width, height);
    }
}

bool ServerOptions::handleEvent(SDL_Window* sdlWindow, const SDL_Event& event, int width, int height)
{
    if (!window_) {
        return false;
    }
    sdlWindow_ = sdlWindow;
    // What had the keys before this event, to tell below which field it has left.
    const bool wasTyping = gui_.focus() != nullptr;
    const bool namingWas = wasTyping && gui_.focus()->id == kName;
    const std::int32_t limitWas = wasTyping && (gui_.focus()->id == kLessonLimit || gui_.focus()->id == kTimeLimit) ? gui_.focus()->id : 0;
    bool took = false;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        // guiServerOptionsProc: Escape closes it (0xE7). Enter or Tab leaves a field.
        if (event.key.key == SDLK_ESCAPE && !event.key.repeat) {
            // (Over it the Advanced Server window takes the keys: nothing; the
            // rename dialog's Cancel.)
            // (Advanced...: nothing either; its notice goes.)
            if (notice_) {
                closeNotice();
            } else if (rename_) {
                closeRename();
            } else if (!advserv_ && !advanced_) {
                gui_.resources().sound(0xE7);
                close();
            }
            took = true;
            break;
        }
        if (wasTyping && (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_TAB)) {
            if (rename_) {
                if (gui::Window* ok = rename_->find(4001)) {
                    onRename(*ok, gui::Clicked); // (Enter: its OK)
                }
                took = true;
                break;
            }
            gui_.setFocus(nullptr);
            if (advserv_) {
                applyAdvServAmount(); // (0x4003: the field left)
            } else if (serverPage_ == 1) {
                commitAccessFields();
            }
            took = true;
            break;
        }
        took = gui_.handleEvent(event, width, height) || wasTyping;
        break;
    case SDL_EVENT_TEXT_INPUT:
        took = gui_.handleEvent(event, width, height) || wasTyping;
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        // The type list closes once the mouse leaves it (it is a drop-down).
        gui::Window* types = window_->find(kTypeList);
        if (types && !types->hidden()) {
            const SDL_FPoint at = gui_.layoutPoint(event.motion.x, event.motion.y, width, height);
            const SDL_Point pos = types->screenPos();
            if (at.x < pos.x || at.y < pos.y || at.x >= pos.x + types->rect.w || at.y >= pos.y + types->rect.h) {
                showTypes(false);
            }
        }
        took = gui_.handleEvent(event, width, height);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // A click away from the fields leaves them. (By the window the press went to: the hover
        // also finds disabled windows, so a label over a field isn't a click away from it.)
        took = gui_.handleEvent(event, width, height);
        if (gui::Window* f = gui_.focus(); f && gui_.pressed() != f) {
            gui_.setFocus(nullptr);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        took = gui_.handleEvent(event, width, height);
        break;
    default:
        break;
    }
    // The name field left (0x4003 for 0x2775): the game's name at once.
    if (window_ && namingWas && (gui_.focus() == nullptr || gui_.focus()->id != kName)) {
        commitName();
    }
    // A limit field left (0x4003 for 0x2796 / 0x2797): into the settings' slot 1.
    if (window_ && limitWas != 0 && (gui_.focus() == nullptr || gui_.focus()->id != limitWas)) {
        commitLimit(limitWas);
    }
    // The rule sets' buttons follow the name typed and the one chosen.
    if (advanced_) {
        ruleButtons();
    }
    // The Access page: a field left is taken (0x4003); Add follows the name typed.
    if (window_ && serverPage_ == 1 && !advserv_) {
        if (wasTyping && gui_.focus() == nullptr) {
            commitAccessFields();
        }
        accessButtons();
    }
    // Typing in a field: the keys are text till it's left. (Closed, close() saw to it.)
    const bool typing = gui_.focus() != nullptr;
    if (window_ && typing != wasTyping && sdlWindow) {
        if (typing) {
            SDL_StartTextInput(sdlWindow);
        } else {
            SDL_StopTextInput(sdlWindow);
        }
    }
    return took;
}

} // namespace noxsdl
