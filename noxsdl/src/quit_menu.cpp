// QuitMenu (quit_menu.hpp): the in-game menu's two layouts, its buttons, and the Yes / No dialog
// over it.
#include "quit_menu.hpp"

namespace noxsdl {

namespace {

// QuitMenu.wnd
constexpr std::int32_t kAutoLoad = 9001, kAutoSave = 9002, kSave = 9003, kQuit = 9004, kOptions = 9005,
                       kResume = 9006, kObserve = 9007, kGameplay = 9008, kVote = 9009;
// dlg.wnd
constexpr std::int32_t kDlgText = 4004, kDlgTitle = 4005, kDlgNo = 4006, kDlgYes = 4007, kDlgOk = 4001,
                       kDlgCancel = 4002;

} // namespace

QuitMenu::QuitMenu(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks)
    : gui_(std::move(res)), windowDir_(std::move(windowDir)), callbacks_(std::move(callbacks))
{
    // wndLoadQuitMenu: loaded once, centred across; hidden until Escape.
    menu_ = gui_.load(windowDir_ / "QuitMenu.wnd", [this](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        gui_.resources().sound(0x2FE); // ButtonPress (menuGameOnButton)
        return onButton(from);
    });
    menu_->rect.x = (640 - menu_->rect.w) / 2;
    // Its own drawing: the screen behind it darkened.
    menu_->draw = [](gui::Window& self, gui::Painter& p) {
        const SDL_Point pos = self.screenPos();
        p.fillAlpha(pos.x, pos.y, self.rect.w, self.rect.h, {0, 0, 0, 128});
    };
    if (gui::Window* vote = menu_->find(kVote)) {
        voteRect_ = vote->rect;
        voteText_ = gui_.text("Vote", "GUIQuit.c");
    }
    saveText_[0] = gui_.text("MultiplayerSaveLabel", "GUIQuit.c");
    saveText_[1] = gui_.text("SoloSaveLabel", "GUIQuit.c");
    if (gui::Window* gameplay = menu_->find(kGameplay)) {
        gameplayText_ = gameplay->text;
    }
    menu_->show(false);
}

void QuitMenu::layout(bool solo)
{
    // The menu's two layouts: the solo game's autosave buttons, or the network game's
    // Observer Mode, Gameplay Controls and Vote; Quit in Vote's place (solo) or below it.
    const auto show = [&](std::int32_t id, bool visible) {
        if (gui::Window* w = menu_->find(id)) {
            w->show(visible);
        }
    };
    show(kAutoLoad, solo);
    show(kAutoSave, solo);
    show(kObserve, !solo);
    show(kGameplay, !solo);
    show(kVote, !solo);
    if (gui::Window* save = menu_->find(kSave)) {
        save->text = saveText_[solo ? 1 : 0];
        if (solo) {
            save->status |= nox::gui::status::Enabled;
        } else {
            save->status &= ~nox::gui::status::Enabled; // (the network game's save: not yet)
            save->text += " - WIP";
        }
    }
    // Gameplay Controls: the host's Server Settings (server_options.hpp); off in a game we don't
    // host.
    if (gui::Window* gameplay = menu_->find(kGameplay)) {
        const bool host = callbacks_.hosting && callbacks_.hosting();
        gameplay->text = gameplayText_;
        if (host) {
            gameplay->status |= nox::gui::status::Enabled;
        } else {
            gameplay->status &= ~nox::gui::status::Enabled;
        }
    }
    // Vote: where a noxcpp server says its map vote can be called, that (MSG_NOXCPP_MAPVOTE kCall); the stock
    // game's vote (a kick, MSG_VOTE) isn't done yet: off, and marked.
    if (gui::Window* vote = menu_->find(kVote)) {
        if (callbacks_.canVoteMap && callbacks_.canVoteMap()) {
            vote->text = "Vote Map";
            vote->status |= nox::gui::status::Enabled;
        } else {
            vote->text = voteText_ + " - WIP";
            vote->status &= ~nox::gui::status::Enabled;
        }
    }
    if (gui::Window* quit = menu_->find(kQuit)) {
        quit->rect.x = voteRect_.x;
        quit->rect.y = voteRect_.y + (solo ? 0 : 45);
    }
    menu_->rect.w = 220;
    menu_->rect.h = solo ? 285 : 330;
}

void QuitMenu::newGame()
{
    // game_guiInit makes the window anew for each game: AutoSave on again (a DEAD
    // refusal turns it off for the rest of the game, loads too).
    close();
    if (gui::Window* autoSave = menu_->find(kAutoSave)) {
        autoSave->status |= nox::gui::status::Enabled;
    }
}

void QuitMenu::toggle()
{
    // Open or shut.
    if (open_) {
        close();
        return;
    }
    if (callbacks_.refused && callbacks_.refused()) {
        return;
    }
    solo_ = callbacks_.solo && callbacks_.solo();
    open_ = true;
    gui_.resources().sound(0x399); // ShellClick
    layout(solo_);
    menu_->show(true);
    gui_.setModal(menu_);
    if (solo_ && callbacks_.pause) {
        callbacks_.pause(1);
    }
}

void QuitMenu::close()
{
    // The menu's close: hidden, its input let go, Game_SetPaused(0).
    closeDialog();
    if (!open_) {
        return;
    }
    open_ = false;
    menu_->show(false);
    gui_.setModal(nullptr);
    if (callbacks_.pause) {
        callbacks_.pause(0);
    }
}

void QuitMenu::closeDialog()
{
    if (dialog_) {
        gui_.destroy(dialog_);
        dialog_ = nullptr;
        if (open_) {
            gui_.setModal(menu_);
        }
    }
}

bool QuitMenu::onButton(gui::Window& from)
{
    // menuGameOnButton.
    // The buttons' work is queued for after the event: it closes this menu and may open other windows.
    // closeThen: closed, then the callback (if set); `soloOnly`: only if the game was solo.
    const auto closeThen = [this](std::function<void()> Callbacks::*callback, bool soloOnly = false) {
        deferred_.push_back([this, callback, soloOnly] {
            const bool solo = solo_;
            close();
            if ((solo || !soloOnly) && callbacks_.*callback) {
                (callbacks_.*callback)();
            }
        });
        return true;
    };
    switch (from.id) {
    case kResume:
        deferred_.push_back([this] { close(); });
        return true;
    case kAutoLoad:
        // 0x2329: closed (unpaused), then paused and asked (or loaded, dying or dead).
        return closeThen(&Callbacks::autoLoad);
    case kAutoSave:
        // 0x232A: our drawable flagged DEAD turns the button off; else closed and, solo,
        // AUTOSAVE asked for.
        if (callbacks_.deadFlag && callbacks_.deadFlag()) {
            from.status &= ~nox::gui::status::Enabled;
            return true;
        }
        return closeThen(&Callbacks::autoSave, true);
    case kSave:
        // 0x232B: closed; solo, the save menu (a network game's CliNet_SendRequestSavePlayer:
        // not yet).
        return closeThen(&Callbacks::save, true);
    case kObserve:
        return closeThen(&Callbacks::observe);
    case kOptions:
        return closeThen(&Callbacks::options);
    case kGameplay:
        // 0x2330: closed, then guiServerOptsLoad.
        return closeThen(&Callbacks::gameplay);
    case kVote:
        return closeThen(&Callbacks::voteMap);
    // Quit asks first (Yes / No), over the menu.
    case kQuit:
        ask(gui_.text("GUIQuit.c:ReallyQuitTitle"), gui_.text("GUIQuit.c:ReallyQuitMessage"), [this] {
            close();
            if (callbacks_.quit) {
                callbacks_.quit();
            }
        });
        return true;
    default:
        return false;
    }
}

void QuitMenu::ask(const std::string& title, const std::string& text, std::function<void()> onYes)
{
    // Gui_Dialog_Create with Yes / No, over the menu.
    closeDialog();
    dialog_ = gui_.load(windowDir_ / "dlg.wnd", [this, onYes = std::move(onYes)](gui::Window&, int message,
                                                                              gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        const bool yes = from.id == kDlgYes || from.id == kDlgOk;
        if (!yes && from.id != kDlgNo && from.id != kDlgCancel) {
            return false;
        }
        gui_.resources().sound(0x2FE); // ButtonPress
        deferred_.push_back([this, yes, onYes] {
            closeDialog();
            if (yes && onYes) {
                onYes();
            }
        });
        return true;
    });
    gui::centreOnScreen(*dialog_);
    dialog_->status |= nox::gui::status::Above;
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = title;
    }
    if (gui::Window* t = dialog_->find(kDlgText)) {
        t->text = text;
    }
    for (const auto id : {kDlgOk, kDlgCancel}) {
        if (gui::Window* b = dialog_->find(id)) {
            b->show(false);
        }
    }
    for (const auto id : {kDlgYes, kDlgNo}) {
        if (gui::Window* b = dialog_->find(id)) {
            b->show(true);
        }
    }
    gui_.setModal(dialog_);
}

void QuitMenu::update(std::uint64_t nowMs)
{
    gui_.update(nowMs);
    // The buttons' queued work, run after the GUI's update: it may close or destroy windows.
    gui::runQueued(deferred_);
}

void QuitMenu::draw(SDL_Renderer* renderer, int width, int height)
{
    if (open_) {
        gui_.draw(renderer, width, height);
    }
}

bool QuitMenu::handleEvent(const SDL_Event& event, int width, int height)
{
    if (!open_) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat) {
        if (dialog_) {
            closeDialog(); // Escape answers No
        } else {
            close();
        }
        return true;
    }
    gui_.handleEvent(event, width, height);
    return true;
}

} // namespace noxsdl
