// noxcpp's mode lists, laid out as the game list lists games: Host Custom (menus.hpp's hostCustom: hosted for others) and the main menu's Game Modes
// (soloModes: on this computer, unlisted). One list, its rows mode_list.hpp's: first what's built
// into the program (its modes, Quest and the Map Editor: marked "(built in)" on a faint gold band),
// then the mods folder's modes, then each mods folder that didn't load, greyed with why. The chosen
// row's settings are shown under the list (a choice, a toggle
// or a number: a click on the left half goes back, on the right half on); Host (Start) starts it,
// Open Mods Folder shows mods\ beside noxcore.exe, Refresh reads it again.
//
// The window is dlg.wnd's frame (no .wnd changed), its parts drawn here in the menus' fonts and
// colours.
#include "menus.hpp"

#include "mode_list.hpp"
#include "mode_ui.hpp"
#include "mods_folder.hpp"

#include "noxcore/modes/registry.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace noxsdl {

namespace st = nox::gui::status;

namespace {

constexpr std::int32_t kDlgCancel = 4002, kDlgTitle = 4005; // (dlg.wnd's)
constexpr std::int32_t kCanvasId = 4200, kRowId = 4201, kOptionId = 4301, kButtonId = 4401;

// The layout, in the game's 640 x 480 units.
constexpr int kWidth = 600;
constexpr int kHeaderY = 32;
constexpr int kRowsY = 48;
constexpr int kRowH = 18;
constexpr int kVisibleRows = 9;
constexpr int kPanelY = kRowsY + kVisibleRows * kRowH + 10;
constexpr int kOptionsY = kPanelY + 18;
constexpr int kOptionH = 18;
constexpr int kVisibleOptions = 5;
constexpr int kButtonsY = kOptionsY + kVisibleOptions * kOptionH + 10;
constexpr int kButtonH = 22;
constexpr int kHeight = kButtonsY + kButtonH + 12;
constexpr int kOptionLabelW = 220; ///< an option line's label, then its value
constexpr int kOptionValueW = 160;

struct Column {
    const char* title;
    int x;
    int w;
};
constexpr Column kColumns[] = {{"Mode", 12, 122}, {"Description", 138, 238}, {"Players", 380, 52}, {"Stock clients", 436, 78}, {"Levels", 518, 70}};

enum Button { OpenFolder, Refresh, Cancel, Host, kButtonCount };
struct ButtonDef {
    const char* label;
    int x;
    int w;
};
constexpr ButtonDef kButtons[kButtonCount] = {{"Open Mods Folder", 12, 132}, {"Refresh", 150, 80}, {"Cancel", 430, 70}, {"Host", 506, 82}};

const nox::gui::Color kGold{240, 180, 42};
const nox::gui::Color kBright{255, 236, 170};
const nox::gui::Color kDim{150, 128, 92};
const nox::gui::Color kGrey{118, 110, 98};

using Row = ModeListRow;

struct ListState {
    ModeListFor list = ModeListFor::HostCustom;
    std::vector<Row> rows;
    int chosen = 0;
    int scroll = 0;
    bool started = false;
    int lastClickRow = -1;
    std::uint64_t lastClickMs = 0;
};

std::vector<Row> buildRows(ModeListFor list)
{
    return modeListRows(list, nox::modes::registry(), modsReport(), [](std::string_view id) {
        const ModeUiEntry* ui = modeUiEntry(id);
        return ui && ui->soloScreens;
    });
}

/// `s` cut with "..." to fit `w` (whole UTF-8 letters).
std::string fit(gui::Painter& p, std::string s, int w, bool large)
{
    if (p.textWidth(s, large) <= w) {
        return s;
    }
    while (!s.empty() && p.textWidth(s + "...", large) > w) {
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
        if (!s.empty()) {
            s.pop_back();
        }
    }
    return s + "...";
}

} // namespace

void Menus::hostCustom()
{
    modeList(ModeListFor::HostCustom);
}

void Menus::soloModes()
{
    // The main menu's Solo Quest (reading "Game Modes" here): the same list, played here. Quest goes
    // on to the stock Solo Quest flow untouched; the Map Editor is the single-player editor (its
    // close back to the main menu); a NoX Core mode is hosted on this computer, unlisted.
    modeList(ModeListFor::GameModes);
}

void Menus::modeList(ModeListFor list)
{
    const bool hosting = list == ModeListFor::HostCustom;
    std::string* const kept = hosting ? &hostModeChosen_ : &soloModeChosen_; // (the row kept for next time)
    const auto state = std::make_shared<ListState>();
    state->list = list;
    state->rows = buildRows(list);
    state->chosen = modeListRowOf(state->rows, *kept);
    const int rowCount = static_cast<int>(state->rows.size());
    state->scroll = std::clamp(state->chosen - kVisibleRows + 1, 0, std::max(0, rowCount - kVisibleRows));

    closeDialog();
    nox::gui::WindowDef def = nox::gui::loadWindowFile(windowDir_ / "dlg.wnd");
    def.width = kWidth;
    def.height = kHeight;
    std::erase_if(def.children, [](const nox::gui::WindowDef& d) { return d.id != kDlgTitle && d.id != kDlgCancel; });
    nox::gui::WindowDef base;
    for (auto& d : def.children) {
        if (d.id == kDlgTitle) {
            d.x = 5;
            d.y = 6;
            d.width = kWidth - 10;
            d.height = 18;
        } else if (d.id == kDlgCancel) {
            base = d; // (the parts are push buttons like it, that track the mouse)
        }
    }
    std::erase_if(def.children, [](const nox::gui::WindowDef& d) { return d.id == kDlgCancel; });
    base.backgroundImage.clear();
    base.enabledImage.clear();
    base.disabledImage.clear();
    base.hiliteImage.clear();
    base.selectedImage.clear();
    base.text.clear();
    base.tooltip.clear();
    base.data = {};
    base.status = st::Enabled | st::NoFocus;
    const auto add = [&](std::int32_t id, int x, int y, int w, int h, std::uint32_t status) {
        nox::gui::WindowDef d = base;
        d.id = id;
        d.x = x;
        d.y = y;
        d.width = w;
        d.height = h;
        d.status = status;
        def.children.push_back(std::move(d));
    };
    add(kCanvasId, 0, 0, kWidth, kHeight, st::NoFocus); // (the headings and the panel's words: takes no input)
    for (int i = 0; i < kVisibleRows; ++i) {
        add(kRowId + i, 8, kRowsY + i * kRowH, kWidth - 16, kRowH, st::Enabled | st::NoFocus);
    }
    for (int i = 0; i < kVisibleOptions; ++i) {
        add(kOptionId + i, 12, kOptionsY + i * kOptionH, kOptionLabelW + kOptionValueW, kOptionH, st::Enabled | st::NoFocus);
    }
    for (int b = 0; b < kButtonCount; ++b) {
        add(kButtonId + b, kButtons[b].x, kButtonsY, kButtons[b].w, kButtonH, st::Enabled | st::NoFocus);
    }

    const auto chosenRow = [state]() -> const Row* {
        return state->chosen >= 0 && state->chosen < static_cast<int>(state->rows.size()) ? &state->rows[static_cast<std::size_t>(state->chosen)] : nullptr;
    };
    const auto choose = [this, state, kept](int row, bool sound) {
        const int n = static_cast<int>(state->rows.size());
        row = std::clamp(row, 0, std::max(0, n - 1));
        if (row != state->chosen && sound) {
            gui_.resources().sound(0x398); // (ShellSelect)
        }
        state->chosen = row;
        if (row < state->scroll) {
            state->scroll = row;
        } else if (row >= state->scroll + kVisibleRows) {
            state->scroll = row - kVisibleRows + 1;
        }
        if (!state->rows.empty() && state->rows[static_cast<std::size_t>(row)].kind != Row::Kind::Broken) {
            *kept = modeListKey(state->rows[static_cast<std::size_t>(row)]);
        }
    };
    const auto start = [this, state, chosenRow, hosting] {
        const Row* row = chosenRow();
        if (state->started || !row || row->kind == Row::Kind::Broken) {
            return;
        }
        state->started = true;
        gui_.resources().sound(0x399);
        const Row r = *row;
        deferred_.push_back([this, r, hosting] {
            closeDialog();
            switch (r.kind) {
            case Row::Kind::Quest:
                if (hosting) {
                    chooseCharacter(std::string(kHostQuestGame)); // (the stock quest flow, unchanged)
                } else {
                    onSoloQuest(); // (the stock Solo Quest flow, unchanged)
                }
                break;
            case Row::Kind::Editor:
                if (hosting) {
                    // (a network character, then the editor with its session dialog: main.cpp)
                    chooseCharacter(std::string(kHostEditGame));
                } else if (callbacks_.editor) {
                    leave([this] { callbacks_.editor(); });
                }
                break;
            default:
                pendingModeOptions_ = modeSettings_[r.id]; // (its row's settings: the difficulty...)
                if (hosting && r.soloScreens) {
                    onSoloScreensMode(r.id); // (its own start: the class and character screens)
                } else {
                    chooseCharacter(modeHostKey(r.id, !hosting));
                }
                break;
            }
        });
    };
    const auto refresh = [this, state, kept] {
        gui_.resources().sound(0x399);
        const std::string was = *kept;
        refreshMods();
        state->rows = buildRows(state->list);
        state->chosen = modeListRowOf(state->rows, was);
        state->scroll = std::clamp(state->chosen - kVisibleRows + 1, 0, std::max(0, static_cast<int>(state->rows.size()) - kVisibleRows));
    };

    dialog_ = gui_.load(def, [this, state, chosenRow, choose, start, refresh](gui::Window&, int message, gui::Window& from) {
        if (message != gui::Clicked) {
            return false;
        }
        if (from.id >= kRowId && from.id < kRowId + kVisibleRows) {
            const int row = state->scroll + (from.id - kRowId);
            if (row >= static_cast<int>(state->rows.size())) {
                return true;
            }
            // (A second click on the same row soon after: as Host, as the game list's join.)
            const std::uint64_t now = SDL_GetTicks();
            const bool again = row == state->lastClickRow && now - state->lastClickMs < 450;
            state->lastClickRow = row;
            state->lastClickMs = now;
            choose(row, false);
            if (again) {
                start();
            }
            return true;
        }
        if (from.id >= kOptionId && from.id < kOptionId + kVisibleOptions) {
            const Row* row = chosenRow();
            const std::size_t k = static_cast<std::size_t>(from.id - kOptionId);
            if (!row || k >= row->options.size()) {
                return true;
            }
            const nox::modes::OptionSpec& s = row->options[k];
            // The value's left half goes back, the rest on.
            const SDL_Point pos = from.screenPos();
            const bool back = mouse_.x < pos.x + kOptionLabelW + kOptionValueW / 2;
            nox::modes::OptionValues& set = modeSettings_[row->id];
            set[s.key] = std::to_string(stepModeOption(s, modeOptionValue(set, s), back ? -1 : 1));
            gui_.resources().sound(0x398);
            return true;
        }
        switch (from.id - kButtonId) {
        case OpenFolder:
            gui_.resources().sound(0x399);
            openModsFolder();
            return true;
        case Refresh: refresh(); return true;
        case Cancel:
            gui_.resources().sound(0x399);
            deferred_.push_back([this] { closeDialog(); });
            return true;
        case Host: start(); return true;
        default: return false;
        }
    });

    // The headings, the column lines, and the chosen row's panel heading (its settings, or why a
    // broken mod didn't load).
    if (gui::Window* canvas = dialog_->find(kCanvasId)) {
        canvas->draw = [chosenRow, hosting](gui::Window& w, gui::Painter& p) {
            const SDL_Point pos = w.screenPos();
            for (const Column& c : kColumns) {
                p.text(c.title, pos.x + c.x, pos.y + kHeaderY, false, kGold);
            }
            p.fill(pos.x + 8, pos.y + kRowsY - 3, kWidth - 16, 1, nox::gui::Color{120, 90, 30});
            p.fill(pos.x + 8, pos.y + kPanelY - 4, kWidth - 16, 1, nox::gui::Color{120, 90, 30});
            const Row* row = chosenRow();
            if (!row) {
                return;
            }
            const int x = pos.x + 12;
            const int y = pos.y + kPanelY;
            switch (row->kind) {
            case Row::Kind::Broken:
                p.text(fit(p, row->name + " didn't load", kWidth - 24, false), x, y, false, kGold);
                p.text(row->description, x, y + 18, false, kDim, kWidth - 24);
                break;
            case Row::Kind::Quest:
                p.text("Quest: the game's own. Its settings are chosen in the game, as always.", x, y, false, kDim, kWidth - 24);
                break;
            case Row::Kind::Editor:
                p.text(hosting ? "Map Editor: the map, who may edit and the sandbox are chosen in its session dialog, after your character."
                               : "Map Editor: your own maps, on this computer. Its File > Host a Session shares one with others.",
                       x, y, false, kDim, kWidth - 24);
                break;
            default:
                if (row->options.empty()) {
                    p.text(fit(p, row->name + ": no settings to choose here.", kWidth - 24, false), x, y, false, kDim);
                } else {
                    p.text(fit(p, row->name + " settings", kWidth - 24, false), x, y, false, kGold);
                    if (row->options.size() > static_cast<std::size_t>(kVisibleOptions)) {
                        p.text("(the first " + std::to_string(kVisibleOptions) + ")", x + 300, y, false, kDim);
                    }
                }
                break;
            }
        };
    }
    for (int i = 0; i < kVisibleRows; ++i) {
        if (gui::Window* w = dialog_->find(kRowId + i)) {
            w->draw = [state, i](gui::Window& win, gui::Painter& p) {
                const int row = state->scroll + i;
                if (row >= static_cast<int>(state->rows.size())) {
                    return;
                }
                const Row& r = state->rows[static_cast<std::size_t>(row)];
                const SDL_Point pos = win.screenPos();
                const bool chosen = row == state->chosen;
                if (chosen) {
                    p.fillAlpha(pos.x, pos.y, win.rect.w, win.rect.h, SDL_Color{240, 180, 42, 48});
                    p.outline(pos.x, pos.y, win.rect.w, win.rect.h, nox::gui::Color{160, 120, 30});
                } else if (r.builtIn()) {
                    p.fillAlpha(pos.x, pos.y, win.rect.w, win.rect.h, SDL_Color{240, 180, 42, 16}); // (built in: a faint gold band)
                }
                if (r.builtIn() && row + 1 < static_cast<int>(state->rows.size()) && !state->rows[static_cast<std::size_t>(row) + 1].builtIn()) {
                    p.fill(pos.x, pos.y + win.rect.h - 1, win.rect.w, 1, nox::gui::Color{120, 90, 30}); // (then the mods)
                }
                const bool broken = r.kind == Row::Kind::Broken;
                const nox::gui::Color c = broken ? kGrey : chosen ? kBright : kGold;
                const nox::gui::Color about = broken ? kGrey : chosen ? kGold : kDim;
                const int ty = pos.y + (kRowH - p.fontHeight(false)) / 2;
                const int dx = pos.x - 8; // (the columns are the dialog's)
                const std::string name = r.builtIn() ? r.name + " (built in)" : r.name;
                p.text(fit(p, name, kColumns[0].w - 4, false), dx + kColumns[0].x, ty, false, c);
                p.text(fit(p, broken ? "Didn't load: " + r.description : r.description, kColumns[1].w - 4, false), dx + kColumns[1].x, ty, false, about);
                p.text(fit(p, r.players.empty() ? "-" : r.players, kColumns[2].w - 4, false), dx + kColumns[2].x, ty, false, about);
                p.text(fit(p, r.stock.empty() ? "-" : r.stock, kColumns[3].w - 4, false), dx + kColumns[3].x, ty, false, about);
                p.text(fit(p, r.levels.empty() ? "-" : r.levels, kColumns[4].w - 4, false), dx + kColumns[4].x, ty, false, about);
            };
        }
    }
    for (int i = 0; i < kVisibleOptions; ++i) {
        if (gui::Window* w = dialog_->find(kOptionId + i)) {
            w->draw = [this, chosenRow, i](gui::Window& win, gui::Painter& p) {
                const Row* row = chosenRow();
                if (!row || row->kind != Row::Kind::Mode || static_cast<std::size_t>(i) >= row->options.size()) {
                    return;
                }
                const nox::modes::OptionSpec& s = row->options[static_cast<std::size_t>(i)];
                const SDL_Point pos = win.screenPos();
                const int ty = pos.y + (kOptionH - p.fontHeight(false)) / 2;
                p.text(fit(p, s.label, kOptionLabelW - 8, false), pos.x, ty, false, kGold);
                const auto it = modeSettings_.find(row->id);
                const int v = it != modeSettings_.end() ? modeOptionValue(it->second, s) : s.defaultValue;
                const std::string value = fit(p, modeOptionText(s, v), kOptionValueW - 40, false);
                const int vx = pos.x + kOptionLabelW;
                p.text("<", vx, ty, false, kDim);
                p.text(value, vx + (kOptionValueW - p.textWidth(value, false)) / 2, ty, false, kBright);
                p.text(">", vx + kOptionValueW - p.textWidth(">", false), ty, false, kDim);
            };
        }
    }
    for (int b = 0; b < kButtonCount; ++b) {
        if (gui::Window* w = dialog_->find(kButtonId + b)) {
            w->draw = [chosenRow, b, hosting](gui::Window& win, gui::Painter& p) {
                const SDL_Point pos = win.screenPos();
                const bool held = (win.state & gui::kPressed) != 0;
                const Row* row = chosenRow();
                const bool off = b == Host && (!row || row->kind == Row::Kind::Broken);
                p.fillAlpha(pos.x, pos.y, win.rect.w, win.rect.h, SDL_Color{0, 0, 0, 120});
                p.outline(pos.x, pos.y, win.rect.w, win.rect.h, off ? nox::gui::Color{90, 80, 60} : nox::gui::Color{200, 150, 40});
                const std::string label = b == Host && !hosting ? "Start" : kButtons[b].label;
                const int tw = p.textWidth(label, false);
                p.text(label, pos.x + (win.rect.w - tw) / 2, pos.y + (kButtonH - p.fontHeight(false)) / 2 + (held ? 1 : 0), false,
                       off ? kGrey : b == Host ? kBright : kGold);
            };
        }
    }
    gui::centreOnScreen(*dialog_);
    dialog_->status |= st::Above;
    if (gui::Window* t = dialog_->find(kDlgTitle)) {
        t->text = hosting ? "Host Custom" : "Game Modes";
        t->largeFont = true;
    }
    // The keys: Up / Down (Home / End, Page Up / Down) move the choice, Enter starts it.
    dialogKeys_ = [state, choose, start](const SDL_KeyboardEvent& key) {
        switch (key.key) {
        case SDLK_UP: choose(state->chosen - 1, true); return true;
        case SDLK_DOWN:
        case SDLK_TAB: choose(state->chosen + 1, true); return true;
        case SDLK_PAGEUP: choose(state->chosen - kVisibleRows, true); return true;
        case SDLK_PAGEDOWN: choose(state->chosen + kVisibleRows, true); return true;
        case SDLK_HOME: choose(0, true); return true;
        case SDLK_END: choose(static_cast<int>(state->rows.size()) - 1, true); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (!key.repeat) {
                start();
            }
            return true;
        default: return false;
        }
    };
    dialogKeysOf_ = dialog_;
    gui_.setModal(dialog_);
}

} // namespace noxsdl
