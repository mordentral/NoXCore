// The front-end menus' screens and their window procedures (menus.hpp lists the screens and how
// they lead to one another); the XWIS chat lobby screen is in menus_wolchat.cpp.
#include "menus.hpp"

#include "cinematics.hpp"
#include "client_level_gate.hpp"
#include "console.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/solo_menu.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/server/rules.hpp"
#include "noxcore/server/solo_save.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <random>
#include <string_view>

namespace noxsdl {

namespace st = nox::gui::status;

namespace {

// MainMenu.wnd
constexpr std::int32_t kMainTop = 110, kMainBottom = 120;
constexpr std::int32_t kSolo = 111, kMultiplay = 112, kSoloQuest = 131, kIntro = 121, kCredits = 122;
// ArnaMain.wnd
constexpr std::int32_t kOnlineTop = 410, kOnlineBottom = 420, kWestwoodOnline = 411, kNetwork = 421;
// OptsBack.wnd
constexpr std::int32_t kOptions = 151, kBack = 152;
// wolapi.wnd
constexpr std::int32_t kLoginName = 1701, kLoginPassword = 1702, kLoginSave = 1703, kLoginSkip = 1705,
                       kLoginGo = 1706, kLoginList = 1708, kLoginSlider = 1709, kLoginUp = 1710, kLoginDown = 1711;
/// How many of the login list's lines show at once (79 high, less its title line, 11 a line).
constexpr int kLoginLinesShown = 6;
// noxworld.wnd
constexpr std::int32_t kWorldListPanel = 10033, kWorldGameList = 10037, kWorldMap = 10020, kWorldMapQuads = 10021,
                       kHost = 10002, kHostQuest = 10003, kList = 10004, kFilters = 10005, kRefresh = 10006,
                       kFind = 10007, kStatus = 10011, kWorldBack = 10010, kJoin = 10001;
// noxworld.wnd's description box (10033): its list, arrows and slider.
constexpr std::int32_t kInfoList = 10034, kInfoUp = 10035, kInfoDown = 10036, kInfoSlider = 10032;
// SelChar.wnd
constexpr std::int32_t kCharNew = 501, kCharLoad = 502, kCharDelete = 503, kCharUp = 504, kCharDown = 505,
                       kCharLabel = 509, kCharLists = 510, kCharKind = 511, kCharName = 512;
// dlg.wnd
constexpr std::int32_t kDlgOk = 4001, kDlgCancel = 4002, kDlgText = 4004, kDlgTitle = 4005, kDlgNo = 4006,
                       kDlgYes = 4007;
/// Class names, for "nox.str:%s"; also SelClass's descriptions.
constexpr const char* kClassNames[3] = {"Warrior", "Wizard", "Conjurer"};
/// Where the world map and the game list sit in noxworld.wnd: our list goes there.
constexpr SDL_Rect kListFrame{216, 27, 384, 424};
// SelClass.wnd: the portraits are warrior 601, wizard 602, conjurer 603 (class = id - 601).
constexpr std::int32_t kClassFirst = 601, kClassLast = 603, kClassText = 605, kClassAccept = 610;
// SelColor.wnd
constexpr std::int32_t kColorSkin = 720, kColorHair = 721, kColorMustache = 722, kColorGoatee = 723,
                       kColorBeard = 724, kColorPants = 725, kColorShirt1 = 726, kColorShirt2 = 727,
                       kColorShoes1 = 728, kColorShoes2 = 729;
constexpr std::int32_t kHairLabel = 711, kMustacheLabel = 712, kGoateeLabel = 713, kBeardLabel = 714;
constexpr std::int32_t kToggleFirst = 731, kToggleLast = 734; // switch 711-714 (id - 20)
constexpr std::int32_t kPreview = 740, kPicker = 760, kSwatchFirst = 761, kSwatchLast = 792, kNameEntry = 751,
                       kColorAccept = 799;

/// The palettes' fixed tables: hair (1) and skin (2). Table 0, the clothes, is
/// modifier.bin's UserColor1-32.
constexpr std::array<std::array<std::uint8_t, 3>, 32> kHairColors{{
    {0xf3, 0xb7, 0x9f}, {0xc7, 0x84, 0x3a}, {0xff, 0xaa, 0x56}, {0x92, 0x79, 0x6a}, {0xff, 0xfb, 0xba},
    {0xf7, 0x44, 0x44}, {0x3f, 0xbb, 0xef}, {0xc4, 0xc2, 0xb1}, {0xfd, 0xf6, 0x66}, {0x73, 0x4d, 0x22},
    {0xe5, 0x79, 0x19}, {0x52, 0x44, 0x3c}, {0xeb, 0xe5, 0x89}, {0x93, 0x00, 0x00}, {0x00, 0x25, 0x44},
    {0xd6, 0xd6, 0xd6}, {0x95, 0x6c, 0x2b}, {0x34, 0x19, 0x00}, {0x96, 0x37, 0x00}, {0x09, 0x00, 0x0c},
    {0x96, 0x92, 0x51}, {0x83, 0x68, 0xb3}, {0x79, 0xa5, 0x42}, {0x75, 0x75, 0x75}, {0x64, 0x82, 0x7d},
    {0x38, 0x30, 0x28}, {0x6c, 0x51, 0x3a}, {0x34, 0x28, 0x27}, {0x8f, 0x8e, 0x7c}, {0x38, 0x1d, 0x68},
    {0x0f, 0x58, 0x00}, {0x57, 0x35, 0x35},
}};
constexpr std::array<std::array<std::uint8_t, 3>, 32> kSkinColors{{
    {0xe9, 0xde, 0xcd}, {0xde, 0xb6, 0xa8}, {0xda, 0x9a, 0x6e}, {0xec, 0xca, 0xb2}, {0x9a, 0x83, 0x69},
    {0x75, 0x5f, 0x4a}, {0xef, 0xe9, 0xc1}, {0xe4, 0xe4, 0xc1}, {0xcb, 0xb9, 0x9c}, {0xc8, 0xa4, 0x6e},
    {0xa9, 0x83, 0x4f}, {0xf3, 0x9e, 0x77}, {0x73, 0x4d, 0x22}, {0x5b, 0x37, 0x14}, {0xf9, 0xcd, 0x8a},
    {0xf0, 0xeb, 0xab}, {0x86, 0x8f, 0x9a}, {0x87, 0x66, 0x43}, {0x73, 0x50, 0x2e}, {0xa5, 0x5d, 0x46},
    {0x63, 0x41, 0x25}, {0x53, 0x34, 0x1d}, {0xa8, 0x7f, 0x54}, {0xab, 0xa0, 0x60}, {0x8d, 0x88, 0x76},
    {0x7f, 0x74, 0x52}, {0x60, 0x54, 0x33}, {0xa2, 0x81, 0x64}, {0x4e, 0x40, 0x2e}, {0x41, 0x32, 0x2d},
    {0xab, 0x9e, 0x74}, {0xaf, 0x74, 0x83},
}};

/// A colour button's data word: palette table in the low half, index in the high half.
constexpr std::uint32_t colorData(std::uint32_t table, std::uint32_t index)
{
    return (index << 16) | table;
}

/// The name check before saving: leading spaces go, the first character becomes '-'
/// if a file name can't have it, trailing spaces go. False if nothing but spaces was typed.
bool cleanName(std::u16string& name)
{
    const auto space = [](char16_t c) { return c == u' ' || (c >= u'\t' && c <= u'\r') || c == 0x3000; };
    const auto first = std::find_if_not(name.begin(), name.end(), space);
    if (first == name.end()) {
        return false;
    }
    name.erase(name.begin(), first);
    if (std::u16string_view(u"*?<>\\/:\"|").find(name.front()) != std::u16string_view::npos) {
        name.front() = u'-';
    }
    while (!name.empty() && space(name.back())) {
        name.pop_back();
    }
    return true;
}

} // namespace

Menus::Menus(gui::Resources res, std::filesystem::path gameDir, BrowserScreen& browser,
             nox::client::PlayerProfile profile, const nox::things::ModifierDb& modifiers, Callbacks callbacks)
    : gui_(std::move(res)), windowDir_(gameDir / "window"), gameDir_(gameDir), saveDir_(gameDir / "Save"),
      saveRoot_(gameDir), profile_(std::move(profile)), browser_(browser), callbacks_(std::move(callbacks)),
      modifiers_(modifiers)
{
    // wndLoadMainBG: the background and the Options / Back bar stay loaded; the shell is
    // up (its windows' text large).
    gui::setShellUp(true);
    background_ = gui_.load(windowDir_ / "MainBG.wnd", nullptr);
    if (gui::Window* loading = background_->find(99)) {
        loading->show(false); // "Loading..." is only for loading a game
    }
    // Its draw function: the background, then the eyes' lids.
    background_->draw = [this](gui::Window& w, gui::Painter& p) { drawBackground(w, p); };
    optsBack_ = gui_.load(windowDir_ / "OptsBack.wnd",
                          [this](gui::Window&, int message, gui::Window& from) {
                              return shellSound(message) && message == gui::Clicked && onOptsBack(from);
                          });
    // The game list's R / F5 as its Refresh button.
    browser_.setRefreshHandler([this](std::uint64_t nowMs) {
        nowMs_ = nowMs;
        refreshGames();
    });
}

Menus::~Menus() = default;

bool Menus::shellSound(int message) const
{
    // The shell screens' procedures (windowMainMenuProc...): ShellSelect as the
    // pointer comes onto a button, ShellClick as one is clicked.
    if (message == gui::Hover) {
        gui_.resources().sound(0x398);
    } else if (message == gui::Clicked) {
        gui_.resources().sound(0x399);
    }
    return true;
}

void Menus::wip(gui::Window* top, std::int32_t id)
{
    // Not hooked up yet: off, and marked.
    if (gui::Window* w = top ? top->find(id) : nullptr) {
        enable(top, id, false);
        w->text += " - WIP";
    }
}

void Menus::reset()
{
    closeDialog();
    if (screen_) {
        gui_.destroy(screen_);
        screen_ = nullptr;
    }
    if (picker_) {
        gui_.destroy(picker_);
        picker_ = nullptr;
    }
    slides_.clear();
    deferred_.clear();
    leaving_ = false;
    credits_.reset();
}

void Menus::showMainMenu()
{
    nowMs_ = SDL_GetTicks(); // (the clock stood still through a game)
    reset();
    showLoading(false);
    stack_.clear();
    enter(State::MainMenu);
}

void Menus::showGameBrowser()
{
    // Back from a game the list is fetched again, from now: the menus' clock stood still through
    // the game, and a refresh from then had timed out before it began.
    nowMs_ = SDL_GetTicks();
    reset();
    stack_ = {State::MainMenu, State::Online};
    if (wolMode_ && wolLogin_ && !skippedLogin_) {
        // Out of the chat: logged in again (the connection was let go for the game), its Back the
        // chat's.
        stack_.push_back(State::Login);
        stack_.push_back(State::Chat);
        if (!wol_) {
            wol_.emplace(wolLogin_->nick, wolLogin_->password, static_cast<std::uint32_t>(nowMs_));
        }
    }
    enter(State::Browser);
}

void Menus::notice(const std::string& titleId, const std::string& textId)
{
    dialog(gui_.text(titleId), gui_.text(textId), 0x21, {});
}

void Menus::askJoinPassword(const std::string& host, const nox::client::PlayerProfile& profile, bool wrong)
{
    // The client's lobby wait on PASSWORD_REQUIRED: Gui_Dialog_Create's title noxworld.c:Password,
    // its text PasswordRequired, 7 (OK, Cancel, the entry), OK on only with the entry not empty;
    // its OK sends SERVER_PASSWORD. (noxsdl's join gives up at the request and is made again
    // here with the password; a refused one, the dialog again with the game's ErrWrongPassword.)
    if (host.empty() || host.front() == '@') {
        return; // (our own game: never asked)
    }
    pendingHost_ = host;
    constexpr std::int32_t kDlgEntry = 4003; // (dlg.wnd's short entry: Gui_Dialog_Create's 4)
    closeDialog();
    dialog_ = gui_.load(windowDir_ / "dlg.wnd", [this, profile](gui::Window&, int message, gui::Window& from) {
        const bool ok = (message == gui::Clicked && from.id == kDlgOk) || (message == gui::EntryEntered && from.id == kDlgEntry);
        const bool cancel = message == gui::Clicked && from.id == kDlgCancel;
        if (!ok && !cancel) {
            return false;
        }
        const gui::Window* entry = dialog_ ? dialog_->find(kDlgEntry) : nullptr;
        const std::string typed = entry ? nox::strings::toUtf8(entry->entry) : std::string();
        if (ok && typed.empty()) {
            return true; // (OK only with something typed)
        }
        gui_.resources().sound(0x399);
        deferred_.push_back([this, ok, typed, profile] {
            closeDialog();
            if (ok) {
                nox::client::PlayerProfile joining = profile;
                joining.password = typed;
                join(joining);
            }
        });
        return true;
    });
    gui::centreOnScreen(*dialog_);
    dialog_->status |= st::Above;
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = gui_.text("Noxworld.c:Password");
    }
    if (gui::Window* t = dialog_->find(kDlgText)) {
        t->text = gui_.text(wrong ? "Noxworld.c:ErrWrongPassword" : "Noxworld.c:PasswordRequired");
    }
    for (const auto id : {kDlgOk, kDlgCancel, kDlgEntry}) {
        if (gui::Window* w = dialog_->find(id)) {
            w->show(true);
        }
    }
    gui_.setModal(dialog_);
    gui_.setFocus(dialog_->find(kDlgEntry));
}

void Menus::tooOldNotice()
{
    dialog(gui_.text("Noxworld.c:Notification"), std::string(nox::lobby::kClientTooOldNotice), 0x21, {});
}

void Menus::enable(gui::Window* top, std::int32_t id, bool on)
{
    if (gui::Window* w = top ? top->find(id) : nullptr) {
        if (on) {
            w->status |= st::Enabled;
        } else {
            w->status &= ~st::Enabled;
            w->state = 0;
        }
    }
}

void Menus::setBackText(const char* id)
{
    // OptsBack's button reads "Quit" on the main menu, "Back" elsewhere.
    if (gui::Window* b = optsBack_->find(kBack)) {
        b->text = gui_.text(id);
    }
}

// Pushes screen `s` on the menu stack and builds it: its .wnd loaded, its slide-in started,
// its lists and buttons filled. Usually run from leave()'s callback, once the old screen is gone.
void Menus::enter(State s)
{
    stack_.push_back(s);
    slides_.clear();
    leaving_ = false;
    const auto slide = [&](std::int32_t id, SDL_Point from, SDL_Point in, SDL_Point out) {
        if (gui::Window* w = screen_->find(id)) {
            slides_.push_back(gui_.slideIn(w, from, in, out));
        }
    };
    switch (s) {
    case State::MainMenu:
        // game_showMainMenu: the top half from 270 above, the bottom half from 510 down.
        screen_ = gui_.load(windowDir_ / "MainMenu.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return shellSound(message) && message == gui::Clicked && onMainMenu(from);
        });
        slide(kMainTop, {0, -270}, {0, 20}, {0, -40});
        slide(kMainBottom, {0, 510}, {0, -20}, {0, 40});
        if (gui::Window* modes = screen_->find(kSoloQuest)) {
            // (noxcpp: Solo Quest's button opens Game Modes, soloModes; the .wnd's own font,
            // only its GeneralPrint:SoloQuest text replaced. No main-menu button has a tooltip.)
            modes->text = "Game Modes";
        }
        setBackText("OptsBack.wnd:Quit");
        optsBack_->show(true);
        break;
    case State::Online:
        // The Online screen: from 240 above and 480 down.
        screen_ = gui_.load(windowDir_ / "ArnaMain.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return shellSound(message) && message == gui::Clicked && onOnline(from);
        });
        slide(kOnlineTop, {0, -240}, {0, 20}, {0, -40});
        slide(kOnlineBottom, {0, 480}, {0, -20}, {0, 40});
        setBackText("OptsBack.wnd:Back");
        optsBack_->show(true);
        break;
    case State::SelChar: {
        // game_showSelChar: from 440 above; the three lists move together.
        screen_ = gui_.load(windowDir_ / "SelChar.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return shellSound(message) && message == gui::Clicked && onSelChar(from);
        });
        slides_.push_back(gui_.slideIn(screen_, {0, -440}, {0, 20}, {0, -40}));
        if (gui::Window* lists = screen_->find(kCharLists)) {
            lists->proc = screen_->proc; // wndSetProc: the lists' clicks reach the screen
        }
        if (gui::Window* label = screen_->find(kCharLabel)) {
            label->text = gui_.text(solo_ ? "LoadLabel" : "LoadCharLabel", "SelChar.c");
        }
        if (solo_) {
            // (game_showSelChar under game flag 0x800: the arrows hidden.)
            for (const auto id : {kCharUp, kCharDown}) {
                if (gui::Window* w = screen_->find(id)) {
                    w->show(false);
                }
            }
        }
        setBackText("OptsBack.wnd:Back");
        optsBack_->show(true);
        enable(screen_, kCharNew, true); // (findAutosaves's start)
        if (solo_) {
            fillSoloList();
        } else {
            loadCharacters();
            fillCharacterList();
        }
        break;
    }
    case State::SelClass: {
        // game_showSelClass: from 460 above. The portraits not picked are darkened.
        screen_ = gui_.load(windowDir_ / "SelClass.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return shellSound(message) && onSelClass(message, from);
        });
        slides_.push_back(gui_.slideIn(screen_, {0, -460}, {0, 20}, {0, -40}));
        chosenClass_ = -1;
        for (std::int32_t id = kClassFirst; id <= kClassLast; ++id) {
            if (gui::Window* w = screen_->find(id)) {
                w->draw = [this](gui::Window& self, gui::Painter& p) {
                    if (self.id != chosenClass_) {
                        const SDL_Point pos = self.screenPos();
                        p.fillAlpha(pos.x, pos.y, self.rect.w, self.rect.h, {0, 0, 0, 128});
                    }
                };
            }
        }
        setBackText("OptsBack.wnd:Back");
        optsBack_->show(true);
        break;
    }
    case State::SelColor:
        // game_showSelColor: from 440 above.
        screen_ = gui_.load(windowDir_ / "SelColor.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return shellSound(message) && message == gui::Clicked && onSelColor(from);
        });
        slides_.push_back(gui_.slideIn(screen_, {0, -440}, {0, 20}, {0, -40}));
        setupSelColor();
        setBackText("OptsBack.wnd:Back");
        optsBack_->show(true);
        break;
    case State::Login:
        // The login: from 480 above.
        screen_ = gui_.load(windowDir_ / "wolapi.wnd", [this](gui::Window&, int message, gui::Window& from) {
            return onLogin(message, from);
        });
        slides_.push_back(gui_.slideIn(screen_, {0, -480}, {0, 20}, {0, -40}));
        setBackText("OptsBack.wnd:Back");
        optsBack_->show(true);
        setupLogin();
        break;
    case State::Browser: {
        // game_showGameSel: the whole screen from 480 above. It has its own Back.
        screen_ = gui_.load(windowDir_ / "noxworld.wnd", [this](gui::Window&, int message, gui::Window& from) {
            // Its procedure (windowMultiplayerSub) clicks with ButtonPress (but for 10035 / 10036 / 10043 /
            // 10044) and makes no sound on pointing.
            if (message == gui::SliderChanged && from.id == kInfoSlider) {
                // The description box's slider (its maximum at the top).
                if (gui::Window* list = screen_->find(kInfoList)) {
                    if (const auto* sl = std::get_if<nox::gui::SliderData>(&from.data)) {
                        const int most = std::max(0, static_cast<int>(list->lines.size()) - std::max(1, list->rect.h / 11));
                        list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
                    }
                }
                return true;
            }
            if (message != gui::Clicked) {
                return false;
            }
            if (from.id != 10035 && from.id != 10036 && from.id != 10043 && from.id != 10044) {
                gui_.resources().sound(0x2FE);
            }
            return onBrowser(from);
        });
        slides_.push_back(gui_.slideIn(screen_, {0, -480}, {0, 20}, {0, -40}));
        optsBack_->show(false);
        for (const auto id : {kWorldListPanel, kWorldGameList, kWorldMap, kWorldMapQuads}) {
            if (gui::Window* w = screen_->find(id)) {
                w->show(false);
            }
        }
        for (std::int32_t id = 10047; id <= 10051; ++id) { // the game list's column buttons
            if (gui::Window* w = screen_->find(id)) {
                w->show(false);
            }
        }
        for (const auto id : {kFilters, kFind}) {
            wip(screen_, id);
        }
        if (gui::Window* list = screen_->find(kList)) {
            list->state |= gui::kPressed; // showing the list
        }
        if (gui::Window* status = screen_->find(kStatus)) {
            status->text = gui_.text("Noxworld.c:JoinServer"); // game_showGameSel, the network (LAN) version's prompt
        }
        if (gui::Window* hostQuest = screen_->find(kHostQuest)) {
            hostQuest->text = "Host Custom"; // (noxcpp: a quest or a NoX Core mode, chosen on its click)
        }
        // The screen draws its background, then our game list in the map's frame.
        screen_->draw = [this](gui::Window& w, gui::Painter& p) {
            const SDL_Point pos = w.screenPos();
            p.image(w.backgroundImage, pos.x + w.imageOffset.x, pos.y + w.imageOffset.y);
            browser_.drawFramed(p.renderer(),
                                p.toOutput(pos.x + kListFrame.x, pos.y + kListFrame.y, kListFrame.w, kListFrame.h),
                                p.pixelSize(false));
        };
        refreshGames();
        break;
    }
    case State::Chat:
        setupChat(); // (menus_wolchat.cpp)
        break;
    }
}

void Menus::leave(std::function<void()> next)
{
    if (leaving_) {
        return;
    }
    closeGameInfo(); // (the game list's description box with it)
    leaving_ = true;
    // Every slide goes back out; the last one to finish tears the screen down.
    auto remaining = std::make_shared<std::size_t>(slides_.size());
    auto finish = [this, next = std::move(next)] {
        deferred_.push_back([this, next] {
            gui_.destroy(screen_);
            screen_ = nullptr;
            if (picker_) {
                gui_.destroy(picker_);
                picker_ = nullptr;
            }
            slides_.clear();
            next();
        });
    };
    if (slides_.empty()) {
        finish();
        return;
    }
    for (gui::Slide* s : slides_) {
        gui_.slideOut(s, [remaining, finish] {
            if (--*remaining == 0) {
                finish();
            }
        });
    }
}

void Menus::back()
{
    if (stack_.size() <= 1) {
        // The main menu's Quit (and Escape there) asks first (ReallyQuit with
        // Yes / No, ShellClick).
        gui_.resources().sound(0x399);
        dialog(gui_.text("GUIQuit.c:ReallyQuitTitle"), gui_.text("GUIQuit.c:ReallyQuitMessage"), 8 | 0x10, [this] {
            if (callbacks_.quit) {
                callbacks_.quit();
            }
        });
        return;
    }
    if (stack_.back() == State::Chat) {
        // The chat's Back and Escape (the chat window's 0x772): logged off (state 9's
        // RequestLogout), back to the login.
        logOffWol();
    }
    leave([this] {
        // Drop this screen, then re-enter the one under it (enter pushes it again).
        stack_.pop_back();
        const State previous = stack_.back();
        stack_.pop_back();
        enter(previous);
    });
}

bool Menus::onMainMenu(gui::Window& from)
{
    // windowMainMenuProc.
    if (from.id == kSolo) {
        onSolo();
        return true;
    }
    if (from.id == kMultiplay) {
        // (0x70: game flag 0x800 off; setQuest(0), the quest mark 0.)
        solo_ = false;
        soloScreensMode_.clear();
        questGame_ = false;
        leave([this] { enter(State::Online); });
        return true;
    }
    if (from.id == kSoloQuest) {
        soloModes(); // (noxcpp's Game Modes list; its Quest is onSoloQuest, the stock flow)
        return true;
    }
    if (from.id == kIntro) {
        // 0x79: with movies\Intro.vqa there, the menu slides out, then
        // client_drawGeneralCallback plays it (any key or click stops it) and
        // game_switchStates brings the main menu back; without it, only the click.
        if (callbacks_.intro && callbacks_.introAvailable && callbacks_.introAvailable()) {
            leave([this] { callbacks_.intro([this] { showMainMenu(); }); });
        }
        return true;
    }
    if (from.id == kCredits) {
        onCredits();
        return true;
    }
    return false;
}

void Menus::onCredits()
{
    // windowMainMenuProc's 0x7A (ShellClick heard): the briefing window made (the full screen
    // black, Briefing.wnd centred in it) and the credits shown in it as the ending's are
    // (Gui_Brief_ShowLockScreen(0xFF, 1, 0)); then the main menu and its background hidden behind
    // it, not slid out (they come back as they were at the credits' end), and the button's lit
    // state cleared. (The briefing's load also loads a "CreditsImage" that nothing shows: the
    // credits set the window's picture to none.)
    if (credits_) {
        return;
    }
    credits_ = std::make_unique<Cinematics>(gui_.resources(), gameDir_, audio_, Cinematics::Hooks{});
    credits_->showCredits();
    if (gui::Window* button = screen_ ? screen_->find(kCredits) : nullptr) {
        button->state &= ~gui::kLit;
    }
}

bool Menus::creditsUp() const
{
    return credits_ && credits_->screenUp();
}

void Menus::onSolo()
{
    // windowMainMenuProc's 0x6F: the solo maps there (checkHasSoloMaps: maps\con01a\con01a.map)
    // and the game's CD (ours is always the whole game's), else
    // "caution" (OK, closed by itself: 0x21). Then game flag 0x800 (0x2000 and 0x10000 off, not a
    // quest: setQuest and the other quest flags all 0), and with a
    // saved game (client_countSaveFiles) the character screen, else the class
    // screen's new character.
    if (!nox::client::solo_menu::hasSoloMaps(gameDir_)) {
        dialog(gui_.text("mainmenu.c:solo"), gui_.text("mainmenu.c:caution"), 0x21, {});
        if (dialog_) {
            gui::setDialogFonts(*dialog_, true); // (as the exe sets them, with 1)
        }
        return;
    }
    solo_ = true;
    soloScreensMode_.clear();
    questGame_ = false;
    viaSelChar_ = nox::client::solo_menu::countSaveFiles(saveRoot_) != 0;
    const State next = viaSelChar_ ? State::SelChar : State::SelClass;
    leave([this, next] { enter(next); });
}

void Menus::onSoloScreensMode(const std::string& modeId)
{
    // noxcpp's Host Custom > a campaign mode (a mode started through the solo screens): the solo
    // game's screens on the mode's own saves (its Save\ folder: modeSaveRoot), the class screen choosing
    // the campaign's chain and the character screen its saved campaign; the game hosted for others
    // to join (callbacks_.soloScreensMode).
    if (!nox::client::solo_menu::hasSoloMaps(gameDir_)) {
        dialog(gui_.text("mainmenu.c:solo"), gui_.text("mainmenu.c:caution"), 0x21, {});
        if (dialog_) {
            gui::setDialogFonts(*dialog_, true);
        }
        return;
    }
    solo_ = true;
    soloScreensMode_ = modeId;
    questGame_ = false;
    viaSelChar_ = nox::client::solo_menu::countSaveFiles(soloRoot()) != 0;
    const State next = viaSelChar_ ? State::SelChar : State::SelClass;
    leave([this, next] { enter(next); });
}

std::filesystem::path Menus::soloRoot() const
{
    return soloScreensMode_.empty() ? saveRoot_ : modeSaveRoot(saveRoot_, soloScreensMode_);
}

void Menus::startSolo(const SoloStart& start)
{
    if (!soloScreensMode_.empty()) {
        if (callbacks_.soloScreensMode) {
            callbacks_.soloScreensMode(soloScreensMode_, start);
        }
    } else if (callbacks_.solo) {
        callbacks_.solo(start);
    }
}

void Menus::onSoloQuest()
{
    // windowMainMenuProc's 0x83: the expansion's registry key (SKU over 0x24FF, else
    // GeneralPrint:InformExpansion; every copy noxsdl runs on has the quest maps), then a network
    // game (engine flag 0x10, game flags 0x2000 / 0x10000, 0x800 off) chosen as a solo quest
    // (the solo-quest flag and its fellow 1, the stage 0), and the
    // Online screen. There, and on noxworld's list (game_showGameSel),
    // the solo-quest flag at 1 presses Network (0x1A5) and Host (0x2712) itself, a quest chosen
    // (setQuest(1)), and clicks the world map at (408, 239): So_Druid, the chat map
    // the host starts on; then the character screen for quest characters. noxsdl goes straight to
    // that character screen (a deliberate deviation).
    solo_ = false;
    soloScreensMode_.clear();
    questGame_ = true;
    pendingHost_ = std::string(kSoloQuestHost);
    viaSelChar_ = nox::client::solo_menu::countQuestCharacters(saveDir_) != 0;
    const State next = viaSelChar_ ? State::SelChar : State::SelClass;
    leave([this, next] { enter(next); });
}

void Menus::showLoading(bool on)
{
    // MainBG's "Loading..." (99) shown as a game starts from the menus.
    if (gui::Window* loading = background_ ? background_->find(99) : nullptr) {
        loading->show(on);
    }
}

void Menus::fillSoloList()
{
    // findAutosaves under game flag 0x800: the save menu's list on the screen's
    // three lists: each of the 14 slots read, a line in each list per slot in its
    // colour: 510 " ", 511 the date and time, 512 "<name> the <class>" with " - "
    // and the place its stage names (AUTOSAVE: " " and "Auto Save"); an empty one " " / EmptySlot
    // / EmptySlot, grey (AUTOSAVE's " " all three); the newest selected in the three. (Its Load
    // and Delete switches are the save menu's window's: none here, so the buttons stay on.)
    gui::Window* lists = screen_ ? screen_->find(kCharLists) : nullptr;
    gui::Window* dates = screen_ ? screen_->find(kCharKind) : nullptr;
    gui::Window* names = screen_ ? screen_->find(kCharName) : nullptr;
    if (!lists || !dates || !names) {
        return;
    }
    namespace saves = nox::client::solo_saves;
    const auto palette = [](int index) {
        const SDL_Color c = paletteColor(static_cast<std::uint8_t>(index));
        return nox::gui::Color{c.r, c.g, c.b, false};
    };
    lists->lines.clear();
    dates->lines.clear();
    names->lines.clear();
    lists->selectedLine = dates->selectedLine = names->selectedLine = -1;
    soloSlots_ = saves::list(soloRoot());
    for (int i = 0; i < static_cast<int>(soloSlots_.size()); ++i) {
        const auto& slot = soloSlots_[static_cast<std::size_t>(i)];
        const nox::gui::Color c = palette(saves::lineColor(slot));
        if (!slot.exists) {
            const std::string empty = i == 0 ? " " : gui_.text("EmptySlot", "GUISave.c");
            lists->lines.push_back({" ", c});
            dates->lines.push_back({empty, c});
            names->lines.push_back({empty, c});
            continue;
        }
        const int cls = static_cast<int>(slot.info.characterClass);
        std::string line = format("GUISave.c:the", {nox::strings::toUtf8(slot.info.name),
                                                     gui_.text("nox.str:" + std::string(saves::className(cls)))});
        if (slot.name == "AUTOSAVE") {
            line += " " + gui_.text("AutoSaveLabel", "GUISave.c");
        } else {
            line += " - " + gui_.text(std::string(saves::stageLabel(slot.info.questStage, cls)));
        }
        lists->lines.push_back({" ", c});
        dates->lines.push_back({saves::whenText(slot.info.time), c});
        names->lines.push_back({line, c});
    }
    if (const int newest = saves::latest(soloSlots_); newest != -1) {
        lists->selectedLine = dates->selectedLine = names->selectedLine = newest;
    }
}

nox::client::PlayerProfile Menus::profileOf(const nox::player::FileInfo& info) const
{
    // The character as saved: name, class, sex, colours and clothes.
    nox::client::PlayerProfile p = profile_;
    p.name = nox::strings::toUtf8(info.name);
    p.playerClass = static_cast<std::uint8_t>(info.characterClass);
    p.female = info.isFemale != 0;
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            p.colors[i * 3 + j] = info.colors[i][j];
        }
        p.clothing[i] = info.clothing[i];
    }
    return p;
}

bool Menus::onSoloSelChar(gui::Window& from)
{
    // windowSelCharProc under game flag 0x800: the slot picked is 0x1FF's (511's).
    namespace menu = nox::client::solo_menu;
    const gui::Window* dates = screen_->find(kCharKind);
    const int i = dates ? dates->selectedLine : -1;
    if (i < 0 || i >= static_cast<int>(soloSlots_.size())) {
        return true; // (-1: nothing)
    }
    const auto& slot = soloSlots_[static_cast<std::size_t>(i)];
    if (from.id == kCharLoad) {
        // 0x1F6: an empty slot NoCanDo; else the slot copied to WORKING (failing,
        // nothing more), its File Info read again and held, the
        // class's first map named, then the saved map's copy (else the game's),
        // switch-to-solo, not multiplayer, no AUTOSAVE at its load, the script ids from the save
        // (LocalHost); "Loading..." (showLoading) and the screen out, the game started.
        if (!slot.exists) {
            gui_.resources().sound(0x39D); // NoCanDo
            return true;
        }
        if (!nox::server::saves::copy(soloRoot(), slot.name, "WORKING")) {
            SDL_Log("menus: %s couldn't be copied to WORKING", slot.name.c_str());
            return true;
        }
        const auto read = nox::client::solo_saves::readPlr(slot.plr);
        if (!read.exists) {
            return true;
        }
        SoloStart start;
        start.info = read.info;
        start.profile = profileOf(read.info);
        start.characterFile = slot.plr;
        start.continueSlot = slot.name;
        // (Map_SetCurrentPath of the class's first map, then of "%s.map" of the listed record's
        // map, as it is: an empty one is ".map".)
        start.map = menu::continueMap(soloRoot(), gameDir_, slot.info.mapName);
        characterFile_ = slot.plr;
        showLoading(true);
        leave([this, start] { startSolo(start); });
        return true;
    }
    // 0x1F7: the slot's folder remembered; AUTOSAVE refused, an empty slot
    // NoCanDo, else asked (DeleteSave, Yes / No); Yes: the folder gone
    // (savegame_rm(name, 1)) and the list made again.
    switch (menu::soloDelete(deleteName_, slot)) {
    case menu::DeleteAnswer::NotAllowed:
        gui_.resources().sound(0x39D);
        dialog(gui_.text("GUISave.c:AutoSaveDeleteTitle"), gui_.text("GUISave.c:AutoSaveDeleteNotAllowed"), 0x21, {});
        break;
    case menu::DeleteAnswer::NoCanDo:
        gui_.resources().sound(0x39D);
        break;
    case menu::DeleteAnswer::Ask:
        dialog(gui_.text("GUISave.c:DeleteSaveTitle"), gui_.text("GUISave.c:DeleteSaveMessage"), 0x38, [this] {
            nox::server::saves::remove(soloRoot(), deleteName_, true);
            fillSoloList();
        });
        break;
    }
    return true;
}

bool Menus::onOnline(gui::Window& from)
{
    // Westwood Online logs in first; Network goes to the game list without a login.
    if (from.id == kWestwoodOnline) {
        leave([this] { enter(State::Login); });
        return true;
    }
    if (from.id == kNetwork) {
        // LAN play: the list asks the LAN alone (refreshServerList's network branch: the
        // discovery broadcast, never WOL), and a game hosted from here isn't posted to XWIS, even
        // after an earlier login (that login was logged off on the way back here).
        wolMode_ = false;
        lanOnly_ = true;
        wolLogin_.reset();
        browser_.setXwisListing(false);
        leave([this] { enter(State::Browser); });
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------ the XWIS login

void Menus::setupLogin()
{
    // The login screen: the saved logins in the list (an empty slot a space), the one last used (here the
    // one matching our account, else the first saved) picked and filled in.
    logins_ = nox::io::savedLogins();
    logins_.resize(nox::io::kSavedLoginSlots + 1);
    if (gui::Window* list = screen_->find(kLoginList)) {
        list->lines.clear();
        for (int i = 0; i < nox::io::kSavedLoginSlots; ++i) {
            const std::string& nick = logins_[static_cast<std::size_t>(i)].nick;
            list->lines.push_back({nick.empty() ? " " : nick, list->textColor});
        }
    }
    int pick = -1;
    for (int i = 0; i < nox::io::kSavedLoginSlots && pick < 0; ++i) {
        const std::string& nick = logins_[static_cast<std::size_t>(i)].nick;
        if (!nick.empty() && nick == profile_.account) {
            pick = i;
        }
    }
    for (int i = 0; i < nox::io::kSavedLoginSlots && pick < 0; ++i) {
        if (!logins_[static_cast<std::size_t>(i)].nick.empty()) {
            pick = i;
        }
    }
    // Its slider's thumb (with UISlider / UISliderLit).
    gui::useSliderThumb(screen_->find(kLoginSlider));
    selectLogin(std::max(pick, 0));
    scrollLogins(std::max(pick - kLoginLinesShown / 2, 0));
    // Register: XWIS makes an account on its first login, so it's our way to go on without one.
    if (gui::Window* skip = screen_->find(kLoginSkip)) {
        skip->text = "Skip Login";
        skip->tooltip = "Go to the game list without logging in to XWIS (no account name is sent)";
    }
    if (gui::Window* go = screen_->find(kLoginGo)) {
        go->tooltip = "Log in to XWIS (a new name makes a new account with this password)";
    }
    // Ours: how to log in to XWIS, across the top, over the screen's background.
    screen_->draw = [](gui::Window& w, gui::Painter& p) {
        const SDL_Point pos = w.screenPos();
        p.image(w.backgroundImage, pos.x + w.imageOffset.x, pos.y + w.imageOffset.y);
        static const std::string kHow =
            "Log in with your XWIS account. A new name becomes your account with the password you type; for a "
            "name registered at xwis.net, use the game password from xwis.net/ga. Skip Login plays without one.";
        p.fillAlpha(pos.x + 36, pos.y + 29, 568, 26, {0, 0, 0, 170});
        p.text(kHow, pos.x + 42, pos.y + 31, false, w.textColor, 556);
    };
}

void Menus::selectLogin(int slot)
{
    if (slot < 0 || slot >= nox::io::kSavedLoginSlots) {
        return;
    }
    loginSlot_ = slot;
    const auto& saved = logins_[static_cast<std::size_t>(slot)];
    if (gui::Window* list = screen_->find(kLoginList)) {
        list->selectedLine = slot;
    }
    if (gui::Window* name = screen_->find(kLoginName)) {
        name->entry = nox::strings::fromUtf8(saved.nick).substr(0, name->entryMax);
    }
    if (gui::Window* password = screen_->find(kLoginPassword)) {
        password->entry = nox::strings::fromUtf8(saved.password).substr(0, password->entryMax);
    }
    // Ticked when its password was saved.
    if (gui::Window* save = screen_->find(kLoginSave)) {
        save->state = saved.password.empty() ? save->state & ~gui::kPressed : save->state | gui::kPressed;
    }
}

void Menus::scrollLogins(int first)
{
    gui::Window* list = screen_ ? screen_->find(kLoginList) : nullptr;
    if (!list) {
        return;
    }
    const int most = std::max(0, static_cast<int>(list->lines.size()) - kLoginLinesShown);
    list->scroll = std::clamp(first, 0, most);
    // The slider (DATA 0-128, its maximum at the top) follows.
    if (gui::Window* slider = screen_->find(kLoginSlider)) {
        if (const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data); sl && most > 0) {
            slider->sliderValue = sl->max - (sl->max - sl->min) * list->scroll / most;
        }
    }
}

bool Menus::onLogin(int message, gui::Window& from)
{
    // The login window's procedure.
    shellSound(message);
    if (message == gui::SliderChanged && from.id == kLoginSlider) {
        gui::Window* list = screen_->find(kLoginList);
        const auto* sl = std::get_if<nox::gui::SliderData>(&from.data);
        if (list && sl) {
            const int most = std::max(0, static_cast<int>(list->lines.size()) - kLoginLinesShown);
            list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
        }
        return true;
    }
    if (message != gui::Clicked) {
        return false;
    }
    gui::Window* list = screen_->find(kLoginList);
    switch (from.id) {
    case kLoginList:
        selectLogin(from.selectedLine);
        return true;
    case kLoginUp:
        scrollLogins(list ? list->scroll - 1 : 0);
        return true;
    case kLoginDown:
        scrollLogins(list ? list->scroll + 1 : 0);
        return true;
    case kLoginSkip:
        // On to the game list without logging in: no account name at all (not a saved login's,
        // nor an earlier login's), XWIS listed through a throwaway login (Westwood Online's list,
        // not Network's).
        skippedLogin_ = true;
        wolLogin_.reset();
        wolMode_ = false;
        lanOnly_ = false;
        profile_.account.clear();
        browser_.setXwisLogin({}, {});
        leave([this] { enter(State::Browser); });
        return true;
    case kLoginGo:
        startLogin();
        return true;
    default:
        return from.id == kLoginSave; // (the box ticks itself)
    }
}

void Menus::startLogin()
{
    const gui::Window* name = screen_->find(kLoginName);
    const gui::Window* password = screen_->find(kLoginPassword);
    const gui::Window* save = screen_->find(kLoginSave);
    std::string nick = name ? nox::strings::toUtf8(name->entry) : std::string();
    while (!nick.empty() && nick.front() == ' ') {
        nick.erase(0, 1);
    }
    while (!nick.empty() && nick.back() == ' ') {
        nick.pop_back();
    }
    const std::string typed = password ? nox::strings::toUtf8(password->entry) : std::string();
    const std::string title = gui_.text("wolpatch.c:LoginFailed");
    if (nick.empty()) {
        dialog(title, gui_.text("Wolreg.c:ErrNoNick"), 1, {});
        return;
    }
    if (!std::isalpha(static_cast<unsigned char>(nick.front()))) {
        dialog(title, gui_.text("Wolreg.c:ErrNickFirstLetter"), 1, {});
        return;
    }
    if (typed.empty()) {
        dialog(title, gui_.text("Wolreg.c:ErrPasswordLength"), 1, {});
        return;
    }
    // A password filled in from the saved login is already encoded; anything typed isn't.
    const auto& picked = logins_[static_cast<std::size_t>(loginSlot_)];
    const bool savedPassword = !picked.password.empty() && typed == picked.password && nick == picked.nick;
    pendingLogin_ = {nick, savedPassword ? typed : nox::lobby::apgar(typed)};
    // Kept in the slot picked if it's free or this nick's, else this nick's slot, else the first
    // free one, else the one picked; not kept without the box ticked.
    pendingSlot_ = -1;
    if (save && (save->state & gui::kPressed)) {
        const auto slotOf = [&](const std::string& who) {
            for (int i = 0; i < nox::io::kSavedLoginSlots; ++i) {
                if (logins_[static_cast<std::size_t>(i)].nick == who) {
                    return i;
                }
            }
            return -1;
        };
        if (picked.nick.empty() || picked.nick == nick) {
            pendingSlot_ = loginSlot_;
        } else if (const int mine = slotOf(nick); mine >= 0) {
            pendingSlot_ = mine;
        } else if (const int free = slotOf(""); free >= 0) {
            pendingSlot_ = free;
        } else {
            pendingSlot_ = loginSlot_;
        }
    }
    // The connection the chat and the game list go on with (WOLAPI's one session).
    wol_.reset();
    wol_.emplace(pendingLogin_.nick, pendingLogin_.password, static_cast<std::uint32_t>(nowMs_));
    wolLoggingIn_ = true;
    // "Connecting...", with Cancel.
    dialog(gui_.text("WolProg.wnd:Connecting"), gui_.text("Wolchat.c:PleaseWait"), 2, {}, [this] {
        wol_.reset();
        wolLoggingIn_ = false;
    });
}

void Menus::loginDone()
{
    // The login's connection logged in, or failed (pollWol).
    wolLoggingIn_ = false;
    closeDialog();
    if (wol_->state() == nox::lobby::WolChat::State::Failed) {
        const nox::lobby::WolChat done = std::move(*wol_);
        wol_.reset();
        SDL_Log("XWIS login as %s failed: %s", pendingLogin_.nick.c_str(), done.error().c_str());
        std::string why;
        if (!done.refused()) {
            why = gui_.text("Wol.c:Connectionerror");
        } else if (done.error().find(" 433 ") != std::string::npos) {
            why = gui_.text("Wol.c:NickInUse");
        } else {
            // The server's own words (after its numeric and our nick: ":server 464 nick :why").
            const auto colon = done.error().find(" :");
            why = colon != std::string::npos ? done.error().substr(colon + 2) : done.error();
        }
        dialog(gui_.text("wolpatch.c:LoginFailed"), why, 1, {});
        return;
    }
    SDL_Log("XWIS: logged in as %s", pendingLogin_.nick.c_str());
    if (pendingSlot_ >= 0) {
        if (nox::io::saveLogin(pendingSlot_, pendingLogin_)) {
            logins_[static_cast<std::size_t>(pendingSlot_)] = pendingLogin_;
        } else {
            SDL_Log("XWIS: couldn't save the login");
        }
    }
    profile_.account = pendingLogin_.nick;
    skippedLogin_ = false;
    wolLogin_ = pendingLogin_;
    browser_.setXwisLogin(pendingLogin_.nick, pendingLogin_.password); // (lists without the connection)
    if (callbacks_.loggedIn) {
        callbacks_.loggedIn(pendingLogin_.nick);
    }
    // Sub-state 3: the login slides out, the chat comes in.
    wolMode_ = true;
    lanOnly_ = false;
    leave([this] { enter(State::Chat); });
}

void Menus::showGameInfo(const nox::lobby::ServerEntry& entry, SDL_FPoint at, int width, int height)
{
    // client_gui_serverInfoBlockCheckExp (the quest expansion check aside: every copy
    // noxsdl runs on has it): the box at the click, on top, taking the mouse,
    // filled (client_gui_serverInfoBlock).
    gui::Window* box = screen_ ? screen_->find(kWorldListPanel) : nullptr;
    gui::Window* list = box ? box->find(kInfoList) : nullptr;
    if (!list) {
        return;
    }
    const SDL_FPoint m = gui::Painter(nullptr, gui_.resources(), width, height).toLayout(at.x, at.y);
    const int mx = static_cast<int>(std::floor(m.x));
    const int my = static_cast<int>(std::floor(m.y));
    // Placed 0x41 left of and 0x14 above the click, then kept inside the screen's list area.
    int x = mx - 0x41;
    int y = my - 0x14;
    if (mx + 0x41 > 600) {
        x = 0x1D6;
    }
    if (my + 100 > 0x1C3) {
        y = 0x14B;
    }
    y = std::max(y, 0x1B); // (0x37 with the world map up: noxsdl shows the list)
    x = std::max(x, 0xD8);
    box->rect.x = x - screen_->rect.x;
    box->rect.y = y - screen_->rect.y;

    const auto& g = entry.game;
    const nox::gui::Color plain = list->textColor;
    const auto colour = [](int index) {
        const SDL_Color c = paletteColor(static_cast<std::uint8_t>(index));
        return nox::gui::Color{c.r, c.g, c.b, false};
    };
    list->lines.clear();
    const auto add = [&](const std::string& text, std::optional<int> index = std::nullopt) {
        list->lines.push_back({text.empty() ? std::string(" ") : text, index ? colour(*index) : plain});
    };
    const auto title = [&](const char* id) { add(gui_.text(std::string("Noxworld.c:") + id), 0xE); };
    const auto gap = [&] { add(" "); }; // (L"\n")
    title("Name");
    add(g.name.empty() ? BrowserScreen::address(g) : g.name);
    if (wolMode_) {
        gap();
        title("GameHost");
        add(g.host);
    }
    gap();
    title("Ping");
    add(entry.pingMs ? std::to_string(*entry.pingMs) : std::string("--"));
    gap();
    title("GameType");
    // The game type by the low game flags: 0x1000 quest, 0x20 CTF, 0x400 Highlander, 0x10 KotR,
    // 0x40 Flagball, 0x80 chat, none of those arena.
    const std::uint32_t f = g.gameFlags & 0xFFFF;
    add(gui_.text(std::string("Noxworld.c:") + ((f & 0x1000)  ? "Quest"
                                                : (f & 0x20)  ? "CTF"
                                                : (f & 0x400) ? "Highlander"
                                                : (f & 0x10)  ? "KotR"
                                                : (f & 0x40)  ? "Flagball"
                                                : (f & 0x80)  ? "Chat"
                                                              : "Arena")));
    if (f & 0x1000) {
        gap();
        title("Stage");
        add(std::to_string(g.questStage));
    }
    gap();
    title("Map");
    add(g.map);
    gap();
    // 0x4000 / 0x8000: an individual or a clan ladder game.
    if (f & 0xC000) {
        add(gui_.text((f & 0x4000) ? "Noxworld.c:Individual" : "Noxworld.c:Clan"), 6);
        add(gui_.text("Noxworld.c:Ladder"), 6);
    }
    gap();
    title("Occupancy");
    add(std::to_string(g.players) + "/" + std::to_string(g.maxPlayers));
    if (f & 0x2000) {  // game flag 0x2000: a network game
        gap();
        title("Resolution");
        static constexpr std::array<const char*, 3> kResolutions = {"640x480", "800x600", "1024x768"};
        const std::size_t res = g.resolution & 0x7Fu;
        add(res < kResolutions.size() ? kResolutions[res] : "");
        gap();
        title("DisabledSpells");
        bool any = false;
        // Spells off in the game: a clear bit in its spell bit set (a bit per spell number).
        for (std::uint32_t spell = 1; things_ && spell <= 0x88; ++spell) {
            const auto* def = things_->findSpell(spell);
            const bool allowed = (g.spells[spell / 8] >> (spell % 8)) & 1u;
            if (def && !allowed && (def->flags & 0x7000000u)) {  // a spell some class can cast (the any-caster, wizard or conjurer bit)
                add(gui_.text(def->title), 4);
                any = true;
            }
        }
        if (!any) {
            add(gui_.text("Noxworld.c:None"), 4);
        }
        gap();
        // Weapons, then armour, off in the game: a clear bit in its 27 weapon / 26 armour bits.
        title("DisabledWeapons");
        any = false;
        for (int k = 0; k < 27; ++k) {
            const std::uint32_t bit = 1u << k;
            if (!(g.weapons & bit)) {
                if (const auto* item = nox::server::ruleWeaponFor(bit)) {
                    add(gui_.text(item->label));
                    any = true;
                }
            }
        }
        if (!any) {
            add(gui_.text("Noxworld.c:None"), 4);
        }
        gap();
        title("DisabledArmor");
        any = false;
        for (int k = 0; k < 26; ++k) {
            const std::uint32_t bit = 1u << k;
            if (!(g.armor & bit)) {
                if (const auto* item = nox::server::ruleArmourFor(bit)) {
                    add(gui_.text(item->label));
                    any = true;
                }
            }
        }
        if (!any) {
            add(gui_.text("Noxworld.c:None"), 4);
        }
    }
    scrollGameInfo(0);
    infoServer_ = BrowserScreen::address(g);
    box->show(true);
    gui_.setModal(box);
}

void Menus::scrollGameInfo(int first)
{
    gui::Window* box = screen_ ? screen_->find(kWorldListPanel) : nullptr;
    gui::Window* list = box ? box->find(kInfoList) : nullptr;
    if (!list) {
        return;
    }
    const int shown = std::max(1, list->rect.h / 11);
    const int most = std::max(0, static_cast<int>(list->lines.size()) - shown);
    list->scroll = std::clamp(first, 0, most);
    if (gui::Window* slider = box->find(kInfoSlider)) {
        if (const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data)) {
            slider->sliderValue = gui::sliderFromScroll(*sl, list->scroll, most);
        }
    }
}

void Menus::closeGameInfo()
{
    // Off the stack, hidden.
    if (!infoServer_) {
        return;
    }
    infoServer_.reset();
    if (gui::Window* box = screen_ ? screen_->find(kWorldListPanel) : nullptr) {
        box->show(false);
        if (gui_.modal() == box) {
            gui_.setModal(nullptr);
        }
    }
}

bool Menus::onBrowser(gui::Window& from)
{
    // windowMultiplayerSub.
    switch (from.id) {
    case kJoin:
        // 0x2711, the description box's Join game: the game it's up for.
        if (const auto host = infoServer_ ? infoServer_ : browser_.selectedServer()) {
            closeGameInfo();
            chooseCharacter(*host);
        }
        return true;
    case kInfoUp:
    case kInfoDown:
        if (gui::Window* list = screen_ ? screen_->find(kInfoList) : nullptr) {
            scrollGameInfo(list->scroll + (from.id == kInfoUp ? -1 : 1));
        }
        return true;
    case kHost:
        // Host: a character, then our own game (noxsdl's server in this process; its map and
        // rules from the console till the host windows are in: kHostGame to the join callback).
        chooseCharacter(std::string(kHostGame));
        return true;
    case kHostQuest:
        // Host Quest (0x2713): Host with a quest chosen (setQuest(1), game flag
        // 0x10000 off), so a quest character (the expansion check: every copy noxsdl
        // runs on has the quest's maps). noxcpp's Host Custom asks first: the quest or a NoX Core
        // mode (the registry's), most of the modes played with a network character, as joining
        // their servers.
        hostCustom();
        return true;
    case kRefresh:
        refreshGames();
        return true;
    case kWorldBack:
        back();
        return true;
    default:
        return false;
    }
}

bool Menus::onOptsBack(gui::Window& from)
{
    // The OptsBack bar's procedure.
    if (from.id == kBack) {
        back();
        return true;
    }
    if (from.id == kOptions && callbacks_.options) {
        deferred_.push_back([this] { callbacks_.options(); }); // game_showOptions
        return true;
    }
    return false;
}

void Menus::stepEyes()
{
    // The background's draw, a frame (Math_RandIntVisual's picks).
    const auto rand = [this](int lo, int hi) {
        eyeSeed_ = eyeSeed_ * 1103515245u + 12345u;
        return lo + static_cast<int>((eyeSeed_ >> 16) % static_cast<std::uint32_t>(hi - lo + 1));
    };
    // Each eye a frame: the wait and rest count down (each frame of rest pushes the wait back to
    // 60-120); at a spell's end it swaps between open and blinking; while the lid shows, a 24% chance of a rest.
    for (auto& e : eyes_) {
        if (e.wait != 0) {
            --e.wait;
        }
        if (e.rest != 0) {
            --e.rest;
            e.wait = rand(60, 120);
        }
        if (--e.timer == 0) {
            if (!e.open) {
                e.open = true;
                e.timer = rand(e.openMin, e.openMax);
            } else {
                e.open = false;
                e.timer = rand(e.blinkMin, e.blinkMax);
                e.wait = rand(60, 90);
            }
        } else if (!e.open && e.wait == 0 && e.rest == 0 && rand(0, 100) > 75) {
            e.rest = rand(4, 8);
        }
    }
}

void Menus::drawBackground(gui::Window& w, gui::Painter& p)
{
    // MainBG's own image (MenuSystemBG), then each lid that's shut: in a blinking spell and not
    // waiting.
    const SDL_Point pos = w.screenPos();
    const bool lit = (w.state & gui::kLit) != 0;
    const std::string& image = lit && !w.hiliteImage.empty() ? w.hiliteImage : w.backgroundImage;
    if ((w.status & nox::gui::status::Image) && !image.empty()) {
        p.image(image, pos.x + w.imageOffset.x, pos.y + w.imageOffset.y);
    }
    for (const auto& e : eyes_) {
        if (!e.open && e.wait == 0) {
            p.image(e.image, e.x, e.y);
        }
    }
}

void Menus::update(std::uint64_t nowMs)
{
    nowMs_ = nowMs;
    gui_.update(nowMs);
    // The eyes at the shell's 30 frames a second.
    if (eyesStepMs_ == 0 || nowMs < eyesStepMs_) {
        eyesStepMs_ = nowMs;
    }
    const bool hidden = creditsUp(); // (the background hidden: its draw, which moves the eyes, not run)
    for (int n = 0; nowMs - eyesStepMs_ >= 33 && n < 30; ++n) {  // at most a second's steps per call
        eyesStepMs_ += 1000 / 30;
        if (!hidden) {
            stepEyes();
        }
    }
    if (credits_) {
        // The credits' client frames (30 a second, as the shell's); gone once the fade back from
        // them is over.
        credits_->update(nowMs, false, 480, 0);
        if (!credits_->briefingShown()) {
            credits_.reset();
        }
    }
    // Work queued by the button handlers, run now that no event is being handled.
    gui::runQueued(deferred_);
    if (!stack_.empty() && stack_.back() == State::Browser) {
        browser_.update(nowMs);
    }
    pollWol();
    if (!clicks_.empty() && clicks_.front().starts_with('+')) {
        // "+ms": the next steps wait that long (for a server's answer).
        if (clickWaitMs_ == 0) {
            clickWaitMs_ = nowMs + static_cast<std::uint64_t>(std::atoi(clicks_.front().c_str() + 1));
        }
        if (nowMs < clickWaitMs_) {
            return;
        }
        clickWaitMs_ = 0;
        clicks_.erase(clicks_.begin());
    }
    // One queued test step once the screens are still: its window looked for in the dialog, the picker,
    // the screen, then the Options / Back bar.
    if (!clicks_.empty() && !gui_.sliding() && !leaving_ && deferred_.empty() && !credits_) {
        const std::string step = clicks_.front();
        clicks_.erase(clicks_.begin());
        // A step is a window id: "~id" only points at it, "id=text" types into it, and "id!" on an
        // entry field presses Enter in it; a plain id clicks it.
        const bool point = !step.empty() && step[0] == '~';
        const auto eq = step.find('=');
        const std::int32_t id = std::atoi(step.c_str() + (point ? 1 : 0));
        gui::Window* w = dialog_ ? dialog_->find(id) : nullptr;
        if (!w && picker_ && !picker_->hidden()) {
            w = picker_->find(id);
        }
        if (!w && screen_) {
            w = screen_->find(id);
        }
        if (!w && optsBack_ && !optsBack_->hidden()) {
            w = optsBack_->find(id);
        }
        if (!w) {
            SDL_Log("menus: no window %d for \"%s\"", id, step.c_str());
        } else if (!w->enabled()) {
            SDL_Log("menus: %d is disabled", id); // as the mouse would find it
        } else if (eq != std::string::npos) {
            SDL_Log("menus: typing into %d", id);
            w->entry = nox::strings::fromUtf8(step.substr(eq + 1)).substr(0, w->entryMax);
        } else {
            // As if the mouse were on its middle.
            const SDL_Point pos = w->screenPos();
            mouse_ = {pos.x + w->rect.w / 2, pos.y + w->rect.h / 2};
            SDL_Log("menus: %s %d", point ? "pointing at" : "clicking", id);
            if (!point && w->type == nox::gui::WindowType::CheckBox) {
                w->state ^= gui::kPressed; // (a check box ticks itself as it's clicked, as the mouse's)
            }
            const bool enter = step.ends_with('!') && w->type == nox::gui::WindowType::EntryField;
            w->notify(enter ? gui::EntryEntered : point ? gui::Hover : gui::Clicked);
        }
    }
}

std::string Menus::format(const std::string& id, std::initializer_list<std::string> args) const
{
    // The string table's formats are printf ones: %s / %S / %ls take the next argument.
    const std::string f = gui_.text(id);
    std::string out;
    auto arg = args.begin();
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 1 < f.size()) {
            std::size_t j = i + 1;
            if (f[j] == 'l' && j + 1 < f.size()) {
                ++j;
            }
            if (f[j] == 's' || f[j] == 'S') {
                if (arg != args.end()) {
                    out += *arg++;
                }
                i = j;
                continue;
            }
            if (f[j] == '%') {
                out += '%';
                i = j;
                continue;
            }
        }
        out += f[i];
    }
    return out;
}

void Menus::dialog(const std::string& title, const std::string& text, std::uint32_t flags,
                   std::function<void()> onOk, std::function<void()> onCancel, const std::string& yesText,
                   const std::string& noText)
{
    closeDialog();
    dialog_ = gui_.load(windowDir_ / "dlg.wnd",
                        [this, onOk = std::move(onOk), onCancel = std::move(onCancel)](gui::Window&, int message,
                                                                                        gui::Window& from) {
                            if (message != gui::Clicked) {
                                return false;
                            }
                            gui_.resources().sound(0x399); // (the screen under it clicks: windowDlgProc forwards)
                            std::function<void()> then;
                            if (from.id == kDlgOk || from.id == kDlgYes) {
                                then = onOk;
                            } else if (from.id == kDlgCancel || from.id == kDlgNo) {
                                then = onCancel;
                            } else {
                                return false;
                            }
                            deferred_.push_back([this, then] {
                                closeDialog();
                                if (then) {
                                    then();
                                }
                            });
                            return true;
                        });
    // Centred, its buttons shown by the flags.
    gui::centreOnScreen(*dialog_);
    dialog_->status |= st::Above;
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = title;
    }
    if (gui::Window* t = dialog_->find(kDlgText)) {
        t->text = text;
    }
    const std::pair<std::uint32_t, std::int32_t> buttons[] = {
        {1, kDlgOk}, {2, kDlgCancel}, {8, kDlgYes}, {0x10, kDlgNo}};
    for (const auto& [bit, id] : buttons) {
        if (gui::Window* b = dialog_->find(id)) {
            b->show((flags & bit) != 0);
        }
    }
    if (gui::Window* b = yesText.empty() ? nullptr : dialog_->find(kDlgYes)) {
        b->text = yesText;
    }
    if (gui::Window* b = noText.empty() ? nullptr : dialog_->find(kDlgNo)) {
        b->text = noText;
    }
    gui_.setModal(dialog_);
}

std::string Menus::modeHostKey(std::string_view modeId, bool local)
{
    return std::string(local ? "@solomode:" : "@hostnetmode:") + std::string(modeId);
}

std::optional<Menus::ModeHost> Menus::modeOfHostKey(std::string_view host)
{
    for (const auto& [prefix, local] : {std::pair{std::string_view("@hostnetmode:"), false}, std::pair{std::string_view("@solomode:"), true}}) {
        if (host.starts_with(prefix) && host.size() > prefix.size()) {
            return ModeHost{std::string(host.substr(prefix.size())), local};
        }
    }
    return std::nullopt;
}

void Menus::loadCharacters()
{
    // findAutosaves: every .plr in the Save folder read into the records that suit the game
    // (a quest character for a quest game, any other for the rest; an unreadable
    // one out of a quest a zeroed record), newest first.
    characters_ = nox::client::solo_menu::listCharacters(saveDir_, questGame_);
}

void Menus::fillCharacterList()
{
    gui::Window* lists = screen_ ? screen_->find(kCharLists) : nullptr;
    gui::Window* kind = screen_ ? screen_->find(kCharKind) : nullptr;
    gui::Window* name = screen_ ? screen_->find(kCharName) : nullptr;
    if (!lists || !kind || !name) {
        return;
    }
    lists->lines.clear();
    kind->lines.clear();
    name->lines.clear();
    // Each record's three lines in palette colour 14 (0x400D's): 0x1FE " ", 0x1FF Multiplayer or
    // Noxworld.c:Quest, 0x200 ElaborateNameFormat (the name, nox.str:<class>) with, for a quest
    // character, " - " Noxworld.c:Stage " %d" (its stage, 0 shown as 1).
    const SDL_Color pc = paletteColor(14);
    const nox::gui::Color color{pc.r, pc.g, pc.b, false};
    for (const auto& c : characters_) {
        const auto cls = static_cast<std::size_t>(c.info.characterClass);
        const std::string className = gui_.text(std::string("nox.str:") + kClassNames[std::min<std::size_t>(cls, 2)]);
        lists->lines.push_back({" ", color});
        std::string line = format("GuiInv.c:ElaborateNameFormat", {nox::strings::toUtf8(c.info.name), className});
        std::string what = gui_.text("Multiplayer", "SelChar.c");
        if (c.info.flags & 4) {
            const int stage = static_cast<std::int8_t>(c.info.questStage);
            line += " - " + gui_.text("Noxworld.c:Stage") + " " + std::to_string(stage == 0 ? 1 : stage);
            what = gui_.text("Noxworld.c:Quest");
        }
        name->lines.push_back({line, color});
        kind->lines.push_back({what, color});
    }
    const int selected = characters_.empty() ? -1 : 0;
    lists->selectedLine = kind->selectedLine = name->selectedLine = selected;
    // findAutosaves: Load and Delete with something to load; New with nothing, or out of a quest, or
    // with fewer than 14 quest characters (client_countPlayerFiles04).
    enable(screen_, kCharLoad, !characters_.empty());
    enable(screen_, kCharDelete, !characters_.empty());
    enable(screen_, kCharNew,
           characters_.empty() || !questGame_ || nox::client::solo_menu::countQuestCharacters(saveDir_) < 14);
}

void Menus::chooseCharacter(const std::string& host)
{
    // (noxcpp: a listed game that wants a newer NoX Core, marked "Update needed": its notice
    // instead of the join, however it was chosen; client_level_gate.hpp)
    if (!host.starts_with('@') && client_level::noticeInsteadOfJoin(browser_.entryFor(host))) {
        tooOldNotice();
        return;
    }
    // noxworld's Join: the saved characters if there are any, else a new one. A listed
    // game flagged 0x1000 is a quest's (the quest mark 1), and so is Host Quest's; Host's isn't.
    pendingHost_ = host;
    solo_ = false;
    soloScreensMode_.clear();
    // (A mode with a generated lobby: a network character, as one joining its server from the list,
    // whose lobby is a chat map with the quest only chosen.)
    // (noxcpp: a co-op campaign is listed as a quest for stock players; a NoX Core guest brings a
    // network character.)
    const auto* selected = browser_.selectedEntry();
    const bool coopGame = selected && selected->extended && (selected->extended->features & nox::lobby::ServerFeature::CoopCampaign);
    questGame_ = host == kHostQuestGame ||
                 (host != kHostGame && host != kHostEditGame && !modeOfHostKey(host) && !coopGame && (browser_.selectedGameFlags() & 0x1000u) != 0);
    // The character screen when there are characters of the game's kind by the File Info's own
    // flag (client_countPlayerFiles02, a quest's 04), not the list's filter: a
    // .plr flagged only 1 (or unread) is listed there but doesn't lead to it.
    namespace menu = nox::client::solo_menu;
    viaSelChar_ = (questGame_ ? menu::countQuestCharacters(saveDir_) : menu::countNetworkCharacters(saveDir_)) != 0;
    const State next = viaSelChar_ ? State::SelChar : State::SelClass;
    leave([this, next] { enter(next); });
}

void Menus::join(const nox::client::PlayerProfile& profile)
{
    if (wol_ && !wolLoggingIn_) {
        // Logged off WOL first (WOLAPI's connection isn't kept through the game here: a hosted
        // game posts itself with the same login), the join once the server has closed it.
        joinAfterLogOff_ = profile;
        logOffWol();
        dialog(gui_.text("Wolchat.c:PleaseWait"), gui_.text("WolChat.c:LoggingOff"), 0, {});
        return;
    }
    if (callbacks_.join && !pendingHost_.empty()) {
        // A listed quest game is joined as a quest client (CLIENT_ACCEPT's flags bit 7); our own
        // hosted quest isn't one yet when its client joins (the quest begins after).
        nox::client::PlayerProfile joining = profile;
        joining.quest = questGame_ && pendingHost_.front() != '@';
        callbacks_.join(pendingHost_, joining);
    }
}

bool Menus::onSelChar(gui::Window& from)
{
    // windowSelCharProc.
    if (solo_ && (from.id == kCharLoad || from.id == kCharDelete)) {
        return onSoloSelChar(from);
    }
    gui::Window* lists = screen_->find(kCharLists);
    gui::Window* kind = screen_->find(kCharKind);
    gui::Window* name = screen_->find(kCharName);
    const int selected = name ? name->selectedLine : -1;
    switch (from.id) {
    case kCharLists:
    case kCharKind:
    case kCharName:
        // The three lists select the same line.
        for (gui::Window* l : {lists, kind, name}) {
            if (l) {
                l->selectedLine = from.selectedLine;
            }
        }
        return true;
    case kCharLoad: {
        if (selected < 0 || selected >= static_cast<int>(characters_.size())) {
            return true;
        }
        // 0x1F6: a record with no path NoCanDo; the file read again (failing,
        // nothing) and held. The character as saved: name, class, sex, colours
        // and clothes; its spell bars come from its file in the game.
        const auto& record = characters_[static_cast<std::size_t>(selected)];
        if (record.path.empty()) {
            gui_.resources().sound(0x39D); // NoCanDo
            return true;
        }
        const auto read = nox::client::solo_saves::readPlr(record.path);
        if (!read.exists) {
            return true;
        }
        const nox::player::FileInfo info = read.infoRead ? read.info : record.info; // (the record kept without one)
        characterFile_ = record.path;
        const nox::client::PlayerProfile p = profileOf(info);
        leave([this, p] { join(p); });
        return true;
    }
    case kCharDelete: {
        if (selected < 0 || selected >= static_cast<int>(characters_.size())) {
            return true;
        }
        const auto path = characters_[static_cast<std::size_t>(selected)].path;
        if (path.empty()) {
            gui_.resources().sound(0x39D); // (0x1F7: a record with no path, NoCanDo)
            return true;
        }
        dialog(gui_.text("GUISave.c:DeleteCharacterTitle"), gui_.text("GUISave.c:DeleteCharacterMessage"), 8 | 0x10,
               [this, path] {
                   std::error_code ec;
                   std::filesystem::remove(path, ec);
                   loadCharacters();
                   fillCharacterList();
               });
        return true;
    }
    case kCharNew:
        // (0x1F5: the script ids' counter set to 1000000000: our new game's own.)
        leave([this] { enter(State::SelClass); });
        return true;
    default:
        return false;
    }
}

bool Menus::onSelClass(int message, gui::Window& from)
{
    // The class screen: pointing at a portrait picks its class and shows its description; clicking
    // one does nothing more. Accept goes on to the looks.
    if (message == gui::Hover) {
        if (from.id >= kClassFirst && from.id <= kClassLast) {
            chosenClass_ = from.id;
            newClass_ = static_cast<std::uint8_t>(from.id - kClassFirst);
            enable(screen_, kClassAccept, true);
            if (gui::Window* t = screen_->find(kClassText)) {
                t->text = gui_.text(kClassNames[newClass_], "SelClass.c");
            }
        }
        return true;
    }
    if (message != gui::Clicked) {
        return false;
    }
    if (from.id == kClassAccept && chosenClass_ >= 0) {
        leave([this] { enter(State::SelColor); });
    }
    return true;
}

// ------------------------------------------------------------------------------ SelColor

void Menus::setupSelColor()
{
    // The clothes' palette from modifier.bin; hair and skin are fixed.
    for (std::size_t i = 0; i < 32; ++i) {
        const auto* m = modifiers_.findModifier("UserColor" + std::to_string(i + 1));
        palette_[0][i] = m ? m->color : Rgb{0, 0, 0};
    }
    palette_[1] = kHairColors;
    palette_[2] = kSkinColors;

    // The starting colours.
    const std::pair<std::int32_t, std::uint32_t> starts[] = {
        {kColorSkin, colorData(2, 2)},    {kColorHair, colorData(1, 9)},    {kColorMustache, colorData(1, 9)},
        {kColorGoatee, colorData(1, 9)},  {kColorBeard, colorData(1, 9)},   {kColorPants, colorData(0, 12)},
        {kColorShirt1, colorData(0, 7)},  {kColorShirt2, colorData(0, 19)}, {kColorShoes1, colorData(0, 23)},
        {kColorShoes2, colorData(0, 6)},
    };
    const auto swatch = [this](gui::Window& self, gui::Painter& p) {
        // The whole button in its colour.
        const SDL_Point pos = self.screenPos();
        const Rgb c = colorOf(self.userData);
        p.fill(pos.x, pos.y, self.rect.w, self.rect.h, nox::gui::Color{c[0], c[1], c[2], false});
    };
    for (const auto& [id, data] : starts) {
        if (gui::Window* w = screen_->find(id)) {
            w->userData = data;
            w->draw = swatch;
        }
    }
    // The name: "DefaultName" to start with (game_showSelColor).
    if (gui::Window* name = screen_->find(kNameEntry)) {
        name->entry = nox::strings::fromUtf8(gui_.text("DefaultName", "SelColor.c")).substr(0, name->entryMax);
    }
    // The picker (760) is a window of its own on top, hidden until a colour is clicked; a click
    // outside it closes it.
    picker_ = gui_.detach(screen_->find(kPicker));
    if (picker_) {
        picker_->show(false);
        picker_->status |= nox::gui::status::Above;
        for (std::int32_t id = kSwatchFirst; id <= kSwatchLast; ++id) {
            if (gui::Window* w = picker_->find(id)) {
                w->draw = swatch;
            }
        }
    }
    if (gui::Window* preview = screen_->find(kPreview)) {
        preview->draw = [this](gui::Window& self, gui::Painter& p) { drawPreview(self, p); };
    }
}

Menus::Rgb Menus::colorOf(std::uint32_t data) const
{
    const std::uint32_t table = data & 0xFFFF;
    const std::uint32_t index = data >> 16;
    if (table >= palette_.size() || index >= 32) {
        return {0, 0, 0};
    }
    return palette_[table][index];
}

Menus::Rgb Menus::colorOf(std::int32_t id) const
{
    const gui::Window* w = screen_ ? screen_->find(id) : nullptr;
    return w ? colorOf(w->userData) : Rgb{0, 0, 0};
}

bool Menus::isEnabled(std::int32_t id) const
{
    const gui::Window* w = screen_ ? screen_->find(id) : nullptr;
    return w && w->enabled();
}

void Menus::drawPreview(gui::Window& w, gui::Painter& p) const
{
    // the body in the skin and hair colours (facial hair that's switched off is skin
    // coloured), bald if the hair is off, then the street clothes in their colours: each
    // item's own material colours, with the chosen ones on the slots its modifiers would take.
    const SDL_Point pos = w.screenPos();
    const Rgb white{255, 255, 255};
    const Rgb skin = colorOf(kColorSkin);
    const auto facial = [&](std::int32_t label, std::int32_t color) {
        return isEnabled(label) ? colorOf(color) : skin;
    };
    SpriteCache::Materials body{};
    body.fill(white);
    body[1] = colorOf(kColorHair);
    body[2] = facial(kGoateeLabel, kColorGoatee);
    body[3] = skin;
    body[4] = facial(kBeardLabel, kColorBeard);
    body[5] = facial(kMustacheLabel, kColorMustache);
    body[6] = skin;
    // Always a man here: SelColor clears the sex (game_showSelColor).
    p.image(isEnabled(kHairLabel) ? "MaleBase" : "MaleBaldBase", pos.x, pos.y, body);

    struct Item {
        const char* image;
        const char* armor;
        /// Modifier position -> colour button (a position past the four: none).
        std::array<std::pair<std::size_t, std::int32_t>, 2> slots;
    };
    // In the armour images' order: sneakers (bit 0), pants (bit 2), shirt (bit 10).
    const Item items[] = {
        {"MaleStreetSneakers", "StreetSneakers", {{{1, kColorShoes1}, {0, kColorShoes2}}}},
        {"MaleStreetPants", "StreetPants", {{{1, kColorPants}, {9, 0}}}},
        {"MaleStreetShirt", "StreetShirt", {{{1, kColorShirt1}, {2, kColorShirt2}}}},
    };
    for (const auto& item : items) {
        SpriteCache::Materials m{};
        m.fill(white);
        if (const auto* def = modifiers_.findArmor(item.armor)) {
            for (std::size_t slot = 1; slot <= 6; ++slot) {
                m[slot] = def->colors[slot - 1];
            }
            for (const auto& [position, button] : item.slots) {
                if (position < def->modifierSlots.size() && def->modifierSlots[position] != 0) {
                    m[def->modifierSlots[position]] = colorOf(button);
                }
            }
        }
        p.image(item.image, pos.x, pos.y, m);
    }
}

void Menus::openPicker(std::int32_t target, SDL_Point at)
{
    // The picker shows the button's palette, modal, its right edge at the
    // mouse and centred on it.
    if (!picker_) {
        return;
    }
    pickerTarget_ = target;
    pickerTable_ = target == kColorSkin ? 2 : target <= kColorBeard ? 1 : 0;  // skin: table 2, hair and facial hair: 1, clothes: 0
    for (std::int32_t id = kSwatchFirst; id <= kSwatchLast; ++id) {
        if (gui::Window* w = picker_->find(id)) {
            w->userData = colorData(pickerTable_, static_cast<std::uint32_t>(id - kSwatchFirst));
        }
    }
    picker_->rect.x = at.x - picker_->rect.w;
    picker_->rect.y = at.y - picker_->rect.h / 2;
    picker_->show(true);
    gui_.setModal(picker_);
}

void Menus::pick(std::uint32_t index)
{
    if (!picker_) {
        return;
    }
    picker_->show(false);
    gui_.setModal(nullptr);
    if (index < 32) {
        if (gui::Window* w = screen_ ? screen_->find(pickerTarget_) : nullptr) {
            w->userData = colorData(pickerTable_, index);
        }
    }
}

bool Menus::onSelColor(gui::Window& from)
{
    // The looks screen's procedure.
    const std::int32_t id = from.id;
    if (id >= kColorSkin && id <= kColorShoes2) {
        openPicker(id, mouse_);
    } else if (id >= kToggleFirst && id <= kToggleLast) {
        // Facial hair (and hair) on or off.
        if (gui::Window* label = screen_->find(id - 20)) {
            enable(screen_, label->id, !label->enabled());
        }
    } else if (id >= kSwatchFirst && id <= kSwatchLast) {
        pick(static_cast<std::uint32_t>(id - kSwatchFirst));
    } else if (id == kColorAccept) {
        if (solo_) {
            startNewSolo();
        } else if (const auto made = saveNewCharacter()) {
            leave([this, p = *made] { join(p); });
        }
    } else {
        return false;
    }
    return true;
}

nox::player::FileInfo Menus::madeCharacter(std::u16string& name) const
{
    // The looks screen's Accept (and its join counterpart): the name (DefaultName if blank or
    // only spaces), the colours (facial hair switched off is skin coloured), the clothes, the
    // class; flags, level and the rest as the record was zeroed.
    name.clear();
    if (const gui::Window* entry = screen_ ? screen_->find(kNameEntry) : nullptr) {
        name = entry->entry;
    }
    const std::u16string fallback = nox::strings::fromUtf8(gui_.text("DefaultName", "SelColor.c"));
    if (name.empty() || !cleanName(name)) {
        name = fallback;
    }
    nox::player::FileInfo info;
    info.flags = 0;
    const Rgb skin = colorOf(kColorSkin);
    const auto facial = [&](std::int32_t label, std::int32_t color) {
        return isEnabled(label) ? colorOf(color) : skin;
    };
    info.colors = {facial(kHairLabel, kColorHair), skin, facial(kMustacheLabel, kColorMustache),
                   facial(kGoateeLabel, kColorGoatee), facial(kBeardLabel, kColorBeard)};
    const std::int32_t clothes[] = {kColorPants, kColorShirt1, kColorShirt2, kColorShoes1, kColorShoes2};
    for (std::size_t i = 0; i < 5; ++i) {
        const gui::Window* w = screen_->find(clothes[i]);
        info.clothing[i] = static_cast<std::uint8_t>(w ? w->userData >> 16 : 0);
    }
    info.name = name;
    info.characterClass = static_cast<nox::player::CharacterClass>(newClass_);
    info.isFemale = 0;
    info.level = 1;
    return info;
}

void Menus::startNewSolo()
{
    // The looks screen's Accept in a solo game, its solo branch (WORKING emptied, the
    // character's client sections written to Save\WORKING\Player.plr by the new character's
    // section writer (info, 1): the bars as the class screen emptied them (its starting set is
    // only a network game's), its File Info, the music), the class's first map; "Loading..." and
    // the screen out, the game started. A file that can't be written: nothing more.
    namespace menu = nox::client::solo_menu;
    std::u16string name;
    const nox::player::FileInfo held = madeCharacter(name);
    const std::string map(menu::firstMap(newClass_));
    nox::player::Music music;
    music.solo = 1;
    if (callbacks_.music) {
        music = callbacks_.music();
    }
    const auto cls = static_cast<nox::player::CharacterClass>(newClass_);
    const auto plr = menu::writeNewCharacter(soloRoot(), held, nox::client::SpellBars{}.toGui(cls), music, map,
                                             nox::client::solo_saves::localTime());
    if (plr.empty()) {
        SDL_Log("menus: the new character couldn't be written to %s", (soloRoot() / "Save" / "WORKING").string().c_str());
        return;
    }
    SDL_Log("menus: saved %s", plr.string().c_str());
    characterFile_ = plr;
    SoloStart start;
    start.info = nox::client::solo_saves::readPlr(plr).info; // (as the section writer left it)
    start.profile = profileOf(held);
    start.characterFile = plr;
    start.map = gameDir_ / "maps" / map / (map + ".map");
    showLoading(true);
    leave([this, start] { startSolo(start); });
}

std::optional<nox::client::PlayerProfile> Menus::saveNewCharacter()
{
    // The looks screen's Accept: the name (DefaultName if blank), the colours, and a new Save\<x>NN.plr.
    std::u16string name;
    nox::player::FileInfo info = madeCharacter(name);
    // (parseFileInfoData's write in a network game: 2, a quest's 4.)
    info.flags = nox::client::solo_menu::fileInfoFlags(info.flags, true, questGame_);
    {
        // GetLocalTime's SYSTEMTIME: year, month, weekday, day, hour, minute, second, ms.
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        info.time = {static_cast<std::uint16_t>(tm.tm_year + 1900), static_cast<std::uint16_t>(tm.tm_mon + 1),
                     static_cast<std::uint16_t>(tm.tm_wday),        static_cast<std::uint16_t>(tm.tm_mday),
                     static_cast<std::uint16_t>(tm.tm_hour),        static_cast<std::uint16_t>(tm.tm_min),
                     static_cast<std::uint16_t>(tm.tm_sec),         static_cast<std::uint16_t>(ms)};
    }

    // The file name: sprintf("%.6s%2.2d.plr") given the wide name, so its UTF-16 bytes up to
    // the first zero one (for most names, the first letter), then the first free number.
    std::u16string stem;
    // The name's UTF-16 bytes, low then high, up to the first zero byte (6 at most).
    for (std::size_t i = 0; i < name.size() * 2 && stem.size() < 6; ++i) {
        const auto byte = static_cast<char16_t>(i % 2 == 0 ? name[i / 2] & 0xFF : name[i / 2] >> 8);
        if (byte == 0) {
            break;
        }
        stem.push_back(byte);
    }
    std::error_code ec;
    std::filesystem::create_directories(saveDir_, ec);
    std::filesystem::path path;
    for (int n = 0; n < 100; ++n) {  // the first free number, 00-99
        const std::u16string number{static_cast<char16_t>(u'0' + n / 10), static_cast<char16_t>(u'0' + n % 10)};
        const auto candidate = saveDir_ / std::filesystem::path(stem + number + u".plr");
        if (!std::filesystem::exists(candidate, ec)) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) {
        SDL_Log("menus: no free character file in %s", saveDir_.string().c_str());
        return std::nullopt;
    }
    info.savePath = path.string();

    // The client sections, as the exe writes them for a new character: the spell bars (one of
    // the class's starting sets at random, the set picker with 0, from the class screen; for a
    // quest the picker with 1, every slot empty), the file info, the music.
    nox::player::PlayerFile file;
    nox::player::Gui bars = nox::client::SpellBars{}.toGui(info.characterClass);
    if (things_ && !questGame_) {
        std::random_device entropy;
        const int presets = nox::client::SpellBars::presetCount(info.characterClass);
        const int preset = nox::util::uniform_int_distribution<int>(0, std::max(presets - 1, 0))(entropy);
        bars = nox::client::SpellBars::defaults(info.characterClass, preset, *things_).toGui(info.characterClass);
    }
    file.sections.push_back({static_cast<std::uint32_t>(nox::player::SectionId::Gui), bars.serialize()});
    file.sections.push_back({static_cast<std::uint32_t>(nox::player::SectionId::FileInfo), info.serialize()});
    file.sections.push_back(
        {static_cast<std::uint32_t>(nox::player::SectionId::Music), nox::player::Music{}.serialize()});
    try {
        nox::player::savePlayerFile(path, file);
    } catch (const std::exception& ex) {
        SDL_Log("menus: saving %s: %s", path.string().c_str(), ex.what());
        return std::nullopt;
    }
    SDL_Log("menus: saved %s", path.string().c_str());
    characterFile_ = path;

    // The profile to join with, from the same looks: 5 colours (RGB each) and 5 clothing picks.
    return profileOf(info);
}

void Menus::draw(SDL_Renderer* renderer, int width, int height)
{
    // The credits' screen in place of the hidden menu.
    if (creditsUp()) {
        credits_->drawScreens(renderer, width, height);
        return;
    }
    gui_.draw(renderer, width, height);
}

void Menus::drawOverAll(SDL_Renderer* renderer, int width, int height) const
{
    if (credits_) {
        credits_->drawOverAll(renderer, width, height);
    }
}

void Menus::handleEvent(const SDL_Event& event, int width, int height, std::uint64_t nowMs)
{
    nowMs_ = nowMs;
    // The mouse kept in layout units (640 x 480) for the picker and the test steps.
    if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        const float x = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
        const float y = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
        const SDL_FPoint at = gui::Painter(nullptr, gui_.resources(), width, height).toLayout(x, y);
        mouse_ = {static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y))};
    }
    if (creditsUp()) {
        // The credits' window is modal with the input (Escape too: any key or click skips them).
        credits_->handleEvent(event, width, height);
        return;
    }
    if (picker_ && !picker_->hidden() && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // A press outside the picker closes it.
        const SDL_Rect r = picker_->rect;
        if (mouse_.x < r.x || mouse_.y < r.y || mouse_.x >= r.x + r.w || mouse_.y >= r.y + r.h) {
            pick(0xDEAD);  // an index of 32 or more only closes it
            return;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
        if (picker_ && !picker_->hidden()) {
            pick(0xDEAD);
            return;
        }
        if (dialog_) {
            closeDialog(); // Escape closes a message box
            if (wolLoggingIn_) {
                wol_.reset(); // (and gives up a login under "Connecting...")
                wolLoggingIn_ = false;
            }
            return;
        }
        // Escape is Back (on the game list: the description box shut first).
        closeGameInfo();
        back();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && dialog_ && dialog_ == dialogKeysOf_ && dialogKeys_ && !gui_.sliding() &&
        dialogKeys_(event.key)) {
        return; // (noxcpp's mode lists' arrows and Enter)
    }
    const bool onBrowserScreen = !stack_.empty() && stack_.back() == State::Browser && !leaving_;
    if (onBrowserScreen && !dialog_ && infoServer_ && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // (noxcpp: a press outside the description box shuts it, and does nothing else; the exe's
        // box is modal till Join or Escape)
        if (gui::Window* box = screen_ ? screen_->find(kWorldListPanel) : nullptr) {
            const SDL_FPoint m = gui::Painter(nullptr, gui_.resources(), width, height).toLayout(event.button.x, event.button.y);
            const SDL_Point pos = box->screenPos();
            if (m.x < static_cast<float>(pos.x) || m.y < static_cast<float>(pos.y) || m.x >= static_cast<float>(pos.x + box->rect.w) ||
                m.y >= static_cast<float>(pos.y + box->rect.h)) {
                closeGameInfo();
                return;
            }
        }
    }
    if (onBrowserScreen && !dialog_ && !infoServer_) { // (the description box has the mouse while it's up)
        if (const auto host = browser_.handleEvent(event, nowMs)) {
            chooseCharacter(*host);
            return;
        }
        if (const auto clicked = browser_.takeClicked()) {
            showGameInfo(clicked->entry, clicked->at, width, height); // (the game list's 0x4010)
            return;
        }
    }
    // No clicks reach the screens while one slides.
    if (!gui_.sliding()) {
        gui_.handleEvent(event, width, height);
    }
}

} // namespace noxsdl
