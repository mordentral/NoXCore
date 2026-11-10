// One address per action: the saved-address dropdown is independent of discovery results.
#include "menus.hpp"

#include "noxcore/strings/string_table.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

namespace noxsdl {
namespace {
constexpr int kOk = 4001, kClose = 4002, kEntry = 4003, kText = 4004, kTitle = 4005, kNo = 4006, kYes = 4007;
constexpr int kDrop = 4100, kRemove = 4101, kList = 4102, kUp = 4103, kDown = 4104, kSlider = 4105;
constexpr int kRows = 8, kRowHeight = 11;
constexpr int kDialogWidth = 240, kDialogHeight = 240, kButtonWidth = 55, kButtonHeight = 20;
constexpr SDL_Rect kInputRect{12, 38, 150, 20}, kDropdownRect{12, 68, 150, 20};
constexpr SDL_Rect kListRect{12, 90, kDropdownRect.w - 18, kRows * kRowHeight};
constexpr int kScrollX = kListRect.x + kListRect.w + 2, kActionX = 172;
constexpr int kLabelWidth = kListRect.w - 14; // list padding and dropdown arrow clearance
struct ServerDialogState {
    std::vector<std::string> addresses;
    int selected = -1;
    bool confirming = false, adding = false;
    std::string pending;
};
}

void Menus::serverDialog()
{
    closeGameInfo();
    closeDialog();
    const auto state = std::make_shared<ServerDialogState>();
    auto def = nox::gui::loadWindowFile(windowDir_ / "dlg.wnd");
    def.width = kDialogWidth;
    def.height = kDialogHeight;
    const auto close = std::find_if(def.children.begin(), def.children.end(), [](const auto& child) { return child.id == kClose; });
    if (close == def.children.end()) {
        throw std::runtime_error("dlg.wnd is missing the Close button");
    }
    const auto button = *close;
    const auto add = [&](int id, int x, int y, int w, int h, nox::gui::WindowType type) -> nox::gui::WindowDef& {
        auto child = button;
        child.id = id; child.x = x; child.y = y; child.width = w; child.height = h;
        child.type = type;
        child.status = nox::gui::status::Enabled | nox::gui::status::Hidden | nox::gui::status::NoFocus;
        child.backgroundImage.clear(); child.enabledImage.clear(); child.disabledImage.clear();
        child.hiliteImage.clear(); child.selectedImage.clear(); child.text.clear(); child.data = {};
        def.children.push_back(std::move(child));
        return def.children.back();
    };
    using Type = nox::gui::WindowType;
    add(kDrop, kDropdownRect.x, kDropdownRect.y, kDropdownRect.w, kDropdownRect.h, Type::PushButton);
    // The Remove button uses the same artwork as Add and Close.
    auto remove = button;
    remove.id = kRemove; remove.x = kActionX; remove.y = kDropdownRect.y;
    def.children.push_back(std::move(remove));
    auto& list = add(kList, kListRect.x, kListRect.y, kListRect.w, kListRect.h, Type::ScrollListBox);
    list.data = nox::gui::ListBoxData{0, kRowHeight - 1, {}};
    list.backgroundColor = {0, 0, 0, false};
    list.selectedColor = {0, 90, 0, false};
    add(kUp, kScrollX, kListRect.y, 16, 15, Type::PushButton);
    add(kDown, kScrollX, kListRect.y + kListRect.h - 15, 16, 15, Type::PushButton);
    add(kSlider, kScrollX, kListRect.y + 15, 16, kListRect.h - 30, Type::VertSlider).data = nox::gui::SliderData{0, 1000};

    // These helpers only operate on this dialog. The underlying addresses are never shortened.
    const auto label = [this](std::string address) {
        gui::Painter painter(nullptr, gui_.resources(), 640, 480);
        if (painter.textWidth(address, false) <= kLabelWidth) return address;
        auto text = nox::strings::fromUtf8(address);
        while (!text.empty() && painter.textWidth(nox::strings::toUtf8(text) + "...", false) > kLabelWidth) text.pop_back();
        return nox::strings::toUtf8(text) + "...";
    };
    const auto scroll = [this, state](int to) {
        auto* list = dialog_->find(kList);
        const int most = std::max(0, static_cast<int>(state->addresses.size()) - kRows);
        list->scroll = std::clamp(to, 0, most);
        auto* slider = dialog_->find(kSlider);
        slider->sliderValue = gui::sliderFromScroll(std::get<nox::gui::SliderData>(slider->data), list->scroll, most);
        gui::setGreyed(dialog_->find(kUp), list->scroll == 0);
        gui::setGreyed(dialog_->find(kDown), list->scroll == most);
    };
    const auto drop = [this, state, scroll](bool open) {
        open = open && !state->addresses.empty() && !state->confirming;
        for (const int id : {kList, kUp, kDown, kSlider}) dialog_->find(id)->show(open);
        if (open) {
            auto* list = dialog_->find(kList);
            list->selectedLine = std::max(0, state->selected);
            scroll(list->selectedLine - kRows + 1);
        }
        gui_.setFocus(open ? nullptr : dialog_->find(kEntry));
    };
    const auto normal = [this, state, drop] {
        state->confirming = false;
        for (const int id : {kOk, kClose, kEntry, kDrop, kRemove}) dialog_->find(id)->show(true);
        for (const int id : {kYes, kNo}) dialog_->find(id)->show(false);
        dialog_->find(kTitle)->text = "Add/Remove Servers";
        auto* text = dialog_->find(kText);
        text->rect = {12, 180, 216, 32};
        text->largeFont = false;
        text->text.clear();
        drop(false);
    };
    const auto reload = [this, state, label] {
        state->selected = -1;
        state->addresses.clear();
        try { state->addresses = browser_.savedServerAddresses(); }
        catch (const std::exception& ex) {
            SDL_Log("saved servers: %s", ex.what());
            dialog_->find(kText)->text = "Could not read servers.txt. Close and try again.";
        }
        auto* list = dialog_->find(kList);
        list->lines.clear(); list->selectedLine = -1; list->scroll = 0;
        for (const auto& address : state->addresses) list->lines.push_back({label(address), list->textColor});
        auto* box = dialog_->find(kDrop);
        box->text = state->addresses.empty() ? "No saved addresses" : "Select saved address";
        box->tooltip.clear();
        gui::setGreyed(box, state->addresses.empty());
        gui::setGreyed(dialog_->find(kRemove), true);
    };
    const auto choose = [this, state, label, drop] {
        const int row = dialog_->find(kList)->selectedLine;
        if (row < 0 || row >= static_cast<int>(state->addresses.size())) return;
        state->selected = row;
        auto* box = dialog_->find(kDrop);
        box->text = label(state->addresses[static_cast<std::size_t>(row)]);
        box->tooltip = state->addresses[static_cast<std::size_t>(row)];
        gui::setGreyed(dialog_->find(kRemove), false);
        drop(false);
    };
    const auto ask = [this, state, drop](bool adding) {
        if (state->confirming || (!adding && state->selected < 0)) return;
        const auto address = adding ? nox::strings::toUtf8(dialog_->find(kEntry)->entry)
                                   : state->addresses[static_cast<std::size_t>(state->selected)];
        if (adding && !normalizedServerAddress(address)) {
            dialog_->find(kText)->text = "Enter a hostname or IPv4 address; optional port: 1-65535.";
            return;
        }
        drop(false);
        state->confirming = true; state->adding = adding; state->pending = address;
        for (const int id : {kOk, kClose, kEntry, kDrop, kRemove}) dialog_->find(id)->show(false);
        for (const int id : {kYes, kNo}) dialog_->find(id)->show(true);
        dialog_->find(kTitle)->text = adding ? "Add this server?" : "Remove saved address?";
        auto* text = dialog_->find(kText);
        text->rect = {12, 38, 216, 145};
        text->text = address;
        dialog_->find(kYes)->text = adding ? "Add" : "Remove";
        gui_.setFocus(nullptr);
    };
    dialog_ = gui_.load(def, [this, state, scroll, drop, choose, ask, normal, reload](gui::Window&, int message, gui::Window& from) {
        if (message == gui::SliderChanged && from.id == kSlider) {
            scroll(gui::scrollFromSlider(std::get<nox::gui::SliderData>(from.data), from.sliderValue,
                                         std::max(0, static_cast<int>(state->addresses.size()) - kRows)));
            return true;
        }
        if (message == gui::EntryEntered && from.id == kEntry) { ask(true); return true; }
        if (message != gui::Clicked) return false;
        switch (from.id) {
        case kClose: deferred_.push_back([this] { closeDialog(); }); break;
        case kDrop: drop(dialog_->find(kList)->hidden()); break;
        case kList: choose(); break;
        case kUp: scroll(dialog_->find(kList)->scroll - 1); break;
        case kDown: scroll(dialog_->find(kList)->scroll + 1); break;
        case kOk: ask(true); break;
        case kRemove: ask(false); break;
        case kNo: normal(); break;
        case kYes:
            if (!state->confirming) break;
            // File writes and refresh happen outside GUI event dispatch.
            deferred_.push_back([this, state, normal, reload, expected = dialog_] {
                if (dialog_ != expected || !state->confirming) return;
                bool succeeded = false;
                std::string result;
                try {
                    bool changed = true;
                    if (state->adding) changed = browser_.addSavedServer(state->pending);
                    else browser_.removeSavedServer(state->pending);
                    SDL_Log("saved servers: %s %s", state->adding ? "added" : "removed", state->pending.c_str());
                    result = !changed ? "Already saved." : state->adding ? "Server added." : "Saved address removed.";
                    succeeded = true;
                } catch (const std::exception& ex) {
                    SDL_Log("saved servers: %s", ex.what());
                    result = "Could not change servers.txt. Check that it is writable and try again.";
                }
                // Reload after failures too: a final rename may have left a partial removal.
                browser_.refreshSavedServers(nowMs_);
                normal(); reload();
                dialog_->find(kText)->text = std::move(result);
                if (succeeded && state->adding) {
                    browser_.selectAddress(state->pending);
                    dialog_->find(kEntry)->entry.clear();
                }
            });
            break;
        default: return false;
        }
        return true;
    });
    gui::centreOnScreen(*dialog_);
    dialog_->status |= nox::gui::status::Above;
    dialog_->find(kTitle)->rect = {5, 6, kDialogWidth - 10, 20};
    auto* entry = dialog_->find(kEntry);
    entry->rect = kInputRect; entry->entryMax = 260; entry->largeFont = false;
    std::get<nox::gui::EntryFieldData>(entry->data).width = kInputRect.w;
    entry->tooltip = "Hostname or IPv4 address, optional :port (default 18590)";
    dialog_->find(kOk)->rect = {kActionX, kInputRect.y, kButtonWidth, kButtonHeight}; dialog_->find(kOk)->text = "Add";
    dialog_->find(kRemove)->text = "Remove";
    dialog_->find(kClose)->rect = {(kDialogWidth - kButtonWidth) / 2, 216, kButtonWidth, kButtonHeight}; dialog_->find(kClose)->text = "Close";
    dialog_->find(kYes)->rect = {45, 206, kButtonWidth, kButtonHeight};
    dialog_->find(kNo)->rect = {140, 206, kButtonWidth, kButtonHeight}; dialog_->find(kNo)->text = "Cancel";
    auto* box = dialog_->find(kDrop); box->dropArrow = true; box->largeFont = false;
    dialog_->find(kList)->largeFont = false;
    for (const auto& [id, image] : {std::pair{kUp, "UIScrollUp"}, std::pair{kDown, "UIScrollDn"}}) {
        auto* arrow = dialog_->find(id);
        arrow->status |= nox::gui::status::Image;
        arrow->backgroundImage = image; arrow->hiliteImage = std::string(image) + "Lit";
    }
    gui::useSliderThumb(dialog_->find(kSlider));
    normal(); reload();
    gui_.setModal(dialog_);
    dialogKeysOf_ = dialog_;
    dialogEvents_ = [this, state, drop, scroll, choose, normal](const SDL_Event& event) {
        const bool open = !dialog_->find(kList)->hidden();
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            if (event.key.repeat) return true;
            if (open) { drop(false); return true; }
            if (state->confirming) { normal(); return true; }
        }
        if (open && event.type == SDL_EVENT_MOUSE_WHEEL) {
            scroll(dialog_->find(kList)->scroll - static_cast<int>(event.wheel.y));
            return true;
        }
        if (open && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const auto pos = dialog_->screenPos();
            const int x = mouse_.x - pos.x, y = mouse_.y - pos.y;
            if (x < kDropdownRect.x || x >= kDropdownRect.x + kDropdownRect.w ||
                y < kDropdownRect.y || y >= kListRect.y + kListRect.h) { drop(false); return true; }
        }
        if (event.type != SDL_EVENT_KEY_DOWN || state->confirming) return false;
        if (open) {
            auto* list = dialog_->find(kList);
            int row = list->selectedLine;
            switch (event.key.key) {
            case SDLK_UP: --row; break;
            case SDLK_DOWN: ++row; break;
            case SDLK_PAGEUP: row -= kRows; break;
            case SDLK_PAGEDOWN: row += kRows; break;
            case SDLK_HOME: row = 0; break;
            case SDLK_END: row = static_cast<int>(state->addresses.size()) - 1; break;
            case SDLK_RETURN: case SDLK_KP_ENTER: choose(); return true;
            default: return true; // typing must not reach the hidden input focus
            }
            list->selectedLine = std::clamp(row, 0, static_cast<int>(state->addresses.size()) - 1);
            if (list->selectedLine < list->scroll) scroll(list->selectedLine);
            else if (list->selectedLine >= list->scroll + kRows) scroll(list->selectedLine - kRows + 1);
            return true;
        }
        if (event.key.key == SDLK_V && (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
            if (char* pasted = SDL_GetClipboardText()) {
                auto* entry = dialog_->find(kEntry);
                const auto text = nox::strings::fromUtf8(pasted);
                SDL_free(pasted);
                if (text.size() >= entry->entryMax) {
                    dialog_->find(kText)->text = "Address is too long. Paste a hostname or IPv4 address with an optional port.";
                } else {
                    entry->entry = text;
                }
            }
            return true;
        }
        return false;
    };
}
} // namespace noxsdl
